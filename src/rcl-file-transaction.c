/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rcl-file-transaction.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#ifdef RCL_FILE_TRANSACTION_TESTING
static RclFileTransactionTestOps test_ops;
void rcl_file_transaction_set_test_ops(const RclFileTransactionTestOps *ops){if(ops)test_ops=*ops;else memset(&test_ops,0,sizeof(test_ops));}
#define TM(p) (test_ops.mkstemp_fn?test_ops.mkstemp_fn(p):g_mkstemp(p))
#define TC(fd) (test_ops.close_fn?test_ops.close_fn(fd):close(fd))
#define TW(p,d,n,e) (test_ops.write_fn?test_ops.write_fn(p,d,n,e):g_file_set_contents(p,d,n,e))
#define TO(p,u,g) (test_ops.chown_fn?test_ops.chown_fn(p,u,g):chown(p,u,g))
#define TP(p,m) (test_ops.chmod_fn?test_ops.chmod_fn(p,m):g_chmod(p,m))
#define TR(a,b) (test_ops.rename_fn?test_ops.rename_fn(a,b):g_rename(a,b))
#define TU(p) (test_ops.unlink_fn?test_ops.unlink_fn(p):g_unlink(p))
#else
#define TM(p) g_mkstemp(p)
#define TC(fd) close(fd)
#define TW(p,d,n,e) g_file_set_contents(p,d,n,e)
#define TO(p,u,g) chown(p,u,g)
#define TP(p,m) g_chmod(p,m)
#define TR(a,b) g_rename(a,b)
#define TU(p) g_unlink(p)
#endif

/*
 * Remove a staged file without hiding a prior failure.  A failed unlink is
 * reported alongside the original error because leaving a named temporary
 * file behind is itself an operational failure.
 */
void
rcl_file_cleanup_temporary ( const gchar *path, GError **error )
{
  GError *cleanup_error = NULL;

  if( TU( path ) == 0 || errno == ENOENT )
    return;

  g_set_error( &cleanup_error, G_IO_ERROR, g_io_error_from_errno(errno),
               _("remove temporary file %s: %s"), path, g_strerror(errno) );

  if( error && *error )
  {
    GQuark domain = (*error)->domain;
    gint code = (*error)->code;
    gchar *message = g_strdup_printf(
      "%s (temporary-file cleanup also failed: %s)",
      (*error)->message, cleanup_error->message );
    g_clear_error( error );
    g_set_error_literal( error, domain, code, message );
    g_free( message );
    g_error_free( cleanup_error );
  }
  else if( error )
    g_propagate_error( error, cleanup_error );
  else
  {
    g_warning( "%s", cleanup_error->message );
    g_error_free( cleanup_error );
  }
}

gboolean
rcl_file_close_temporary ( int fd, const gchar *temporary_path,
                           const gchar *destination_path, GError **error )
{
  int saved_errno;

  g_return_val_if_fail( temporary_path != NULL, FALSE );
  g_return_val_if_fail( destination_path != NULL, FALSE );

  /*
   * On Linux, close() releases the descriptor even when it reports EINTR.
   * Never retry it: the descriptor number may already have been reused.
   */
  if( TC( fd ) == 0 )
    return TRUE;

  saved_errno = errno;
  {
    GError *close_error = NULL;

    g_set_error( &close_error, G_IO_ERROR,
                 g_io_error_from_errno(saved_errno),
                 _("close temporary file for %s: %s"),
                 destination_path, g_strerror(saved_errno) );

    /*
     * Keep the close failure even when the caller does not request an
     * error object.  Cleanup may append a secondary failure to this error.
     */
    rcl_file_cleanup_temporary( temporary_path, &close_error );

    if( error )
      g_propagate_error( error, close_error );
    else
    {
      g_warning( "%s", close_error->message );
      g_error_free( close_error );
    }
  }

  return FALSE;
}

void
rcl_file_snapshot_clear ( RclFileSnapshot *snapshot )
{
  if( !snapshot )
    return;

  g_free( snapshot->path );
  g_free( snapshot->contents );
  memset( snapshot, 0, sizeof(*snapshot) );
}

gboolean
rcl_file_snapshot_take ( RclFileSnapshot *snapshot,
                         const gchar *path,
                         GError **error )
{
  GStatBuf st, st_after;
  GString *contents;
  gchar buffer[8192];
  int fd;
  gboolean ok = FALSE;

  g_return_val_if_fail( snapshot != NULL, FALSE );
  g_return_val_if_fail( path != NULL, FALSE );

  memset( snapshot, 0, sizeof(*snapshot) );
  snapshot->path = g_strdup( path );

  /*
   * Open once and obtain metadata from that descriptor.  A separate
   * path-based stat after reading could describe a replacement inode if
   * another process atomically replaced the configuration file meanwhile.
   */
  fd = open( path, O_RDONLY | O_CLOEXEC );
  if( fd < 0 )
  {
    int saved_errno = errno;

    if( saved_errno == ENOENT )
    {
      /*
       * Confirm that the path is still absent.  It may have been created
       * between the failed open() and this check.
       */
      if( g_stat( path, &st ) == 0 )
      {
        g_set_error( error, G_IO_ERROR, G_IO_ERROR_BUSY,
                     _("file %s appeared while its snapshot was being opened"),
                     path );
        rcl_file_snapshot_clear( snapshot );
        return FALSE;
      }

      if( errno == ENOENT )
      {
        snapshot->existed = FALSE;
        return TRUE;
      }

      saved_errno = errno;
    }

    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                 _("open snapshot %s: %s"), path, g_strerror(saved_errno) );
    rcl_file_snapshot_clear( snapshot );
    return FALSE;
  }

  if( fstat( fd, &st ) != 0 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 _("stat snapshot %s: %s"), path, g_strerror(errno) );
    goto out;
  }

  contents = g_string_sized_new( 0 );
  for( ;; )
  {
    ssize_t n = read( fd, buffer, sizeof(buffer) );

    if( n < 0 && errno == EINTR )
      continue;

    if( n < 0 )
    {
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                   _("read snapshot %s: %s"), path, g_strerror(errno) );
      g_string_free( contents, TRUE );
      goto out;
    }

    if( n == 0 )
      break;

    g_string_append_len( contents, buffer, n );
  }

  /*
   * Verify that the pathname still resolves to the opened inode.  fstat()
   * describes the original descriptor even if another process atomically
   * replaces the path while the read is in progress.
   */
  {
    GStatBuf path_after;

    if( g_stat( path, &path_after ) != 0 )
    {
      int saved_errno = errno;

      if( saved_errno == ENOENT )
        g_set_error( error, G_IO_ERROR, G_IO_ERROR_BUSY,
                     _("file %s was removed while its snapshot was being read"),
                     path );
      else
        g_set_error( error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                     _("stat snapshot path %s: %s"),
                     path, g_strerror(saved_errno) );

      g_string_free( contents, TRUE );
      goto out;
    }

    if( st.st_dev != path_after.st_dev || st.st_ino != path_after.st_ino )
    {
      g_set_error( error, G_IO_ERROR, G_IO_ERROR_BUSY,
                   _("file %s was replaced while its snapshot was being read"),
                   path );
      g_string_free( contents, TRUE );
      goto out;
    }
  }

  /*
   * Reject in-place modifications during the read.  The pathname check
   * above handles atomic replacement; this comparison catches writes to the
   * opened inode itself.
   */
  if( fstat( fd, &st_after ) != 0 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 _("re-stat snapshot %s: %s"), path, g_strerror(errno) );
    g_string_free( contents, TRUE );
    goto out;
  }

  if( st.st_dev != st_after.st_dev ||
      st.st_ino != st_after.st_ino ||
      st.st_size != st_after.st_size ||
      st.st_mtim.tv_sec != st_after.st_mtim.tv_sec ||
      st.st_mtim.tv_nsec != st_after.st_mtim.tv_nsec ||
      st.st_ctim.tv_sec != st_after.st_ctim.tv_sec ||
      st.st_ctim.tv_nsec != st_after.st_ctim.tv_nsec )
  {
    g_set_error( error, G_IO_ERROR, G_IO_ERROR_BUSY,
                 _("file %s changed while its snapshot was being read"), path );
    g_string_free( contents, TRUE );
    goto out;
  }

  snapshot->length = contents->len;
  snapshot->contents = g_string_free( contents, FALSE );
  snapshot->mode = st.st_mode & 07777;
  snapshot->uid = st.st_uid;
  snapshot->gid = st.st_gid;
  snapshot->existed = TRUE;
  ok = TRUE;

out:
  if( close( fd ) != 0 && ok )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 _("close snapshot %s: %s"), path, g_strerror(errno) );
    ok = FALSE;
  }

  if( !ok )
    rcl_file_snapshot_clear( snapshot );

  return ok;
}

gboolean
rcl_file_snapshot_restore ( const RclFileSnapshot *snapshot,
                            GError **error )
{
  gchar *tmp_path;
  int fd;
  gboolean ok = FALSE;

  g_return_val_if_fail( snapshot != NULL, FALSE );
  g_return_val_if_fail( snapshot->path != NULL, FALSE );

  if( !snapshot->existed )
  {
    if( g_unlink( snapshot->path ) != 0 && errno != ENOENT )
    {
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                   _("remove %s during rollback: %s"),
                   snapshot->path, g_strerror(errno) );
      return FALSE;
    }
    return TRUE;
  }

  /*
   * Restore through a sibling temporary file.  Never write directly to the
   * destination: readers must see either the failed replacement or the
   * complete restored file, never truncated intermediate contents.
   */
  tmp_path = g_strdup_printf( "%s.XXXXXX", snapshot->path );
  fd = TM( tmp_path );
  if( fd == -1 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 _("create rollback temporary file for %s: %s"),
                 snapshot->path, g_strerror(errno) );
    g_free( tmp_path );
    return FALSE;
  }
  /*
   * close() can report delayed write/metadata errors.  Do not retry close()
   * after EINTR: on Linux the descriptor has already been released.
   */
  if( TC( fd ) != 0 )
  {
    int saved_errno = errno;
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                 _("close rollback temporary file %s: %s"),
                 tmp_path, g_strerror(saved_errno) );
    rcl_file_cleanup_temporary( tmp_path, error );
    g_free( tmp_path );
    return FALSE;
  }

  if( !TW( tmp_path, snapshot->contents,
             (gssize)snapshot->length, error ) )
  {
    g_prefix_error( error, "restore %s: ", snapshot->path );
    goto out;
  }

  /* chown may clear setuid/setgid bits: restore ownership before mode. */
  if( TO( tmp_path, snapshot->uid, snapshot->gid ) != 0 )
  {
    int saved_errno = errno;
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                 _("restore ownership on temporary file for %s: %s"), snapshot->path, g_strerror(saved_errno) );
    goto out;
  }
  if( TP( tmp_path, snapshot->mode ) != 0 )
  {
    int saved_errno = errno;
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                 _("restore permissions on temporary file for %s: %s"), snapshot->path, g_strerror(saved_errno) );
    goto out;
  }
  if( TR( tmp_path, snapshot->path ) != 0 )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 _("rollback rename(%s, %s): %s"),
                 tmp_path, snapshot->path, g_strerror(errno) );
    goto out;
  }

  ok = TRUE;

out:
  if( !ok )
    rcl_file_cleanup_temporary( tmp_path, error );
  g_free( tmp_path );
  return ok;
}

void
rcl_report_write_failure ( GError **error, GError *write_error )
{
  if( !write_error )
    return;

  if( error )
    g_propagate_error( error, write_error );
  else
  {
    /* Preserve the primary error and all rollback failures in one warning. */
    g_warning( "%s", write_error->message );
    g_error_free( write_error );
  }
}

void
rcl_report_rollback_failure ( GError **error, GError *rollback_error )
{
  if( !rollback_error )
    return;

  if( error && *error )
  {
    GQuark domain = (*error)->domain;
    gint code = (*error)->code;
    gchar *message = g_strdup_printf( "%s (rollback also failed: %s)", (*error)->message, rollback_error->message );
    g_clear_error( error );
    g_set_error_literal( error, domain, code, message );
    g_free( message );
    g_error_free( rollback_error );
  }
  else if( error )
    g_propagate_error( error, rollback_error );
  else
  {
    g_warning( "%s", rollback_error->message );
    g_error_free( rollback_error );
  }
}

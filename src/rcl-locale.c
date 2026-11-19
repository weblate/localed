/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glib-object.h>
#include <glib/gi18n-lib.h>

#include "rcl-locale.h"
#include "rcl-file-utils.h"
#include "rcl-file-transaction.h"
#include "rcl-xkb-config.h"

#ifdef RCL_LOCALE_TESTING
/*
 * Fail one forward configuration rename in the D-Bus integration tests.
 * Rollback uses rcl-file-transaction.c directly and is deliberately not
 * intercepted, allowing the test to verify restoration after a partial
 * multi-file update.
 */
static gchar *test_fail_rename_destination;

void
rcl_locale_test_fail_rename_to ( const gchar *destination )
{
  g_free( test_fail_rename_destination );
  test_fail_rename_destination = g_strdup( destination );
}

static int
rcl_locale_rename ( const gchar *source, const gchar *destination )
{
  if( test_fail_rename_destination &&
      g_strcmp0( destination, test_fail_rename_destination ) == 0 )
  {
    errno = EIO;
    return -1;
  }

  return g_rename( source, destination );
}
#else
#define rcl_locale_rename g_rename
#endif

/* --------------------------------------------------------------------------
   The org.freedesktop.locale1 introspection XML.

   Generated once at startup by g_dbus_node_info_new_for_xml() and kept for
   the lifetime of the process.  Property/method names here must match the
   handler and getter/setter functions below, and mirror the upstream
   systemd-localed interface so existing clients (gnome-control-center,
   localectl-alikes) work unmodified.
   -------------------------------------------------------------------------- */
static const gchar locale1_introspection_xml[] =
  "<node>"
  "  <interface name='org.freedesktop.locale1'>"
  /* ---- read-only properties ---- */
  "    <property name='Locale'               type='as' access='read'/>"
  "    <property name='VConsoleKeymap'       type='s'  access='read'/>"
  "    <property name='VConsoleKeymapToggle' type='s'  access='read'/>"
  "    <property name='X11Layout'            type='s'  access='read'/>"
  "    <property name='X11Model'             type='s'  access='read'/>"
  "    <property name='X11Variant'           type='s'  access='read'/>"
  "    <property name='X11Options'           type='s'  access='read'/>"
  /* ---- methods ---- */
  "    <method name='SetLocale'>"
  "      <arg name='locale'      type='as' direction='in'/>"
  "      <arg name='interactive' type='b'  direction='in'/>"
  "    </method>"
  "    <method name='SetVConsoleKeyboard'>"
  "      <arg name='keymap'        type='s' direction='in'/>"
  "      <arg name='keymap_toggle' type='s' direction='in'/>"
  "      <arg name='convert'       type='b' direction='in'/>"
  "      <arg name='interactive'   type='b' direction='in'/>"
  "    </method>"
  "    <method name='SetX11Keyboard'>"
  "      <arg name='layout'      type='s' direction='in'/>"
  "      <arg name='model'       type='s' direction='in'/>"
  "      <arg name='variant'     type='s' direction='in'/>"
  "      <arg name='options'     type='s' direction='in'/>"
  "      <arg name='convert'     type='b' direction='in'/>"
  "      <arg name='interactive' type='b' direction='in'/>"
  "    </method>"
  "  </interface>"
  "</node>";

/* --------------------------------------------------------------------------
   Private data
   -------------------------------------------------------------------------- */
struct _RclLocaleDaemonPrivate
{
  GDBusConnection  *connection;
  GDBusNodeInfo    *introspection;
  guint             registration_id;
  gboolean          debug;

  /*
   * Authorization requests use the daemon's private system-bus connection.
   * This avoids PolkitAuthority creating a separate GDBus connection whose
   * default exit-on-close behavior can terminate the daemon during bus loss.
   */
  GCancellable     *auth_cancellable;

  /* Cached property values.  Refreshed by rcl_daemon_sync_dbus_properties(). */
  gchar  **locale;                 /* GStrv, "KEY=value" entries, never NULL */
  gchar   *vconsole_keymap;
  gchar   *vconsole_keymap_toggle;
  gchar   *x11_layout;
  gchar   *x11_model;
  gchar   *x11_variant;
  gchar   *x11_options;
};

/* --------------------------------------------------------------------------
   GObject boilerplate
   -------------------------------------------------------------------------- */
G_DEFINE_TYPE_WITH_PRIVATE( RclLocaleDaemon, rcl_locale_daemon, RCL_TYPE_DAEMON )

/* Also define the abstract RclDaemon base just enough that GObject is happy.
   In a real multi-daemon codebase this would live in its own rcl-daemon.c. */
G_DEFINE_TYPE( RclDaemon, rcl_daemon, G_TYPE_OBJECT )

static void rcl_daemon_class_init( RclDaemonClass *klass ) {}
static void rcl_daemon_init( RclDaemon *self ) {}

static gboolean
rcl_keyboard_value_is_safe(const gchar *value);

/* --------------------------------------------------------------------------
   Known locale variables, in the canonical order systemd-localed and
   `localectl` expect when printing/comparing the Locale array.
   -------------------------------------------------------------------------- */
static const gchar *const locale_variable_names[] =
{
  "LANG",
  "LANGUAGE",
  "LC_CTYPE",
  "LC_NUMERIC",
  "LC_TIME",
  "LC_COLLATE",
  "LC_MONETARY",
  "LC_MESSAGES",
  "LC_PAPER",
  "LC_NAME",
  "LC_ADDRESS",
  "LC_TELEPHONE",
  "LC_MEASUREMENT",
  "LC_IDENTIFICATION",
  NULL
};

static gboolean
rcl_locale_variable_is_known( const gchar *name )
{
  guint i;
  for( i = 0; locale_variable_names[i] != NULL; i++ )
    if( g_strcmp0( name, locale_variable_names[i] ) == 0 )
      return TRUE;
  return FALSE;
}

/* --------------------------------------------------------------------------
   Small built-in console-keymap <-> X11 layout conversion table.

   This is intentionally a short, conservative subset (not the full kbd(4)
   keymap list) covering common single-layout console keymaps.  It is only
   consulted when a Set* call passes convert=TRUE; entries with no match
   simply leave the other side unset rather than guessing.
   -------------------------------------------------------------------------- */
typedef struct
{
  const gchar *vconsole_keymap;
  const gchar *x11_layout;
  const gchar *x11_variant;
} RclKeymapMapEntry;

static const RclKeymapMapEntry keymap_map[] =
{
  { "us",         "us", ""        },
  { "uk",         "gb", ""        },
  { "de",         "de", ""        },
  { "de-latin1",  "de", ""        },
  { "fr",         "fr", ""        },
  { "fr-latin1",  "fr", ""        },
  { "es",         "es", ""        },
  { "it",         "it", ""        },
  { "pt-latin1",  "pt", ""        },
  { "ru",         "ru", ""        },
  { "ru4",        "ru", ""        },
  { "se-latin1",  "se", ""        },
  { "no-latin1",  "no", ""        },
  { "dk-latin1",  "dk", ""        },
  { "nl",         "nl", ""        },
  { "trq",        "tr", ""        },
  { "pl",         "pl", ""        },
  { "cz-lat2",    "cz", ""        },
  { "fi",         "fi", ""        },
  { "gr",         "gr", ""        },
  { "il",         "il", ""        },
  { "jp106",      "jp", ""        },
  { "ua",         "ua", ""        },
  { "be-latin1",  "be", ""        },
  { "ch",         "ch", "de_nodeadkeys" },
  { "ch-fr",      "ch", "fr"      },
  { NULL,         NULL, NULL      }
};

/* --------------------------------------------------------------------------
   Language fallback table for LANGUAGE auto-derivation.

   This is a verbatim copy of the upstream systemd language-fallback-map
   (src/locale/language-fallback-map in the systemd source tree, installed
   at ${pkgdatadir}/language-fallback-map).

   When SetLocale is called with LANG= but without LANGUAGE=, the new LANG
   value is looked up here (exact match on the bare locale code, e.g.
   "en_AU" rather than "en_AU.UTF-8") and, if found, LANGUAGE is set to
   the corresponding colon-separated fallback chain.

   The table has only 13 entries and changes very infrequently in upstream,
   so we embed it directly rather than reading a runtime data file.
   -------------------------------------------------------------------------- */
typedef struct
{
  const gchar *lang;          /* bare locale code, e.g. "en_AU"    */
  const gchar *language;      /* LANGUAGE chain, e.g. "en_AU:en_GB" */
} RclLanguageFallbackEntry;

static const RclLanguageFallbackEntry language_fallback_map[] =
{
  { "csb_PL",  "csb:pl"     },
  { "en_AU",   "en_AU:en_GB" },
  { "en_IE",   "en_IE:en_GB" },
  { "en_NZ",   "en_NZ:en_GB" },
  { "en_ZA",   "en_ZA:en_GB" },
  { "fr_BE",   "fr_BE:fr_FR" },
  { "fr_CA",   "fr_CA:fr_FR" },
  { "fr_CH",   "fr_CH:fr_FR" },
  { "fr_LU",   "fr_LU:fr_FR" },
  { "it_CH",   "it_CH:it_IT" },
  { "mai_IN",  "mai:hi"      },
  { "nds_DE",  "nds:de"      },
  { "szl_PL",  "szl:pl"      },
  { NULL,      NULL           }
};

/*
 * rcl_find_language_fallback:
 *
 * Look up 'lang' in language_fallback_map using an exact-match comparison,
 * matching upstream systemd-localed semantics exactly.  The caller passes
 * the raw LANG value (e.g. "en_AU" or "en_AU.UTF-8").  Because the map
 * keys use bare locale codes without an encoding suffix, a value like
 * "en_AU.UTF-8" will NOT match "en_AU" — this is intentional and matches
 * upstream behaviour (the fallback table is only consulted when the locale
 * is specified without an explicit encoding).
 *
 * Returns the LANGUAGE fallback string (static, must not be freed), or NULL
 * if no match is found.
 */
static const gchar *
rcl_find_language_fallback( const gchar *lang )
{
  guint i;

  if( !lang || lang[0] == '\0' )
    return NULL;

  for( i = 0; language_fallback_map[i].lang != NULL; i++ )
    if( g_strcmp0( lang, language_fallback_map[i].lang ) == 0 )
      return language_fallback_map[i].language;

  return NULL;
}

static gboolean
rcl_keymap_to_x11( const gchar *keymap, gchar **out_layout, gchar **out_variant )
{
  guint i;
  for( i = 0; keymap_map[i].vconsole_keymap != NULL; i++ )
  {
    if( g_strcmp0( keymap, keymap_map[i].vconsole_keymap ) == 0 )
    {
      *out_layout  = g_strdup( keymap_map[i].x11_layout );
      *out_variant = g_strdup( keymap_map[i].x11_variant );
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
rcl_x11_to_keymap( const gchar *layout,
                   const gchar *variant,
                   gchar **out_keymap )
{
  guint i;
  /*
   * Match the first layout and its corresponding first variant.  XKB uses
   * positional correspondence between comma-separated layout and variant
   * lists; ignoring the variant can select a different console map (for
   * example, "ch" + "fr" must map to "ch-fr", not "ch").
   */
  gchar *first_layout = NULL;
  gchar *first_variant = NULL;
  const gchar *comma;
  gboolean found = FALSE;

  if( !layout || layout[0] == '\0' )
    return FALSE;

  comma = strchr( layout, ',' );
  first_layout = comma ? g_strndup( layout, (gsize)(comma - layout) )
                       : g_strdup( layout );

  if( !variant )
    variant = "";

  comma = strchr( variant, ',' );
  first_variant = comma ? g_strndup( variant, (gsize)(comma - variant) )
                        : g_strdup( variant );

  for( i = 0; keymap_map[i].vconsole_keymap != NULL; i++ )
  {
    if( g_strcmp0( first_layout, keymap_map[i].x11_layout ) == 0 &&
        g_strcmp0( first_variant, keymap_map[i].x11_variant ) == 0 )
    {
      *out_keymap = g_strdup( keymap_map[i].vconsole_keymap );
      found = TRUE;
      break;
    }
  }

  g_free( first_layout );
  g_free( first_variant );
  return found;
}

/* --------------------------------------------------------------------------
   Small file-reading helpers (KEY=value / KEY="value" files)
   -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
   X11 keyboard config (xorg.conf.d "InputClass" snippet) reader/writer.

   We read/write a minimal, fixed-shape file of the form:

     Section "InputClass"
         Identifier "system-keyboard"
         MatchIsKeyboard "on"
         Option "XkbLayout" "us"
         Option "XkbModel" "pc105"
         Option "XkbVariant" ""
         Option "XkbOptions" ""
     EndSection

   matching what systemd-localed and most distro X11 configs use. Options
   with an empty value are still emitted (most tools treat a present-but-
   empty Option the same as an absent one).
   -------------------------------------------------------------------------- */
static void
read_x11_keyboard_conf( const gchar *path,
                        gchar **out_layout, gchar **out_model,
                        gchar **out_variant, gchar **out_options )
{
  gchar *contents = NULL;
  gchar *layout = NULL, *model = NULL, *variant = NULL, *options = NULL;
  GError *error = NULL;

  *out_layout  = g_strdup( "" );
  *out_model   = g_strdup( "" );
  *out_variant = g_strdup( "" );
  *out_options = g_strdup( "" );

  if( !g_file_get_contents( path, &contents, NULL, &error ) )
  {
    /*
     * A missing Xorg snippet is a normal unconfigured state.  Other read
     * failures must be visible in the journal rather than silently looking
     * identical to a missing file.
     */
    if( !g_error_matches( error, G_FILE_ERROR, G_FILE_ERROR_NOENT ) )
      g_warning( "read_x11_keyboard_conf: cannot read %s: %s",
                 path, error ? error->message : "unknown read error" );
    g_clear_error( &error );
    return;
  }

  if( !rcl_xkb_config_parse( contents, &layout, &model, &variant,
                             &options, &error ) )
  {
    g_warning( "read_x11_keyboard_conf: cannot parse %s: %s",
               path, error ? error->message : "unknown parser error" );
    g_clear_error( &error );
    g_free( contents );
    return;
  }

  g_free( contents );

  g_free( *out_layout );  *out_layout = layout;
  g_free( *out_model );   *out_model = model;
  g_free( *out_variant ); *out_variant = variant;
  g_free( *out_options ); *out_options = options;

  /* Validate values read from disk against the same allowlist used by
   * SetX11Keyboard. Unsafe values are not exposed as D-Bus properties. */
#define SANITISE_XKB(field, name, kind)                                  \
  do {                                                                      \
    if( (*field)[0] != '\0' && !rcl_xkb_value_is_valid( kind, *field ) )   \
    {                                                                       \
      g_warning( "read_x11_keyboard_conf: invalid value for %s, ignoring", \
                 name );                                                    \
      g_clear_pointer( field, g_free );                                     \
      *field = g_strdup( "" );                                              \
    }                                                                       \
  } while(0)

  SANITISE_XKB( out_layout,  "XkbLayout",  RCL_XKB_VALUE_LAYOUT );
  SANITISE_XKB( out_model,   "XkbModel",   RCL_XKB_VALUE_MODEL );
  SANITISE_XKB( out_variant, "XkbVariant", RCL_XKB_VALUE_VARIANT );
  SANITISE_XKB( out_options, "XkbOptions", RCL_XKB_VALUE_OPTIONS );

#undef SANITISE_XKB
}

static gboolean
write_x11_keyboard_conf( const gchar *path,
                         const gchar *layout, const gchar *model,
                         const gchar *variant, const gchar *options,
                         GError **error )
{
  GString *out;
  gchar   *dir;
  gchar   *tmp_path;
  gboolean ok;

  dir = g_path_get_dirname( path );
  if( g_mkdir_with_parents( dir, 0755 ) != 0 && errno != EEXIST )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 _("mkdir -p %s: %s"), dir, g_strerror(errno) );
    g_free( dir );
    return FALSE;
  }
  g_free( dir );
  out = g_string_new( NULL );
  g_string_append( out, "# Written by localed - see localed(8)\n" );
  g_string_append( out, "Section \"InputClass\"\n" );
  g_string_append( out, "    Identifier \"system-keyboard\"\n" );
  g_string_append( out, "    MatchIsKeyboard \"on\"\n" );
  g_string_append_printf( out, "    Option \"XkbLayout\" \"%s\"\n",  layout  ? layout  : "" );
  g_string_append_printf( out, "    Option \"XkbModel\" \"%s\"\n",   model   ? model   : "" );
  g_string_append_printf( out, "    Option \"XkbVariant\" \"%s\"\n", variant ? variant : "" );
  g_string_append_printf( out, "    Option \"XkbOptions\" \"%s\"\n", options ? options : "" );
  g_string_append( out, "EndSection\n" );

  tmp_path = g_strdup_printf( "%s.XXXXXX", path );
  {
    int fd = g_mkstemp( tmp_path );
    if( fd == -1 )
    {
      int saved_errno = errno;
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                   _("create temporary file for %s: %s"),
                   path, g_strerror(saved_errno) );
      g_free( tmp_path );
      g_string_free( out, TRUE );
      return FALSE;
    }

    if( !rcl_file_close_temporary( fd, tmp_path, path, error ) )
    {
      g_free( tmp_path );
      g_string_free( out, TRUE );
      return FALSE;
    }
  }
  ok = g_file_set_contents( tmp_path, out->str, (gssize)out->len, error );

  if( ok )
  {
    /* 00-keyboard.conf must be world-readable so Xorg (which may run as a
     * dedicated user on hardened setups) can read it.  g_file_set_contents()
     * creates the temp file 0600; chmod it to 0644 before the rename so the
     * final file is always accessible, even when localed creates it from
     * scratch.  Mirrors the g_chmod() calls in rcl_write_lang_sh() and
     * rcl_write_rc_keymap_toggle(). */
    if( !rcl_file_apply_mode_for_write( tmp_path, path, 0644, error ) )
      ok = FALSE;

    if( ok && rcl_locale_rename( tmp_path, path ) != 0 )
    {
      int saved_errno = errno;
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                   _("rename(%s, %s): %s"),
                   tmp_path, path, g_strerror(saved_errno) );
      ok = FALSE;
    }
  }

  if( !ok )
    rcl_file_cleanup_temporary( tmp_path, error );

  g_free( tmp_path );
  g_string_free( out, TRUE );
  return ok;
}

/* --------------------------------------------------------------------------
   Slackware rc.keymap reader/writer.

   Slackware's console keymap is configured via /etc/rc.d/rc.keymap, a small
   shell script of the form:

     #!/bin/bash
     # Load the keyboard map.  More maps are in /usr/share/kbd/keymaps.
     if [ -x /usr/bin/loadkeys ]; then
       /usr/bin/loadkeys us
     fi

   We parse the loadkeys argument on read and regenerate the whole script on
   write so the file is always in a clean, known format.

   VConsoleKeymapToggle has no Slackware equivalent so it is persisted in a
   small sidecar file (RCL_RC_KEYMAP_TOGGLE_FILE).  The sidecar contains
   only the bare keymap name on a single line; an absent or empty file means
   no toggle keymap is configured.
   -------------------------------------------------------------------------- */

/* Extract the keymap argument from a Slackware rc.keymap script.
 * Returns a newly-allocated string, or NULL/empty-string when not found. */
static gchar *
rcl_read_rc_keymap( const gchar *path )
{
  gchar  *contents = NULL;
  gchar **lines;
  gchar  *result   = NULL;
  gsize   i;

  if( !g_file_get_contents( path, &contents, NULL, NULL ) )
    return g_strdup( "" );

  lines = g_strsplit( contents, "\n", -1 );
  g_free( contents );

  for( i = 0; lines[i] != NULL && result == NULL; i++ )
  {
    gchar *line = g_strstrip( lines[i] );

    /* Match:  /usr/bin/loadkeys <keymap>
     *    or:  loadkeys <keymap>
     * Ignore comment lines and blank lines. */
    if( line[0] == '#' || line[0] == '\0' )
      continue;

    /* Skip the [ -x /usr/bin/loadkeys ] test line — it contains ']'.
     * On the actual invocation line, the keymap is the last whitespace-
     * delimited token after "loadkeys". */
    if( strstr( line, "loadkeys" ) && !strchr( line, ']' ) )
    {
      const gchar *p = strstr( line, "loadkeys" );
      p += strlen( "loadkeys" );
      while( *p == ' ' || *p == '\t' ) p++;
      if( *p && *p != '#' && *p != '\0' )
      {
        /* $NF: walk to the last token on the line */
        const gchar *tok = p;
        while( *p )
        {
          if( ( *p == ' ' || *p == '\t' ) && *(p+1) && *(p+1) != ' ' && *(p+1) != '\t' )
            tok = p + 1;
          p++;
        }
        /* trim trailing whitespace */
        const gchar *end = p;
        while( end > tok && ( *(end-1) == ' ' || *(end-1) == '\t' || *(end-1) == '\n' ) )
          end--;
        if( end > tok )
        {
          result = g_strndup( tok, (gsize)(end - tok) );

          /* The writer quotes the keymap: /usr/bin/loadkeys "us"
           * Strip surrounding double-quotes so the stored value is bare. */
          if( result[0] == '"' )
          {
            gsize rlen = strlen( result );
            if( rlen >= 2 && result[rlen-1] == '"' )
            {
              gchar *unquoted = g_strndup( result + 1, rlen - 2 );
              g_free( result );
              result = unquoted;
            }
          }
        }
      }
    }
  }

  g_strfreev( lines );

  /* Validate the parsed keymap against the same allowlist used by
   * SetVConsoleKeyboard and rcl_read_rc_keymap_toggle().  A manually-edited
   * rc.keymap with a loadkeys argument containing characters outside
   * [A-Za-z0-9\-_.,+:] (path traversal, shell metacharacters, spaces) is
   * reset to empty rather than served as the VConsoleKeymap D-Bus property. */
  if( result && result[0] != '\0' && !rcl_keyboard_value_is_safe( result ) )
  {
    g_warning( "rcl_read_rc_keymap: unsafe keymap value in %s, ignoring", path );
    g_free( result );
    result = NULL;
  }

  return result ? result : g_strdup( "" );
}

/* Write a Slackware rc.keymap script for the given keymap.
 * An empty keymap writes a no-op script (preserves the file but calls
 * no loadkeys, consistent with "keymap cleared" semantics). */
static gboolean
rcl_write_rc_keymap( const gchar *path, const gchar *keymap, GError **error )
{
  GString *out;
  gchar   *tmp_path;
  gchar   *dir;
  gboolean ok;

  dir = g_path_get_dirname( path );
  if( g_mkdir_with_parents( dir, 0755 ) != 0 && errno != EEXIST )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 _("mkdir -p %s: %s"), dir, g_strerror(errno) );
    g_free( dir );
    return FALSE;
  }
  g_free( dir );
  out = g_string_new( NULL );
  g_string_append( out,
    "#!/bin/bash\n"
    "# Written by localed - see localed(8)\n"
    "# More keymaps are in /usr/share/kbd/keymaps\n"
    "if [ -x /usr/bin/loadkeys ]; then\n" );

  if( keymap && keymap[0] != '\0' )
    g_string_append_printf( out, "  /usr/bin/loadkeys \"%s\"\n", keymap );
  else
    g_string_append( out, "  : # no keymap configured\n" );

  g_string_append( out, "fi\n" );

  tmp_path = g_strdup_printf( "%s.XXXXXX", path );
  {
    int fd = g_mkstemp( tmp_path );
    if( fd == -1 )
    {
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                   _("create temporary file for %s: %s"), path, g_strerror(errno) );
      g_free( tmp_path );
      g_string_free( out, TRUE );
      return FALSE;
    }
    if( !rcl_file_close_temporary( fd, tmp_path, path, error ) )
    {
      g_free( tmp_path );
      g_string_free( out, TRUE );
      return FALSE;
    }
  }
  ok = g_file_set_contents( tmp_path, out->str, (gssize)out->len, error );

  if( ok )
  {
    /* Preserve existing mode, defaulting to executable 0755. */
    if( !rcl_file_apply_mode_for_write( tmp_path, path, 0755, error ) )
    {
      ok = FALSE;
    }

    if( ok && rcl_locale_rename( tmp_path, path ) != 0 )
    {
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                   _("rename(%s, %s): %s"), tmp_path, path, g_strerror(errno) );
      ok = FALSE;
    }
  }

  if( !ok )
    rcl_file_cleanup_temporary( tmp_path, error );

  g_free( tmp_path );
  g_string_free( out, TRUE );
  return ok;
}

/* Read the toggle keymap from the sidecar file.
 * Returns a newly-allocated string (empty if absent or invalid). */
static gchar *
rcl_read_rc_keymap_toggle( const gchar *path )
{
  gchar *contents = NULL;
  gchar *result;

  if( !g_file_get_contents( path, &contents, NULL, NULL ) )
    return g_strdup( "" );

  result = g_strstrip( contents );  /* in-place, returns same pointer */
  result = g_strdup( result );
  g_free( contents );

  /* Validate against the same allowlist used by SetVConsoleKeyboard and all
   * other keyboard string fields.  A manually-edited sidecar with characters
   * outside [A-Za-z0-9\-_.,+:] is reset to empty rather than served as a
   * D-Bus property, consistent with the SANITISE_XKB pattern used for X11
   * fields and the safe-charset check in rcl_read_rc_keymap(). */
  if( result[0] != '\0' && !rcl_keyboard_value_is_safe( result ) )
  {
    g_warning( "rcl_read_rc_keymap_toggle: unsafe value in %s, ignoring", path );
    g_free( result );
    result = g_strdup( "" );
  }

  return result;
}

/* Write (or remove) the toggle keymap sidecar file.
 * An empty keymap_toggle removes the file. */
static gboolean
rcl_write_rc_keymap_toggle( const gchar *path,
                             const gchar *keymap_toggle,
                             GError     **error )
{
  if( !keymap_toggle || keymap_toggle[0] == '\0' )
  {
    /* Remove the sidecar; ENOENT is fine */
    if( g_unlink( path ) != 0 && errno != ENOENT )
    {
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                   _("unlink(%s): %s"), path, g_strerror(errno) );
      return FALSE;
    }
    return TRUE;
  }

  {
    gchar    *line     = g_strdup_printf( "%s\n", keymap_toggle );
    gchar    *tmp_path = g_strdup_printf( "%s.XXXXXX", path );
    int       fd;
    gboolean  ok;
    fd = g_mkstemp( tmp_path );

    if( fd == -1 )
    {
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                   _("create temporary file for %s: %s"), path, g_strerror(errno) );
      g_free( tmp_path );
      return FALSE;
    }
    if( !rcl_file_close_temporary( fd, tmp_path, path, error ) )
    {
      g_free( tmp_path );
      g_free( line );
      return FALSE;
    }
    ok = g_file_set_contents( tmp_path, line, -1, error );

    if( ok )
    {
      /* Make the sidecar world-readable (0644) so it is consistent with
       * lang.sh and rc.keymap permissions; g_file_set_contents creates the
       * temp file 0600 which would leave the sidecar unreadable by non-root
       * readers if created from scratch. */
      if( !rcl_file_apply_mode_for_write( tmp_path, path, 0644, error ) )
      {
        ok = FALSE;
      }

      if( ok && rcl_locale_rename( tmp_path, path ) != 0 )
      {
        g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                     _("rename(%s, %s): %s"), tmp_path, path, g_strerror(errno) );
        ok = FALSE;
      }
    }

    if( !ok )
      rcl_file_cleanup_temporary( tmp_path, error );

    g_free( tmp_path );
    g_free( line );
    return ok;
  }
}

/* Maximum accepted length for locale and keyboard configuration values. */
#define MAX_XKB_VALUE_LEN  256

/* --------------------------------------------------------------------------
   Input validation
   -------------------------------------------------------------------------- */

/* Locale values (e.g. "en_US.UTF-8") and the LANGUAGE priority list
   (e.g. "en_US:en_GB:en") share a conservative safe charset. */
static gboolean
rcl_locale_value_is_safe( const gchar *value )
{
  const gchar *p;

  if( !value || strlen( value ) > MAX_XKB_VALUE_LEN )
    return FALSE;

  for( p = value; *p; p++ )
  {
    guchar c = (guchar) *p;
    if( g_ascii_isalnum(c) )
      continue;
    switch( c )
    {
      case '_': case '.': case '-': case '@': case ':': case ',':
        continue;
      default:
        return FALSE;
    }
  }
  return TRUE;
}

/* Console keymap names and X11 layout/model/variant/options strings are
   restricted to a strict allowlist: alphanumeric characters plus the handful
   of punctuation that appears in real keymap and XKB identifiers.
   Shell metacharacters ($, `, !, (, ), ;, |, &, >, <, *, ?, ~, {, }, space,
   etc.) are implicitly denied — they never appear in legitimate values and
   would be dangerous if written into rc.keymap or xorg.conf.d (both are
   executed/sourced as shell or parsed by Xorg). */
static gboolean
rcl_keyboard_value_is_safe( const gchar *value )
{
  const gchar *p;

  /* Match the reader's cap so accepted values read back unchanged. */
  if( !value || strlen( value ) > MAX_XKB_VALUE_LEN )
    return FALSE;

  for( p = value; *p; p++ )
  {
    guchar c = (guchar) *p;
    if( g_ascii_isalnum(c) )
      continue;
    switch( c )
    {
      /* Separators used in XKB option strings ("grp:win_space_toggle"),
         layout lists ("us,de"), and variant names ("nodeadkeys"). */
      case '-': case '_': case '.': case ',': case '+': case ':':
        continue;
      default:
        return FALSE;
    }
  }
  return TRUE;
}

/* --------------------------------------------------------------------------
   Property refresh
   -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
   Slackware lang.sh reader/writer.

   /etc/profile.d/lang.sh is sourced by /etc/profile at login.  It contains
   shell variable assignments of the form:

     export LANG="en_US.UTF-8"
     export LC_COLLATE="C"

   We parse only the lines that match "export KEY=..." for known locale
   variable names, and regenerate the file on write with the same format so
   it stays valid POSIX sh.

   On read:  returns a GHashTable (key->value) of locale variable assignments.
   On write: produces an atomic temp-file + rename to avoid partial writes.
   Existing lines for unknown variable names (comments, PATH exports, etc.)
   are preserved on write via a read-modify-write strategy.
   -------------------------------------------------------------------------- */

/*
 * rcl_read_lang_sh:
 * Parse /etc/profile.d/lang.sh and return a GHashTable (key->value) of
 * known locale variable assignments.  Lines of the form
 *   export KEY=value
 *   export KEY="value"
 * are recognised; the "export " prefix and optional quotes are stripped.
 * Returns an empty table (never NULL) when the file is missing.
 * The caller must g_hash_table_unref() the result.
 */
static GHashTable *
rcl_read_lang_sh( const gchar *path )
{
  GHashTable *table = g_hash_table_new_full( g_str_hash, g_str_equal,
                                              g_free, g_free );
  gchar  *contents = NULL;
  gchar **lines;
  gsize   i;

  if( !g_file_get_contents( path, &contents, NULL, NULL ) )
    return table;

  lines = g_strsplit( contents, "\n", -1 );
  g_free( contents );

  for( i = 0; lines[i] != NULL; i++ )
  {
    gchar *line = g_strstrip( lines[i] );
    gchar *rest, *eq, *key, *val;
    gsize  len;

    if( line[0] == '#' || line[0] == '\0' )
      continue;

    /* Must start with "export " */
    if( strncmp( line, "export ", 7 ) != 0 )
      continue;

    rest = line + 7;
    while( *rest == ' ' || *rest == '\t' )
      rest++;

    eq = strchr( rest, '=' );
    if( !eq )
      continue;

    key = g_strndup( rest, (gsize)(eq - rest) );
    g_strstrip( key );

    /* Only store known locale variables */
    if( !rcl_locale_variable_is_known( key ) )
    {
      g_free( key );
      continue;
    }

    val = g_strdup( eq + 1 );
    g_strstrip( val );

    /* Strip surrounding double quotes */
    len = strlen( val );
    if( len >= 2 && val[0] == '"' && val[len-1] == '"' )
    {
      gchar *unquoted = g_strndup( val + 1, len - 2 );
      g_free( val );
      val = unquoted;
    }

    /* Validate the value against the same safe-charset used by SetLocale.
     * Values that fail (e.g. from manual edits to lang.sh) are silently
     * skipped so a corrupt file cannot inject unsafe strings into the
     * Locale D-Bus property. */
    if( val[0] != '\0' && !rcl_locale_value_is_safe( val ) )
    {
      g_warning( "rcl_read_lang_sh: skipping unsafe value for %s", key );
      g_free( val );
      g_free( key );
      continue;
    }

    g_hash_table_replace( table, key, val );
  }

  g_strfreev( lines );
  return table;
}

/*
 * rcl_write_lang_sh:
 * Write updated locale variable assignments back to /etc/profile.d/lang.sh,
 * preserving any lines not in known_keys (comments, unrecognised exports,
 * etc.) using a read-modify-write strategy.
 *
 * known_keys / new_values are parallel NULL-terminated arrays of const gchar *.
 * A NULL or empty new_values[i] removes that key from the file.
 * All written values are double-quoted and preceded by "export ".
 *
 * The write is atomic (temp file + rename).
 */
static gboolean
rcl_write_lang_sh( const gchar         *path,
                   const gchar *const  *known_keys,
                   const gchar *const  *new_values,
                   GError             **error )
{
  GHashTable    *new_vals;
  GString       *out;
  gchar         *contents = NULL;
  GError        *read_error = NULL;
  gchar        **lines;
  gsize          i;
  gchar         *tmp_path;
  gboolean       ok;
  gchar         *dir;

  dir = g_path_get_dirname( path );
  if( g_mkdir_with_parents( dir, 0755 ) != 0 && errno != EEXIST )
  {
    g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                 _("mkdir -p %s: %s"), dir, g_strerror(errno) );
    g_free( dir );
    return FALSE;
  }
  g_free( dir );
  /* Read the existing file before constructing the replacement.  A missing
   * file is expected on first use; permission and I/O errors are not.  Do
   * not silently replace a file that we were unable to read and preserve. */
  if( !g_file_get_contents( path, &contents, NULL, &read_error ) )
  {
    if( !g_error_matches( read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT ) )
    {
      g_prefix_error( &read_error, "read %s: ", path );
      g_propagate_error( error, read_error );
      return FALSE;
    }
    g_clear_error( &read_error );
  }

  /* Build the updated value map from the incoming arrays.
   * Ownership contract: keys and values are borrowed pointers into the
   * caller's static/stack arrays (locale_variable_names[], language_fallback_map[],
   * and the locale GStrv).  NULL destructors are intentional — g_hash_table_unref
   * must NOT free them.  Any future caller passing heap-allocated strings must
   * either switch to g_hash_table_new_full with g_free destructors or dup first. */
  new_vals = g_hash_table_new_full( g_str_hash, g_str_equal, NULL, NULL );
  for( i = 0; known_keys[i] != NULL; i++ )
    g_hash_table_replace( new_vals,
                          (gpointer) known_keys[i],
                          (gpointer) (new_values[i] ? new_values[i] : "") );

  out = g_string_new( "# Written by localed - see localed(8)\n" );

  /* Re-read the file to preserve non-locale lines.
   *
   * Trust assumption: lang.sh is a root-owned file under /etc.  Non-locale
   * lines (comments, PATH exports, etc.) are copied verbatim from the
   * existing file without further sanitisation.  An attacker who can already
   * write to lang.sh has full root-level control of the login environment
   * by other means, so preserving those lines does not widen the attack
   * surface beyond what already exists.  Any caller that sources user-
   * supplied content into lang.sh before calling this function violates
   * that assumption and must sanitise the content itself. */
  if( contents != NULL )
  {
    lines = g_strsplit( contents, "\n", -1 );
    g_free( contents );

    for( i = 0; lines[i] != NULL; i++ )
    {
      gchar *line = g_strstrip( lines[i] );
      gchar *rest, *eq, *key;
      gboolean is_locale_export = FALSE;

      if( line[0] == '\0' )
        continue;

      /* Drop old "Written by localed" header; we wrote a fresh one above */
      if( strstr( line, "Written by localed" ) )
        continue;

      if( line[0] == '#' )
      {
        g_string_append_printf( out, "%s\n", line );
        continue;
      }

      if( strncmp( line, "export ", 7 ) == 0 )
      {
        rest = line + 7;
        while( *rest == ' ' || *rest == '\t' )
          rest++;
        eq = strchr( rest, '=' );
        if( eq )
        {
          key = g_strndup( rest, (gsize)(eq - rest) );
          g_strstrip( key );
          if( rcl_locale_variable_is_known( key ) )
            is_locale_export = TRUE;
          g_free( key );
        }
      }

      if( !is_locale_export )
        g_string_append_printf( out, "%s\n", line );
      /* Known locale exports are rewritten below in canonical order */
    }

    g_strfreev( lines );
  }

  /* Append locale variables in canonical order */
  {
    guint j;
    for( j = 0; locale_variable_names[j] != NULL; j++ )
    {
      const gchar *val = g_hash_table_lookup( new_vals,
                                              locale_variable_names[j] );
      if( val && val[0] != '\0' )
        g_string_append_printf( out, "export %s=\"%s\"\n",
                                locale_variable_names[j], val );
    }
  }

  g_hash_table_unref( new_vals );

  tmp_path = g_strdup_printf( "%s.XXXXXX", path );
  {
    int fd = g_mkstemp( tmp_path );
    if( fd == -1 )
    {
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                   _("create temporary file for %s: %s"), path, g_strerror(errno) );
      g_free( tmp_path );
      g_string_free( out, TRUE );
      return FALSE;
    }
    if( !rcl_file_close_temporary( fd, tmp_path, path, error ) )
    {
      g_free( tmp_path );
      g_string_free( out, TRUE );
      return FALSE;
    }
  }
  ok = g_file_set_contents( tmp_path, out->str, (gssize)out->len, error );

  if( ok )
  {
    /* lang.sh must be world-executable (0755) so login shells can source it.
     * /etc/profile sources files in /etc/profile.d/ via '.', which requires
     * execute permission.  g_file_set_contents() creates the temp file 0600;
     * chmod it to 0755 before the rename so the final file is always
     * accessible, even when localed creates it from scratch. */
    if( !rcl_file_apply_mode_for_write( tmp_path, path, 0755, error ) )
    {
      ok = FALSE;
    }

    if( ok && rcl_locale_rename( tmp_path, path ) != 0 )
    {
      g_set_error( error, G_IO_ERROR, g_io_error_from_errno(errno),
                   _("rename(%s, %s): %s"), tmp_path, path, g_strerror(errno) );
      ok = FALSE;
    }
  }

  if( !ok )
    rcl_file_cleanup_temporary( tmp_path, error );

  g_free( tmp_path );
  g_string_free( out, TRUE );
  return ok;
}

/* Write the C-shell counterpart of Slackware's system locale settings.
 * lang.csh intentionally retains Slackware's LC_COLLATE=C default; lang.sh
 * does not force LC_COLLATE, so Bourne-shell users follow LANG unless an
 * explicit LC_COLLATE was selected. */
static gboolean
rcl_write_lang_csh( const gchar         *path,
                    const gchar *const  *known_keys,
                    const gchar *const  *new_values,
                    GError             **error )
{
  GHashTable *values;
  GString *out;
  gchar *dir, *tmp_path;
  gboolean ok = FALSE;
  guint i;
  const gchar *lang, *collate;

  dir = g_path_get_dirname(path);
  if( g_mkdir_with_parents(dir, 0755) != 0 && errno != EEXIST )
  {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                _("mkdir -p %s: %s"), dir, g_strerror(errno));
    g_free(dir);
    return FALSE;
  }
  g_free(dir);
  values = g_hash_table_new(g_str_hash, g_str_equal);
  for( i = 0; known_keys[i] != NULL; i++ )
    g_hash_table_replace(values, (gpointer)known_keys[i],
                         (gpointer)(new_values[i] ? new_values[i] : ""));

  out = g_string_new("#!/bin/csh\n");
  g_string_append(out, "# Written by localed - see localed(8)\n");
  g_string_append(out, "# Set the system locale.\n");

  lang = g_hash_table_lookup(values, "LANG");
  if( lang && lang[0] )
    g_string_append_printf(out, "setenv LANG %s\n", lang);

  for( i = 1; locale_variable_names[i] != NULL; i++ )
  {
    const gchar *key = locale_variable_names[i];
    const gchar *value = g_hash_table_lookup(values, key);
    if( value && value[0] && g_strcmp0(key, "LC_COLLATE") != 0 )
      g_string_append_printf(out, "setenv %s %s\n", key, value);
  }

  collate = g_hash_table_lookup(values, "LC_COLLATE");
  if( !collate || !collate[0] )
    collate = "C";
  g_string_append_printf(out, "setenv LC_COLLATE %s\n", collate);
  g_string_append(out, "# End of /etc/profile.d/lang.csh\n");

  g_hash_table_unref(values);

  tmp_path = g_strdup_printf("%s.XXXXXX", path);
  {
    int fd = g_mkstemp(tmp_path);
    if( fd == -1 )
    {
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                  _("create temporary file for %s: %s"), path, g_strerror(errno));
      g_free(tmp_path);
      g_string_free(out, TRUE);
      return FALSE;
    }
    if( !rcl_file_close_temporary( fd, tmp_path, path, error ) )
    {
      g_free( tmp_path );
      g_string_free( out, TRUE );
      return FALSE;
    }
  }

  ok = g_file_set_contents(tmp_path, out->str, (gssize)out->len, error);
  if( ok && !rcl_file_apply_mode_for_write(tmp_path, path, 0755, error) )
    ok = FALSE;
  if( ok && g_rename(tmp_path, path) != 0 )
  {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                _("rename(%s, %s): %s"), tmp_path, path, g_strerror(errno));
    ok = FALSE;
  }
  if( !ok )
    rcl_file_cleanup_temporary(tmp_path, error);

  g_free(tmp_path);
  g_string_free(out, TRUE);
  return ok;
}

/* Build the canonical "KEY=value" GStrv for the Locale property from
   whatever known LANG/LANGUAGE/LC_xxx keys are present in RCL_LANG_SH_FILE. */
static gchar **
read_locale_strv( void )
{
  GHashTable *pairs = rcl_read_lang_sh( RCL_LANG_SH_FILE );
  GPtrArray  *arr   = g_ptr_array_new();
  guint       i;

  for( i = 0; locale_variable_names[i] != NULL; i++ )
  {
    const gchar *val = g_hash_table_lookup( pairs, locale_variable_names[i] );
    if( val && val[0] != '\0' )
      g_ptr_array_add( arr, g_strdup_printf( "%s=%s", locale_variable_names[i], val ) );
  }

  g_ptr_array_add( arr, NULL );
  g_hash_table_unref( pairs );

  return (gchar **) g_ptr_array_free( arr, FALSE );
}

static gboolean
strv_equal( gchar **a, gchar **b )
{
  guint i;

  if( !a || !b )
    return a == b;

  for( i = 0; a[i] != NULL && b[i] != NULL; i++ )
    if( g_strcmp0( a[i], b[i] ) != 0 )
      return FALSE;

  return a[i] == NULL && b[i] == NULL;
}

/*
 * Populate (or refresh) all cached properties from the underlying files,
 * then emit PropertiesChanged for anything that actually changed.
 */
void
rcl_daemon_sync_dbus_properties( RclLocaleDaemon *daemon )
{
  RclLocaleDaemonPrivate *priv = daemon->priv;
  GVariantBuilder changed_props;
  gboolean any_changed = FALSE;

  g_variant_builder_init( &changed_props, G_VARIANT_TYPE("a{sv}") );

#define UPDATE_STR_PROP( field, new_val, dbus_name )              \
  do {                                                            \
    gchar *_nv = (new_val);                                       \
    if( g_strcmp0( priv->field, _nv ) != 0 )                     \
    {                                                             \
      g_free( priv->field );                                      \
      priv->field = _nv ? g_strdup(_nv) : g_strdup("");          \
      g_variant_builder_add( &changed_props, "{sv}",             \
                             dbus_name,                           \
                             g_variant_new_string(priv->field) );\
      any_changed = TRUE;                                         \
    }                                                             \
    g_free( _nv );                                                \
  } while(0)

  /* ---- Locale (array of "KEY=value") ---- */
  {
    gchar **new_locale = read_locale_strv();
    if( !strv_equal( priv->locale, new_locale ) )
    {
      g_strfreev( priv->locale );
      priv->locale = new_locale;
      g_variant_builder_add( &changed_props, "{sv}", "Locale",
                             g_variant_new_strv( (const gchar * const *) priv->locale, -1 ) );
      any_changed = TRUE;
    }
    else
    {
      g_strfreev( new_locale );
    }
  }

  /* ---- rc.keymap (Slackware console keyboard) ---- */
  {
    UPDATE_STR_PROP( vconsole_keymap,
                     rcl_read_rc_keymap( RCL_RC_KEYMAP_FILE ),
                     "VConsoleKeymap" );
    /* Toggle is persisted in a small sidecar file */
    UPDATE_STR_PROP( vconsole_keymap_toggle,
                     rcl_read_rc_keymap_toggle( RCL_RC_KEYMAP_TOGGLE_FILE ),
                     "VConsoleKeymapToggle" );
  }

  /* ---- X11 keyboard config ---- */
  {
    gchar *layout, *model, *variant, *options;
    read_x11_keyboard_conf( RCL_X11_KEYBOARD_FILE, &layout, &model, &variant, &options );
    UPDATE_STR_PROP( x11_layout,  layout,  "X11Layout" );
    UPDATE_STR_PROP( x11_model,   model,   "X11Model" );
    UPDATE_STR_PROP( x11_variant, variant, "X11Variant" );
    UPDATE_STR_PROP( x11_options, options, "X11Options" );
  }

#undef UPDATE_STR_PROP

  /* Emit PropertiesChanged only if something actually changed */
  if( any_changed && priv->connection && priv->registration_id > 0 )
  {
    GError *err = NULL;
    g_dbus_connection_emit_signal(
      priv->connection,
      NULL,
      "/org/freedesktop/locale1",
      "org.freedesktop.DBus.Properties",
      "PropertiesChanged",
      g_variant_new( "(sa{sv}as)",
                     "org.freedesktop.locale1",
                     &changed_props,
                     NULL ),
      &err );

    if( err )
    {
      g_warning( "PropertiesChanged emission failed: %s", err->message );
      g_error_free( err );
    }
  }
  else
  {
    g_variant_builder_clear( &changed_props );
  }
}

/* --------------------------------------------------------------------------
   PolKit authorisation (asynchronous)

   The authorisation check is performed asynchronously so the agent password
   dialog never blocks the single-threaded GMainLoop.  Each in-flight call
   carries an AuthCtx describing the operation to run once (and only if) it
   is authorised.  The operation handlers below complete the invocation
   exactly once each.
   -------------------------------------------------------------------------- */

typedef enum
{
  OP_SET_LOCALE,
  OP_SET_VCONSOLE_KEYBOARD,
  OP_SET_X11_KEYBOARD,
} LocaleOp;

typedef struct
{
  RclLocaleDaemon        *daemon;       /* owned ref */
  GDBusMethodInvocation  *invocation;   /* owned ref; completed exactly once */
  GCancellable           *cancellable;  /* owned ref to this daemon session */
  LocaleOp                op;

  gchar  **locale;        /* OP_SET_LOCALE: owned GStrv */

  gchar   *keymap;        /* OP_SET_VCONSOLE_KEYBOARD */
  gchar   *keymap_toggle;

  gchar   *x11_layout;    /* OP_SET_X11_KEYBOARD */
  gchar   *x11_model;
  gchar   *x11_variant;
  gchar   *x11_options;

  gboolean convert;       /* shared by the two keyboard ops */

  GDBusConnection *auth_connection;
  gchar *cancellation_id;
  gulong cancel_handler_id;
} AuthCtx;

typedef struct
{
  GDBusConnection *connection;
  gchar *cancellation_id;
} AuthCancelData;

static void
auth_cancel_data_free( gpointer user_data )
{
  AuthCancelData *data = user_data;

  g_clear_object( &data->connection );
  g_free( data->cancellation_id );
  g_free( data );
}

static void
on_cancel_authorization_done( GObject *source,
                              GAsyncResult *result,
                              gpointer user_data )
{
  GError *error = NULL;
  GVariant *reply;

  reply = g_dbus_connection_call_finish( G_DBUS_CONNECTION(source),
                                         result, &error );
  if( reply )
    g_variant_unref( reply );
  else if( error )
  {
    g_debug( "PolicyKit cancellation request could not be delivered: %s",
             error->message );
    g_clear_error( &error );
  }
}

static void
on_auth_cancelled( GCancellable *cancellable, gpointer user_data )
{
  AuthCancelData *data = user_data;

  /*
   * CancelCheckAuthorization is best-effort. The local GDBus call is also
   * cancelled, and its completion path independently prevents a cancelled
   * request from applying any privileged change.
   */
  g_dbus_connection_call(
    data->connection,
    "org.freedesktop.PolicyKit1",
    "/org/freedesktop/PolicyKit1/Authority",
    "org.freedesktop.PolicyKit1.Authority",
    "CancelCheckAuthorization",
    g_variant_new( "(s)", data->cancellation_id ),
    NULL, /* CancelCheckAuthorization has no output arguments. */
    G_DBUS_CALL_FLAGS_NONE,
    -1,
    NULL,
    on_cancel_authorization_done,
    NULL );
}

static void
auth_ctx_free( AuthCtx *ctx )
{
  if( !ctx )
    return;

  if( ctx->cancellable && ctx->cancel_handler_id )
  {
    g_cancellable_disconnect( ctx->cancellable, ctx->cancel_handler_id );
    ctx->cancel_handler_id = 0;
  }

  g_clear_object( &ctx->auth_connection );
  g_free( ctx->cancellation_id );
  g_clear_object( &ctx->daemon );
  g_clear_object( &ctx->invocation );
  g_clear_object( &ctx->cancellable );
  g_strfreev( ctx->locale );
  g_free( ctx->keymap );
  g_free( ctx->keymap_toggle );
  g_free( ctx->x11_layout );
  g_free( ctx->x11_model );
  g_free( ctx->x11_variant );
  g_free( ctx->x11_options );
  g_free( ctx );
}

static gboolean rcl_write_locale_files( const gchar *const *known_keys,
                                         const gchar *const *new_values,
                                         GError **error );
static gboolean rcl_write_vconsole_files( const gchar *keymap,
                                          const gchar *keymap_toggle,
                                          GError **error );
static gboolean rcl_write_x11_vconsole_files( const gchar *layout,
                                              const gchar *model,
                                              const gchar *variant,
                                              const gchar *options,
                                              const gchar *keymap,
                                              GError **error );
static gboolean rcl_write_vconsole_x11_files( const gchar *keymap,
                                              const gchar *keymap_toggle,
                                              const gchar *layout,
                                              const gchar *model,
                                              const gchar *variant,
                                              const gchar *options,
                                              GError **error );

/* ---- operation handlers: run only after a successful authorisation ---- */

static void
do_set_locale( RclLocaleDaemon *daemon, gchar **locale,
               GDBusMethodInvocation *invocation )
{
  const gchar *known_keys[ G_N_ELEMENTS(locale_variable_names) ];
  const gchar *new_values[ G_N_ELEMENTS(locale_variable_names) ];
  guint   i, n_known = 0;
  GError *error = NULL;

  /*
   * Bare-locale shorthand: if exactly one string with no '=' is provided,
   * treat it as LANG=<value>, matching upstream systemd-localed behaviour.
   * Example: SetLocale(["en_US.UTF-8"], true)  ->  LANG=en_US.UTF-8
   */
  if( locale[0] != NULL && locale[1] == NULL && strchr(locale[0], '=') == NULL )
  {
    if( !rcl_locale_value_is_safe( locale[0] ) )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Invalid locale specification: '%s'", locale[0] );
      return;
    }
    gchar *expanded = g_strdup_printf( "LANG=%s", locale[0] );
    g_free( locale[0] );
    locale[0] = expanded;
  }

  /* Reject oversized arrays before doing any per-entry work.
   * The maximum useful number of entries equals the number of known locale
   * variables; anything larger is either a client bug or a DoS attempt.
   * G_N_ELEMENTS includes the NULL sentinel, so subtract 1 for the count. */
  {
    guint n_entries = 0;
    while( locale[n_entries] != NULL )
      n_entries++;
    if( n_entries > G_N_ELEMENTS(locale_variable_names) - 1 )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Too many locale entries (max %u)",
        (guint)(G_N_ELEMENTS(locale_variable_names) - 1) );
      return;
    }
  }

  /* Validate every entry first; reject the whole call on the first bad one,
     same semantics as systemd-localed. */
  for( i = 0; locale[i] != NULL; i++ )
  {
    gchar *eq = strchr( locale[i], '=' );
    g_autofree gchar *key = NULL;
    const gchar *val;

    if( !eq )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Locale assignment '%s' is not of the form KEY=value", locale[i] );
      return;
    }

    key = g_strndup( locale[i], (gsize)(eq - locale[i]) );
    val = eq + 1;

    if( !rcl_locale_variable_is_known( key ) )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Unknown locale variable '%s'", key );
      return;
    }

    if( val[0] != '\0' && !rcl_locale_value_is_safe( val ) )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Invalid value for %s", key );
      return;
    }
  }

  /* Build the known_keys/new_values pair arrays. SetLocale replaces the
     whole locale set (systemd semantics): rcl_write_lang_sh() drops every
     known locale export from the file and re-emits only these, so any
     variable not supplied in the call is removed. */
  for( i = 0; locale_variable_names[i] != NULL; i++ )
  {
    guint j;
    const gchar *match = NULL;

    for( j = 0; locale[j] != NULL; j++ )
    {
      gchar *eq = strchr( locale[j], '=' );
      gsize  klen = eq ? (gsize)(eq - locale[j]) : 0;
      if( eq && strncmp( locale[j], locale_variable_names[i], klen ) == 0 &&
          locale_variable_names[i][klen] == '\0' )
      {
        match = eq + 1;
        break;
      }
    }

    if( match )
    {
      known_keys[n_known] = locale_variable_names[i];
      new_values[n_known] = match;
      n_known++;
    }
  }
  known_keys[n_known] = NULL;
  new_values[n_known] = NULL;

  /*
   * LANGUAGE auto-derivation (gap 1 vs systemd 257).
   *
   * If LANG was supplied in this call but LANGUAGE was not, check the
   * language-fallback table.  If a match is found, inject LANGUAGE into
   * the write arrays so it is persisted atomically in the same
   * write_key_value_file() call.
   *
   * Conditions for the injection:
   *   a) LANG= is being set by this call (match != NULL for "LANG")
   *   b) LANGUAGE= is NOT being set by this call
   *   c) rcl_find_language_fallback() returns a non-NULL chain
   *
   * The injected LANGUAGE value is appended to known_keys/new_values
   * (the arrays are sized G_N_ELEMENTS(locale_variable_names) so there is
   * always room for one more entry before the NULL sentinel).
   */
  {
    gboolean lang_set      = FALSE;
    gboolean language_set  = FALSE;
    guint    k;

    for( k = 0; k < n_known; k++ )
    {
      if( g_strcmp0( known_keys[k], "LANG"     ) == 0 ) lang_set     = TRUE;
      if( g_strcmp0( known_keys[k], "LANGUAGE" ) == 0 ) language_set = TRUE;
    }

    if( lang_set && !language_set )
    {
      /* Find the LANG value we are about to write */
      const gchar *lang_value = NULL;
      for( k = 0; k < n_known; k++ )
        if( g_strcmp0( known_keys[k], "LANG" ) == 0 )
        {
          lang_value = new_values[k];
          break;
        }

      if( lang_value && lang_value[0] != '\0' )
      {
        const gchar *fallback = rcl_find_language_fallback( lang_value );
        if( fallback )
        {
          g_debug( "LANG=%s -> auto-setting LANGUAGE=%s", lang_value, fallback );
          known_keys[n_known] = "LANGUAGE";
          new_values[n_known] = fallback;
          n_known++;
          known_keys[n_known] = NULL;
          new_values[n_known] = NULL;
        }
      }
    }
  }

  if( !rcl_write_locale_files( known_keys, new_values, &error ) )
  {
    rcl_daemon_sync_dbus_properties( daemon );
    g_dbus_method_invocation_return_gerror( invocation, error );
    g_error_free( error );
    return;
  }

  rcl_daemon_sync_dbus_properties( daemon );
  g_dbus_method_invocation_return_value( invocation, NULL );
}

/*
 * Apply a GNOME AccountsService language update without making a recursive
 * D-Bus call to our own locale1 service. The bridge in main.c has already
 * checked the affected user's membership in PRIVILEGED_GROUP.
 *
 * Match SetLocale semantics: replace the system locale assignments with LANG,
 * and apply the same LANGUAGE fallback when the selected value has a mapping.
 */
gboolean
rcl_locale_daemon_set_language_from_accounts( RclLocaleDaemon *daemon,
                                              const gchar *language,
                                              GError **error )
{
  const gchar *known_keys[3] = { "LANG", NULL, NULL };
  const gchar *new_values[3] = { language, NULL, NULL };
  const gchar *fallback;

  if (!language || !rcl_locale_value_is_safe(language))
  {
    g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                        _("Invalid locale value from AccountsService"));
    return FALSE;
  }

  fallback = rcl_find_language_fallback(language);
  if (fallback)
  {
    known_keys[1] = "LANGUAGE";
    new_values[1] = fallback;
  }

  if (!rcl_write_locale_files(known_keys, new_values, error))
  {
    rcl_daemon_sync_dbus_properties(daemon);
    return FALSE;
  }

  rcl_daemon_sync_dbus_properties(daemon);
  return TRUE;
}

static void
do_set_vconsole_keyboard( RclLocaleDaemon *daemon,
                          const gchar *keymap, const gchar *keymap_toggle,
                          gboolean convert,
                          GDBusMethodInvocation *invocation )
{
  GError *error = NULL;

  if( keymap[0] != '\0' && !rcl_keyboard_value_is_safe( keymap ) )
  {
    g_dbus_method_invocation_return_error( invocation,
      G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Invalid keymap '%s'", keymap );
    return;
  }
  if( keymap_toggle[0] != '\0' && !rcl_keyboard_value_is_safe( keymap_toggle ) )
  {
    g_dbus_method_invocation_return_error( invocation,
      G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
      "Invalid keymap toggle '%s'", keymap_toggle );
    return;
  }

  if( convert && keymap[0] != '\0' )
  {
    gchar *x11_layout = NULL, *x11_variant = NULL;
    if( rcl_keymap_to_x11( keymap, &x11_layout, &x11_variant ) )
    {
      RclLocaleDaemonPrivate *priv = daemon->priv;
      if( !rcl_write_vconsole_x11_files( keymap, keymap_toggle,
                                         x11_layout, priv->x11_model,
                                         x11_variant, priv->x11_options,
                                         &error ) )
      {
        rcl_daemon_sync_dbus_properties( daemon );
        g_dbus_method_invocation_return_gerror( invocation, error );
        g_clear_error( &error );
        g_free( x11_layout );
        g_free( x11_variant );
        return;
      }
      g_free( x11_layout );
      g_free( x11_variant );
    }
    else
    {
      g_debug( "convert: no built-in X11 mapping for keymap '%s'", keymap );
      if( !rcl_write_vconsole_files( keymap, keymap_toggle, &error ) )
      {
        rcl_daemon_sync_dbus_properties( daemon );
        g_dbus_method_invocation_return_gerror( invocation, error );
        g_clear_error( &error );
        return;
      }
    }
  }
  else if( !rcl_write_vconsole_files( keymap, keymap_toggle, &error ) )
  {
    rcl_daemon_sync_dbus_properties( daemon );
    g_dbus_method_invocation_return_gerror( invocation, error );
    g_clear_error( &error );
    return;
  }

  rcl_daemon_sync_dbus_properties( daemon );
  g_dbus_method_invocation_return_value( invocation, NULL );
}

static gboolean
rcl_x11_keyboard_values_are_valid( const gchar *layout,
                                   const gchar *model,
                                   const gchar *variant,
                                   const gchar *options )
{
  return rcl_xkb_value_is_valid( RCL_XKB_VALUE_LAYOUT, layout ) &&
         rcl_xkb_value_is_valid( RCL_XKB_VALUE_MODEL, model ) &&
         rcl_xkb_value_is_valid( RCL_XKB_VALUE_VARIANT, variant ) &&
         rcl_xkb_value_is_valid( RCL_XKB_VALUE_OPTIONS, options );
}

static void
do_set_x11_keyboard( RclLocaleDaemon *daemon,
                     const gchar *layout, const gchar *model,
                     const gchar *variant, const gchar *options,
                     gboolean convert,
                     GDBusMethodInvocation *invocation )
{
  GError *error = NULL;

  if( !rcl_x11_keyboard_values_are_valid( layout, model, variant, options ) )
  {
    g_dbus_method_invocation_return_error( invocation,
      G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
      "Invalid X11 keyboard configuration value" );
    return;
  }

  if( convert && layout[0] != '\0' )
  {
    gchar *vc_keymap = NULL;
    if( rcl_x11_to_keymap( layout, variant, &vc_keymap ) )
    {
      if( !rcl_write_x11_vconsole_files( layout, model, variant, options,
                                         vc_keymap, &error ) )
      {
        rcl_daemon_sync_dbus_properties( daemon );
        g_dbus_method_invocation_return_gerror( invocation, error );
        g_clear_error( &error );
        g_free( vc_keymap );
        return;
      }
      g_free( vc_keymap );
    }
    else
    {
      g_debug( "convert: no built-in console mapping for X11 layout '%s'", layout );
      if( !write_x11_keyboard_conf( RCL_X11_KEYBOARD_FILE,
                                    layout, model, variant, options, &error ) )
      {
        rcl_daemon_sync_dbus_properties( daemon );
        g_dbus_method_invocation_return_gerror( invocation, error );
        g_clear_error( &error );
        return;
      }
    }
  }
  else if( !write_x11_keyboard_conf( RCL_X11_KEYBOARD_FILE,
                                     layout, model, variant, options, &error ) )
  {
    rcl_daemon_sync_dbus_properties( daemon );
    g_dbus_method_invocation_return_gerror( invocation, error );
    g_clear_error( &error );
    return;
  }

  rcl_daemon_sync_dbus_properties( daemon );
  g_dbus_method_invocation_return_value( invocation, NULL );
}

#if HAVE_GNOME_BACKEND || HAVE_PLASMA6_BACKEND
/* Trusted in-process keyboard entry points used by the GNOME input-source
 * backend. Public D-Bus requests continue through authorized D-Bus handlers. */
gboolean
rcl_locale_daemon_set_vconsole_keyboard_internal(
    RclLocaleDaemon *daemon,
    const gchar *keymap,
    const gchar *keymap_toggle,
    GError **error)
{
  g_return_val_if_fail(RCL_IS_LOCALE_DAEMON(daemon), FALSE);
  g_return_val_if_fail(keymap != NULL, FALSE);
  g_return_val_if_fail(keymap_toggle != NULL, FALSE);

  if ((keymap[0] != '\0' && !rcl_keyboard_value_is_safe(keymap)) ||
      (keymap_toggle[0] != '\0' &&
       !rcl_keyboard_value_is_safe(keymap_toggle)))
  {
    g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                        _("Invalid console keyboard configuration value"));
    return FALSE;
  }

  if (!rcl_write_vconsole_files(keymap, keymap_toggle, error))
  {
    rcl_daemon_sync_dbus_properties(daemon);
    return FALSE;
  }

  rcl_daemon_sync_dbus_properties(daemon);
  return TRUE;
}

gboolean
rcl_locale_daemon_set_x11_keyboard_internal(
    RclLocaleDaemon *daemon,
    const gchar *layout,
    const gchar *model,
    const gchar *variant,
    const gchar *options,
    GError **error)
{
  g_return_val_if_fail(RCL_IS_LOCALE_DAEMON(daemon), FALSE);
  g_return_val_if_fail(layout != NULL, FALSE);
  g_return_val_if_fail(model != NULL, FALSE);
  g_return_val_if_fail(variant != NULL, FALSE);
  g_return_val_if_fail(options != NULL, FALSE);

  if (!rcl_x11_keyboard_values_are_valid(layout, model, variant, options))
  {
    g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                        _("Invalid X11 keyboard configuration value"));
    return FALSE;
  }

  if (!write_x11_keyboard_conf(RCL_X11_KEYBOARD_FILE, layout, model,
                               variant, options, error))
  {
    rcl_daemon_sync_dbus_properties(daemon);
    return FALSE;
  }

  rcl_daemon_sync_dbus_properties(daemon);
  return TRUE;
}
#endif /* HAVE_GNOME_BACKEND || HAVE_PLASMA6_BACKEND */

/* ---- asynchronous PolicyKit D-Bus authorization ---- */

static void
on_check_done( GObject *source, GAsyncResult *res, gpointer user_data )
{
  AuthCtx  *ctx = user_data;
  GVariant *reply;
  GVariant *details = NULL;
  GError   *error = NULL;
  gboolean  authorized = FALSE;
  gboolean  challenge = FALSE;

  reply = g_dbus_connection_call_finish( G_DBUS_CONNECTION(source),
                                         res, &error );

  if( !reply )
  {
    if( g_cancellable_is_cancelled( ctx->cancellable ) )
      g_dbus_method_invocation_return_dbus_error(
        ctx->invocation,
        "org.freedesktop.DBus.Error.Disconnected",
        "Daemon connection was closed before authorization completed" );
    else
      g_dbus_method_invocation_return_gerror( ctx->invocation, error );

    g_clear_error( &error );
    auth_ctx_free( ctx );
    return;
  }

  g_variant_get( reply, "((bb@a{ss}))", &authorized, &challenge, &details );
  g_variant_unref( details );
  g_variant_unref( reply );

  /*
   * Cancellation can race with a successful PolicyKit reply. Check the
   * daemon-session cancellable before performing any privileged file update.
   */
  if( g_cancellable_is_cancelled( ctx->cancellable ) )
  {
    g_dbus_method_invocation_return_dbus_error(
      ctx->invocation,
      "org.freedesktop.DBus.Error.Disconnected",
      "Daemon connection was closed before authorization completed" );
    auth_ctx_free( ctx );
    return;
  }

  if( !authorized )
  {
    if( challenge )
      g_dbus_method_invocation_return_dbus_error(
        ctx->invocation,
        "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired",
        "Authentication is required to perform the requested action" );
    else
      g_dbus_method_invocation_return_error(
        ctx->invocation, G_DBUS_ERROR, G_DBUS_ERROR_AUTH_FAILED,
        "Not authorised to perform the requested action" );

    auth_ctx_free( ctx );
    return;
  }

  switch( ctx->op )
  {
    case OP_SET_LOCALE:
      do_set_locale( ctx->daemon, ctx->locale, ctx->invocation );
      break;
    case OP_SET_VCONSOLE_KEYBOARD:
      do_set_vconsole_keyboard( ctx->daemon, ctx->keymap, ctx->keymap_toggle,
                                ctx->convert, ctx->invocation );
      break;
    case OP_SET_X11_KEYBOARD:
      do_set_x11_keyboard( ctx->daemon, ctx->x11_layout, ctx->x11_model,
                           ctx->x11_variant, ctx->x11_options,
                           ctx->convert, ctx->invocation );
      break;
  }

  auth_ctx_free( ctx );
}

/* ---- asynchronous PolicyKit authorization starter ---- */

static void
check_polkit_async( AuthCtx *ctx, const gchar *action_id, gboolean interactive )
{
  RclLocaleDaemonPrivate *priv = ctx->daemon->priv;
  GVariantBuilder subject_details;
  GVariantBuilder details;
  GVariant *parameters;
  AuthCancelData *cancel_data;
  const gchar *sender;
  guint flags;

  if( !priv->connection || g_dbus_connection_is_closed(priv->connection) )
  {
    g_dbus_method_invocation_return_dbus_error(
      ctx->invocation,
      "org.freedesktop.DBus.Error.Disconnected",
      "System bus connection is unavailable" );
    auth_ctx_free( ctx );
    return;
  }

  ctx->cancellable = g_object_ref( priv->auth_cancellable );
  if( g_cancellable_is_cancelled( ctx->cancellable ) )
  {
    g_dbus_method_invocation_return_dbus_error(
      ctx->invocation,
      "org.freedesktop.DBus.Error.Disconnected",
      "Authorization was cancelled before it could be started" );
    auth_ctx_free( ctx );
    return;
  }

  sender = g_dbus_method_invocation_get_sender( ctx->invocation );

  g_variant_builder_init( &subject_details, G_VARIANT_TYPE("a{sv}") );
  g_variant_builder_add( &subject_details, "{sv}", "name",
                         g_variant_new_string(sender) );

  g_variant_builder_init( &details, G_VARIANT_TYPE("a{ss}") );

  flags = interactive ? 1u : 0u; /* AllowUserInteraction */

  ctx->auth_connection = g_object_ref( priv->connection );
  ctx->cancellation_id = g_uuid_string_random();

  parameters = g_variant_new(
    "((s@a{sv})s@a{ss}us)",
    "system-bus-name",
    g_variant_builder_end( &subject_details ),
    action_id,
    g_variant_builder_end( &details ),
    flags,
    ctx->cancellation_id );

  /*
   * Queue CheckAuthorization before connecting the cancellation handler.
   * If the cancellable is already cancelled, g_cancellable_connect() invokes
   * the handler immediately; queuing the check first preserves D-Bus message
   * order so CancelCheckAuthorization cannot overtake its own request.
   */
  g_dbus_connection_call(
    ctx->auth_connection,
    "org.freedesktop.PolicyKit1",
    "/org/freedesktop/PolicyKit1/Authority",
    "org.freedesktop.PolicyKit1.Authority",
    "CheckAuthorization",
    parameters,
    G_VARIANT_TYPE( "((bba{ss}))" ),
    G_DBUS_CALL_FLAGS_NONE,
    -1,
    ctx->cancellable,
    on_check_done,
    ctx );

  cancel_data = g_new0( AuthCancelData, 1 );
  cancel_data->connection = g_object_ref( ctx->auth_connection );
  cancel_data->cancellation_id = g_strdup( ctx->cancellation_id );

  ctx->cancel_handler_id =
    g_cancellable_connect( ctx->cancellable,
                           G_CALLBACK(on_auth_cancelled),
                           cancel_data,
                           auth_cancel_data_free );
}

/* Commit the two Slackware locale files as one logical operation.  Each
 * individual rename is atomic; if the second write fails, restore the first
 * file so callers do not observe a knowingly mixed locale configuration. */
static gboolean
rcl_write_locale_files( const gchar *const *known_keys,
                        const gchar *const *new_values, GError **error )
{
  RclFileSnapshot sh = { 0 }, csh = { 0 };
  GError *write_error = NULL, *rollback_error = NULL;
  gboolean ok = FALSE;

  if( !rcl_file_snapshot_take( &sh, RCL_LANG_SH_FILE, &write_error ) ||
      !rcl_file_snapshot_take( &csh, RCL_LANG_CSH_FILE, &write_error ) )
    goto fail;
  if( !rcl_write_lang_sh( RCL_LANG_SH_FILE, known_keys, new_values, &write_error ) )
    goto fail;
  if( !rcl_write_lang_csh( RCL_LANG_CSH_FILE, known_keys, new_values, &write_error ) )
  {
    if( !rcl_file_snapshot_restore( &sh, &rollback_error ) )
      rcl_report_rollback_failure( &write_error, rollback_error );
    goto fail;
  }
  ok = TRUE;
  goto out;

fail:
  rcl_report_write_failure( error, write_error );
out:
  rcl_file_snapshot_clear( &sh );
  rcl_file_snapshot_clear( &csh );
  return ok;
}

/* Commit the console keymap and its toggle sidecar together. */
static gboolean
rcl_write_vconsole_files( const gchar *keymap, const gchar *keymap_toggle,
                          GError **error )
{
  RclFileSnapshot keymap_snapshot = { 0 }, toggle_snapshot = { 0 };
  GError *write_error = NULL, *rollback_error = NULL;
  gboolean ok = FALSE;

  if( !rcl_file_snapshot_take( &keymap_snapshot, RCL_RC_KEYMAP_FILE, &write_error ) ||
      !rcl_file_snapshot_take( &toggle_snapshot, RCL_RC_KEYMAP_TOGGLE_FILE, &write_error ) )
    goto fail;
  if( !rcl_write_rc_keymap( RCL_RC_KEYMAP_FILE, keymap, &write_error ) )
    goto fail;
  if( !rcl_write_rc_keymap_toggle( RCL_RC_KEYMAP_TOGGLE_FILE,
                                   keymap_toggle, &write_error ) )
  {
    if( !rcl_file_snapshot_restore( &keymap_snapshot, &rollback_error ) )
      rcl_report_rollback_failure( &write_error, rollback_error );
    goto fail;
  }
  ok = TRUE;
  goto out;

fail:
  rcl_report_write_failure( error, write_error );
out:
  rcl_file_snapshot_clear( &keymap_snapshot );
  rcl_file_snapshot_clear( &toggle_snapshot );
  return ok;
}

/* The optional X11/console conversion is part of the requested update.
 * Snapshot both files so a failed second write does not leave the source
 * setting changed while the converted setting remains stale. */
static gboolean
rcl_write_x11_vconsole_files( const gchar *layout, const gchar *model,
                              const gchar *variant, const gchar *options,
                              const gchar *keymap, GError **error )
{
  RclFileSnapshot x11 = { 0 };
  GError *write_error = NULL, *rollback_error = NULL;
  gboolean ok = FALSE;

  if( !rcl_file_snapshot_take( &x11, RCL_X11_KEYBOARD_FILE, &write_error ) )
    goto fail;
  if( !write_x11_keyboard_conf( RCL_X11_KEYBOARD_FILE, layout, model,
                                variant, options, &write_error ) )
    goto fail;
  if( !rcl_write_rc_keymap( RCL_RC_KEYMAP_FILE, keymap, &write_error ) )
  {
    if( !rcl_file_snapshot_restore( &x11, &rollback_error ) )
      rcl_report_rollback_failure( &write_error, rollback_error );
    goto fail;
  }
  ok = TRUE;
  goto out;
fail:
  rcl_report_write_failure( error, write_error );
out:
  rcl_file_snapshot_clear( &x11 );
  return ok;
}

static gboolean
rcl_write_vconsole_x11_files( const gchar *keymap, const gchar *keymap_toggle,
                              const gchar *layout, const gchar *model,
                              const gchar *variant, const gchar *options,
                              GError **error )
{
  RclFileSnapshot vc = { 0 }, toggle = { 0 };
  GError *write_error = NULL, *rollback_error = NULL;
  gboolean keymap_written = FALSE, toggle_written = FALSE, ok = FALSE;

  if( !rcl_file_snapshot_take( &vc, RCL_RC_KEYMAP_FILE, &write_error ) ||
      !rcl_file_snapshot_take( &toggle, RCL_RC_KEYMAP_TOGGLE_FILE, &write_error ) )
    goto fail;
  if( !rcl_write_rc_keymap( RCL_RC_KEYMAP_FILE, keymap, &write_error ) )
    goto fail;
  keymap_written = TRUE;
  if( !rcl_write_rc_keymap_toggle( RCL_RC_KEYMAP_TOGGLE_FILE,
                                   keymap_toggle, &write_error ) )
    goto rollback;
  toggle_written = TRUE;
  if( !write_x11_keyboard_conf( RCL_X11_KEYBOARD_FILE, layout, model,
                                variant, options, &write_error ) )
    goto rollback;
  ok = TRUE;
  goto out;
rollback:
  if( toggle_written && !rcl_file_snapshot_restore( &toggle, &rollback_error ) )
    rcl_report_rollback_failure( &write_error, rollback_error );
  rollback_error = NULL;
  if( keymap_written && !rcl_file_snapshot_restore( &vc, &rollback_error ) )
    rcl_report_rollback_failure( &write_error, rollback_error );
fail:
  rcl_report_write_failure( error, write_error );
out:
  rcl_file_snapshot_clear( &vc );
  rcl_file_snapshot_clear( &toggle );
  return ok;
}

/* --------------------------------------------------------------------------
   D-Bus property getter
   -------------------------------------------------------------------------- */
static GVariant *
handle_get_property( GDBusConnection  *connection,
                     const gchar      *sender,
                     const gchar      *object_path,
                     const gchar      *interface_name,
                     const gchar      *property_name,
                     GError          **error,
                     gpointer          user_data )
{
  RclLocaleDaemon        *daemon = RCL_LOCALE_DAEMON( user_data );
  RclLocaleDaemonPrivate *priv   = daemon->priv;

#define RETURN_STR(field) \
  return g_variant_new_string( priv->field ? priv->field : "" )

  if( g_strcmp0(property_name, "Locale") == 0 )
    return g_variant_new_strv( (const gchar * const *) priv->locale, -1 );

  if( g_strcmp0(property_name, "VConsoleKeymap"      ) == 0 ) RETURN_STR(vconsole_keymap);
  if( g_strcmp0(property_name, "VConsoleKeymapToggle") == 0 ) RETURN_STR(vconsole_keymap_toggle);
  if( g_strcmp0(property_name, "X11Layout"           ) == 0 ) RETURN_STR(x11_layout);
  if( g_strcmp0(property_name, "X11Model"            ) == 0 ) RETURN_STR(x11_model);
  if( g_strcmp0(property_name, "X11Variant"          ) == 0 ) RETURN_STR(x11_variant);
  if( g_strcmp0(property_name, "X11Options"          ) == 0 ) RETURN_STR(x11_options);

#undef RETURN_STR

  g_set_error( error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
               _("Unknown property '%s'"), property_name );
  return NULL;
}

/* Properties are read-only via D-Bus; all changes go through Set* methods. */
static gboolean
handle_set_property( GDBusConnection  *connection,
                     const gchar      *sender,
                     const gchar      *object_path,
                     const gchar      *interface_name,
                     const gchar      *property_name,
                     GVariant         *value,
                     GError          **error,
                     gpointer          user_data )
{
  g_set_error( error, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY,
               _("Property '%s' is read-only; use the Set* methods"), property_name );
  return FALSE;
}

/* --------------------------------------------------------------------------
   D-Bus method handler
   -------------------------------------------------------------------------- */
static void
handle_method_call( GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data )
{
  RclLocaleDaemon *daemon = RCL_LOCALE_DAEMON( user_data );
  AuthCtx         *ctx;

  if( g_strcmp0(method_name, "SetLocale") == 0 )
  {
    gchar    **locale;
    gboolean   interactive;
    GVariant  *locale_v;

    g_variant_get( parameters, "(@asb)", &locale_v, &interactive );
    locale = g_variant_dup_strv( locale_v, NULL );
    g_variant_unref( locale_v );

    ctx = g_new0( AuthCtx, 1 );
    ctx->daemon     = g_object_ref( daemon );
    ctx->invocation = g_object_ref( invocation );
    ctx->op         = OP_SET_LOCALE;
    ctx->locale     = locale;

    check_polkit_async( ctx, RCL_LOCALE_POLKIT_SET_LOCALE, interactive );
    return;
  }

  if( g_strcmp0(method_name, "SetVConsoleKeyboard") == 0 )
  {
    const gchar *keymap, *keymap_toggle;
    gboolean     convert, interactive;

    g_variant_get( parameters, "(&s&sbb)", &keymap, &keymap_toggle,
                   &convert, &interactive );

    ctx = g_new0( AuthCtx, 1 );
    ctx->daemon         = g_object_ref( daemon );
    ctx->invocation = g_object_ref( invocation );
    ctx->op             = OP_SET_VCONSOLE_KEYBOARD;
    ctx->keymap         = g_strdup( keymap );
    ctx->keymap_toggle  = g_strdup( keymap_toggle );
    ctx->convert        = convert;

    check_polkit_async( ctx, RCL_LOCALE_POLKIT_SET_KEYBOARD, interactive );
    return;
  }

  if( g_strcmp0(method_name, "SetX11Keyboard") == 0 )
  {
    const gchar *layout, *model, *variant, *options;
    gboolean     convert, interactive;

    g_variant_get( parameters, "(&s&s&s&sbb)", &layout, &model, &variant,
                   &options, &convert, &interactive );

    /*
     * Reject malformed values before contacting PolicyKit.  The operation
     * handler repeats this validation after authorization as defense in depth.
     */
    if( !rcl_x11_keyboard_values_are_valid( layout, model, variant, options ) )
    {
      g_dbus_method_invocation_return_error( invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Invalid X11 keyboard configuration value" );
      return;
    }

    ctx = g_new0( AuthCtx, 1 );
    ctx->daemon      = g_object_ref( daemon );
    ctx->invocation = g_object_ref( invocation );
    ctx->op          = OP_SET_X11_KEYBOARD;
    ctx->x11_layout  = g_strdup( layout );
    ctx->x11_model   = g_strdup( model );
    ctx->x11_variant = g_strdup( variant );
    ctx->x11_options = g_strdup( options );
    ctx->convert     = convert;

    check_polkit_async( ctx, RCL_LOCALE_POLKIT_SET_KEYBOARD, interactive );
    return;
  }

  g_dbus_method_invocation_return_error( invocation,
    G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
    "Unknown method '%s'", method_name );
}

/* --------------------------------------------------------------------------
   D-Bus vtable
   -------------------------------------------------------------------------- */
static const GDBusInterfaceVTable interface_vtable = {
  handle_method_call,
  handle_get_property,
  handle_set_property,
};

/* --------------------------------------------------------------------------
   RclDaemon virtual method implementations
   -------------------------------------------------------------------------- */

RclDaemon *
rcl_daemon_new( void )
{
  return RCL_DAEMON( g_object_new(RCL_TYPE_LOCALE_DAEMON, NULL) );
}

gboolean
rcl_daemon_startup( RclDaemon *daemon, GDBusConnection *connection )
{
  RclLocaleDaemon        *self = RCL_LOCALE_DAEMON( daemon );
  RclLocaleDaemonPrivate *priv = self->priv;
  GDBusInterfaceInfo     *iface_info;
  GError *error = NULL;

  /*
   * Startup may be called again after a bus/name loss. Always release any
   * previous registration, introspection data, and authorization session
   * before attaching the new connection.
   */
  rcl_daemon_shutdown( daemon );

  priv->connection       = g_object_ref( connection );
  priv->auth_cancellable = g_cancellable_new();
  priv->introspection = g_dbus_node_info_new_for_xml(
                          locale1_introspection_xml, &error );
  if( !priv->introspection )
  {
    g_warning( "Failed to parse introspection XML: %s", error->message );
    g_error_free( error );
    rcl_daemon_shutdown( daemon );
    return FALSE;
  }

  iface_info = g_dbus_node_info_lookup_interface(
                 priv->introspection, "org.freedesktop.locale1" );

  priv->registration_id = g_dbus_connection_register_object(
    connection,
    "/org/freedesktop/locale1",
    iface_info,
    &interface_vtable,
    self,
    NULL,
    &error );

  if( priv->registration_id == 0 )
  {
    g_warning( "Could not register D-Bus object: %s", error->message );
    g_error_free( error );
    rcl_daemon_shutdown( daemon );
    return FALSE;
  }

  rcl_daemon_sync_dbus_properties( self );

  g_debug( "localed D-Bus object registered at /org/freedesktop/locale1" );
  return TRUE;
}

void
rcl_daemon_shutdown( RclDaemon *daemon )
{
  RclLocaleDaemon        *self = RCL_LOCALE_DAEMON( daemon );
  RclLocaleDaemonPrivate *priv = self->priv;

  if( priv->connection && priv->registration_id > 0 )
  {
    g_dbus_connection_unregister_object( priv->connection,
                                         priv->registration_id );
    priv->registration_id = 0;
  }

  g_clear_object( &priv->connection );

  if( priv->introspection )
  {
    g_dbus_node_info_unref( priv->introspection );
    priv->introspection = NULL;
  }

  /*
   * Cancel outstanding authorization checks before releasing the session.
   * Their callbacks may still run, but cancellation prevents them from
   * executing the protected Set* operation after this daemon session ends.
   */
  if( priv->auth_cancellable )
    g_cancellable_cancel( priv->auth_cancellable );
  g_clear_object( &priv->auth_cancellable );

}

void
rcl_daemon_set_debug( RclDaemon *daemon, gboolean debug )
{
  RCL_LOCALE_DAEMON(daemon)->priv->debug = debug;
}

/* --------------------------------------------------------------------------
   GObject lifecycle
   -------------------------------------------------------------------------- */

static void
rcl_locale_daemon_finalize( GObject *object )
{
  RclLocaleDaemonPrivate *priv = RCL_LOCALE_DAEMON(object)->priv;

  /* Guard against finalization without an explicit rcl_daemon_shutdown() call
   * (e.g. on a startup error path).  Unregistering an already-unregistered
   * object is a no-op, and g_clear_object on a NULL pointer is safe. */
  if( priv->connection && priv->registration_id > 0 )
  {
    g_dbus_connection_unregister_object( priv->connection,
                                         priv->registration_id );
    priv->registration_id = 0;
  }
  g_clear_object( &priv->connection );

  g_strfreev( priv->locale );
  g_free( priv->vconsole_keymap );
  g_free( priv->vconsole_keymap_toggle );
  g_free( priv->x11_layout );
  g_free( priv->x11_model );
  g_free( priv->x11_variant );
  g_free( priv->x11_options );

  G_OBJECT_CLASS(rcl_locale_daemon_parent_class)->finalize(object);
}

static void
rcl_locale_daemon_class_init( RclLocaleDaemonClass *klass )
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = rcl_locale_daemon_finalize;
}

static void
rcl_locale_daemon_init( RclLocaleDaemon *self )
{
  self->priv = rcl_locale_daemon_get_instance_private( self );
  self->priv->locale = g_new0( gchar *, 1 ); /* empty GStrv until first sync */
}

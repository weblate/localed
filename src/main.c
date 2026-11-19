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

#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib-unix.h>
#include <glib-object.h>
#include <glib/gi18n-lib.h>
#include <locale.h>

#include "rcl-locale.h"
#include "backends/backend-interface.h"
#if HAVE_GNOME_BACKEND
#include "backends/gnome/gnome-backend.h"
#elif HAVE_PLASMA6_BACKEND
#include "backends/plasma6/plasma6-backend.h"
#endif
#include "rcl-accounts-service.h"

#define LOCALE_SERVICE_NAME  "org.freedesktop.locale1"

/*
 * Reconnect delay after losing the D-Bus name (milliseconds).
 * Long enough that a dbus-daemon restart has time to settle, short enough
 * that the service is not visibly absent.
 */
#define RECONNECT_DELAY_MS     2500

/* ───────────────────────────────────────────── */

typedef struct RclState
{
  RclDaemon  *daemon;
  GMainLoop  *loop;

  gboolean    debug;
  gboolean    verbose;
  gboolean    replace;

  /*
   * Ownership state.
   *
   * watch_id   – handle returned by g_bus_own_name_on_connection().
   * bus_connection – private connection used for locale1 name ownership.
   * has_name     – TRUE once name_acquired has fired.
   * reconnect_id – pending reconnect timer; 0 if none.
   * reconnecting – suppresses callbacks from the connection being retired.
   * exiting      – set TRUE by signal handlers so name_lost does not reschedule.
   * sync_id      – pending debounce timer for file-change resync; 0 if none.
   */
  guint       watch_id;
  GDBusConnection *bus_connection;
  gulong      bus_closed_id;
  gboolean    has_name;
  guint       reconnect_id;
  gboolean    reconnecting;
  gboolean    exiting;
  guint       sync_id;
  RclAccountsService *accounts_service;
#if HAVE_GNOME_BACKEND || HAVE_PLASMA6_BACKEND
  RclBackend *backend;
#endif
} RclState;

/* ───────────────────────────────────────────── */

static void
rcl_state_free(RclState *state)
{
  if (state->sync_id)
  {
    g_source_remove(state->sync_id);
    state->sync_id = 0;
  }

  if (state->reconnect_id)
  {
    g_source_remove(state->reconnect_id);
    state->reconnect_id = 0;
  }

#if HAVE_GNOME_BACKEND || HAVE_PLASMA6_BACKEND
  g_clear_object(&state->backend);
#endif
  g_clear_object(&state->accounts_service);

  if (state->watch_id)
  {
    state->reconnecting = TRUE;
    g_bus_unown_name(state->watch_id);
    state->watch_id = 0;
  }
  if (state->bus_connection && state->bus_closed_id)
  {
    g_signal_handler_disconnect(state->bus_connection,
                                state->bus_closed_id);
    state->bus_closed_id = 0;
  }
  g_clear_object(&state->bus_connection);

  rcl_daemon_shutdown(state->daemon);
  g_clear_object(&state->daemon);
  g_clear_pointer(&state->loop, g_main_loop_unref);
  g_free(state);
}

static RclState *
rcl_state_new(void)
{
  RclState *state = g_new0(RclState, 1);

  state->daemon = rcl_daemon_new();
  state->loop   = g_main_loop_new(NULL, FALSE);

  return state;
}

/* ───────────────────────────────────────────── */

#define LOG(state, fmt, ...) \
  do { \
    if ((state)->debug || (state)->verbose) \
      g_message("[localed] " fmt, ##__VA_ARGS__); \
  } while (0)

/* ───────────────────────────────────────────── */

static void rcl_acquire_name(RclState *state);   /* forward */
static gboolean rcl_main_reconnect_cb(gpointer user_data); /* forward */

static void
rcl_schedule_reconnect(RclState *state, const gchar *reason)
{
  if (state->exiting || state->reconnecting)
    return;

  if (state->reconnect_id)
    return;

  g_warning("%s; scheduling reconnect in %d ms",
            reason, RECONNECT_DELAY_MS);
  {
    GSource *source = g_timeout_source_new(RECONNECT_DELAY_MS);

    /*
     * Bus teardown can leave GDBus I/O sources continuously ready. Give
     * recovery precedence over ordinary bus traffic so a busy connection
     * cannot starve the reconnect deadline.
     */
    g_source_set_priority(source, G_PRIORITY_HIGH);
    g_source_set_callback(source, rcl_main_reconnect_cb, state, NULL);
    state->reconnect_id =
      g_source_attach(source, g_main_loop_get_context(state->loop));
    g_source_unref(source);
  }

  LOG(state, "D-Bus reconnect timer registered");
}

static void
rcl_main_bus_closed(GDBusConnection *connection,
                    gboolean remote_peer_vanished,
                    GError *error,
                    gpointer user_data)
{
  RclState *state = user_data;

  if (connection != state->bus_connection || state->exiting ||
      state->reconnecting)
    return;

  LOG(state, "system D-Bus connection closed: %s",
      error ? error->message : "connection closed");
  rcl_schedule_reconnect(state, "system D-Bus connection closed");
}

/*
 * Retry name acquisition after a transient D-Bus disconnect. Clear the old
 * daemon registration first so startup does not retain the previous
 * connection or introspection data.
 */
static gboolean
rcl_main_reconnect_cb(gpointer user_data)
{
  RclState *state = user_data;

  state->reconnect_id = 0;

  LOG(state, "D-Bus reconnect timer fired");
  LOG(state, "attempting to re-acquire D-Bus name");

  state->reconnecting = TRUE;

  if (state->watch_id)
  {
    g_bus_unown_name(state->watch_id);
    state->watch_id = 0;
  }

  if (state->bus_connection && state->bus_closed_id)
  {
    g_signal_handler_disconnect(state->bus_connection,
                                state->bus_closed_id);
    state->bus_closed_id = 0;
  }

  /* Unregister locale1 before releasing the old connection. */
  rcl_daemon_shutdown(state->daemon);
  g_clear_object(&state->bus_connection);
  state->has_name = FALSE;
  state->reconnecting = FALSE;

  rcl_acquire_name(state);

  return G_SOURCE_REMOVE;
}

static void
rcl_main_name_acquired(GDBusConnection *connection,
                       const gchar     *name,
                       gpointer         user_data)
{
  RclState *state = user_data;

  state->has_name = TRUE;

#if HAVE_GNOME_BACKEND || HAVE_PLASMA6_BACKEND
  /* Start the selected desktop backend only after locale1 is registered. */
  if (state->backend &&
      !rcl_backend_start(state->backend, NULL))
    g_warning("could not start desktop backend session discovery");
#endif

  /*
   * --replace controls the initial name request only.  Once this instance
   * owns locale1, it must recover like any other daemon after a bus restart
   * or a later name loss.
   */
  state->replace = FALSE;

  LOG(state, "name acquired: %s", name);
}

/* ───────────────────────────────────────────── */

/*
 * rcl_main_name_lost:
 *
 * If the name was never acquired, another instance is likely running and
 * startup fails. If it was lost after successful startup, retry after a
 * short delay unless this instance is exiting.
 */
static void
rcl_main_name_lost(GDBusConnection *connection,
                   const gchar     *name,
                   gpointer         user_data)
{
  RclState *state = user_data;

  if (state->exiting || state->reconnecting)
    return;

  LOG(state, "D-Bus name lost: %s", name);

  if (!state->has_name)
  {
    /*
     * A closed private connection can report name-lost before name-acquired
     * during initial startup. Retry that transport failure; only treat a
     * live connection losing an unowned name as a competing instance.
     */
    if (connection && g_dbus_connection_is_closed(connection))
    {
      rcl_schedule_reconnect(state, "system D-Bus connection closed");
      return;
    }

    g_warning("could not acquire %s – is another instance running?", name);
    g_main_loop_quit(state->loop);
    return;
  }

  rcl_schedule_reconnect(state, "D-Bus name lost");
}

/* ───────────────────────────────────────────── */

static gboolean
rcl_main_sigint_cb(gpointer user_data)
{
  RclState *state = user_data;
  LOG(state, "SIGINT received");
  g_warning("main loop quit requested by SIGINT");
  state->exiting = TRUE;
  g_main_loop_quit(state->loop);
  return G_SOURCE_REMOVE;
}

static gboolean
rcl_main_sigterm_cb(gpointer user_data)
{
  RclState *state = user_data;
  LOG(state, "SIGTERM received");
  g_warning("main loop quit requested by SIGTERM");
  state->exiting = TRUE;
  g_main_loop_quit(state->loop);
  return G_SOURCE_REMOVE;
}

/* ───────────────────────────────────────────── */

static void
rcl_main_log_handler_cb(const gchar    *log_domain,
                        GLogLevelFlags  log_level,
                        const gchar    *message,
                        gpointer        user_data)
{
  if (isatty(STDERR_FILENO))
  {
    /*
     * Interactive use: colourised output to stderr.
     * Errors/warnings in red, everything else in blue,
     * timestamp prefix in green.
     */
    gchar timebuf[32];
    time_t t;
    struct tm tm_buf;
    time(&t);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", localtime_r(&t, &tm_buf));
    const char *color = (log_level & (G_LOG_LEVEL_ERROR    |
                                      G_LOG_LEVEL_CRITICAL |
                                      G_LOG_LEVEL_WARNING))
                          ? "\033[31m" : "\033[34m";
    g_printerr("\033[32mTI:%s\t%s%s\033[0m\n", timebuf, color, message);
  }
  else
  {
    /*
     * Daemon context (launched by dbus-daemon, rc script, etc.):
     * route through syslog(3) using the LOG_DAEMON facility so
     * messages land in /var/log/syslog (or wherever daemon.* is
     * directed in /etc/syslog.conf).
     *
     * Note: LOG_DEBUG messages are filtered by most syslog.conf
     * defaults.  To see them add "daemon.debug /var/log/debug" to
     * /etc/syslog.conf and send syslogd a SIGHUP.
     */
    int priority;
    switch (log_level & G_LOG_LEVEL_MASK)
    {
      case G_LOG_LEVEL_ERROR:
      case G_LOG_LEVEL_CRITICAL: priority = LOG_CRIT;    break;
      case G_LOG_LEVEL_WARNING:  priority = LOG_WARNING; break;
      case G_LOG_LEVEL_MESSAGE:  priority = LOG_NOTICE;  break;
      case G_LOG_LEVEL_INFO:     priority = LOG_INFO;    break;
      case G_LOG_LEVEL_DEBUG:    priority = LOG_DEBUG;   break;
      default:                   priority = LOG_INFO;    break;
    }
    syslog(priority, "%s%s%s",
           log_domain ? log_domain : "",
           log_domain ? ": "       : "",
           message);
  }
}

/* ───────────────────────────────────────────── */

#define SYNC_DEBOUNCE_MS 100

static gboolean
rcl_main_sync_timeout_cb(gpointer user_data)
{
  RclState *state = user_data;

  state->sync_id = 0;
  LOG(state, "file change detected → syncing DBus properties");
  rcl_daemon_sync_dbus_properties(RCL_LOCALE_DAEMON(state->daemon));
  return G_SOURCE_REMOVE;
}

/* Coalesce bursts of file events (and our own writes) into one resync. */
static void
rcl_main_inotify_cb(gpointer user_data)
{
  RclState *state = user_data;

  if (!state->sync_id)
    state->sync_id = g_timeout_add(SYNC_DEBOUNCE_MS,
                                   rcl_main_sync_timeout_cb, state);
}

/* ───────────────────────────────────────────── */

/*
 * rcl_acquire_name:
 *
 * Establish a fresh private system-bus connection for every ownership
 * attempt. g_bus_own_name() uses GLib's shared bus connection, which can
 * remain closed after dbus-daemon restarts and cannot reliably be reused.
 *
 * G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT is always set so that a
 * subsequent invocation with --replace can take over cleanly.
 */
static void
rcl_acquire_name(RclState *state)
{
  GBusNameOwnerFlags flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  GError *error = NULL;
  gchar *address;

  if (state->exiting)
    return;

  if (state->replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!address)
    goto failed;

  state->bus_connection = g_dbus_connection_new_for_address_sync(
    address,
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
    G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
    NULL, NULL, &error);
  g_free(address);

  if (!state->bus_connection)
    goto failed;

  g_dbus_connection_set_exit_on_close(state->bus_connection, FALSE);
  state->bus_closed_id =
    g_signal_connect(state->bus_connection, "closed",
                     G_CALLBACK(rcl_main_bus_closed), state);

  LOG(state, "system bus connected");

  if (!rcl_daemon_startup(state->daemon, state->bus_connection))
  {
    g_warning("Could not startup daemon on the new system-bus connection");
    if (state->bus_closed_id)
    {
      g_signal_handler_disconnect(state->bus_connection,
                                  state->bus_closed_id);
      state->bus_closed_id = 0;
    }
    g_clear_object(&state->bus_connection);
    rcl_schedule_reconnect(state, "locale1 D-Bus object registration failed");
    return;
  }

  LOG(state, "daemon startup complete");

  state->watch_id =
    g_bus_own_name_on_connection(state->bus_connection,
                                 LOCALE_SERVICE_NAME,
                                 flags,
                                 rcl_main_name_acquired,
                                 rcl_main_name_lost,
                                 state,
                                 NULL);
  LOG(state, "watching for D-Bus name %s", LOCALE_SERVICE_NAME);
  return;

failed:
  g_warning("could not connect to system D-Bus: %s",
            error ? error->message : "unknown error");
  g_clear_error(&error);
  g_clear_object(&state->bus_connection);

  rcl_schedule_reconnect(state, "system D-Bus connection attempt failed");
}

/* ───────────────────────────────────────────── */

gint main(gint argc, gchar **argv)
{
  GError         *error = NULL;
  GOptionContext *context;

  gboolean debug   = FALSE;
  gboolean verbose = FALSE;
  gboolean replace = FALSE;

  RclState *state;

  GFile        *locale_file;
  GFile        *rc_keymap_file;
  GFile        *toggle_file;
  GFile        *x11_keyboard_file;
  GFileMonitor *locale_mon;
  GFileMonitor *rc_keymap_mon;
  GFileMonitor *toggle_mon;
  GFileMonitor *x11_keyboard_mon;

  const GOptionEntry options[] =
  {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace, N_("Replace existing daemon"),        NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, N_("Verbose output"),                 NULL },
    { "debug",   'd', 0, G_OPTION_ARG_NONE, &debug,   N_("Debug output (implies verbose)"), NULL },
    { NULL }
  };

  setlocale(LC_ALL, "");
  bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  context = g_option_context_new("");
  g_option_context_add_main_entries(context, options, GETTEXT_PACKAGE);

  if (!g_option_context_parse(context, &argc, &argv, &error))
  {
    g_warning("arg parse failed: %s", error->message);
    g_error_free(error);
    g_option_context_free(context);
    return 1;
  }

  g_option_context_free(context);

  if (debug)
    verbose = TRUE;

  state          = rcl_state_new();
  state->debug   = debug;
  state->verbose = verbose;
  state->replace = replace;

  rcl_daemon_set_debug(state->daemon, debug);

  /*
   * Open the syslog connection immediately so any subsequent g_warning() /
   * g_debug() calls that reach rcl_main_log_handler_cb in daemon context
   * have a valid syslog handle.  LOG_PID stamps every line with our PID;
   * LOG_NDELAY opens the socket now rather than on the first message.
   * LOG_DAEMON is the conventional facility for system daemons.
   */
  openlog("localed", LOG_PID | LOG_NDELAY, LOG_DAEMON);

  /*
   * Install the log handler unconditionally for all domains and levels,
   * including fatal flags.  The handler itself decides whether to write
   * to syslog or to the terminal based on isatty(STDERR_FILENO).
   * G_LOG_LEVEL_DEBUG messages are passed to syslog, as in hostnamed.
   */
  g_log_set_handler(NULL,
                    G_LOG_LEVEL_MASK |
                    G_LOG_FLAG_FATAL |
                    G_LOG_FLAG_RECURSION,
                    rcl_main_log_handler_cb,
                    NULL);

  /* signals */
  g_unix_signal_add(SIGINT,  rcl_main_sigint_cb,  state);
  g_unix_signal_add(SIGTERM, rcl_main_sigterm_cb, state);

  /*
   * File monitoring.
   *
   * RCL_LANG_SH_FILE / RCL_RC_KEYMAP_FILE / RCL_X11_KEYBOARD_FILE come from
   * config.h (meson options locale_conf / rc_keymap / x11_keyboard_conf).
   * G_FILE_MONITOR_WATCH_MOVES catches both direct writes and atomic
   * rename-based writes (which is how localed itself, and the bundled
   * localectl client, persist changes).
   */
  locale_file = g_file_new_for_path(RCL_LANG_SH_FILE);
  locale_mon  = g_file_monitor_file(locale_file,
                                    G_FILE_MONITOR_WATCH_MOVES,
                                    NULL, &error);
  if (locale_mon)
    g_signal_connect_swapped(locale_mon, "changed",
                             G_CALLBACK(rcl_main_inotify_cb), state);
  else
  {
    g_warning("could not watch %s: %s", RCL_LANG_SH_FILE, error->message);
    g_clear_error(&error);
  }

  rc_keymap_file = g_file_new_for_path(RCL_RC_KEYMAP_FILE);
  rc_keymap_mon  = g_file_monitor_file(rc_keymap_file,
                                       G_FILE_MONITOR_WATCH_MOVES,
                                       NULL, &error);
  if (rc_keymap_mon)
    g_signal_connect_swapped(rc_keymap_mon, "changed",
                             G_CALLBACK(rcl_main_inotify_cb), state);
  else
  {
    g_warning("could not watch %s: %s", RCL_RC_KEYMAP_FILE, error->message);
    g_clear_error(&error);
  }

  toggle_file = g_file_new_for_path(RCL_RC_KEYMAP_TOGGLE_FILE);
  toggle_mon  = g_file_monitor_file(toggle_file,
                                    G_FILE_MONITOR_WATCH_MOVES,
                                    NULL, &error);
  if (toggle_mon)
    g_signal_connect_swapped(toggle_mon, "changed",
                             G_CALLBACK(rcl_main_inotify_cb), state);
  else
  {
    g_warning("could not watch %s: %s", RCL_RC_KEYMAP_TOGGLE_FILE, error->message);
    g_clear_error(&error);
  }

  x11_keyboard_file = g_file_new_for_path(RCL_X11_KEYBOARD_FILE);
  x11_keyboard_mon  = g_file_monitor_file(x11_keyboard_file,
                                          G_FILE_MONITOR_WATCH_MOVES,
                                          NULL, &error);
  if (x11_keyboard_mon)
    g_signal_connect_swapped(x11_keyboard_mon, "changed",
                             G_CALLBACK(rcl_main_inotify_cb), state);
  else
  {
    g_warning("could not watch %s: %s", RCL_X11_KEYBOARD_FILE, error->message);
    g_clear_error(&error);
  }

  /* AccountsService integration is managed independently from the daemon. */
  state->accounts_service = rcl_accounts_service_new(
      RCL_LOCALE_DAEMON(state->daemon));
  if (!rcl_accounts_service_start(state->accounts_service, &error))
  {
    g_warning("could not start AccountsService integration: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
  }

#if HAVE_GNOME_BACKEND
  state->backend =
      rcl_gnome_backend_new(RCL_LOCALE_DAEMON(state->daemon));
#elif HAVE_PLASMA6_BACKEND
  state->backend =
      rcl_plasma6_backend_new(RCL_LOCALE_DAEMON(state->daemon));
#endif

  /* Acquire the D-Bus name for the first time */
  rcl_acquire_name(state);

  g_debug("localed started (%s)", PACKAGE_VERSION);

  g_debug("entering localed main loop");
  g_main_loop_run(state->loop);

  /* ── Cleanup ───────────────────────────────────────────────────────────── */

  g_clear_object(&locale_mon);
  g_clear_object(&rc_keymap_mon);
  g_clear_object(&toggle_mon);
  g_clear_object(&x11_keyboard_mon);
  g_clear_object(&locale_file);
  g_clear_object(&rc_keymap_file);
  g_clear_object(&toggle_file);
  g_clear_object(&x11_keyboard_file);

  /* rcl_state_free cancels any pending resync timer and unowns the name */
  rcl_state_free(state);

  closelog();

  return 0;
}

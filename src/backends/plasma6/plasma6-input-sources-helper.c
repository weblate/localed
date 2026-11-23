/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Per-user Plasma 6 keyboard-layout monitor.
 *
 * The KDE D-Bus LayoutNames structure contains shortName, displayName and
 * longName; it does not contain the XKB variant. Layouts and the active index
 * come from org.kde.KeyboardLayouts, while variants/options are read from the
 * user's kxkbrc configuration.
 *
 * A snapshot is emitted on stdout using the same snapshot protocol as the other desktop backends:
 *   (as layouts, as variants, as options, s active_layout, s active_variant)
 */

#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>

#include "plasma6-input-sources-helper.h"

#define KDE_KEYBOARD_NAME       "org.kde.keyboard"
#define KDE_KEYBOARD_PATH       "/Layouts"
#define KDE_KEYBOARD_INTERFACE  "org.kde.KeyboardLayouts"
#define KDE_CONFIG_FILE         ".config/kxkbrc"

typedef struct
{
  GDBusConnection *connection;
  GMainLoop *loop;
  guint name_watch_id;
  guint list_changed_id;
  guint layout_changed_id;
  gulong closed_id;
  gboolean stopping;
} Plasma6Helper;

static gchar **
split_kconfig_list(const gchar *value, guint expected_length)
{
  gchar **parts;
  guint length = 0;
  guint i;

  if (!value || !*value)
    parts = g_new0(gchar *, 1);
  else
    parts = g_strsplit(value, ",", -1);

  while (parts[length])
    length++;

  if (length > expected_length)
  {
    for (i = expected_length; i < length; i++)
      g_free(parts[i]);
    parts[expected_length] = NULL;
    length = expected_length;
  }

  parts = g_realloc_n(parts, length + 1, sizeof(gchar *));
  for (i = 0; i < length; i++)
  {
    g_strstrip(parts[i]);
    if (!parts[i])
      parts[i] = g_strdup("");
  }

  while (length < expected_length)
  {
    parts = g_realloc_n(parts, length + 2, sizeof(gchar *));
    parts[length++] = g_strdup("");
    parts[length] = NULL;
  }

  return parts;
}

static gchar **
read_kconfig_list(GKeyFile *key_file, const gchar *key, guint expected_length)
{
  gchar *value = g_key_file_get_string(key_file, "Layout", key, NULL);
  gchar **result = split_kconfig_list(value, expected_length);

  g_free(value);
  return result;
}

static gchar **
read_kconfig_options(GKeyFile *key_file)
{
  gchar *value = g_key_file_get_string(key_file, "Layout", "Options", NULL);
  gchar **options;

  if (!value || !*value)
    options = g_new0(gchar *, 1);
  else
  {
    guint i;
    options = g_strsplit(value, ",", -1);
    for (i = 0; options[i]; i++)
      g_strstrip(options[i]);
  }

  g_free(value);
  return options;
}

static void
emit_snapshot(Plasma6Helper *helper)
{
  GVariant *reply = NULL;
  GVariant *layout_list = NULL;
  GVariantIter iter;
  const gchar *short_name;
  const gchar *display_name;
  const gchar *long_name;
  GPtrArray *layouts = NULL;
  GPtrArray *variants_array = NULL;
  gchar **variants = NULL;
  gchar **options = NULL;
  gchar **layout_strv = NULL;
  gchar **variant_strv = NULL;
  gchar *config_path = NULL;
  GKeyFile *key_file = NULL;
  GError *error = NULL;
  guint active_index = 0;
  const gchar *active_layout = "";
  const gchar *active_variant = "";
  GVariant *active_reply = NULL;
  GVariant *snapshot = NULL;
  gchar *printed = NULL;
  guint count = 0;
  guint i;

  if (helper->stopping || !helper->connection)
    return;

  reply = g_dbus_connection_call_sync(
      helper->connection,
      KDE_KEYBOARD_NAME,
      KDE_KEYBOARD_PATH,
      KDE_KEYBOARD_INTERFACE,
      "getLayoutsList",
      NULL,
      G_VARIANT_TYPE("(a(sss))"),
      G_DBUS_CALL_FLAGS_NONE,
      5000,
      NULL,
      &error);
  if (!reply)
  {
    g_printerr(_("could not read Plasma keyboard layouts: %s\n"),
               error ? error->message : "unknown D-Bus error");
    g_clear_error(&error);
    return;
  }

  g_variant_get(reply, "(@a(sss))", &layout_list);
  layouts = g_ptr_array_new_with_free_func(g_free);
  variants_array = g_ptr_array_new_with_free_func(g_free);

  g_variant_iter_init(&iter, layout_list);
  while (g_variant_iter_next(&iter, "(&s&s&s)",
                             &short_name, &display_name, &long_name))
  {
    /* KWin's LayoutNames is shortName, displayName, longName. */
    g_ptr_array_add(layouts, g_strdup(short_name));
    count++;
    (void) display_name;
    (void) long_name;
  }

  if (count == 0)
  {
    g_printerr(_ ("Plasma reports no configured keyboard layouts\n"));
    goto out;
  }

  config_path = g_build_filename(g_get_home_dir(), KDE_CONFIG_FILE, NULL);
  key_file = g_key_file_new();
  if (!g_key_file_load_from_file(key_file, config_path,
                                 G_KEY_FILE_NONE, &error))
  {
    g_printerr(_("could not read %s: %s\n"), config_path,
               error ? error->message : "unknown configuration error");
    g_clear_error(&error);
  }

  variants = read_kconfig_list(key_file, "VariantList", count);
  options = read_kconfig_options(key_file);

  for (i = 0; i < count; i++)
    g_ptr_array_add(variants_array, g_strdup(variants[i] ? variants[i] : ""));

  active_reply = g_dbus_connection_call_sync(
      helper->connection,
      KDE_KEYBOARD_NAME,
      KDE_KEYBOARD_PATH,
      KDE_KEYBOARD_INTERFACE,
      "getLayout",
      NULL,
      G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE,
      5000,
      NULL,
      &error);
  if (!active_reply)
  {
    g_printerr(_("could not read active Plasma keyboard layout: %s\n"),
               error ? error->message : "unknown D-Bus error");
    g_clear_error(&error);
    goto out;
  }

  g_variant_get(active_reply, "(u)", &active_index);
  if (active_index >= count)
  {
    g_printerr(_("Plasma active layout index %u is outside layout list of %u\n"),
               active_index, count);
    goto out;
  }

  active_layout = g_ptr_array_index(layouts, active_index);
  active_variant = g_ptr_array_index(variants_array, active_index);

  g_ptr_array_add(layouts, NULL);
  g_ptr_array_add(variants_array, NULL);
  layout_strv = (gchar **) g_ptr_array_free(layouts, FALSE);
  layouts = NULL;
  variant_strv = (gchar **) g_ptr_array_free(variants_array, FALSE);
  variants_array = NULL;

  snapshot = g_variant_ref_sink(g_variant_new("(^as^as^as&s&s)",
                                               layout_strv,
                                               variant_strv,
                                               options,
                                               active_layout,
                                               active_variant));
  printed = g_variant_print(snapshot, TRUE);
  g_print("%s\n", printed);
  fflush(stdout);

out:
  g_free(printed);
  g_clear_pointer(&snapshot, g_variant_unref);
  g_clear_pointer(&active_reply, g_variant_unref);
  g_clear_pointer(&layout_list, g_variant_unref);
  g_clear_pointer(&reply, g_variant_unref);
  g_clear_pointer(&key_file, g_key_file_unref);
  g_free(config_path);
  g_strfreev(layout_strv);
  g_strfreev(variant_strv);
  g_strfreev(variants);
  g_strfreev(options);
  if (layouts)
    g_ptr_array_unref(layouts);
  if (variants_array)
    g_ptr_array_unref(variants_array);
}

static void
keyboard_signal_cb(GDBusConnection *connection,
                   const gchar *sender_name,
                   const gchar *object_path,
                   const gchar *interface_name,
                   const gchar *signal_name,
                   GVariant *parameters,
                   gpointer user_data)
{
  Plasma6Helper *helper = user_data;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) parameters;

  if (g_strcmp0(signal_name, "layoutChanged") == 0 ||
      g_strcmp0(signal_name, "layoutListChanged") == 0)
    emit_snapshot(helper);
}

static void
unsubscribe_keyboard_signals(Plasma6Helper *helper)
{
  if (!helper->connection)
    return;

  if (helper->list_changed_id)
  {
    g_dbus_connection_signal_unsubscribe(helper->connection,
                                         helper->list_changed_id);
    helper->list_changed_id = 0;
  }

  if (helper->layout_changed_id)
  {
    g_dbus_connection_signal_unsubscribe(helper->connection,
                                         helper->layout_changed_id);
    helper->layout_changed_id = 0;
  }
}

static void
keyboard_name_appeared(GDBusConnection *connection,
                       const gchar *name,
                       const gchar *name_owner,
                       gpointer user_data)
{
  Plasma6Helper *helper = user_data;

  (void) name;
  (void) name_owner;

  if (helper->stopping || connection != helper->connection)
    return;

  unsubscribe_keyboard_signals(helper);
  helper->list_changed_id =
    g_dbus_connection_signal_subscribe(
        connection, KDE_KEYBOARD_NAME, KDE_KEYBOARD_INTERFACE,
        "layoutListChanged", KDE_KEYBOARD_PATH, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, keyboard_signal_cb, helper, NULL);
  helper->layout_changed_id =
    g_dbus_connection_signal_subscribe(
        connection, KDE_KEYBOARD_NAME, KDE_KEYBOARD_INTERFACE,
        "layoutChanged", KDE_KEYBOARD_PATH, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, keyboard_signal_cb, helper, NULL);

  emit_snapshot(helper);
}

static void
keyboard_name_vanished(GDBusConnection *connection,
                       const gchar *name,
                       gpointer user_data)
{
  Plasma6Helper *helper = user_data;

  (void) name;

  if (connection != helper->connection)
    return;

  unsubscribe_keyboard_signals(helper);
  g_printerr(_("Plasma keyboard service disappeared; waiting for it to return\n"));
}

static void
session_bus_closed(GDBusConnection *connection,
                   gboolean remote_peer_vanished,
                   GError *error,
                   gpointer user_data)
{
  Plasma6Helper *helper = user_data;

  (void) connection;
  (void) remote_peer_vanished;

  g_printerr(_("Plasma session bus closed: %s\n"),
             error ? error->message : "connection closed");
  g_main_loop_quit(helper->loop);
}

int
rcl_plasma6_input_sources_helper_run(void)
{
  Plasma6Helper helper = { 0 };
  GError *error = NULL;

  helper.connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!helper.connection)
  {
    g_printerr(_("could not connect to Plasma session bus: %s\n"),
               error ? error->message : "unknown error");
    g_clear_error(&error);
    return 1;
  }

  g_dbus_connection_set_exit_on_close(helper.connection, FALSE);
  helper.loop = g_main_loop_new(NULL, FALSE);
  helper.closed_id =
    g_signal_connect(helper.connection, "closed",
                     G_CALLBACK(session_bus_closed), &helper);
  helper.name_watch_id =
    g_bus_watch_name_on_connection(helper.connection,
                                    KDE_KEYBOARD_NAME,
                                    G_BUS_NAME_WATCHER_FLAGS_NONE,
                                    keyboard_name_appeared,
                                    keyboard_name_vanished,
                                    &helper, NULL);

  g_main_loop_run(helper.loop);

  helper.stopping = TRUE;
  if (helper.name_watch_id)
    g_bus_unwatch_name(helper.name_watch_id);
  unsubscribe_keyboard_signals(&helper);
  if (helper.closed_id)
    g_signal_handler_disconnect(helper.connection, helper.closed_id);
  g_clear_object(&helper.connection);
  g_main_loop_unref(helper.loop);
  return 0;
}

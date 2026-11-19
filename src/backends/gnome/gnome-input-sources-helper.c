/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Per-user GNOME input-source settings monitor.
 *
 * This process is launched by localed after dropping to the selected
 * session user's UID and GID. It never writes system configuration. It
 * emits a single GVariant tuple per settings update on stdout:
 *   (as layouts, as variants, as options, s active_layout, s active_variant)
 *
 * GNOME's mru-sources ordering is authoritative for the selected source.
 * The first XKB entry is the active layout; current is not reliably updated
 * by all GNOME Shell input-source switching paths.
 */

#include <gio/gio.h>
#include <stdio.h>
#include <glib.h>

static void
emit_settings(GSettings *settings)
{
  GVariant *sources;
  GVariant *options;
  GVariant *mru_sources;
  GVariantIter iter;
  const gchar *type;
  const gchar *id;
  gboolean active_source_found = FALSE;
  gchar *active_layout = g_strdup("");
  gchar *active_variant = g_strdup("");
  GPtrArray *layouts = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *variants = g_ptr_array_new_with_free_func(g_free);
  gchar **option_strv;
  GVariant *snapshot;
  gchar *printed;

  sources = g_settings_get_value(settings, "sources");
  mru_sources = g_settings_get_value(settings, "mru-sources");
  g_variant_iter_init(&iter, sources);

  while (g_variant_iter_next(&iter, "(&s&s)", &type, &id))
  {
    gchar **parts = NULL;
    gchar *layout = NULL;
    gchar *variant = NULL;

    /* current indexes the complete sources array, including non-XKB
     * sources. Keep that index separate from the filtered XKB arrays. */
    if (g_strcmp0(type, "xkb") == 0)
    {
      parts = g_strsplit(id, "+", 2);
      layout = g_strdup(parts[0]);
      variant = g_strdup(parts[1] ? parts[1] : "");

      g_ptr_array_add(layouts, layout);
      g_ptr_array_add(variants, variant);
      g_strfreev(parts);
    }

  }

  /* mru-sources is ordered most-recently-used first. Select its first XKB
   * entry, while keeping the complete configured sources list above. */
  g_variant_iter_init(&iter, mru_sources);
  while (g_variant_iter_next(&iter, "(&s&s)", &type, &id))
  {
    if (g_strcmp0(type, "xkb") == 0)
    {
      gchar **parts = g_strsplit(id, "+", 2);
      g_free(active_layout);
      g_free(active_variant);
      active_layout = g_strdup(parts[0]);
      active_variant = g_strdup(parts[1] ? parts[1] : "");
      g_strfreev(parts);
      active_source_found = TRUE;
      break;
    }
  }

  if (!active_source_found)
  {
    g_clear_pointer(&active_layout, g_free);
    g_clear_pointer(&active_variant, g_free);
    active_layout = g_strdup("");
    active_variant = g_strdup("");
  }

  options = g_settings_get_value(settings, "xkb-options");
  option_strv = g_variant_dup_strv(options, NULL);

  g_ptr_array_add(layouts, NULL);
  g_ptr_array_add(variants, NULL);

  snapshot = g_variant_ref_sink(g_variant_new("(^as^as^as&s&s)",
                           (gchar **) layouts->pdata,
                           (gchar **) variants->pdata,
                           option_strv,
                           active_layout,
                           active_variant));
  printed = g_variant_print(snapshot, TRUE);

  g_print("%s\n", printed);
  fflush(stdout);

  g_free(printed);
  g_variant_unref(snapshot);
  g_strfreev(option_strv);
  g_free(active_layout);
  g_free(active_variant);
  g_variant_unref(options);
  g_variant_unref(sources);
  g_variant_unref(mru_sources);
  g_ptr_array_unref(layouts);
  g_ptr_array_unref(variants);
}

static void
settings_changed(GSettings *settings, const gchar *key, gpointer user_data)
{
  (void) user_data;

  if (g_strcmp0(key, "sources") == 0 ||
      g_strcmp0(key, "mru-sources") == 0 ||
      g_strcmp0(key, "xkb-options") == 0)
    emit_settings(settings);
}

int
rcl_gnome_input_sources_helper_run(void)
{
  GSettingsSchemaSource *source;
  GSettingsSchema *schema;
  GSettings *settings;
  GMainLoop *loop;

  source = g_settings_schema_source_get_default();
  if (!source)
  {
    g_printerr("%s\n", _("no GSettings schema source available"));
    return 1;
  }

  schema = g_settings_schema_source_lookup(
      source, "org.gnome.desktop.input-sources", TRUE);
  if (!schema)
  {
    g_printerr("%s\n", _("GNOME input-sources GSettings schema is unavailable"));
    return 1;
  }

  settings = g_settings_new_full(schema, NULL, NULL);
  g_signal_connect(settings, "changed",
                   G_CALLBACK(settings_changed), NULL);

  emit_settings(settings);
  loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);

  g_main_loop_unref(loop);
  g_object_unref(settings);
  g_settings_schema_unref(schema);
  return 0;
}

/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <gio/gio.h>
#include <glib.h>

#include "plasma6-backend.h"

static void
test_plasma6_formats_lang(void)
{
  const gchar *locales[] = { "ru_RU.UTF-8", "sr_RS.UTF-8@latin", "C" };
  guint i;

  for (i = 0; i < G_N_ELEMENTS(locales); i++)
  {
    g_autofree gchar *contents = g_strdup_printf(
        "[Formats]\nLANG=%s\n\n[Translations]\nLANGUAGE=ru:en_US\n",
        locales[i]);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *locale =
        rcl_plasma6_backend_locale_from_localerc(contents, &error);

    g_assert_no_error(error);
    g_assert_cmpstr(locale, ==, locales[i]);
  }
}

static void
test_plasma6_formats_lang_missing(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *locale = rcl_plasma6_backend_locale_from_localerc(
      "[Formats]\nLC_TIME=en_US.UTF-8\n[Translations]\nLANGUAGE=ru:en_US\n",
      &error);

  g_assert_null(locale);
  g_assert_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND);
}

static void
test_plasma6_formats_lang_invalid(void)
{
  const gchar *values[] = { "", "ru:en_US", "ru_RU.UTF-8 # comment" };
  guint i;

  for (i = 0; i < G_N_ELEMENTS(values); i++)
  {
    g_autofree gchar *contents = g_strdup_printf("[Formats]\nLANG=%s\n",
                                                  values[i]);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *locale =
        rcl_plasma6_backend_locale_from_localerc(contents, &error);

    g_assert_null(locale);
    g_assert_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE);
  }
}

static void
test_plasma6_layout_names_signature(void)
{
  GVariant *reply;
  GVariant *layouts;
  GVariantIter iter;
  const gchar *short_name;
  const gchar *display_name;
  const gchar *long_name;
  guint count = 0;

  /*
   * This is the exact LayoutNames tuple returned by
   * org.kde.KeyboardLayouts.getLayoutsList(): shortName, displayName,
   * longName. The second field is not an XKB variant.
   */
  reply = g_variant_parse(G_VARIANT_TYPE("(a(sss))"),
                          "([('us', 'us', 'English (US)'), "
                          "('es', 'es', 'Spanish')],)",
                          NULL, NULL, NULL);
  g_assert_nonnull(reply);
  g_variant_get(reply, "(@a(sss))", &layouts);
  g_variant_iter_init(&iter, layouts);

  while (g_variant_iter_next(&iter, "(&s&s&s)",
                             &short_name, &display_name, &long_name))
  {
    if (count == 0)
    {
      g_assert_cmpstr(short_name, ==, "us");
      g_assert_cmpstr(display_name, ==, "us");
      g_assert_cmpstr(long_name, ==, "English (US)");
    }
    else
    {
      g_assert_cmpstr(short_name, ==, "es");
      g_assert_cmpstr(display_name, ==, "es");
      g_assert_cmpstr(long_name, ==, "Spanish");
    }
    count++;
  }

  g_assert_cmpuint(count, ==, 2);
  g_variant_unref(layouts);
  g_variant_unref(reply);
}

static void
test_plasma6_snapshot_format(void)
{
  const gchar *output =
      "(['us', 'es'], ['', ''], @as [], 'es', '')";
  GError *error = NULL;
  GVariant *snapshot;

  snapshot = g_variant_parse(
      G_VARIANT_TYPE(RCL_PLASMA6_BACKEND_SNAPSHOT_TYPE),
      output, NULL, NULL, &error);

  g_assert_no_error(error);
  g_assert_nonnull(snapshot);
  g_assert_true(g_variant_is_of_type(
      snapshot, G_VARIANT_TYPE(RCL_PLASMA6_BACKEND_SNAPSHOT_TYPE)));

  g_variant_unref(snapshot);
}

static void
test_plasma6_backend_start_stop(void)
{
  GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  RclBackend *backend;
  RclLocaleDaemon *daemon;
  GError *error = NULL;
  guint i;

  g_test_dbus_up(bus);
  g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), TRUE);

  daemon = g_object_new(RCL_TYPE_LOCALE_DAEMON, NULL);
  g_assert_nonnull(daemon);

  g_assert_true(rcl_locale_daemon_set_x11_keyboard_internal(
      daemon, "us,es", "", "", "", &error));
  g_assert_no_error(error);
  g_assert_true(rcl_locale_daemon_set_vconsole_keyboard_internal(
      daemon, "es", "", &error));
  g_assert_no_error(error);

  backend = rcl_plasma6_backend_new(daemon);
  for (i = 0; i < 2; i++)
  {
    g_assert_true(rcl_backend_start(backend, &error));
    g_assert_no_error(error);
    rcl_backend_stop(backend);
  }

  g_object_unref(backend);
  g_object_unref(daemon);

  g_unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
  g_test_dbus_down(bus);
  g_object_unref(bus);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/plasma6/formats-lang", test_plasma6_formats_lang);
  g_test_add_func("/plasma6/formats-lang-missing",
                  test_plasma6_formats_lang_missing);
  g_test_add_func("/plasma6/formats-lang-invalid",
                  test_plasma6_formats_lang_invalid);
  g_test_add_func("/plasma6/layout-names-signature",
                  test_plasma6_layout_names_signature);
  g_test_add_func("/plasma6/snapshot-format",
                  test_plasma6_snapshot_format);
  g_test_add_func("/plasma6/backend-start-stop",
                  test_plasma6_backend_start_stop);
  return g_test_run();
}

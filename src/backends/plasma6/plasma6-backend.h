/*
 * Copyright (C) 2026 Nathaniel Russell <naterussell83@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>
#include "rcl-locale.h"
#include "../backend-interface.h"

/* Shared helper snapshot: layouts, variants, options, active layout, active variant. */
#define RCL_PLASMA6_BACKEND_SNAPSHOT_TYPE "(asasasss)"

G_BEGIN_DECLS

#define RCL_TYPE_PLASMA6_BACKEND (rcl_plasma6_backend_get_type())
G_DECLARE_FINAL_TYPE(RclPlasma6Backend, rcl_plasma6_backend,
                     RCL, PLASMA6_BACKEND, GObject)

RclBackend *rcl_plasma6_backend_new(RclLocaleDaemon *daemon);
gboolean rcl_plasma6_backend_start(RclPlasma6Backend *self, GError **error);
void rcl_plasma6_backend_stop(RclPlasma6Backend *self);
/* Read the configured locale from Plasma 6 Formats/LANG. */
gchar *rcl_plasma6_backend_locale_from_localerc(const gchar *contents,
                                                GError **error);

/* Returns the one unambiguous active local Plasma session, if available. */
gboolean rcl_plasma6_backend_get_active_session(RclPlasma6Backend *self,
                                                guint *uid,
                                                gchar **session_id,
                                                gchar **object_path,
                                                gchar **seat,
                                                gchar **session_type,
                                                gchar **desktop);

G_END_DECLS

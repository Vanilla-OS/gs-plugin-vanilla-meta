/*
 * Copyright (C) 2023 Mateus Melchiades
 */

#pragma once

#include <glib.h>
#include <gnome-software.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define GS_TYPE_PLUGIN_VANILLA_META (gs_plugin_vanilla_meta_get_type ())

G_DECLARE_FINAL_TYPE (GsPluginVanillaMeta, gs_plugin_vanilla_meta, GS, PLUGIN_VANILLA_META, GsPlugin)

G_END_DECLS


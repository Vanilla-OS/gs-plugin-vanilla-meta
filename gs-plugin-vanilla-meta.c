/*
 * Copyright (C) 2023 Mateus Melchiades
 */

#include <glib.h>
#include <gnome-software.h>

#include "gs-plugin-vanilla-meta.h"

struct _GsPluginVanillaMeta {
    GsPlugin parent;

    /* private data here */
};

G_DEFINE_TYPE(GsPluginVanillaMeta, gs_plugin_vanilla_meta, GS_TYPE_PLUGIN)

static void
gs_plugin_vanilla_meta_init(GsPluginVanillaMeta *self)
{
    GsPlugin *plugin = GS_PLUGIN(self);

    gs_plugin_add_rule(plugin, GS_PLUGIN_RULE_RUN_BEFORE, "appstream");
}

static void
gs_plugin_vanilla_meta_list_apps_async(GsPlugin *plugin, GsAppQuery *query,
                                       GsPluginListAppsFlags flags, GCancellable *cancellable,
                                       GAsyncReadyCallback callback, gpointer user_data)
{
    GsPluginVanillaMeta *self = GS_PLUGIN_VANILLA_META(plugin);
    g_autoptr(GTask) task     = NULL;
    const gchar *const *keywords;
    g_autoptr(GsAppList) list = gs_app_list_new();

    task =
        gs_plugin_list_apps_data_new_task(plugin, query, flags, cancellable, callback, user_data);
    g_task_set_source_tag(task, gs_plugin_vanilla_meta_list_apps_async);

    if (query == NULL || gs_app_query_get_keywords(query) == NULL ||
        gs_app_query_get_n_properties_set(query) != 1) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported query");
        return;
    }

    keywords = gs_app_query_get_keywords(query);

    for (gsize i = 0; keywords[i] != NULL; i++) {
        if (g_str_equal(keywords[i], "fotoshop")) {
            g_autoptr(GsApp) app = gs_app_new("org.gimp.GIMP");
            gs_app_add_quirk(app, GS_APP_QUIRK_IS_WILDCARD);
            gs_app_list_add(list, app);
        }
    }

    g_task_return_pointer(task, g_steal_pointer(&list), g_object_unref);
}

static GsAppList *
gs_plugin_vanilla_meta_list_apps_finish(GsPlugin *plugin, GAsyncResult *result, GError **error)
{
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
gs_plugin_vanilla_meta_class_init(GsPluginVanillaMetaClass *klass)
{
    GsPluginClass *plugin_class    = GS_PLUGIN_CLASS(klass);

    plugin_class->list_apps_async  = gs_plugin_vanilla_meta_list_apps_async;
    plugin_class->list_apps_finish = gs_plugin_vanilla_meta_list_apps_finish;
}

GType
gs_plugin_query_type(void)
{
    return GS_TYPE_PLUGIN_VANILLA_META;
}

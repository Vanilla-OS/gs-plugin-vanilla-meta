/*
 * Copyright (C) 2023 Mateus Melchiades
 */

#include <glib.h>
#include <gnome-software.h>
#include <gnome-software/gs-appstream.h>
#include <xmlb.h>

#include "gs-plugin-vanilla-meta.h"
#include "gs-vanilla-meta-util.h"

static void claim_app_list(GsPluginVanillaMeta *self, GsAppList *list);
static void refresh_metadata_thread_cb(GTask *task,
                                       gpointer source_object,
                                       gpointer task_data,
                                       GCancellable *cancellable);
static gboolean add_apps_from_metadata_file(GsPluginVanillaMeta *self,
                                            GFile *metadata_file,
                                            GCancellable *cancellable,
                                            GError **error);
static void list_apps_thread_cb(GTask *task,
                                gpointer source_object,
                                gpointer task_data,
                                GCancellable *cancellable);
static void refine_thread_cb(GTask *task,
                             gpointer source_object,
                             gpointer task_data,
                             GCancellable *cancellable);

const gchar *gz_metadata_filename   = ".cache/vanilla_meta/metadata.xml.gz";
const gchar *metadata_silo_filename = ".cache/vanilla_meta/metadata.xmlb";
const gchar *metadata_url           = "";

struct _GsPluginVanillaMeta {
    GsPlugin parent;
    GsWorkerThread *worker; /* (owned) */
    // TODO: Create mutex lock for silo
    XbSilo *silo;
};

G_DEFINE_TYPE(GsPluginVanillaMeta, gs_plugin_vanilla_meta, GS_TYPE_PLUGIN)

static void
gs_plugin_vanilla_meta_dispose(GObject *object)
{
    GsPluginVanillaMeta *self = GS_PLUGIN_VANILLA_META(object);

    g_clear_object(&self->worker);
    G_OBJECT_CLASS(gs_plugin_vanilla_meta_parent_class)->dispose(object);
}

static void
gs_plugin_vanilla_meta_setup_async(GsPlugin *plugin,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
    GsPluginVanillaMeta *self = GS_PLUGIN_VANILLA_META(plugin);
    g_autoptr(GTask) task     = NULL;

    task = g_task_new(plugin, cancellable, callback, user_data);
    g_task_set_source_tag(task, gs_plugin_vanilla_meta_setup_async);

    // Start up a worker thread to process all the plugin’s function calls.
    self->worker = gs_worker_thread_new("gs-plugin-vanilla-meta");

    g_task_return_boolean(task, true);
}

static gboolean
gs_plugin_vanilla_meta_setup_finish(GsPlugin *plugin, GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

static void
gs_plugin_vanilla_meta_init(GsPluginVanillaMeta *self)
{
    GsPlugin *plugin = GS_PLUGIN(self);

    gs_plugin_set_appstream_id(plugin, "org.vanillaos.meta");

    gs_plugin_add_rule(plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");
}

gboolean
gs_plugin_add_sources(GsPlugin *plugin, GsAppList *list, GCancellable *cancellable, GError **error)
{
    g_autoptr(GsApp) app = NULL;

    // Create source
    app = gs_app_new("org.vanillaos.vanilla-meta");
    gs_app_set_kind(app, AS_COMPONENT_KIND_REPOSITORY);
    gs_app_set_state(app, GS_APP_STATE_INSTALLED);
    gs_app_add_quirk(app, GS_APP_QUIRK_NOT_LAUNCHABLE);
    gs_app_set_size_download(app, GS_SIZE_TYPE_UNKNOWABLE, 0);
    gs_app_set_management_plugin(app, plugin);
    gs_vanilla_meta_app_set_packaging_info(app);
    gs_app_set_scope(app, AS_COMPONENT_SCOPE_USER);

    gs_app_set_metadata(app, "GnomeSoftware::SortKey", "200");
    gs_app_set_metadata(app, "GnomeSoftware::InstallationKind", "System Installation");
    gs_app_add_quirk(app, GS_APP_QUIRK_PROVENANCE);

    gs_app_set_name(app, GS_APP_QUALITY_NORMAL, "VanillaOS Meta");
    gs_app_set_summary(app, GS_APP_QUALITY_LOWEST,
                       "Applications installable via Apx with pre-defined container configuration");

    /* origin_ui on a remote is the repo dialogue section name,
     * not the remote title */
    gs_app_set_origin_ui(app, "Apx Apps");
    gs_app_set_description(app, GS_APP_QUALITY_NORMAL,
                           "This repository contains a set of popular applications installable via "
                           "Apx and pre-configured by the Vanilla OS team to guarantee that they "
                           "are using the most compatible container and configurations.");
    gs_app_set_url(app, AS_URL_KIND_HOMEPAGE, "https://vanillaos.org");

    gs_app_list_add(list, app);

    // TODO: Add related apps (the ones installed from our repo)

    return true;
}

void
gs_plugin_adopt_app(GsPlugin *plugin, GsApp *app)
{
    if (gs_app_get_metadata_item(app, "Vanilla::apx_container") != NULL) {
        g_debug("I should adopt app %s", gs_app_get_name(app));
        gs_app_set_management_plugin(app, plugin);
        gs_vanilla_meta_app_set_packaging_info(app);
    }
}

static void
claim_app_list(GsPluginVanillaMeta *self, GsAppList *list)
{
    for (guint i = 0; i < gs_app_list_length(list); i++) {
        GsApp *app = gs_app_list_index(list, i);

        g_debug("%s belongs to us", gs_app_get_id(app));

        if (gs_app_has_quirk(app, GS_APP_QUIRK_IS_WILDCARD)) {
            g_debug("App %s is wildcard. Skipping..", gs_app_get_id(app));
            continue;
        }

        gs_app_set_scope(app, AS_COMPONENT_SCOPE_USER);
        gs_app_set_branch(app, "main");
        gs_app_set_origin(app, "vanilla-meta");
        gs_app_set_origin_appstream(app, "vanilla-meta");
        gs_app_set_origin_ui(app, "VanillaOS Meta");

        gs_app_set_management_plugin(app, GS_PLUGIN(self));
        gs_vanilla_meta_app_set_packaging_info(app);
    }
}

static void
gs_plugin_vanilla_meta_refresh_metadata_async(GsPlugin *plugin,
                                              guint64 cache_age_secs,
                                              GsPluginRefreshMetadataFlags flags,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data)
{
    GsPluginVanillaMeta *self = GS_PLUGIN_VANILLA_META(plugin);
    g_autoptr(GTask) task     = NULL;

    task = g_task_new(plugin, cancellable, callback, user_data);
    g_task_set_source_tag(task, gs_plugin_vanilla_meta_refresh_metadata_async);
    g_task_set_task_data(task, gs_plugin_refresh_metadata_data_new(cache_age_secs, flags),
                         (GDestroyNotify)gs_plugin_refresh_metadata_data_free);

    gs_worker_thread_queue(self->worker, G_PRIORITY_LOW, refresh_metadata_thread_cb,
                           g_steal_pointer(&task));
}

static void
refresh_metadata_thread_cb(GTask *task,
                           gpointer source_object,
                           gpointer task_data,
                           GCancellable *cancellable)
{
    GsPluginVanillaMeta *self         = GS_PLUGIN_VANILLA_META(source_object);
    GsPluginRefreshMetadataData *data = task_data;

    g_autoptr(GError) error          = NULL;
    g_autoptr(GFile) f               = g_file_new_for_uri(metadata_url);
    g_autoptr(GFileInputStream) f_is = NULL;

    g_autoptr(GFile) gz_file            = g_file_new_for_path(gz_metadata_filename);
    g_autoptr(SoupSession) soup_session = NULL;

    soup_session = gs_build_soup_session();

    // Is the metadata missing or too old?
    if (gs_utils_get_file_age(gz_file) >= data->cache_age_secs) {
        g_debug("I should refresh metadata");

        // Download metadata file
        f_is = g_file_read(f, cancellable, &error);
        if (error != NULL) {
            g_debug("Could not open input stream for uri: %s", error->message);
            g_task_return_error(task, g_steal_pointer(&error));
            return;
        }

        if (!g_file_copy(f, gz_file, G_FILE_COPY_OVERWRITE, cancellable, NULL, NULL, &error)) {
            g_debug("Could not copy file to disk: %s", error->message);
            g_task_return_error(task, g_steal_pointer(&error));
            return;
        }
    } else {
        g_debug("Cache is only %zu seconds old, https packets aren't free, ya know?",
                gs_utils_get_file_age(gz_file));
    }

    add_apps_from_metadata_file(self, gz_file, cancellable, &error);
    g_task_return_boolean(task, TRUE);
}

/*
 * REFERENCE:
 * https://gitlab.gnome.org/GNOME/gnome-software/-/blob/main/plugins/flatpak/gs-flatpak.c
 * Function `gs_flatpak_add_apps_from_xremote` (line 888)
 */
static gboolean
add_apps_from_metadata_file(GsPluginVanillaMeta *self,
                            GFile *metadata_file,
                            GCancellable *cancellable,
                            GError **error)
{
    const gchar *const *locales       = g_get_language_names();
    g_autoptr(XbBuilder) builder      = xb_builder_new();
    g_autoptr(XbBuilderSource) source = xb_builder_source_new();
    g_autoptr(GFile) silo_file        = g_file_new_for_path(metadata_silo_filename);

    g_debug("Loading app silo");

    // Add current locales
    for (guint i = 0; locales[i] != NULL; i++)
        xb_builder_add_locale(builder, locales[i]);

    // Check appstream file exists, otherwise try to download it
    if (!g_file_query_exists(metadata_file, cancellable)) {
        // TODO: Separate metadata download from `refresh_metadata_async`
        // so we can call it again here.
        g_debug("Metadata file doesn't exist and should be downloaded");
    }

    // Load file into silo
    if (!xb_builder_source_load_file(source, metadata_file,
                                     XB_BUILDER_SOURCE_FLAG_WATCH_FILE |
                                         XB_BUILDER_SOURCE_FLAG_LITERAL_TEXT,
                                     cancellable, error)) {
        g_debug("Failed to load xml file for builder");
        return false;
    }

    // Import source to builder
    xb_builder_import_source(builder, source);

    // Save to silo
    self->silo = xb_builder_ensure(builder, silo_file,
                                   XB_BUILDER_COMPILE_FLAG_IGNORE_INVALID |
                                       XB_BUILDER_COMPILE_FLAG_SINGLE_LANG,
                                   cancellable, error);

    if (self->silo == NULL) {
        g_debug("Failed to create silo: %s", (*error)->message);
        return false;
    }

    return true;
}

static gboolean
gs_plugin_vanilla_meta_refresh_metadata_finish(GsPlugin *plugin,
                                               GAsyncResult *result,
                                               GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

static void
gs_plugin_vanilla_meta_list_apps_async(GsPlugin *plugin,
                                       GsAppQuery *query,
                                       GsPluginListAppsFlags flags,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    GsPluginVanillaMeta *self = GS_PLUGIN_VANILLA_META(plugin);
    g_autoptr(GTask) task     = NULL;

    task =
        gs_plugin_list_apps_data_new_task(plugin, query, flags, cancellable, callback, user_data);
    g_task_set_source_tag(task, gs_plugin_vanilla_meta_list_apps_async);

    gs_worker_thread_queue(self->worker, G_PRIORITY_LOW, list_apps_thread_cb,
                           g_steal_pointer(&task));
}

static void
list_apps_thread_cb(GTask *task,
                    gpointer source_object,
                    gpointer task_data,
                    GCancellable *cancellable)
{
    GsPluginVanillaMeta *self  = GS_PLUGIN_VANILLA_META(source_object);
    GsPluginListAppsData *data = task_data;

    const gchar *const *keywords = NULL;

    /* g_autoptr(GsAppList) list     = gs_app_list_new(); */
    g_autoptr(GsAppList) list_tmp = gs_app_list_new();
    g_autoptr(GError) error       = NULL;

    if (data->query != NULL) {
        keywords = gs_app_query_get_keywords(data->query);
    }

    // TODO: Move this and other appstream-related queries to a separate function
    if (keywords != NULL) {
        if (!gs_appstream_search(GS_PLUGIN(self), self->silo, keywords, list_tmp, cancellable,
                                 &error)) {
            g_debug("Error while searching: %s", error->message);
            g_task_return_error(task, g_steal_pointer(&error));
            return;
        }
    }

    claim_app_list(self, list_tmp);

    // TODO: Remove this once we can install packages
    for (guint i = 0; i < gs_app_list_length(list_tmp); i++) {
        g_debug("%s", gs_app_to_string(gs_app_list_index(list_tmp, i)));
        gs_app_set_state(gs_app_list_index(list_tmp, i), GS_APP_STATE_AVAILABLE);
    }

    /* for (guint i = 0; i < gs_app_list_length(list_tmp); i++) { */
    /*     g_autoptr(GsApp) app = gs_app_list_index(list_tmp, i); */
    /*     gchar *id = gs_utils_build_unique_id(AS_COMPONENT_SCOPE_USER, AS_BUNDLE_KIND_PACKAGE, */
    /*                                          gs_app_get_origin(app), gs_app_get_id(app), */
    /*                                          gs_app_get_branch(app)); */
    /*     g_debug("App unique id: %s", id); */
    /*     gs_plugin_cache_add(GS_PLUGIN(self), id, app); */
    /* } */

    /* gs_app_list_add_list(list, tmp_list); */

    g_task_return_pointer(task, g_steal_pointer(&list_tmp), g_object_unref);
    /* g_task_return_pointer (task, g_steal_pointer (&list), g_object_unref); */
}

static GsAppList *
gs_plugin_list_apps_finish(GsPlugin *plugin, GAsyncResult *result, GError **error)
{
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
gs_plugin_vanilla_meta_refine_async(GsPlugin *plugin,
                                    GsAppList *list,
                                    GsPluginRefineFlags flags,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
    GsPluginVanillaMeta *self = GS_PLUGIN_VANILLA_META(plugin);
    g_autoptr(GTask) task     = NULL;

    task = gs_plugin_refine_data_new_task(plugin, list, flags, cancellable, callback, user_data);

    g_task_set_source_tag(task, gs_plugin_vanilla_meta_refine_async);

    gs_worker_thread_queue(self->worker, G_PRIORITY_LOW, refine_thread_cb, g_steal_pointer(&task));
}

static void
refine_thread_cb(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    GsPluginVanillaMeta *self = GS_PLUGIN_VANILLA_META(source_object);
    GsPluginRefineData *data  = task_data;

    for (guint i = 0; i < gs_app_list_length(data->list); i++) {
        GsApp *app = gs_app_list_index(data->list, i);

        // If the app isn't ours, do nothing
        if (!gs_app_has_management_plugin(app, GS_PLUGIN(self)))
            continue;

        /* const gchar *origin           = gs_app_get_origin(app); */
        const gchar *source           = gs_app_get_source_default(app);
        g_autofree gchar *source_safe = NULL;
        g_autofree gchar *xpath       = NULL;
        g_autoptr(XbNode) component   = NULL;
        g_autoptr(GError) error_local = NULL;

        /* find using source and origin */
        source_safe = xb_string_escape(source);
        xpath       = g_strdup_printf("components/component/"
                                            "bundle[@container]/..");
        component   = xb_silo_query_first(self->silo, xpath, &error_local);

        if (component == NULL) {
            g_debug("no match for %s: %s", xpath, error_local->message);
            g_clear_error(&error_local);
        }

        gs_appstream_refine_app(GS_PLUGIN(self), app, self->silo, component, data->flags,
                                &error_local);

        g_debug("Refined %s", gs_app_get_id(app));
        g_debug("%s", gs_app_to_string(app));
    }

    g_task_return_boolean(task, true);
}

static gboolean
gs_plugin_vanilla_meta_refine_finish(GsPlugin *plugin, GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

static void
gs_plugin_vanilla_meta_class_init(GsPluginVanillaMetaClass *klass)
{
    GObjectClass *object_class  = G_OBJECT_CLASS(klass);
    GsPluginClass *plugin_class = GS_PLUGIN_CLASS(klass);

    object_class->dispose = gs_plugin_vanilla_meta_dispose;

    plugin_class->setup_async             = gs_plugin_vanilla_meta_setup_async;
    plugin_class->setup_finish            = gs_plugin_vanilla_meta_setup_finish;
    plugin_class->refresh_metadata_async  = gs_plugin_vanilla_meta_refresh_metadata_async;
    plugin_class->refresh_metadata_finish = gs_plugin_vanilla_meta_refresh_metadata_finish;
    plugin_class->list_apps_async         = gs_plugin_vanilla_meta_list_apps_async;
    plugin_class->list_apps_finish        = gs_plugin_list_apps_finish;
    plugin_class->refine_async            = gs_plugin_vanilla_meta_refine_async;
    plugin_class->refine_finish           = gs_plugin_vanilla_meta_refine_finish;
}

GType
gs_plugin_query_type(void)
{
    return GS_TYPE_PLUGIN_VANILLA_META;
}

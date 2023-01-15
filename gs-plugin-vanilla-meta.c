/*
 * Copyright (C) 2023 Mateus Melchiades
 */

#include <glib.h>
#include <gnome-software.h>
#include <gnome-software/gs-appstream.h>
#include <xmlb.h>

#include "gs-plugin-vanilla-meta.h"
#include "gs-vanilla-meta-util.h"

#if LIBXMLB_CHECK_VERSION(0, 3, 0)
static gboolean gs_vanilla_meta_tokenize_cb(XbBuilderFixup *self,
                                            XbBuilderNode *bn,
                                            gpointer user_data,
                                            GError **error);
#endif
static gboolean gs_vanilla_meta_set_origin_cb(XbBuilderFixup *self,
                                              XbBuilderNode *bn,
                                              gpointer user_data,
                                              GError **error);
static gboolean plugin_vanillameta_pick_apx_desktop_file_cb(GsPlugin *plugin,
                                                            GsApp *app,
                                                            const gchar *filename,
                                                            GKeyFile *key_file);
static void enable_repository_thread_cb(GTask *task,
                                        gpointer source_object,
                                        gpointer task_data,
                                        GCancellable *cancellable);
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
gs_plugin_vanilla_meta_finalize(GObject *object)
{
    G_OBJECT_CLASS(gs_plugin_vanilla_meta_parent_class)->finalize(object);
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

    g_task_return_boolean(task, TRUE);
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

    gs_plugin_set_appstream_id(plugin, "vanilla_meta");

    gs_plugin_add_rule(plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");
}

gboolean
gs_plugin_add_sources(GsPlugin *plugin, GsAppList *list, GCancellable *cancellable, GError **error)
{
    g_autoptr(GsApp) app = NULL;

    // Create source
    /* app = gs_app_new("org.vanillaos.vanilla-meta"); */
    app = gs_app_new("vanilla_meta");
    gs_app_set_kind(app, AS_COMPONENT_KIND_REPOSITORY);
    gs_app_set_state(app, GS_APP_STATE_INSTALLED);
    gs_app_add_quirk(app, GS_APP_QUIRK_NOT_LAUNCHABLE);
    gs_app_set_size_download(app, GS_SIZE_TYPE_UNKNOWABLE, 0);
    gs_app_set_management_plugin(app, plugin);
    gs_vanilla_meta_app_set_packaging_info(app);
    gs_app_set_scope(app, AS_COMPONENT_SCOPE_USER);

    gs_app_set_metadata(app, "GnomeSoftware::SortKey", "200");
    gs_app_set_metadata(app, "GnomeSoftware::InstallationKind", "User Installation");
    gs_app_add_quirk(app, GS_APP_QUIRK_PROVENANCE);

    gs_app_set_name(app, GS_APP_QUALITY_NORMAL, "VanillaOS Meta");
    gs_app_set_summary(app, GS_APP_QUALITY_LOWEST,
                       "Applications installable via Apx with pre-defined container configuration");

    /* origin_ui on a remote is the repo dialogue section name,
     * not the remote title */
    gs_app_set_origin_ui(app, "Apx Applications");
    gs_app_set_description(app, GS_APP_QUALITY_NORMAL,
                           "This repository contains a set of popular applications installable via "
                           "Apx and pre-configured by the Vanilla OS team to guarantee that they "
                           "are using the most compatible container and configurations.");
    gs_app_set_url(app, AS_URL_KIND_HOMEPAGE, "https://vanillaos.org");

    gs_app_list_add(list, app);

    // TODO: Add related apps (the ones installed from our repo)

    return TRUE;
}

static void
gs_plugin_vanilla_meta_enable_repository_async(GsPlugin *plugin,
                                               GsApp *repository,
                                               GsPluginManageRepositoryFlags flags,
                                               GCancellable *cancellable,
                                               GAsyncReadyCallback callback,
                                               gpointer user_data)
{
    GsPluginVanillaMeta *self = GS_PLUGIN_VANILLA_META(plugin);
    g_autoptr(GTask) task     = NULL;

    task = gs_plugin_manage_repository_data_new_task(plugin, repository, flags, cancellable,
                                                     callback, user_data);
    g_task_set_source_tag(task, gs_plugin_vanilla_meta_enable_repository_async);

    /* only process this app if was created by this plugin */
    if (!gs_app_has_management_plugin(repository, plugin)) {
        g_task_return_boolean(task, TRUE);
        return;
    }

    /* is a source */
    g_assert(gs_app_get_kind(repository) == AS_COMPONENT_KIND_REPOSITORY);

    gs_worker_thread_queue(self->worker, G_PRIORITY_LOW, enable_repository_thread_cb,
                           g_steal_pointer(&task));
}

static void
enable_repository_thread_cb(GTask *task,
                            gpointer source_object,
                            gpointer task_data,
                            GCancellable *cancellable)
{
    GsPluginVanillaMeta *self          = GS_PLUGIN_VANILLA_META(source_object);
    GsPluginManageRepositoryData *data = task_data;

    gs_app_set_state(data->repository, GS_APP_STATE_INSTALLED);
    gs_plugin_repository_changed(GS_PLUGIN(self), data->repository);

    g_task_return_boolean(task, TRUE);
}

static gboolean
gs_plugin_vanilla_meta_enable_repository_finish(GsPlugin *plugin,
                                                GAsyncResult *result,
                                                GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

static void
gs_plugin_vanilla_meta_disable_repository_async(GsPlugin *plugin,
                                                GsApp *repository,
                                                GsPluginManageRepositoryFlags flags,
                                                GCancellable *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data)
{
    GsPluginVanillaMeta *self = GS_PLUGIN_VANILLA_META(plugin);

    /* only process this app if was created by this plugin */
    if (!gs_app_has_management_plugin(repository, plugin)) {
        g_autoptr(GTask) task = NULL;

        task = g_task_new(self, cancellable, callback, user_data);
        g_task_set_source_tag(task, gs_plugin_vanilla_meta_disable_repository_async);
        g_task_return_boolean(task, TRUE);
        return;
    }

    gs_app_set_state(repository, GS_APP_STATE_AVAILABLE);
    gs_plugin_repository_changed(GS_PLUGIN(self), repository);
}

static gboolean
gs_plugin_vanilla_meta_disable_repository_finish(GsPlugin *plugin,
                                                 GAsyncResult *result,
                                                 GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

static gboolean
plugin_vanillameta_pick_apx_desktop_file_cb(GsPlugin *plugin,
                                            GsApp *app,
                                            const gchar *filename,
                                            GKeyFile *key_file)
{
    return strstr(filename, "/snapd/") == NULL && strstr(filename, "/snap/") == NULL &&
           strstr(filename, "/flatpak/") == NULL &&
           g_key_file_has_group(key_file, "Desktop Entry") &&
           !g_key_file_has_key(key_file, "Desktop Entry", "X-Flatpak", NULL) &&
           !g_key_file_has_key(key_file, "Desktop Entry", "X-SnapInstanceName", NULL);
}

gboolean
gs_plugin_launch(GsPlugin *plugin, GsApp *app, GCancellable *cancellable, GError **error)
{
    /* only process this app if was created by this plugin */
    if (!gs_app_has_management_plugin(app, plugin))
        return TRUE;

    return gs_plugin_app_launch_filtered(plugin, app, plugin_vanillameta_pick_apx_desktop_file_cb,
                                         NULL, error);
}

gboolean
gs_plugin_app_install(GsPlugin *plugin, GsApp *app, GCancellable *cancellable, GError **error)
{
    const gchar *package_name       = NULL;
    const gchar *container_flag     = NULL;
    const gchar *app_container_name = gs_app_get_metadata_item(app, "Vanilla::container");

    // Only process this app if was created by this plugin
    if (!gs_app_has_management_plugin(app, plugin))
        return TRUE;

    // Check container exists, otherwise run init for it
    GInputStream *input_stream;

    input_stream = gs_vanilla_meta_run_subprocess(
        "podman container ls --noheading -a | rev | cut -d\' \' -f 1 | rev",
        G_SUBPROCESS_FLAGS_STDOUT_PIPE, cancellable, error);

    if (input_stream != NULL) {
        g_autoptr(GByteArray) output = g_byte_array_new();
        gchar buffer[4096];
        gsize nread = 0;
        gboolean success;
        g_auto(GStrv) splits = NULL;

        gs_app_set_state(app, GS_APP_STATE_INSTALLING);

        while (success = g_input_stream_read_all(input_stream, buffer, sizeof(buffer), &nread,
                                                 cancellable, error),
               success && nread > 0) {
            g_byte_array_append(output, (const guint8 *)buffer, nread);
        }

        // If we have a valid output
        if (success && output->len > 0) {
            // NUL-terminate the array, to use it as a string
            g_byte_array_append(output, (const guint8 *)"", 1);

            splits = g_strsplit((gchar *)output->data, "\n", -1);

            if (app_container_name == NULL) {
                g_debug("Install: Container name not set for %s, cannot install",
                        gs_app_get_name(app));
                gs_app_set_state(app, GS_APP_STATE_AVAILABLE);
                return FALSE;
            }

            gboolean container_installed = FALSE;
            for (guint i = 0; i < g_strv_length(splits); i++) {
                if (g_strcmp0(splits[i], app_container_name) == 0) {
                    // Container is installed, nothing to do for now
                    g_debug("Container %s already initialized", app_container_name);
                    container_installed = TRUE;
                    break;
                }
            }

            // Initialize container
            if (!container_installed) {
                g_debug("Install: Running init for container %s", app_container_name);

                container_flag = apx_container_flag_from_name(app_container_name);

                const gchar *init_cmd = g_strdup_printf("apx %s init", container_flag);
                gs_vanilla_meta_run_subprocess(init_cmd, G_SUBPROCESS_FLAGS_STDOUT_SILENCE,
                                               cancellable, error);
            }
        }
    }

    // Install package and process output
    if (container_flag == NULL)
        container_flag = apx_container_flag_from_name(app_container_name);

    package_name = gs_app_get_source_default(app);
    if (package_name == NULL) {
        g_debug("Install: Package name for %s is null, can't install", gs_app_get_name(app));
        gs_app_set_state(app, GS_APP_STATE_AVAILABLE);
        return FALSE;
    }

    g_debug("Installing app %s, using container flag `%s` and package name `%s`",
            gs_app_get_name(app), container_flag, package_name);

    g_clear_object(&input_stream);
    const gchar *install_cmd = g_strdup_printf("apx %s install %s", container_flag, package_name);

    input_stream = gs_vanilla_meta_run_subprocess(install_cmd, G_SUBPROCESS_FLAGS_STDOUT_SILENCE,
                                                  cancellable, error);
    if (input_stream != NULL) {
        gs_app_set_state(app, GS_APP_STATE_INSTALLED);
        return TRUE;
    } else {
        gs_app_set_state(app, GS_APP_STATE_AVAILABLE);
        return FALSE;
    }
}

gboolean
gs_plugin_update_app(GsPlugin *plugin, GsApp *app, GCancellable *cancellable, GError **error)
{
    return TRUE;
}

gboolean
gs_plugin_app_upgrade_trigger(GsPlugin *plugin,
                              GsApp *app,
                              GCancellable *cancellable,
                              GError **error)
{
    return TRUE;
}

gboolean
gs_plugin_app_remove(GsPlugin *plugin, GsApp *app, GCancellable *cancellable, GError **error)
{
    return FALSE;
}

gboolean
gs_plugin_download_app(GsPlugin *plugin, GsApp *app, GCancellable *cancellable, GError **error)
{
    return FALSE;
}

gboolean
gs_plugin_download(GsPlugin *plugin, GsAppList *apps, GCancellable *cancellable, GError **error)
{
    return FALSE;
}

gboolean
gs_plugin_update(GsPlugin *plugin, GsAppList *apps, GCancellable *cancellable, GError **error)
{
    return FALSE;
}

gboolean
gs_plugin_add_updates(GsPlugin *plugin, GsAppList *list, GCancellable *cancellable, GError **error)
{
    return FALSE;
}

gboolean
gs_plugin_add_updates_historical(GsPlugin *plugin,
                                 GsAppList *list,
                                 GCancellable *cancellable,
                                 GError **error)
{
    return TRUE;
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

        if (!gs_app_has_management_plugin(app, NULL))
            continue;

        g_debug("%s belongs to us", gs_app_get_id(app));

        if (gs_app_has_quirk(app, GS_APP_QUIRK_IS_WILDCARD)) {
            g_debug("App %s is wildcard. Skipping..", gs_app_get_id(app));
            continue;
        }

        /* gs_app_set_scope(app, AS_COMPONENT_SCOPE_USER); */
        /* gs_app_set_branch(app, "main"); */

        gs_app_set_origin(app, "vanilla_meta");

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

#if LIBXMLB_CHECK_VERSION(0, 3, 0)
static gboolean
gs_vanilla_meta_tokenize_cb(XbBuilderFixup *self,
                            XbBuilderNode *bn,
                            gpointer user_data,
                            GError **error)
{
    const gchar *const elements_to_tokenize[] = {"id",   "keyword", "launchable", "mimetype",
                                                 "name", "summary", NULL};
    if (xb_builder_node_get_element(bn) != NULL &&
        g_strv_contains(elements_to_tokenize, xb_builder_node_get_element(bn)))
        xb_builder_node_tokenize_text(bn);
    return TRUE;
}
#endif

static gboolean
gs_vanilla_meta_set_origin_cb(XbBuilderFixup *self,
                              XbBuilderNode *bn,
                              gpointer user_data,
                              GError **error)
{
    const char *remote_name = (char *)user_data;
    if (g_strcmp0(xb_builder_node_get_element(bn), "components") == 0) {
        xb_builder_node_set_attr(bn, "origin", remote_name);
    }
    return TRUE;
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
        return FALSE;
    }

    // Run fixups
#if LIBXMLB_CHECK_VERSION(0, 3, 0)
    g_debug("Adding fixup_tokenize");
    g_autoptr(XbBuilderFixup) fixup_tokenize = NULL;

    fixup_tokenize = xb_builder_fixup_new("TextTokenize", gs_vanilla_meta_tokenize_cb, NULL, NULL);
    xb_builder_fixup_set_max_depth(fixup_tokenize, 2);
    xb_builder_source_add_fixup(source, fixup_tokenize);
#endif

    g_debug("Adding fixup_origin");
    g_autoptr(XbBuilderFixup) fixup_origin = NULL;

    fixup_origin = xb_builder_fixup_new("SetOrigin", gs_vanilla_meta_set_origin_cb,
                                        g_strdup("vanilla_meta"), g_free);
    xb_builder_fixup_set_max_depth(fixup_origin, 1);
    xb_builder_source_add_fixup(source, fixup_origin);

    // Import source to builder
    xb_builder_import_source(builder, source);

    // Save to silo
    self->silo = xb_builder_ensure(builder, silo_file,
                                   XB_BUILDER_COMPILE_FLAG_IGNORE_INVALID |
                                       XB_BUILDER_COMPILE_FLAG_SINGLE_LANG,
                                   cancellable, error);

    if (self->silo == NULL) {
        g_debug("Failed to create silo: %s", (*error)->message);
        return FALSE;
    }

    return TRUE;
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
    GsApp *alternate_of          = NULL;

    /* g_autoptr(GsAppList) list     = gs_app_list_new(); */
    g_autoptr(GsAppList) list_tmp = gs_app_list_new();
    g_autoptr(GError) error       = NULL;

    if (data->query != NULL) {
        keywords     = gs_app_query_get_keywords(data->query);
        alternate_of = gs_app_query_get_alternate_of(data->query);
    }

    // TODO: Move this and other appstream-related queries to a separate function
    if (keywords != NULL) {
        if (!gs_appstream_search(GS_PLUGIN(self), self->silo, keywords, list_tmp, cancellable,
                                 &error)) {
            g_debug("Error while searching: %s", error->message);
            g_task_return_error(task, g_steal_pointer(&error));
            return;
        }

        if (alternate_of != NULL &&
            !gs_appstream_add_alternates(self->silo, alternate_of, list_tmp, cancellable, &error)) {
            g_debug("Error while fetching alternates: %s", error->message);
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
    /**/
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
        /* if (!gs_app_has_management_plugin(app, GS_PLUGIN(self))) */
        /*     continue; */

        const gchar *source           = gs_app_get_source_default(app);
        g_autofree gchar *source_safe = NULL;
        g_autofree gchar *xpath       = NULL;
        g_autoptr(XbNode) component   = NULL;
        g_autoptr(GError) error_local = NULL;
        const gchar *container_name   = NULL;
        XbNodeChildIter iter;
        g_autoptr(XbNode) child = NULL;

        /* find using source and origin */
        source_safe = xb_string_escape(source);
        xpath       = g_strdup_printf("components[@origin='vanilla_meta']/component/"
                                            "bundle[@container][text()='%s']/..",
                                      source_safe);
        component   = xb_silo_query_first(self->silo, xpath, &error_local);

        if (component == NULL) {
            g_debug("no match for %s: %s", xpath, error_local->message);
            g_clear_error(&error_local);

            g_task_return_boolean(task, TRUE);
            return;
        }

        gs_appstream_refine_app(GS_PLUGIN(self), app, self->silo, component, data->flags,
                                &error_local);

        // Iterate node's children until we find container name
        xb_node_child_iter_init(&iter, component);
        while (xb_node_child_iter_next(&iter, &child)) {
            container_name = xb_node_get_attr(child, "container");
            if (container_name != NULL)
                break;
        }

        gs_app_set_metadata(app, "Vanilla::container", container_name);
        g_debug("Adding container %s to app %s", container_name, gs_app_get_name(app));

        /* gs_app_set_origin(app, "vanilla_meta"); */
        /* gs_app_set_origin_appstream(app, "vanilla_meta"); */
        /* gs_app_set_origin_ui(app, "VanillaOS Meta"); */

        gs_vanilla_meta_app_set_packaging_info(app);
        g_debug("Refined %s", gs_app_get_id(app));
        g_debug("%s", gs_app_get_origin_appstream(app));
        /* g_debug("%s", gs_app_to_string(app)); */
    }

    g_task_return_boolean(task, TRUE);
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

    object_class->dispose  = gs_plugin_vanilla_meta_dispose;
    object_class->finalize = gs_plugin_vanilla_meta_finalize;

    plugin_class->setup_async               = gs_plugin_vanilla_meta_setup_async;
    plugin_class->setup_finish              = gs_plugin_vanilla_meta_setup_finish;
    plugin_class->enable_repository_async   = gs_plugin_vanilla_meta_enable_repository_async;
    plugin_class->enable_repository_finish  = gs_plugin_vanilla_meta_enable_repository_finish;
    plugin_class->disable_repository_async  = gs_plugin_vanilla_meta_disable_repository_async;
    plugin_class->disable_repository_finish = gs_plugin_vanilla_meta_disable_repository_finish;
    plugin_class->refresh_metadata_async    = gs_plugin_vanilla_meta_refresh_metadata_async;
    plugin_class->refresh_metadata_finish   = gs_plugin_vanilla_meta_refresh_metadata_finish;
    plugin_class->list_apps_async           = gs_plugin_vanilla_meta_list_apps_async;
    plugin_class->list_apps_finish          = gs_plugin_list_apps_finish;
    plugin_class->refine_async              = gs_plugin_vanilla_meta_refine_async;
    plugin_class->refine_finish             = gs_plugin_vanilla_meta_refine_finish;
}

GType
gs_plugin_query_type(void)
{
    return GS_TYPE_PLUGIN_VANILLA_META;
}

/*
 * Copyright (C) 2023 Mateus Melchiades
 */

#include <glib.h>
#include <gnome-software.h>
#include <xmlb.h>

#include "gs-plugin-vanilla-meta.h"

static void progress_cb(gsize bytes_downloaded, gsize total_download_size, gpointer user_data);
static void download_file_cb(GObject *source_object, GAsyncResult *result, gpointer user_data);
static gboolean add_apps_from_metadata_file(GsPluginVanillaMeta *self,
                                            GFile *metadata_file,
                                            GCancellable *cancellable,
                                            GError **error);

const gchar *gz_metadata_filename   = ".cache/vanilla_meta/metadata.xml.gz";
const gchar *metadata_filename      = ".cache/vanilla_meta/metadata.xml";
const gchar *metadata_silo_filename = ".cache/vanilla_meta/metadata.xmlb";
const gchar *metadata_url           = "";

struct _GsPluginVanillaMeta {
    GsPlugin parent;
    XbSilo *silo;
};

G_DEFINE_TYPE(GsPluginVanillaMeta, gs_plugin_vanilla_meta, GS_TYPE_PLUGIN)

static void
gs_plugin_vanilla_meta_init(GsPluginVanillaMeta *self)
{
    GsPlugin *plugin = GS_PLUGIN(self);

    gs_plugin_add_rule(plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");
}

void
gs_plugin_adopt_app(GsPlugin *plugin, GsApp *app)
{
    if (gs_app_get_metadata_item(app, "Vanilla::apx_container") != NULL) {
        g_debug("I should adopt app %s", gs_app_get_name(app));
        gs_app_set_management_plugin(app, plugin);
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
    g_autoptr(GFile) gz_file            = g_file_new_for_path(gz_metadata_filename);
    g_autoptr(GTask) task               = NULL;
    g_autoptr(SoupSession) soup_session = NULL;
    g_autoptr(GError) error             = NULL;

    task = g_task_new(plugin, cancellable, callback, user_data);
    g_task_set_source_tag(task, gs_plugin_vanilla_meta_refresh_metadata_async);

    soup_session = gs_build_soup_session();

    // Is the metadata missing or too old?
    if (gs_utils_get_file_age(gz_file) >= cache_age_secs) {
        g_debug("I should refresh metadata");

        gs_download_file_async(soup_session, metadata_url, gz_file, G_PRIORITY_LOW, progress_cb,
                               plugin, cancellable, download_file_cb, g_steal_pointer(&task));

        return;
    } else {
        g_debug("Cache is only %zu seconds old, https packets aren't free, ya know?",
                gs_utils_get_file_age(gz_file));

        add_apps_from_metadata_file(GS_PLUGIN_VANILLA_META(plugin), gz_file, cancellable, &error);
    }

    g_task_return_boolean(task, TRUE);
}

static void
progress_cb(gsize bytes_downloaded, gsize total_download_size, gpointer user_data)
{
    g_debug("Downloaded %zu of %zu bytes", bytes_downloaded, total_download_size);
}

static void
download_file_cb(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task         = g_steal_pointer(&user_data);
    GsPluginVanillaMeta *self     = GS_PLUGIN_VANILLA_META(g_task_get_source_object(task));
    g_autoptr(GError) local_error = NULL;
    SoupSession *soup_session     = SOUP_SESSION(source_object);
    GCancellable *cancellable     = g_task_get_cancellable(task);

    g_autoptr(GFile) gz_file                        = g_file_new_for_path(gz_metadata_filename);
    g_autoptr(GFile) xml_file                       = NULL;
    g_autoptr(GZlibDecompressor) decompressor       = NULL;
    g_autoptr(GError) error                         = NULL;
    g_autoptr(GFileInputStream) gz_file_inputstream = NULL;
    g_autoptr(GFileIOStream) xml_file_iostream      = NULL;
    g_autoptr(GOutputStream) xml_outputstream       = NULL;
    g_autoptr(GInputStream) stream_data             = NULL;

    if (!gs_download_file_finish(soup_session, result, &local_error)) {
        g_debug("Error while downloading metadata: %s", local_error->message);
        g_task_return_error(task, g_steal_pointer(&local_error));
    } else {
        g_debug("Successfully downloaded new metadata");

        // NOTE: Maybe we won't even need to do this, as xmlb can load from a compressed xml
        // Such hard work, FOR NOTHING!!!
        g_debug("Uncompressing metadata");
        // Uncompress gzip into xml metadata
        decompressor        = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP);
        gz_file_inputstream = g_file_read(gz_file, cancellable, &error);
        stream_data         = g_converter_input_stream_new(G_INPUT_STREAM(gz_file_inputstream),
                                                           G_CONVERTER(decompressor));

        // Delete old xml file if it exists
        if (g_file_query_exists(xml_file, cancellable)) {
            g_debug("Deleting old metadata file");
            g_file_delete(xml_file, cancellable, &error);
        }

        // Create new file to store uncompressed data
        g_debug("Creating new metadata file");
        xml_file = g_file_new_for_path(metadata_filename);
        xml_file_iostream =
            g_file_create_readwrite(xml_file, G_FILE_CREATE_NONE, cancellable, &error);
        xml_outputstream = g_io_stream_get_output_stream(G_IO_STREAM(xml_file_iostream));

        // Read from gz and write to xml
        gssize read_count   = 1;
        guint8 buffer[1024] = {0};

        g_debug("Writing metadata");
        while (read_count) {
            read_count = g_input_stream_read(stream_data, buffer, 1024, cancellable, &error);
            g_output_stream_write(xml_outputstream, buffer, 1024, cancellable, &error);
        }

        // Delete gz file
        /* g_file_delete(gz_file, cancellable, &error); */

        // Build silo
        add_apps_from_metadata_file(self, gz_file, cancellable, &error);

        g_task_return_boolean(task, TRUE);
    }
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

/* static void */
/* gs_plugin_vanilla_meta_list_apps_async(GsPlugin *plugin, */
/*                                        GsAppQuery *query, */
/*                                        GsPluginListAppsFlags flags, */
/*                                        GCancellable *cancellable, */
/*                                        GAsyncReadyCallback callback, */
/*                                        gpointer user_data) */
/* { */
/**/
/* } */
/**/
/* static GsAppList * */
/* gs_plugin_list_apps_finish(GsPlugin *plugin, GAsyncResult *result, GError **error) */
/* { */
/*     return g_task_propagate_pointer(G_TASK(result), error); */
/* } */

static void
gs_plugin_vanilla_meta_class_init(GsPluginVanillaMetaClass *klass)
{
    GsPluginClass *plugin_class = GS_PLUGIN_CLASS(klass);

    plugin_class->refresh_metadata_async  = gs_plugin_vanilla_meta_refresh_metadata_async;
    plugin_class->refresh_metadata_finish = gs_plugin_vanilla_meta_refresh_metadata_finish;
    /* plugin_class->list_apps_async         = gs_plugin_vanilla_meta_list_apps_async; */
    /* plugin_class->list_apps_finish        = gs_plugin_list_apps_finish; */
}

GType
gs_plugin_query_type(void)
{
    return GS_TYPE_PLUGIN_VANILLA_META;
}

/*
 * Copyright (C) 2023 Mateus Melchiades
 */

#include <glib.h>
#include <gnome-software.h>

#include "gs-plugin-vanilla-meta.h"

static void progress_cb(gsize bytes_downloaded, gsize total_download_size, gpointer user_data);
static void download_file_cb(GObject *source_object, GAsyncResult *result, gpointer user_data);

const gchar *gz_metadata_filename = ".cache/vanilla_meta/metadata.xml.gz";
const gchar *metadata_filename    = ".cache/vanilla_meta/metadata.xml";
const gchar *metadata_url         = "";

struct _GsPluginVanillaMeta {
    GsPlugin parent;

    /* private data here */
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

        g_debug("Uncompressing metadata");
        // Uncompress gzip into xml metadata
        decompressor        = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP);
        gz_file_inputstream = g_file_read(gz_file, cancellable, &error);
        stream_data         = g_converter_input_stream_new(G_INPUT_STREAM(gz_file_inputstream),
                                                           G_CONVERTER(decompressor));

        g_debug("Creating new metadata file");
        // Create new file to store uncompressed data
        xml_file = g_file_new_for_path(metadata_filename);
        xml_file_iostream =
            g_file_create_readwrite(xml_file, G_FILE_CREATE_NONE, cancellable, &error);
        xml_outputstream = g_io_stream_get_output_stream(G_IO_STREAM(xml_file_iostream));

        // Read from gz and write to xml
        gssize read_count  = 1;
        guint8 buffer[100] = {};

        g_debug("Writing metadata");
        while (read_count) {
            read_count = g_input_stream_read(G_INPUT_STREAM(gz_file_inputstream), buffer, 100,
                                             cancellable, &error);
            g_output_stream_write(xml_outputstream, buffer, 100, cancellable, &error);
        }

        // Delete gz file
        g_file_delete(gz_file, cancellable, &error);

        g_task_return_boolean(task, TRUE);
    }
}

static gboolean
gs_plugin_vanilla_meta_refresh_metadata_finish(GsPlugin *plugin,
                                               GAsyncResult *result,
                                               GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

static void
gs_plugin_vanilla_meta_class_init(GsPluginVanillaMetaClass *klass)
{
    GsPluginClass *plugin_class = GS_PLUGIN_CLASS(klass);

    plugin_class->refresh_metadata_async  = gs_plugin_vanilla_meta_refresh_metadata_async;
    plugin_class->refresh_metadata_finish = gs_plugin_vanilla_meta_refresh_metadata_finish;
}

GType
gs_plugin_query_type(void)
{
    return GS_TYPE_PLUGIN_VANILLA_META;
}

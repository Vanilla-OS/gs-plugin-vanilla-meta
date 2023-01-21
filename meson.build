project('gs_plugin_vanilla_meta', 'c')

files = [
  'gs-plugin-vanilla-meta.c',
  'gs-vanilla-meta-util.c'
]

deps = [
  dependency('glib-2.0', version : '>= 2.70.0'),
  dependency('gnome-software'),
  dependency('xmlb', version: '>= 0.1.7', fallback: ['libxmlb', 'libxmlb_dep']),
  dependency('polkit-gobject-1')
]

conf = configuration_data()
conf.set_quoted('GETTEXT_PACKAGE', 'gnome-software')
conf.set('HAVE_POLKIT', 1)
configure_file(
  output : 'config.h',
  configuration : conf
)

args = [
  '-fPIC',
  '-DI_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE',
  '-DG_LOG_DOMAIN="GsPluginVanillaMeta"'
]

shared_module(
  'gs_plugin_vanilla_meta',
  files,
  dependencies: deps,
  c_args: args
)

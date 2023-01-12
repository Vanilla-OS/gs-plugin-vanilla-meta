project('gs_plugin_vanilla_meta', 'c')

files = [
  'gs-plugin-vanilla-meta.c'
]

deps = [
  dependency('glib-2.0', version : '>= 2.70.0'),
  dependency('gnome-software')
]

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

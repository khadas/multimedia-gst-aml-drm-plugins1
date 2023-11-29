gstpbutils_dep = dependency('gstreamer-pbutils-1.0', version : '>=1.16', required : true)

gstamlparse_sources = [
  'plugin.c',
  'gst_aml_audio_parse.c'
  ]

gstamlparse = library('gstamlparse',
  gstamlparse_sources,
  c_args: plugin_base_args,
  dependencies : [gst_dep, gstbase_dep, gstpbutils_dep],
  include_directories: include_dir,
  install : true,
  install_dir : plugins_install_dir,
)
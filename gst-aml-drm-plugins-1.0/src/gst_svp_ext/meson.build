pthread = dependency('threads', required: true)
gstamlsvpext_sources = [
  'gst_svp_meta.cpp',
  'gst_svp_logging.cpp',
  'gst_svp_scopedlock.cpp',
  'amlogic_svp_impl.cpp',
  'gst_svp_performance.cpp'
]

gstamlsvpext = library('gstamlsvpext',
  gstamlsvpext_sources,
  cpp_args: plugin_base_args,
  dependencies : [gst_dep, gstbase_dep, pthread],
  link_with : gstsecmemallocator,
  include_directories: include_dir,
  install : true,
  version: '0.0.0',
  soversion : '0'
)

gstamlsvppayload = library('gstamlsvppayload',
  'plugins/gst_svp_payload.cpp',
  cpp_args: plugin_base_args,
  dependencies : [gst_dep, gstbase_dep],
  link_with : [gstsecmemallocator, gstamlsvpext],
  include_directories: include_dir,
  install : true,
  install_dir : plugins_install_dir
)
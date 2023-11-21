gstdummydrm = library('gstdummydrm',
  'gstdummydrm.c',
  c_args: plugin_base_args,
  dependencies : [gst_dep, gstbase_dep],
  link_with : gstsecmemallocator,
  include_directories: include_dir,
  install : true,
  install_dir : plugins_install_dir
)
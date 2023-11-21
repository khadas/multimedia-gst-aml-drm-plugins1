gstcodecparsers_dep = dependency('gstreamer-codecparsers-1.0', version : '>=1.16', required : true)
gstvideo_dep = dependency('gstreamer-video-1.0', version : '>=1.16', required : true)
gstpb_dep = dependency('gstreamer-pbutils-1.0', version : '>=1.16', required : true)

gstamlsecparse_sources = [
  'gst_aml_sec_parse.c',
  'gstav1_sec_trans.c',
  'gsth264_sec_parse.c',
  'gsth265_sec_parse.c',
  'gstvp9_sec_trans.c'
]

gstamlsecparse = library('gstamlsecparse',
  gstamlsecparse_sources,
  c_args: plugin_base_args,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, gstcodecparsers_dep, gstpb_dep],
  link_with : gstsecmemallocator,
  include_directories: include_dir,
  install : true,
  install_dir : plugins_install_dir
)
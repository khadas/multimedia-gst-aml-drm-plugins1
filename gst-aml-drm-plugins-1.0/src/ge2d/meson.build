libge2d = cc.find_library('ge2d', required: false)
if libge2d.found()
  gstdrmbufferpool = dependency('gstdrmbufferpool', required:true)
  gstvideo_dep = dependency('gstreamer-video-1.0', version : '>=1.16', required : true)
  gstallocators_dep = dependency('gstreamer-allocators-1.0', version : '>=1.16', required : true)
  gstge2d_sources = [
    'gst_ge2d_flip.c',
    'gst_ge2d.c'
  ]
  gstge2d = library('gstge2d',
    gstge2d_sources,
    c_args: plugin_base_args,
    dependencies : [gst_dep, gstbase_dep, gstvideo_dep, gstallocators_dep, gstdrmbufferpool, libge2d],
    link_with : gstsecmemallocator,
    include_directories: include_dir,
    install : true,
    install_dir : plugins_install_dir
  )
endif
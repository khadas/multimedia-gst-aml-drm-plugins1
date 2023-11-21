libwfd_hdcp = dependency('libwfd_hdcp', required: false)

if libwfd_hdcp.found()
    gstamlhdcp = library('gstamlhdcp',
    'gstamlhdcp.c',
    c_args: plugin_base_args,
    dependencies : [gst_dep, gstbase_dep, libwfd_hdcp],
    link_with : gstsecmemallocator,
    include_directories: include_dir,
    install : true,
    install_dir : plugins_install_dir
    )
endif
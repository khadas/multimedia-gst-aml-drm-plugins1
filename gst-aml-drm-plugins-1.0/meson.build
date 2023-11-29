project('gst-aml-drm-plugins', ['c', 'cpp'], version : '1.0.0')

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

cc = meson.get_compiler('c')

gst_version = meson.project_version()

api_version = '1.0'

gst_dep = dependency('gstreamer-1.0', version : '>=1.16', required : true)
gstbase_dep = dependency('gstreamer-base-1.0', version : '>=1.16', required : true)

plugin_base_args = ['-DHAVE_CONFIG_H']

cdata = configuration_data()
cdata.set_quoted('PACKAGE_VERSION', gst_version)
cdata.set_quoted('PACKAGE', meson.project_name())
cdata.set_quoted('GST_LICENSE', 'LGPL')
cdata.set_quoted('GST_API_VERSION', api_version)
cdata.set_quoted('GST_PACKAGE_NAME', 'Amlogic GStreamer DRM Plug-ins')
cdata.set_quoted('GST_PACKAGE_ORIGIN', 'https://www.amlogic.com')
configure_file(output : 'config.h', configuration : cdata)

include_dir = [
    include_directories('.'),
    include_directories('src/secmem')
]

subdir('src/secmem')
subdir('src/secure_parse')
subdir('src/hdcp')
subdir('src/dummy')
subdir('src/gst_svp_ext')
subdir('src/ge2d')
subdir('src/aml_parse')
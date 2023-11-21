pkgconfig = import('pkgconfig')
libsecmem = dependency('libsecmem', required: true)
gstallocators_dep = dependency('gstreamer-allocators-1.0', version : '>=1.16', required : true)

gstsecmemallocator_dependencies = [gst_dep, gstbase_dep, gstallocators_dep, libsecmem]


gstsecmemallocator = library('gstsecmemallocator',
  'gstsecmemallocator.c',
  dependencies : gstsecmemallocator_dependencies,
  install : true,
  version: '0.0.0',
  soversion : '0'
)

install_headers('gstsecmemallocator.h', subdir: 'gstreamer-1.0/gst/allocators')

pkgconfig.generate(gstsecmemallocator,
  filebase: 'gstsecmemallocator',
  libraries : gstsecmemallocator_dependencies,
  subdirs: ['gstreamer-1.0']
)

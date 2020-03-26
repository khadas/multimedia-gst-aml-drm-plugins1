# Note: plugindir is set in configure

##############################################################################
# TODO: change libgstplugin.la to something else, e.g. libmysomething.la     #
##############################################################################
plugin_LTLIBRARIES = libgsth264secparse.la

##############################################################################
# TODO: for the next set of variables, name the prefix if you named the .la, #
#  e.g. libmysomething.la => libmysomething_la_SOURCES                       #
#                            libmysomething_la_CFLAGS                        #
#                            libmysomething_la_LIBADD                        #
#                            libmysomething_la_LDFLAGS                       #
##############################################################################

# sources used to compile this plug-in
libgsth264secparse_la_SOURCES = gsth264_sec_parse.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgsth264secparse_la_CFLAGS = $(GST_CFLAGS) -I$(top_srcdir)/include -I$(top_srcdir)/src/secmem
libgsth264secparse_la_LIBADD = $(GST_LIBS) $(top_builddir)/src/secmem/libgstsecmemallocator.la -L$(TARGET_DIR)/usr/lib/gstreamer-1.0 -lgstvideoparsersbad
libgsth264secparse_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgsth264secparse_la_LIBTOOLFLAGS = --tag=disable-static

include_HEADERS = gsth264_sec_parse.h

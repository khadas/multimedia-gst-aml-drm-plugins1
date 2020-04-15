# Note: plugindir is set in configure

##############################################################################
# TODO: change libgstplugin.la to something else, e.g. libmysomething.la     #
##############################################################################
plugin_LTLIBRARIES = libgstamlsecparse.la

##############################################################################
# TODO: for the next set of variables, name the prefix if you named the .la, #
#  e.g. libmysomething.la => libmysomething_la_SOURCES                       #
#                            libmysomething_la_CFLAGS                        #
#                            libmysomething_la_LIBADD                        #
#                            libmysomething_la_LDFLAGS                       #
##############################################################################

# sources used to compile this plug-in
libgstamlsecparse_la_SOURCES = gst_aml_sec_parse.c gsth264_sec_parse.c gstvp9_sec_trans.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstamlsecparse_la_CFLAGS = $(GST_CFLAGS) -I$(top_srcdir)/include -I$(top_srcdir)/src/secmem
libgstamlsecparse_la_LIBADD = $(GST_LIBS) $(top_builddir)/src/secmem/libgstsecmemallocator.la -L$(TARGET_DIR)/usr/lib/gstreamer-1.0 -lgstvideoparsersbad
libgstamlsecparse_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstamlsecparse_la_LIBTOOLFLAGS = --tag=disable-static

include_HEADERS = gsth264_sec_parse.h  gstvp9_sec_trans.h

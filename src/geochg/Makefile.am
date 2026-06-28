## Process this file with automake to produce Makefile.in
PFLAGS = -module -avoid-version -no-undefined

libgeochg_la_LDFLAGS = $(PFLAGS)
libgeochg_la_LIBADD  = @COMPIZ_LIBS@ -lm
nodist_libgeochg_la_SOURCES = geochg_options.c geochg_options.h
dist_libgeochg_la_SOURCES   = geochg.c

BUILT_SOURCES = $(nodist_libgeochg_la_SOURCES)

AM_CPPFLAGS =                                    \
	-I$(top_srcdir)/include                  \
	@COMPIZ_CFLAGS@                          \
	-DDATADIR='"$(compdatadir)"'             \
	-DLIBDIR='"$(libdir)"'                   \
	-DLOCALEDIR="\"@datadir@/locale\""

moduledir = $(plugindir)

module_LTLIBRARIES = libgeochg.la

CLEANFILES = *_options.c *_options.h

geochg_options.h: ../../metadata/geochg.xml.in
		$(BCOP_BIN) --header $@ $<

geochg_options.c: ../../metadata/geochg.xml.in geochg_options.h
		$(BCOP_BIN) --source $@ $<

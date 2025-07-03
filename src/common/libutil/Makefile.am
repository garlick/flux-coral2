AM_CFLAGS = \
	$(WARNING_CFLAGS) \
	$(CODE_COVERAGE_CFLAGS)

AM_LDFLAGS = \
	$(CODE_COVERAGE_LIBS)

AM_CPPFLAGS = \
	$(CODE_COVERAGE_CPPFLAGS) \
	-I$(top_srcdir)/src/common/libccan

noinst_LTLIBRARIES = libutil.la

libutil_la_SOURCES = \
	errprintf.h \
	errprintf.c \
	eventlog.h \
	eventlog.c \
	idf58.h \
	fsd.h \
	fsd.c \
	monotime.h \
	monotime.c

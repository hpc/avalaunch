ACLOCAL_AMFLAGS = -I m4

SUBDIRS = hostfile .
noinst_HEADERS = event_handler.h list.h node.h pollfds.h print_errmsg.h readlibs.h session.h timer_util.h
include_HEADERS = pmi.h ring.h
bin_PROGRAMS = avalaunch
lib_LTLIBRARIES = libpmi.la

libpmi_la_SOURCES = \
  mpir.c \
  ring.c ring.h \
  pmi.c pmi.h
libpmi_la_LDFLAGS = -lpthread -lrt

avalaunch_SOURCES = \
  main.c \
  node.c \
  print_errmsg.c print_errmsg.h \
  event_handler.c event_handler.h \
  pollfds.c pollfds.h \
  readlibs.c readlibs.h \
  session.c session.h \
  timer_util.c timer_util.h
avalaunch_CFLAGS  = -pthread -Wall -g
avalaunch_LDADD   = hostfile/libhostfile.a
avalaunch_LDFLAGS = -lpthread -lrt

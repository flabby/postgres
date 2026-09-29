MODULE_big = test_memory_protector
OBJS = $(WIN32RES) test_memory_protector.o
PGFILEDESC = "test_memory_protector - memory pressure policy tests"
EXTENSION = test_memory_protector
DATA = test_memory_protector--1.0.sql
REGRESS = test_memory_protector
REGRESS_OPTS = --temp-config=$(srcdir)/observe.conf
ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_memory_protector
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# This test ABI includes the experimental backend identity structures.
ifndef USE_PGXS
test_memory_protector.o: $(top_srcdir)/src/include/postmaster/memory_protector.h
endif

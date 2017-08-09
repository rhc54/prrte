#
# Copyright (c) 2014-2017 Intel, Inc. All rights reserved.
# Copyright (c) 2015      Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2015-2016 Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

EXTRA_DIST = autogen.subdirs

SUBDIRS = pmix

sources = \
        pmix1x.h \
        pmix1x_component.c \
        pmix1x.c \
        pmix1x_client.c \
        pmix1x_server_south.c \
        pmix1x_server_north.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_opal_pmix_pmix12x_DSO
component_noinst =
component_install = mca_pmix_pmix12x.la
else
component_noinst = libmca_pmix_pmix12x.la
component_install =
endif

mcacomponentdir = $(opallibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_pmix_pmix12x_la_SOURCES = $(sources)
mca_pmix_pmix12x_la_CFLAGS = $(opal_pmix_pmix12x_CFLAGS)
mca_pmix_pmix12x_la_CPPFLAGS = \
        -I$(srcdir)/pmix/include $(opal_pmix_pmix12x_CPPFLAGS)
mca_pmix_pmix12x_la_LDFLAGS = -module -avoid-version $(opal_pmix_pmix12x_LDFLAGS)
mca_pmix_pmix12x_la_LIBADD = $(opal_pmix_pmix12x_LIBS)
mca_pmix_pmix12x_la_DEPENDENCIES = $(opal_pmix_pmix12x_LIBS)

noinst_LTLIBRARIES = $(component_noinst)
libmca_pmix_pmix12x_la_SOURCES =$(sources)
libmca_pmix_pmix12x_la_CFLAGS = $(opal_pmix_pmix12x_CFLAGS)
libmca_pmix_pmix12x_la_CPPFLAGS = -I$(srcdir)/pmix/include $(opal_pmix_pmix12x_CPPFLAGS)
libmca_pmix_pmix12x_la_LDFLAGS = -module -avoid-version $(opal_pmix_pmix12x_LDFLAGS)
libmca_pmix_pmix12x_la_LIBADD = $(opal_pmix_pmix12x_LIBS)
libmca_pmix_pmix12x_la_DEPENDENCIES = $(opal_pmix_pmix12x_LIBS)

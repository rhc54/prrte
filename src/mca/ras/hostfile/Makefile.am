#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

EXTRA_DIST = \
        help-ras-hostfile.txt

sources = \
        ras_hostfile.h \
        ras_hostfile_component.c \
        ras_hostfile_module.c


# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_ras_hostfile_DSO
lib =
lib_sources =
component = prte_mca_ras_hostfile.la
component_sources = $(sources)
else
lib = libprtemca_ras_hostfile.la
lib_sources = $(sources)
component =
component_sources =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component)
prte_mca_ras_hostfile_la_SOURCES = $(component_sources)
prte_mca_ras_hostfile_la_LDFLAGS = -module -avoid-version
prte_mca_ras_hostfile_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(lib)
libprtemca_ras_hostfile_la_SOURCES = $(lib_sources)
libprtemca_ras_hostfile_la_LDFLAGS = -module -avoid-version

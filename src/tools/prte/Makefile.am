#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2009 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
# Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

include $(top_srcdir)/Makefile.prte-rules

MD_FILES = \
	prte.1.md \
	prte-map.1.md
	prterun.1.md
man_pages_from_md = $(MD_FILES:.1.md=.1)
EXTRA_DIST = $(MD_FILES) $(man_pages_from_md)
AM_LDFLAGS = $(PRTE_EXTRA_LIB_LDFLAGS) $(prte_hwloc_LDFLAGS) $(prte_libevent_LDFLAGS) $(prte_pmix_LDFLAGS)
AM_CFLAGS = \
            -DPRTE_CONFIGURE_USER="\"@PRTE_CONFIGURE_USER@\"" \
            -DPRTE_CONFIGURE_HOST="\"@PRTE_CONFIGURE_HOST@\"" \
            -DPRTE_CONFIGURE_DATE="\"@PRTE_CONFIGURE_DATE@\"" \
            -DPRTE_BUILD_USER="\"$$USER\"" \
            -DPRTE_BUILD_HOST="\"$${HOSTNAME:-`(hostname || uname -n) | sed 1q`}\"" \
            -DPRTE_BUILD_DATE="\"`$(top_srcdir)/config/getdate.sh`\"" \
            -DPRTE_BUILD_CFLAGS="\"@CFLAGS@\"" \
            -DPRTE_BUILD_CPPFLAGS="\"@CPPFLAGS@\"" \
            -DPRTE_BUILD_LDFLAGS="\"@LDFLAGS@\"" \
            -DPRTE_BUILD_LIBS="\"@LIBS@\"" \
            -DPRTE_CC_ABSOLUTE="\"@PRTE_CC_ABSOLUTE@\"" \
            -DPRTE_GREEK_VERSION="\"@PRTE_GREEK_VERSION@\"" \
            -DPRTE_REPO_REV="\"@PRTE_REPO_REV@\"" \
            -DPRTE_RELEASE_DATE="\"@PRTE_RELEASE_DATE@\""

bin_PROGRAMS = prte

if PRTE_ENABLE_MAN_PAGES
man_MANS = $(man_pages_from_md)
endif

prte_SOURCES = \
        main.c \
        prte.c \
        prte.h

prte_LDADD = \
    $(PRTE_EXTRA_LTLIB) \
    $(prte_libevent_LIBS) \
    $(prte_hwloc_LIBS) \
    $(prte_pmix_LIBS) \
	$(top_builddir)/src/libprrte.la

install-exec-hook:
	(cd $(DESTDIR)$(bindir); rm -f prterun$(EXEEXT); $(LN_S) prte$(EXEEXT) prterun$(EXEEXT))
	(cd $(DESTDIR)$(mandir)/man1; rm -f prte-rank.1 ; $(LN_S) prte-map.1 prte-rank.1)
	(cd $(DESTDIR)$(mandir)/man1; rm -f prte-bind.1 ; $(LN_S) prte-map.1 prte-bind.1)

uninstall-local:
	rm -f $(DESTDIR)$(bindir)/prterun$(EXEEXT)

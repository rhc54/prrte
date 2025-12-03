/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/include/prte_socket_errno.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/util/pmix_net.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "ras_hostfile.h"
#include "src/mca/ras/base/ras_private.h"

/*
 * Local functions
 */
static int ras_hostfile_register(void);
static int ras_hostfile_open(void);
static int ras_hostfile_close(void);
static int prte_mca_ras_hostfile_component_query(pmix_mca_base_module_t **module, int *priority);

prte_mca_ras_hostfile_component_t prte_mca_ras_hostfile_component = {
    .super = {
        PRTE_RAS_BASE_VERSION_2_0_0,

        /* Component name and version */
        .pmix_mca_component_name = "hostfile",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PRTE_MAJOR_VERSION,
                                   PRTE_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_open_component = ras_hostfile_open,
        .pmix_mca_close_component = ras_hostfile_close,
        .pmix_mca_query_component = prte_mca_ras_hostfile_component_query,
        .pmix_mca_register_component_params = ras_hostfile_register
    }
};
PMIX_MCA_BASE_COMPONENT_INIT(prte, ras, hostfile)

static int ras_hostfile_register(void)
{
    pmix_mca_base_component_t *component = &prte_mca_ras_hostfile_component.super;

    prte_mca_ras_hostfile_component.max_length = 32000;
    (void) pmix_mca_base_component_var_register(component, "max_envar_length",
                                                "Maximum length of the HOSTFILE_NODELIST envar we should allow",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &prte_mca_ras_hostfile_component.max_length);

    prte_mca_ras_hostfile_component.use_all = false;
    (void) pmix_mca_base_component_var_register(component, "use_entire_allocation",
                                                "Use entire allocation (not just job step nodes) for this application",
                                                PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                                &prte_mca_ras_hostfile_component.use_all);

    return PRTE_SUCCESS;
}

static int ras_hostfile_open(void)
{
    return PRTE_SUCCESS;
}

static int ras_hostfile_close(void)
{
    return PRTE_SUCCESS;
}

static int prte_mca_ras_hostfile_component_query(pmix_mca_base_module_t **module, int *priority)
{
    PMIX_OUTPUT_VERBOSE((2, prte_ras_base_framework.framework_output,
                         "%s ras:hostfile: available for selection",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    *priority = 30;
    *module = (pmix_mca_base_module_t *) &prte_ras_hostfile_module;
    return PRTE_SUCCESS;
}

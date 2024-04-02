/* -*- C -*-
 *
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef GRPCOMM_DIRECT_H
#define GRPCOMM_DIRECT_H

#include "prte_config.h"

#include "src/mca/grpcomm/grpcomm.h"

BEGIN_C_DECLS

/*
 * Grpcomm interfaces
 */

PRTE_MODULE_EXPORT extern prte_grpcomm_base_component_t prte_mca_grpcomm_direct_component;
extern prte_grpcomm_base_module_t prte_grpcomm_direct_module;

PRTE_MODULE_EXPORT extern int prte_grpcomm_direct_grp_construct(prte_grpcomm_coll_t *coll,
                         										prte_pmix_grp_caddy_t *cd);

PRTE_MODULE_EXPORT extern void prte_grpcomm_direct_grp_recv(int status, pmix_proc_t *sender,
                                  							pmix_data_buffer_t *buffer,
                                  							prte_rml_tag_t tag, void *cbdata);

END_C_DECLS

#endif

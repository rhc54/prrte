/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/iof/iof.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"

#include "src/prted/pmix/pmix_server_internal.h"

static void relcb(void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t *) cbdata;

   if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(status);

   if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

void prte_pmix_group_release(int status, pmix_data_buffer_t *buf, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t *) cbdata;
    prte_pmix_mdx_caddy_t *cd2;
    int32_t cnt;
    pmix_status_t rc = PMIX_SUCCESS;
    bool assignedID = false, endptsgiven = false;
    bool used = false;
    size_t cid;
    pmix_proc_t *members = NULL, *finmembers = NULL;
    size_t num_members, nfinmembers;
    pmix_data_array_t darray;
    pmix_info_t info;
    pmix_data_buffer_t dbuf;
    pmix_byte_object_t bo, endpts = PMIX_BYTE_OBJECT_STATIC_INIT;
    pmix_data_array_t *grpinfo = NULL;
    int32_t byused;
    pmix_server_pset_t *pset;
    void *ilist;

    PMIX_ACQUIRE_OBJECT(cd);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s group request complete",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    if (PRTE_SUCCESS != status) {
        rc = prte_pmix_convert_rc(status);
        goto complete;
    }

    /* if this was a destruct operation, then there is nothing
     * further we need do */
    if (PMIX_GROUP_DESTRUCT == cd->op) {
        /* find this group ID on our list of groups */
        PMIX_LIST_FOREACH(pset, &prte_pmix_server_globals.groups, pmix_server_pset_t)
        {
            if (0 == strcmp(pset->name, cd->grpid)) {
                pmix_list_remove_item(&prte_pmix_server_globals.groups, &pset->super);
                PMIX_RELEASE(pset);
                break;
            }
        }
        rc = status;
        goto complete;
    }

    /* unpack the ctrls buf sent by the grpcomm allgather */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    PMIX_DATA_BUFFER_CONSTRUCT(&dbuf);
    PMIX_DATA_BUFFER_LOAD(&dbuf, bo.bytes, bo.size);

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &dbuf, &info, &cnt, PMIX_INFO);
    while (PMIX_SUCCESS == rc) {
        if (PMIX_CHECK_KEY(&info, PMIX_GROUP_CONTEXT_ID)) {
            PMIX_VALUE_GET_NUMBER(rc, &info.value, cid, size_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_DESTRUCT(&dbuf);
                goto complete;
            }
            assignedID = true;

        } else if (PMIX_CHECK_KEY(&info, PMIX_GROUP_ADD_MEMBERS)) {
            num_members = info.value.data.darray->size;
            PMIX_PROC_CREATE(members, num_members);
            memcpy(members, info.value.data.darray->array, num_members * sizeof(pmix_proc_t));

        } else if (PMIX_CHECK_KEY(&info, PMIX_GROUP_MEMBERSHIP)) {
            nfinmembers = info.value.data.darray->size;
            PMIX_PROC_CREATE(finmembers, nfinmembers);
            memcpy(finmembers, info.value.data.darray->array, nfinmembers * sizeof(pmix_proc_t));

        } else if (PMIX_CHECK_KEY(&info, PMIX_GROUP_ENDPT_DATA)) {
            endpts.bytes = info.value.data.bo.bytes;
            endpts.size = info.value.data.bo.size;
            // protect the data
            info.value.data.bo.bytes = NULL;
            info.value.data.bo.size = 0;

        } else if (PMIX_CHECK_KEY(&info, PMIX_GROUP_INFO_ARRAY)) {
            grpinfo = info.value.data.darray;
            // protect the data
            info.value.data.darray = NULL;
        }
        /* cleanup */
        PMIX_INFO_DESTRUCT(&info);
        /* get the next object */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &dbuf, &info, &cnt, PMIX_INFO);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&dbuf);

    /* the unpacking loop will have ended when the unpack either
     * went past the end of the buffer */
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    rc = PMIX_SUCCESS;

   /* add it to our list of known groups */
    pset = PMIX_NEW(pmix_server_pset_t);
    pset->name = strdup(cd->grpid);
    if (NULL != finmembers) {
        pset->num_members = nfinmembers;
        PMIX_PROC_CREATE(pset->members, pset->num_members);
        memcpy(pset->members, finmembers, nfinmembers * sizeof(pmix_proc_t));
    } else {
        pset->num_members = cd->nprocs;
        PMIX_PROC_CREATE(pset->members, pset->num_members);
        memcpy(pset->members, cd->procs, cd->nprocs * sizeof(pmix_proc_t));
    }
    pmix_list_append(&prte_pmix_server_globals.groups, &pset->super);

    if (NULL != members) {
        // still need to generate invite event for procs
        // that might be on nodes that were not involved
        // in the original collective

        PMIX_INFO_LIST_START(ilist);

        // provide the group ID since the invitee won't have it
        PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_ID, cd->grpid, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }

        // set the range to be only procs that were added
        darray.type = PMIX_PROC;
        darray.array = members;
        darray.size = num_members;
        // load the array - note: this copies the array!
        PMIX_INFO_LIST_ADD(rc, ilist, PMIX_EVENT_CUSTOM_RANGE, &darray, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }

        // mark that this event stays local and does not go up to the host
        PMIX_INFO_LIST_ADD(rc, ilist, PMIX_EVENT_STAYS_LOCAL, NULL, PMIX_BOOL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }

        // pass back the final group membership
        darray.type = PMIX_PROC;
        if (NULL != finmembers) {
            darray.array = finmembers;
            darray.size = nfinmembers;
        } else {
            darray.array = cd->procs;
            darray.size = cd->nprocs;
        }
        // load the array - note: this copies the array!
        PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }

        if (assignedID) {
            PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_CONTEXT_ID, &cid, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }

        // add the job-level info
        PMIX_DATA_BUFFER_CONSTRUCT(&dbuf);
        rc = PMIx_server_collect_job_info(finmembers, nfinmembers, &dbuf);
        if (PMIX_SUCCESS == rc) {
            PMIx_Data_buffer_unload(&dbuf, &bo.bytes, &bo.size);
            PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_JOB_INFO, &bo, PMIX_BYTE_OBJECT);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            PMIX_BYTE_OBJECT_DESTRUCT(&bo);
        }
        PMIX_DATA_BUFFER_DESTRUCT(&dbuf);

        if (0 < endpts.size) {
            PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_ENDPT_DATA, &endpts, PMIX_BYTE_OBJECT);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }

        if (NULL != grpinfo) {
            PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_INFO_ARRAY, grpinfo, PMIX_DATA_ARRAY);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }

        if (NULL != members) {
            darray.type = PMIX_PROC;
            darray.array = members;
            darray.size = num_members;
            PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_ADD_MEMBERS, &darray, PMIX_DATA_ARRAY);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }
        PMIX_INFO_LIST_CONVERT(rc, ilist, &darray);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        cd2 = PMIX_NEW(prte_pmix_mdx_caddy_t);
        cd2->info = (pmix_info_t*)darray.array;
        cd2->ninfo = darray.size;
        PMIX_INFO_LIST_RELEASE(ilist);

        // notify local procs
        PMIx_Notify_event(PMIX_GROUP_INVITED, &prte_process_info.myproc, PMIX_RANGE_CUSTOM,
                          cd2->info, cd2->ninfo, opcbfunc, cd2);
    }

    if (NULL != cd->infocbfunc) {
        // service the procs that are part of the collective

        PMIX_INFO_LIST_START(ilist);
        // pass back the final group membership
        darray.type = PMIX_PROC;
        if (NULL != finmembers) {
            darray.array = finmembers;
            darray.size = nfinmembers;
        } else {
            darray.array = cd->procs;
            darray.size = cd->nprocs;
        }
        // load the array - note: this copies the array!
        PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }

        if (assignedID) {
            PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_CONTEXT_ID, &cid, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }

        if (0 < endpts.size) {
            PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_ENDPT_DATA, &endpts, PMIX_BYTE_OBJECT);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }

        if (NULL != grpinfo) {
            PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_INFO_ARRAY, grpinfo, PMIX_DATA_ARRAY);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }

        if (NULL != members) {
            darray.type = PMIX_PROC;
            darray.array = members;
            darray.size = num_members;
            PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_ADD_MEMBERS, &darray, PMIX_DATA_ARRAY);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }
        PMIX_INFO_LIST_CONVERT(rc, ilist, &darray);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        cd->info = (pmix_info_t*)darray.array;
        cd->ninfo = darray.size;
        PMIX_INFO_LIST_RELEASE(ilist);

        /* return to the local procs in the collective */
        cd->infocbfunc(rc, cd->info, cd->ninfo, cd->cbdata, relcb, cd);
    }

complete:
    if (NULL != cd->procs) {
        PMIX_PROC_FREE(cd->procs, cd->nprocs);
    }
    if (NULL != finmembers) {
        PMIX_PROC_FREE(finmembers, nfinmembers);
    }
    if (NULL != members) {
        PMIX_PROC_FREE(members, num_members);
    }
    if (0 < endpts.size) {
        PMIX_BYTE_OBJECT_DESTRUCT(&endpts);
    }
    if (NULL != grpinfo) {
        PMIx_Data_array_free(grpinfo);
    }
    if (NULL == cd->infocbfunc) {
        PMIX_RELEASE(cd);
    }
}

static void local_complete(int sd, short args, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd = (prte_pmix_mdx_caddy_t*)cbdata;
    pmix_server_pset_t *pset;
    pmix_data_array_t *members;
    pmix_proc_t *p;
    void *ilist;
    pmix_info_t istat;
    pmix_status_t rc;
    size_t n;
    pmix_data_array_t darray;

    if (PMIX_GROUP_CONSTRUCT == cd->op) {

        PMIX_INFO_LIST_START(ilist);

        // construct the group membership
        members = PMIx_Data_array_create(cd->nprocs, PMIX_PROC);
        p = (pmix_proc_t*)members->array;
        memcpy(p, cd->procs, cd->nprocs * sizeof(pmix_proc_t));
        PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_MEMBERSHIP, members, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }

        PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_ID, cd->grpid, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }

        // check if they gave us any grp info
        for (n=0; n < cd->ninfo; n++) {
            if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_INFO_ARRAY)) {
                PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_INFO_ARRAY, cd->info[n].value.data.darray, PMIX_DATA_ARRAY);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                }
                break;
            }
        }

        /* add it to our list of known groups */
        pset = PMIX_NEW(pmix_server_pset_t);
        pset->name = strdup(cd->grpid);
        pset->num_members = cd->nprocs;
        PMIX_PROC_CREATE(pset->members, pset->num_members);
        memcpy(pset->members, cd->procs, cd->nprocs * sizeof(pmix_proc_t));
        pmix_list_append(&prte_pmix_server_globals.groups, &pset->super);

        // protect the procs array
        cd->procs = NULL;
        cd->nprocs = 0;

        // convert the info list - this will replace the incoming info
        // which belong to the PMIx server library
        PMIX_INFO_LIST_CONVERT(rc, ilist, &darray);
        cd->info = (pmix_info_t*)darray.array;
        cd->ninfo = darray.size;
        PMIX_INFO_LIST_RELEASE(ilist);

        // return this to them
        cd->infocbfunc(PMIX_SUCCESS, cd->info, cd->ninfo, cd->cbdata, relcb, cd);

    } else {
        /* find this group ID on our list of groups and remove it */
        PMIX_LIST_FOREACH(pset, &prte_pmix_server_globals.groups, pmix_server_pset_t)
        {
            if (0 == strcmp(pset->name, cd->grpid)) {
                pmix_list_remove_item(&prte_pmix_server_globals.groups, &pset->super);
                PMIX_RELEASE(pset);
                break;
            }
        }
        // return their callback
        cd->infocbfunc(PMIX_SUCCESS, NULL, 0, cd->cbdata, NULL, NULL);
        // protect the procs array
        cd->procs = NULL;
        cd->nprocs = 0;

        PMIX_RELEASE(cd);
    }
}

pmix_status_t pmix_server_group_fn(pmix_group_operation_t op, char *grpid,
                                   const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_mdx_caddy_t *cd;
    int rc;
    size_t i;
    bool assignID = false;
    pmix_server_pset_t *pset;
    bool fence = false;
    bool force_local = false;
    pmix_proc_t *members = NULL;
    pmix_proc_t *mbrs, *p;
    size_t num_members = 0;
    size_t nmembers;
    size_t bootstrap = 0;
    bool copied = false;
    struct timeval tv = {0, 0};

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s Group request recvd with %lu directives",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned long)ndirs);

    /* they are required to pass us an id */
    if (NULL == grpid) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* check the directives */
    for (i = 0; i < ndirs; i++) {
        /* see if they want a context id assigned */
        if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ASSIGN_CONTEXT_ID)) {
            assignID = PMIX_INFO_TRUE(&directives[i]);

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_EMBED_BARRIER)) {
            fence = PMIX_INFO_TRUE(&directives[i]);

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_TIMEOUT)) {
            tv.tv_sec = directives[i].value.data.uint32;

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_LOCAL_ONLY)) {
            force_local = PMIX_INFO_TRUE(&directives[i]);

#ifdef PMIX_GROUP_BOOTSTRAP
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_BOOTSTRAP)) {
            PMIX_VALUE_GET_NUMBER(rc, &directives[i].value, bootstrap, size_t);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
#endif

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ADD_MEMBERS)) {
            // there can be more than one entry here as this is the aggregate
            // of info keys from local procs that called group_construct
            if (NULL == members) {
                members = (pmix_proc_t*)directives[i].value.data.darray->array;
                num_members = directives[i].value.data.darray->size;
            } else {
                // need to aggregate these
                mbrs = (pmix_proc_t*)directives[i].value.data.darray->array;
                nmembers = directives[i].value.data.darray->size;
                // create a new array
                PMIX_PROC_CREATE(p, nmembers * num_members);
                // xfer data across
                memcpy(p, members, num_members * sizeof(pmix_proc_t));
                memcpy(&p[num_members], mbrs, nmembers * sizeof(pmix_proc_t));
                // release the old array - avoid releasing the one passed in to us
                if (copied) {
                    PMIX_PROC_FREE(members, num_members);
                }
                // complete the xfer
                members = p;
                num_members = num_members + nmembers;
                copied = true;
            }
        }
    }
    if (0 < tv.tv_sec) {
        if (copied) {
            PMIX_PROC_FREE(members, num_members);
        }
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* if they don't want us to do a fence and they don't want a
     * context id assigned and they aren't adding members, or they
     * insist on forcing local completion of the operation, then
     * we are done */
    if ((!fence && !assignID && NULL == members) || force_local) {
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s group request - purely local",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        if (force_local && assignID) {
            // we cannot do that
            if (copied) {
                PMIX_PROC_FREE(members, num_members);
            }
            return PMIX_ERR_BAD_PARAM;
        }
        cd = PMIX_NEW(prte_pmix_mdx_caddy_t);
        cd->op = op;
        cd->grpid = strdup(grpid);
        cd->procs = (pmix_proc_t*)procs;
        cd->nprocs = nprocs;
        cd->info = (pmix_info_t*)directives;
        cd->ninfo = ndirs;
        cd->infocbfunc = cbfunc;
        cd->cbdata = cbdata;
        PRTE_PMIX_THREADSHIFT(cd, prte_event_base, local_complete);
        if (copied) {
            PMIX_PROC_FREE(members, num_members);
        }
        return PMIX_SUCCESS;
    }

    cd = PMIX_NEW(prte_pmix_mdx_caddy_t);
    cd->op = op;
    cd->grpid = strdup(grpid);
    /* have to copy the procs in case we add members */
    PMIX_PROC_CREATE(cd->procs, nprocs);
    memcpy(cd->procs, procs, nprocs * sizeof(pmix_proc_t));
    cd->nprocs = nprocs;
    cd->grpcbfunc = prte_pmix_group_release;
    cd->infocbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* compute the signature of this collective */
    cd->sig = PMIX_NEW(prte_grpcomm_signature_t);
    cd->sig->groupID = strdup(grpid);
    if (NULL != procs) {
        cd->sig->sz = nprocs;
        cd->sig->signature = (pmix_proc_t *) malloc(cd->sig->sz * sizeof(pmix_proc_t));
        memcpy(cd->sig->signature, procs, cd->sig->sz * sizeof(pmix_proc_t));
    }
    cd->sig->bootstrap = bootstrap;
    if (NULL != members) {
        cd->sig->nmembers = num_members;
        if (copied) {
            cd->sig->addmembers = members;
        } else {
            cd->sig->addmembers = (pmix_proc_t *) malloc(num_members * sizeof(pmix_proc_t));
            memcpy(cd->sig->addmembers, members, num_members * sizeof(pmix_proc_t));
        }
    }
    /* setup the ctrls blob - this will include any "add_members" directive */
    rc = prte_pack_ctrl_options(&cd->ctrls, directives, ndirs);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
        return rc;
    }

    /* pass it to the global collective algorithm */
    if (PRTE_SUCCESS != (rc = prte_grpcomm.allgather(cd))) {
        PRTE_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

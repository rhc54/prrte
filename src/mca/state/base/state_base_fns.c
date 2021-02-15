/*
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "prte_config.h"
#include "constants.h"

#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <pmix.h>
#include <pmix_server.h>

#include "src/class/prte_list.h"
#include "src/event/event-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/argv.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/runtime/prte_data_server.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rml/rml.h"
#include "src/mca/routed/routed.h"
#include "src/util/session_dir.h"
#include "src/threads/threads.h"
#include "src/util/show_help.h"

#include "src/mca/state/base/base.h"
#include "src/mca/state/base/state_private.h"

void prte_state_base_activate_job_state(prte_job_t *jdata,
                                        prte_job_state_t state)
{
    prte_list_item_t *itm, *any=NULL, *error=NULL;
    prte_state_t *s;
    prte_state_caddy_t *caddy;

    for (itm = prte_list_get_first(&prte_job_states);
         itm != prte_list_get_end(&prte_job_states);
         itm = prte_list_get_next(itm)) {
        s = (prte_state_t*)itm;
        if (s->job_state == PRTE_JOB_STATE_ANY) {
            /* save this place */
            any = itm;
        }
        if (s->job_state == PRTE_JOB_STATE_ERROR) {
            error = itm;
        }
        if (s->job_state == state) {
            PRTE_REACHING_JOB_STATE(jdata, state, s->priority);
            if (NULL == s->cbfunc) {
                PRTE_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                                     "%s NULL CBFUNC FOR JOB %s STATE %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     (NULL == jdata) ? "ALL" : PRTE_JOBID_PRINT(jdata->nspace),
                                     prte_job_state_to_str(state)));
                return;
            }
            caddy = PRTE_NEW(prte_state_caddy_t);
            if (NULL != jdata) {
                caddy->jdata = jdata;
                caddy->job_state = state;
                PRTE_RETAIN(jdata);
            }
            PRTE_THREADSHIFT(caddy, prte_event_base, s->cbfunc, s->priority);
            return;
        }
    }
    /* if we get here, then the state wasn't found, so execute
     * the default handler if it is defined
     */
    if (PRTE_JOB_STATE_ERROR < state && NULL != error) {
        s = (prte_state_t*)error;
    } else if (NULL != any) {
        s = (prte_state_t*)any;
    } else {
        PRTE_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                             "ACTIVATE: JOB STATE %s NOT REGISTERED", prte_job_state_to_str(state)));
        return;
    }
    if (NULL == s->cbfunc) {
        PRTE_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                             "ACTIVATE: ANY STATE HANDLER NOT DEFINED"));
        return;
    }
    caddy = PRTE_NEW(prte_state_caddy_t);
    if (NULL != jdata) {
        caddy->jdata = jdata;
        caddy->job_state = state;
        PRTE_RETAIN(jdata);
    }
    PRTE_REACHING_JOB_STATE(jdata, state, s->priority);
    PRTE_THREADSHIFT(caddy, prte_event_base, s->cbfunc, s->priority);
}


int prte_state_base_add_job_state(prte_job_state_t state,
                                  prte_state_cbfunc_t cbfunc,
                                  int priority)
{
    prte_list_item_t *item;
    prte_state_t *st;

    /* check for uniqueness */
    for (item = prte_list_get_first(&prte_job_states);
         item != prte_list_get_end(&prte_job_states);
         item = prte_list_get_next(item)) {
        st = (prte_state_t*)item;
        if (st->job_state == state) {
            PRTE_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                                 "DUPLICATE STATE DEFINED: %s",
                                 prte_job_state_to_str(state)));
            return PRTE_ERR_BAD_PARAM;
        }
    }

    st = PRTE_NEW(prte_state_t);
    st->job_state = state;
    st->cbfunc = cbfunc;
    st->priority = priority;
    prte_list_append(&prte_job_states, &(st->super));

    return PRTE_SUCCESS;
}

int prte_state_base_set_job_state_callback(prte_job_state_t state,
                                           prte_state_cbfunc_t cbfunc)
{
    prte_list_item_t *item;
    prte_state_t *st;

    for (item = prte_list_get_first(&prte_job_states);
         item != prte_list_get_end(&prte_job_states);
         item = prte_list_get_next(item)) {
        st = (prte_state_t*)item;
        if (st->job_state == state) {
            st->cbfunc = cbfunc;
            return PRTE_SUCCESS;
        }
    }

    /* if not found, assume SYS priority and install it */
    st = PRTE_NEW(prte_state_t);
    st->job_state = state;
    st->cbfunc = cbfunc;
    st->priority = PRTE_SYS_PRI;
    prte_list_append(&prte_job_states, &(st->super));

    return PRTE_SUCCESS;
}

int prte_state_base_set_job_state_priority(prte_job_state_t state,
                                           int priority)
{
    prte_list_item_t *item;
    prte_state_t *st;

    for (item = prte_list_get_first(&prte_job_states);
         item != prte_list_get_end(&prte_job_states);
         item = prte_list_get_next(item)) {
        st = (prte_state_t*)item;
        if (st->job_state == state) {
            st->priority = priority;
            return PRTE_SUCCESS;
        }
    }
    return PRTE_ERR_NOT_FOUND;
}

int prte_state_base_remove_job_state(prte_job_state_t state)
{
    prte_list_item_t *item;
    prte_state_t *st;

    for (item = prte_list_get_first(&prte_job_states);
         item != prte_list_get_end(&prte_job_states);
         item = prte_list_get_next(item)) {
        st = (prte_state_t*)item;
        if (st->job_state == state) {
            prte_list_remove_item(&prte_job_states, item);
            PRTE_RELEASE(item);
            return PRTE_SUCCESS;
        }
    }
    return PRTE_ERR_NOT_FOUND;
}

void prte_state_base_print_job_state_machine(void)
{
    prte_list_item_t *item;
    prte_state_t *st;

    prte_output(0, "PRTE_JOB_STATE_MACHINE:");
    for (item = prte_list_get_first(&prte_job_states);
         item != prte_list_get_end(&prte_job_states);
         item = prte_list_get_next(item)) {
        st = (prte_state_t*)item;
        prte_output(0, "\tState: %s cbfunc: %s",
                    prte_job_state_to_str(st->job_state),
                    (NULL == st->cbfunc) ? "NULL" : "DEFINED");
    }
}


/****    PROC STATE MACHINE    ****/
void prte_state_base_activate_proc_state(pmix_proc_t *proc,
                                         prte_proc_state_t state)
{
    prte_list_item_t *itm, *any=NULL, *error=NULL;
    prte_state_t *s;
    prte_state_caddy_t *caddy;

    for (itm = prte_list_get_first(&prte_proc_states);
         itm != prte_list_get_end(&prte_proc_states);
         itm = prte_list_get_next(itm)) {
        s = (prte_state_t*)itm;
        if (s->proc_state == PRTE_PROC_STATE_ANY) {
            /* save this place */
            any = itm;
        }
        if (s->proc_state == PRTE_PROC_STATE_ERROR) {
            error = itm;
        }
        if (s->proc_state == state) {
            PRTE_REACHING_PROC_STATE(proc, state, s->priority);
            if (NULL == s->cbfunc) {
                PRTE_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                                     "%s NULL CBFUNC FOR PROC %s STATE %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(proc),
                                     prte_proc_state_to_str(state)));
                return;
            }
            caddy = PRTE_NEW(prte_state_caddy_t);
            caddy->name = *proc;
            caddy->proc_state = state;
            PRTE_THREADSHIFT(caddy, prte_event_base, s->cbfunc, s->priority);
            return;
        }
    }
    /* if we get here, then the state wasn't found, so execute
     * the default handler if it is defined
     */
    if (PRTE_PROC_STATE_ERROR < state && NULL != error) {
        s = (prte_state_t*)error;
    } else if (NULL != any) {
        s = (prte_state_t*)any;
    } else {
        PRTE_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                             "INCREMENT: ANY STATE NOT FOUND"));
        return;
    }
    if (NULL == s->cbfunc) {
        PRTE_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                             "ACTIVATE: ANY STATE HANDLER NOT DEFINED"));
        return;
    }
    caddy = PRTE_NEW(prte_state_caddy_t);
    caddy->name = *proc;
    caddy->proc_state = state;
    PRTE_REACHING_PROC_STATE(proc, state, s->priority);
    PRTE_THREADSHIFT(caddy, prte_event_base, s->cbfunc, s->priority);
}

int prte_state_base_add_proc_state(prte_proc_state_t state,
                                   prte_state_cbfunc_t cbfunc,
                                   int priority)
{
    prte_list_item_t *item;
    prte_state_t *st;

    /* check for uniqueness */
    for (item = prte_list_get_first(&prte_proc_states);
         item != prte_list_get_end(&prte_proc_states);
         item = prte_list_get_next(item)) {
        st = (prte_state_t*)item;
        if (st->proc_state == state) {
            PRTE_OUTPUT_VERBOSE((1, prte_state_base_framework.framework_output,
                                 "DUPLICATE STATE DEFINED: %s",
                                 prte_proc_state_to_str(state)));
            return PRTE_ERR_BAD_PARAM;
        }
    }

    st = PRTE_NEW(prte_state_t);
    st->proc_state = state;
    st->cbfunc = cbfunc;
    st->priority = priority;
    prte_list_append(&prte_proc_states, &(st->super));

    return PRTE_SUCCESS;
}

int prte_state_base_set_proc_state_callback(prte_proc_state_t state,
                                            prte_state_cbfunc_t cbfunc)
{
    prte_list_item_t *item;
    prte_state_t *st;

    for (item = prte_list_get_first(&prte_proc_states);
         item != prte_list_get_end(&prte_proc_states);
         item = prte_list_get_next(item)) {
        st = (prte_state_t*)item;
        if (st->proc_state == state) {
            st->cbfunc = cbfunc;
            return PRTE_SUCCESS;
        }
    }
    return PRTE_ERR_NOT_FOUND;
}

int prte_state_base_set_proc_state_priority(prte_proc_state_t state,
                                            int priority)
{
    prte_list_item_t *item;
    prte_state_t *st;

    for (item = prte_list_get_first(&prte_proc_states);
         item != prte_list_get_end(&prte_proc_states);
         item = prte_list_get_next(item)) {
        st = (prte_state_t*)item;
        if (st->proc_state == state) {
            st->priority = priority;
            return PRTE_SUCCESS;
        }
    }
    return PRTE_ERR_NOT_FOUND;
}

int prte_state_base_remove_proc_state(prte_proc_state_t state)
{
    prte_list_item_t *item;
    prte_state_t *st;

    for (item = prte_list_get_first(&prte_proc_states);
         item != prte_list_get_end(&prte_proc_states);
         item = prte_list_get_next(item)) {
        st = (prte_state_t*)item;
        if (st->proc_state == state) {
            prte_list_remove_item(&prte_proc_states, item);
            PRTE_RELEASE(item);
            return PRTE_SUCCESS;
        }
    }
    return PRTE_ERR_NOT_FOUND;
}

void prte_state_base_print_proc_state_machine(void)
{
    prte_list_item_t *item;
    prte_state_t *st;

    prte_output(0, "PRTE_PROC_STATE_MACHINE:");
    for (item = prte_list_get_first(&prte_proc_states);
         item != prte_list_get_end(&prte_proc_states);
         item = prte_list_get_next(item)) {
        st = (prte_state_t*)item;
        prte_output(0, "\tState: %s cbfunc: %s",
                    prte_proc_state_to_str(st->proc_state),
                    (NULL == st->cbfunc) ? "NULL" : "DEFINED");
    }
}

void prte_state_base_local_launch_complete(int fd, short argc, void *cbdata)
{
    prte_state_caddy_t *state = (prte_state_caddy_t*)cbdata;
    prte_job_t *jdata = state->jdata;

    if (prte_report_launch_progress) {
        if (0 == jdata->num_daemons_reported % 100 ||
            jdata->num_daemons_reported == prte_process_info.num_daemons) {
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_REPORT_PROGRESS);
        }
    }
    PRTE_RELEASE(state);
}

void prte_state_base_cleanup_job(int fd, short argc, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_job_t *jdata;

    PRTE_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;

    PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                         "%s state:base:cleanup on job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == jdata) ? "NULL" : PRTE_JOBID_PRINT(jdata->nspace)));

    /* flag that we were notified */
    jdata->state = PRTE_JOB_STATE_NOTIFIED;
    /* send us back thru job complete */
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_TERMINATED);
    PRTE_RELEASE(caddy);
}

void prte_state_base_report_progress(int fd, short argc, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_job_t *jdata;

     PRTE_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;

   prte_output(prte_clean_output, "App launch reported: %d (out of %d) daemons - %d (out of %d) procs",
                (int)jdata->num_daemons_reported, (int)prte_process_info.num_daemons,
                (int)jdata->num_launched, (int)jdata->num_procs);
    PRTE_RELEASE(caddy);
}

void prte_state_base_notify_data_server(pmix_proc_t *target)
{
    pmix_data_buffer_t *buf;
    int rc, room = -1;
    uint8_t cmd = PRTE_PMIX_PURGE_PROC_CMD;

    /* if nobody local to us published anything, then we can ignore this */
    if (PMIX_NSPACE_INVALID(prte_pmix_server_globals.server.nspace)) {
        return;
    }

    PMIX_DATA_BUFFER_CREATE(buf);

    /* pack the room number */
    rc = PMIx_Data_pack(NULL, buf, &room, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }

    /* load the command */
    rc = PMIx_Data_pack(NULL, buf, &cmd, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }

    /* provide the target */
    rc = PMIx_Data_pack(NULL, buf, target, 1, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }

    /* send the request to the server */
    rc = prte_rml.send_buffer_nb(&prte_pmix_server_globals.server, buf,
                                 PRTE_RML_TAG_DATA_SERVER,
                                 prte_rml_send_callback, NULL);
    if (PRTE_SUCCESS != rc) {
        PRTE_RELEASE(buf);
    }
}

static void _send_notification(int status,
                               prte_proc_state_t state,
                               pmix_proc_t *proc,
                               pmix_proc_t *target)
{
    pmix_data_buffer_t *buf;
    prte_grpcomm_signature_t sig;
    int rc;
    pmix_proc_t daemon;
    pmix_byte_object_t pbo;
    pmix_info_t *info;
    size_t ninfo;
    pmix_data_buffer_t pbkt;
    pmix_data_range_t range = PMIX_RANGE_CUSTOM;
    pmix_status_t code, ret;

    prte_output_verbose(5, prte_state_base_framework.framework_output,
                        "%s state:base:sending notification %s proc %s target %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_ERROR_NAME(status),
                        PRTE_NAME_PRINT(proc),
                        PRTE_NAME_PRINT(target));

    /* pack the info for sending */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

    /* pack the status code */
    code = prte_pmix_convert_rc(status);
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &code, 1, PMIX_STATUS))) {
        PMIX_ERROR_LOG(ret);
        return;
    }
    /* pack the source - it cannot be me as that will cause
     * the pmix server to upcall the event back to me */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, proc, 1, PMIX_PROC))) {
        PMIX_ERROR_LOG(ret);
        return;
    }
    /* pack the range */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &range, 1, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(ret);
        return;
    }

    /* setup the info */
    ninfo = 2;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROC, proc, PMIX_PROC);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_CUSTOM_RANGE, target, PMIX_PROC);

    /* pack the number of infos */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_FREE(info, ninfo);
        return;
    }
    /* pack the infos themselves */
    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, info, ninfo, PMIX_INFO))) {
        PMIX_ERROR_LOG(ret);
        PMIX_INFO_FREE(info, ninfo);
        return;
    }
    PMIX_INFO_FREE(info, ninfo);

    /* unload the data buffer */
    ret = PMIx_Data_unload(&pbkt, &pbo);

    /* insert into pmix_data_buffer_t */
    PMIX_DATA_BUFFER_CREATE(buf);

    /* we need to add a flag indicating this came from an invalid proc so that we will
     * inject it into our own PMIx server library */
    if (PRTE_SUCCESS != (rc = PMIx_Data_pack(NULL, buf, &PRTE_NAME_INVALID->rank, 1, PMIX_PROC_RANK))) {
        PMIX_ERROR_LOG(rc);
        free(pbo.bytes);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }
    if (PRTE_SUCCESS != (rc = PMIx_Data_pack(NULL, buf, &pbo, 1, PMIX_BYTE_OBJECT))) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        free(pbo.bytes);
        return;
    }
    free(pbo.bytes);

    if (PMIX_RANK_WILDCARD == target->rank) {
        /* xcast it to everyone */
        PRTE_CONSTRUCT(&sig, prte_grpcomm_signature_t);
        sig.signature = (pmix_proc_t*)malloc(sizeof(pmix_proc_t));
        PMIX_LOAD_PROCID(&sig.signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
        sig.sz = 1;

        if (PRTE_SUCCESS != (rc = prte_grpcomm.xcast(&sig, PRTE_RML_TAG_NOTIFICATION, buf))) {
            PRTE_ERROR_LOG(rc);
        }
        PRTE_DESTRUCT(&sig);
        PMIX_DATA_BUFFER_RELEASE(buf);
    } else {
        /* get the daemon hosting the proc to be notified */
        PMIX_LOAD_PROCID(&daemon, PRTE_PROC_MY_NAME->nspace, prte_get_proc_daemon_vpid(target));
        /* send the notification to that daemon */
        prte_output_verbose(5, prte_state_base_framework.framework_output,
                            "%s state:base:sending notification %s to proc %s at daemon %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PRTE_ERROR_NAME(status),
                            PRTE_NAME_PRINT(target),
                            PRTE_NAME_PRINT(&daemon));
        if (PRTE_SUCCESS != (rc = prte_rml.send_buffer_nb(&daemon, buf,
                                                          PRTE_RML_TAG_NOTIFICATION,
                                                          prte_rml_send_callback, NULL))) {
            PRTE_ERROR_LOG(rc);
            PRTE_RELEASE(buf);
        }
    }
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    prte_pmix_lock_t *lock = (prte_pmix_lock_t*)cbdata;
    PRTE_PMIX_WAKEUP_THREAD(lock);
}

void prte_state_base_track_procs(int fd, short argc, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    pmix_proc_t *proc;
    prte_proc_state_t state;
    prte_job_t *jdata;
    prte_proc_t *pdata;
    int i;
    pmix_proc_t parent, target;
    prte_pmix_lock_t lock;

    PRTE_ACQUIRE_OBJECT(caddy);
    proc = &caddy->name;
    state = caddy->proc_state;

    prte_output_verbose(5, prte_state_base_framework.framework_output,
                        "%s state:base:track_procs called for proc %s state %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(proc),
                        prte_proc_state_to_str(state));

    /* get the job object for this proc */
    if (NULL == (jdata = prte_get_job_data_object(proc->nspace))) {
        goto cleanup;
    }
    pdata = (prte_proc_t*)prte_pointer_array_get_item(jdata->procs, proc->rank);
    if (NULL == pdata) {
        goto cleanup;
    }

    if (PRTE_PROC_STATE_RUNNING == state) {
        /* update the proc state */
        if (pdata->state < PRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        jdata->num_launched++;
        if (1 == jdata->num_launched) {
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_STARTED);
        }
        if (jdata->num_launched == jdata->num_procs) {
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_RUNNING);
        }
    } else if (PRTE_PROC_STATE_REGISTERED == state) {
        /* update the proc state */
        if (pdata->state < PRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        jdata->num_reported++;
        if (jdata->num_reported == jdata->num_procs) {
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_REGISTERED);
        }
    } else if (PRTE_PROC_STATE_IOF_COMPLETE == state) {
        /* update the proc state */
        if (pdata->state < PRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        /* Release the IOF file descriptors */
        if (NULL != prte_iof.close) {
            prte_iof.close(proc, PRTE_IOF_STDALL);
        }
        PRTE_FLAG_SET(pdata, PRTE_PROC_FLAG_IOF_COMPLETE);
        if (PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_WAITPID)) {
            PRTE_ACTIVATE_PROC_STATE(proc, PRTE_PROC_STATE_TERMINATED);
        }
    } else if (PRTE_PROC_STATE_WAITPID_FIRED == state) {
        /* update the proc state */
        if (pdata->state < PRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        PRTE_FLAG_SET(pdata, PRTE_PROC_FLAG_WAITPID);
        if (PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_IOF_COMPLETE)) {
            PRTE_ACTIVATE_PROC_STATE(proc, PRTE_PROC_STATE_TERMINATED);
        }
    } else if (PRTE_PROC_STATE_TERMINATED == state) {
        if (pdata->state == state) {
            prte_output_verbose(5, prte_state_base_framework.framework_output,
                                 "%s state:base:track_procs proc %s already in state %s. Skip transition.",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(proc),
                                 prte_proc_state_to_str(state));
            goto cleanup;
        }

        /* update the proc state */
        PRTE_FLAG_UNSET(pdata, PRTE_PROC_FLAG_ALIVE);
        if (pdata->state < PRTE_PROC_STATE_TERMINATED) {
            pdata->state = state;
        }
        if (PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_LOCAL)) {
            PRTE_PMIX_CONSTRUCT_LOCK(&lock);
            PMIx_server_deregister_client(proc, opcbfunc, &lock);
            PRTE_PMIX_WAIT_THREAD(&lock);
            PRTE_PMIX_DESTRUCT_LOCK(&lock);

            /* Clean up the session directory as if we were the process
             * itself.  This covers the case where the process died abnormally
             * and didn't cleanup its own session directory.
             */
            if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_DEBUGGER_DAEMON) &&
                !PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
                prte_session_dir_finalize(proc);
            }
        }
        /* if we are trying to terminate and our routes are
         * gone, then terminate ourselves IF no local procs
         * remain (might be some from another job)
         */
        if (prte_prteds_term_ordered &&
            0 == prte_routed.num_routes()) {
            for (i=0; i < prte_local_children->size; i++) {
                if (NULL != (pdata = (prte_proc_t*)prte_pointer_array_get_item(prte_local_children, i)) &&
                    PRTE_FLAG_TEST(pdata, PRTE_PROC_FLAG_ALIVE)) {
                    /* at least one is still alive */
                    goto cleanup;
                }
            }
            /* call our appropriate exit procedure */
            PRTE_OUTPUT_VERBOSE((5, prte_state_base_framework.framework_output,
                                 "%s state:base all routes and children gone - exiting",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            goto cleanup;
        }
        /* track job status */
        jdata->num_terminated++;
        if (jdata->num_terminated == jdata->num_procs) {
            /* if requested, check fd status for leaks */
            if (prte_state_base_run_fdcheck) {
                prte_state_base_check_fds(jdata);
            }
            /* if ompi-server is around, then notify it to purge
             * any session-related info */
            if (NULL != prte_data_server_uri) {
                PMIX_LOAD_PROCID(&target, jdata->nspace, PMIX_RANK_WILDCARD);
                prte_state_base_notify_data_server(&target);
            }
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_TERMINATED);
        } else if (PRTE_PROC_STATE_TERMINATED < pdata->state &&
                   !prte_job_term_ordered) {
            /* if this was an abnormal term, notify the other procs of the termination */
            PMIX_LOAD_PROCID(&parent, jdata->nspace, PMIX_RANK_WILDCARD);

            /* if ft prte is enabled, a PMIx event has already been produced
             * and this is redundant. */
            if (!prte_enable_ft) {
                _send_notification(PRTE_ERR_PROC_ABORTED, pdata->state, &pdata->name, &parent);
            }
        }
    }

 cleanup:
    PRTE_RELEASE(caddy);
}

void prte_state_base_check_all_complete(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_job_t *jdata;
    prte_proc_t *proc;
    int i;
    int32_t j;
    prte_job_t *job;
    prte_node_t *node;
    prte_job_map_t *map;
    int32_t index;
    bool one_still_alive;
    pmix_rank_t lowest=0;
    int32_t i32, *i32ptr;
    prte_pmix_lock_t lock;

    PRTE_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;

    prte_output_verbose(2, prte_state_base_framework.framework_output,
                        "%s state:base:check_job_complete on job %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (NULL == jdata) ? "NULL" : PRTE_JOBID_PRINT(jdata->nspace));

    if (NULL == jdata || PMIX_CHECK_NSPACE(jdata->nspace, PRTE_PROC_MY_NAME->nspace)) {
        /* just check to see if the daemons are complete */
        PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                             "%s state:base:check_job_complete - received NULL job, checking daemons",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        goto CHECK_DAEMONS;
    } else {
        /* mark the job as terminated, but don't override any
         * abnormal termination flags
         */
        if (jdata->state < PRTE_JOB_STATE_UNTERMINATED) {
            jdata->state = PRTE_JOB_STATE_TERMINATED;
        }
    }

    /* tell the IOF that the job is complete */
    if (NULL != prte_iof.complete) {
        prte_iof.complete(jdata);
    }

    /* tell the PMIx server to release its data */
    PRTE_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_server_deregister_nspace(jdata->nspace, opcbfunc, &lock);
    PRTE_PMIX_WAIT_THREAD(&lock);
    PRTE_PMIX_DESTRUCT_LOCK(&lock);

    i32ptr = &i32;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_NUM_NONZERO_EXIT, (void**)&i32ptr, PMIX_INT32) && !prte_abort_non_zero_exit) {
        if (!prte_report_child_jobs_separately || 1 == PRTE_LOCAL_JOBID(jdata->nspace)) {
            /* update the exit code */
            PRTE_UPDATE_EXIT_STATUS(lowest);
        }

        /* warn user */
        prte_show_help("help-state-base.txt", "normal-termination-but", true,
                    (1 == PRTE_LOCAL_JOBID(jdata->nspace)) ? "the primary" : "child",
                    (1 == PRTE_LOCAL_JOBID(jdata->nspace)) ? "" : PRTE_LOCAL_JOBID_PRINT(jdata->nspace),
                    i32, (1 == i32) ? "process returned\na non-zero exit code." :
                    "processes returned\nnon-zero exit codes.");
    }

    PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                         "%s state:base:check_job_completed declared job %s terminated with state %s - checking all jobs",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace),
                         prte_job_state_to_str(jdata->state)));

    /* if this job is a continuously operating one, then don't do
     * anything further - just return here
     */
    if (NULL != jdata &&
        (prte_get_attribute(&jdata->attributes, PRTE_JOB_CONTINUOUS_OP, NULL, PMIX_BOOL) ||
         PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RECOVERABLE))) {
        goto CHECK_ALIVE;
    }

    /* if the job that is being checked is the HNP, then we are
     * trying to terminate the orteds. In that situation, we
     * do -not- check all jobs - we simply notify the HNP
     * that the orteds are complete. Also check special case
     * if jdata is NULL - we want
     * to definitely declare the job done if the orteds
     * have completed, no matter what else may be happening.
     * This can happen if a ctrl-c hits in the "wrong" place
     * while launching
     */
 CHECK_DAEMONS:
    if (jdata == NULL || PMIX_CHECK_NSPACE(jdata->nspace, PRTE_PROC_MY_NAME->nspace)) {
        if (0 == prte_routed.num_routes()) {
            /* orteds are done! */
            PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                 "%s orteds complete - exiting",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            if (NULL == jdata) {
                jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
            }
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_DAEMONS_TERMINATED);
            PRTE_RELEASE(caddy);
            return;
        }
        PRTE_RELEASE(caddy);
        return;
    }

    /* Release the resources used by this job. Since some errmgrs may want
     * to continue using resources allocated to the job as part of their
     * fault recovery procedure, we only do this once the job is "complete".
     * Note that an aborted/killed job -is- flagged as complete and will
     * therefore have its resources released. We need to do this after
     * we call the errmgr so that any attempt to restart the job will
     * avoid doing so in the exact same place as the current job
     */
    if (NULL != jdata->map && jdata->state == PRTE_JOB_STATE_TERMINATED) {
        map = jdata->map;
        for (index = 0; index < map->nodes->size; index++) {
            if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(map->nodes, index))) {
                continue;
            }
            PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                 "%s releasing procs for job %s from node %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_JOBID_PRINT(jdata->nspace), node->name));
            for (i = 0; i < node->procs->size; i++) {
                if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(node->procs, i))) {
                    continue;
                }
                if (!PMIX_CHECK_NSPACE(proc->name.nspace, jdata->nspace)) {
                    /* skip procs from another job */
                    continue;
                }
                if (!PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_DEBUGGER_DAEMON) &&
                    !PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_TOOL)) {
                    node->slots_inuse--;
                    node->num_procs--;
                }
                PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                     "%s releasing proc %s from node %s",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     PRTE_NAME_PRINT(&proc->name), node->name));
                /* set the entry in the node array to NULL */
                prte_pointer_array_set_item(node->procs, i, NULL);
                /* release the proc once for the map entry */
                PRTE_RELEASE(proc);
            }
            /* set the node location to NULL */
            prte_pointer_array_set_item(map->nodes, index, NULL);
            /* maintain accounting */
            PRTE_RELEASE(node);
        }
        PRTE_RELEASE(map);
        jdata->map = NULL;
    }

 CHECK_ALIVE:
    /* now check to see if all jobs are done - trigger notification of this jdata
     * object when we find it
     */
    one_still_alive = false;
    for (j=0; j < prte_job_data->size; j++) {
        job = (prte_job_t*)prte_pointer_array_get_item(prte_job_data, j);
        if (NULL == job) {
            continue;
        }
        /* skip the daemon job */
        if (PMIX_CHECK_NSPACE(job->nspace, PRTE_PROC_MY_NAME->nspace)) {
            continue;
        }
        /* if this is the job we are checking AND it normally terminated,
         * then activate the "notify_completed" state - this will release
         * the job state, but is provided so that the HNP main code can
         * take alternative actions if desired. If the state is killed_by_cmd,
         * then go ahead and release it. We cannot release it if it
         * abnormally terminated as mpirun needs the info so it can
         * report appropriately to the user
         *
         * NOTE: do not release the primary job (j=1) so we
         * can pretty-print completion message
         */
        if (NULL != jdata && PMIX_CHECK_NSPACE(job->nspace, jdata->nspace)) {
            if (jdata->state == PRTE_JOB_STATE_TERMINATED) {
                PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                     "%s state:base:check_job_completed state is terminated - activating notify",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_NOTIFY_COMPLETED);
                one_still_alive = true;
            } else if (jdata->state == PRTE_JOB_STATE_KILLED_BY_CMD ||
                       jdata->state == PRTE_JOB_STATE_NOTIFIED) {
                PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                     "%s state:base:check_job_completed state is killed or notified - cleaning up",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                /* release this object, ensuring that the
                 * pointer array internal accounting
                 * is maintained!
                 */
                prte_pointer_array_set_item(prte_job_data, j, NULL);
                PRTE_RELEASE(jdata);
            }
            continue;
        }
        /* if the job is flagged to not be monitored, skip it */
        if (PRTE_FLAG_TEST(job, PRTE_JOB_FLAG_DO_NOT_MONITOR)) {
            continue;
        }
        /* when checking for job termination, we must be sure to NOT check
         * our own job as it - rather obviously - has NOT terminated!
         */
        if (PRTE_JOB_STATE_NOTIFIED != job->state) {
            /* we have at least one job that is not done yet - we cannot
             * just return, though, as we need to ensure we cleanout the
             * job data for the job that just completed
             */
            PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                 "%s state:base:check_job_completed job %s is not terminated (%d:%d)",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_JOBID_PRINT(job->nspace),
                                 job->num_terminated, job->num_procs));
            one_still_alive = true;
        }
        else {
            PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                                 "%s state:base:check_job_completed job %s is terminated (%d vs %d [%s])",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_JOBID_PRINT(job->nspace),
                                 job->num_terminated, job->num_procs,
                                 (NULL == jdata) ? "UNKNOWN" : prte_job_state_to_str(jdata->state) ));
        }
    }

    /* if a job is still alive, we just return */
    if (one_still_alive) {
        PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                             "%s state:base:check_job_completed at least one job is not terminated",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PRTE_RELEASE(caddy);
        return;
    }
    /* if we get here, then all jobs are done, so terminate */
    PRTE_OUTPUT_VERBOSE((2, prte_state_base_framework.framework_output,
                         "%s state:base:check_job_completed all jobs terminated",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* stop the job timeout event, if set */
    if (NULL != prte_mpiexec_timeout) {
        PRTE_RELEASE(prte_mpiexec_timeout);
        prte_mpiexec_timeout = NULL;
    }

    /* set the exit status to 0 - this will only happen if it
     * wasn't already set by an error condition
     */
    PRTE_UPDATE_EXIT_STATUS(0);

    /* order daemon termination - this tells us to cleanup
     * our local procs as well as telling remote daemons
     * to die
     */
    prte_plm.terminate_orteds();

    PRTE_RELEASE(caddy);
}


void prte_state_base_check_fds(prte_job_t *jdata)
{
    int nfds, i, fdflags, flflags;
    char path[1024], info[256], **list=NULL, *status, *result, *r2;
    ssize_t rc;
    struct flock fl;
    bool flk;
    int cnt = 0;

    /* get the number of available file descriptors
     * for this daemon */
    nfds = getdtablesize();
    result = NULL;
    /* loop over them and get their info */
    for (i=0; i < nfds; i++) {
        fdflags = fcntl(i, F_GETFD);
        if (-1 == fdflags) {
            /* no open fd in that slot */
            continue;
        }
        flflags = fcntl(i, F_GETFL);
        if (-1 == flflags) {
            /* no open fd in that slot */
            continue;
        }
        snprintf(path, 1024, "/proc/self/fd/%d", i);
        memset(info, 0, 256);
        /* read the info about this fd */
        rc = readlink(path, info, 256);
        if (-1 == rc) {
            /* this fd is unavailable */
            continue;
        }
        /* get any file locking status */
        fl.l_type = F_WRLCK;
        fl.l_whence = 0;
        fl.l_start = 0;
        fl.l_len = 0;
        if (-1 == fcntl(i, F_GETLK, &fl)) {
            flk = false;
        } else {
            flk = true;
        }
        /* construct the list of capabilities */
        if (fdflags & FD_CLOEXEC) {
            prte_argv_append_nosize(&list, "cloexec");
        }
        if (flflags & O_APPEND) {
            prte_argv_append_nosize(&list, "append");
        }
        if (flflags & O_NONBLOCK) {
            prte_argv_append_nosize(&list, "nonblock");
        }
        /* from the man page:
         *  Unlike the other values that can be specified in flags,
         * the access mode values O_RDONLY, O_WRONLY, and O_RDWR,
         * do not specify individual bits.  Rather, they define
         * the low order two bits of flags, and defined respectively
         * as 0, 1, and 2. */
        if (O_RDONLY == (flflags & 3)) {
            prte_argv_append_nosize(&list, "rdonly");
        } else if (O_WRONLY == (flflags & 3)) {
            prte_argv_append_nosize(&list, "wronly");
        } else {
            prte_argv_append_nosize(&list, "rdwr");
        }
        if (flk && F_UNLCK != fl.l_type) {
            if (F_WRLCK == fl.l_type) {
                prte_argv_append_nosize(&list, "wrlock");
            } else {
                prte_argv_append_nosize(&list, "rdlock");
            }
        }
        if (NULL != list) {
            status = prte_argv_join(list, ' ');
            prte_argv_free(list);
            list = NULL;
            if (NULL == result) {
                prte_asprintf(&result, "    %d\t(%s)\t%s\n", i, info, status);
            } else {
                prte_asprintf(&r2, "%s    %d\t(%s)\t%s\n", result, i, info, status);
                free(result);
                result = r2;
            }
            free(status);
        }
        ++cnt;
    }
    prte_asprintf(&r2, "%s: %d open file descriptors after job %d completed\n%s",
             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), cnt, PRTE_LOCAL_JOBID(jdata->nspace), result);
    prte_output(0, "%s", r2);
    free(result);
    free(r2);
}

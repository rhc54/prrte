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
 * Copyright (c) 2011-2016 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 * The PRTE Group Communications
 *
 * grpcomm provides the collective communication services that span the
 * DVM's daemons: scalable broadcast (xcast), allgather/barrier (fence),
 * and PMIx group operations (group).  It is not intended for
 * point-to-point communication (the RML does that), nor as a
 * high-performance channel for large-scale data transfer.
 *
 * This was an MCA framework until it was collapsed into plain code, for
 * the same reason src/rml was: there was only ever one component, and the
 * choice that actually matters - which algorithm a collective uses - is
 * per-operation and per-message-size, which a component cannot express.
 * Do not reintroduce the component/module abstraction unless a genuine
 * second implementation of the *whole* interface turns up; to vary an
 * algorithm, add a movement strategy inside the operation instead.
 */

#ifndef PRTE_GRPCOMM_H
#define PRTE_GRPCOMM_H

/*
 * includes
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include "src/class/pmix_bitmap.h"
#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"
#include "src/rml/rml_types.h"

BEGIN_C_DECLS

typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    pmix_lock_t lock;
    pmix_group_operation_t op;
    char *grpid;
    const pmix_proc_t *procs;
    size_t nprocs;
    const pmix_info_t *directives;
    size_t ndirs;
    pmix_info_t *info;
    size_t ninfo;
    pmix_info_cbfunc_t cbfunc;
    void *cbdata;
} prte_pmix_grp_caddy_t;
PRTE_EXPORT PMIX_CLASS_DECLARATION(prte_pmix_grp_caddy_t);

/* define a callback function to be invoked upon
 * collective completion */
typedef void (*prte_grpcomm_cbfunc_t)(int status, pmix_data_buffer_t *buf, void *cbdata);

/* Completion callback for xcast_nb: invoked on the DVM master once the reliable
 * xcast has been confirmed received by every daemon in the DVM (i.e., when the
 * broadcast's ACKs have all flowed back up the tree).  It fires on the progress
 * thread; a handler that needs to do more than trivial work should thread-shift
 * onto a fresh event rather than run inside the xcast call stack. */
typedef void (*prte_grpcomm_xcast_complete_fn_t)(void *cbdata);

/* Register grpcomm's MCA parameters and open its verbosity stream.  Called
 * from prte_register_params(), alongside the other subsystems' registrations,
 * long before prte_grpcomm_init(). */
PRTE_EXPORT void prte_grpcomm_register(void);

/* Stand grpcomm up: construct the operation trackers and register the
 * persistent RML receives.  Called once per daemon, from the ess.  A
 * failure here leaves no trackers and no receives, so every collective
 * from then on would quietly do nothing - the caller must treat it as
 * fatal rather than press on. */
PRTE_EXPORT int prte_grpcomm_init(void);

/* Tear down the trackers and cancel the RML receives. */
PRTE_EXPORT void prte_grpcomm_finalize(void);

/* Respond to a change in the routing tree - a daemon died, revived, or this
 * node was re-parented/promoted - by repairing, restarting or ending the
 * collectives in flight.  Invoked on *every* daemon by src/rml, twice per
 * death (LOCAL scope then GLOBAL scope); which scope a collective keys on
 * depends on what it needs, so read the guide before adding a third. */
PRTE_EXPORT void prte_grpcomm_fault_handler(const prte_rml_recovery_status_t *status);

/* Scalably broadcast a message to every daemon in the DVM, to be delivered
 * at the given tag.  Non-destructive to msg (the caller still owns it).
 * Returns PRTE_SUCCESS once the broadcast has been *accepted*, which is not
 * the same as completed - use xcast_nb if you need to know it landed. */
PRTE_EXPORT int prte_grpcomm_xcast(prte_rml_tag_t tag, pmix_data_buffer_t *msg);

/* As xcast, but when cbfunc is non-NULL it fires on the master once the whole
 * DVM has confirmed receipt.  cbfunc/cbdata are ignored on non-master daemons.
 * xcast is just xcast_nb(tag, msg, NULL, NULL). */
PRTE_EXPORT int prte_grpcomm_xcast_nb(prte_rml_tag_t tag, pmix_data_buffer_t *msg,
                                      prte_grpcomm_xcast_complete_fn_t cbfunc, void *cbdata);

/* Non-blocking allgather/barrier across the daemons hosting procs.  A barrier
 * supplies no data.  cbfunc is invoked with the gathered buffer on completion.
 * Returns PRTE_SUCCESS once the request has been queued. */
PRTE_EXPORT int prte_grpcomm_fence(const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t info[], size_t ninfo, char *data,
                                   size_t ndata, pmix_modex_cbfunc_t cbfunc, void *cbdata);

/* Size any fence tracker that was left waiting on a job this daemon had not
 * built yet, and re-test it.  A daemon relays the launch broadcast to its
 * children before acting on its own copy, so a child can launch, fence, and
 * roll its contribution up to us before we know the job - at which point the
 * participants cannot be derived and the tracker is held unsized rather than
 * thrown away.  Call this once a job has been constructed locally; that is the
 * only event that can change the answer.  Cheap when nothing is waiting, which
 * is the overwhelmingly common case. */
PRTE_EXPORT void prte_grpcomm_fence_resolve_pending(void);

/* Tell grpcomm that procs of this namespace arrived here by restart.
 *
 * A fence's generation - which tells one fence over a participant set from the
 * next over the same set - is derived by counting the fences this daemon has
 * retired, which is only right if it took part in all of them.  A proc
 * relocated onto a daemon that never hosted one of that job breaks that, so
 * such a daemon learns the generation from the traffic instead of counting.
 * Called from the launch path when PRTE_JOB_FLAG_RESTART is set. */
PRTE_EXPORT void prte_grpcomm_fence_note_relocation(const pmix_nspace_t nspace);

/* PMIx group construct/destruct/cancel.  Basically a fence, but with enough
 * differences - context-id assignment, membership assembly, bootstrap - to
 * warrant its own path rather than over-complicating the fence code. */
PRTE_EXPORT int prte_grpcomm_group(pmix_group_operation_t op, char *grpid,
                                   const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata);

END_C_DECLS

#endif

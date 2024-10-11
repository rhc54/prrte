/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef RML_BASE_COMMON_H_
#define RML_BASE_COMMON_H_

#include "prte_config.h"

#include "rml.h"
#include "rml_hdr.h"

/* State machine for connection operations */
typedef struct {
    pmix_object_t super;
    prte_rml_peer_t *peer;
    prte_event_t ev;
} prte_rml_conn_op_t;
PMIX_CLASS_DECLARATION(prte_rml_conn_op_t);


#define PRTE_ACTIVATE_RML_CONN_STATE(p, cbfunc)                                             \
    do {                                                                                    \
        prte_rml_conn_op_t *cop;                                                        \
        pmix_output_verbose(5, prte_rml_base.rml_output,                    \
                            "%s:[%s:%d] connect to %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), \
                            __FILE__, __LINE__, PRTE_NAME_PRINT((&(p)->name)));             \
        cop = PMIX_NEW(prte_rml_conn_op_t);                                             \
        cop->peer = (p);                                                                    \
        PRTE_PMIX_THREADSHIFT(cop, prte_event_base, (cbfunc));                              \
    } while (0);

#define PRTE_ACTIVATE_RML_ACCEPT_STATE(s, a, cbfunc)                               \
    do {                                                                           \
        prte_rml_conn_op_t *cop;                                               \
        cop = PMIX_NEW(prte_rml_conn_op_t);                                    \
        prte_event_set(prte_event_base, &cop->ev, s, PRTE_EV_READ, (cbfunc), cop); \
        PMIX_POST_OBJECT(cop);                                                     \
        prte_event_add(&cop->ev, 0);                                               \
    } while (0);

#define PRTE_RETRY_RML_CONN_STATE(p, cbfunc, tv)                                                  \
    do {                                                                                          \
        prte_rml_conn_op_t *cop;                                                              \
        pmix_output_verbose(5, prte_rml_base.rml_output,                          \
                            "%s:[%s:%d] retry connect to %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), \
                            __FILE__, __LINE__, PRTE_NAME_PRINT((&(p)->name)));                   \
        cop = PMIX_NEW(prte_rml_conn_op_t);                                                   \
        cop->peer = (p);                                                                          \
        prte_event_evtimer_set(prte_event_base, &cop->ev, (cbfunc), cop);                         \
        PMIX_POST_OBJECT(cop);                                                                    \
        prte_event_evtimer_add(&cop->ev, (tv));                                                   \
    } while (0);

PRTE_EXPORT void prte_rml_set_socket_options(int sd);
PRTE_EXPORT char *prte_rml_state_print(prte_rml_state_t state);
PRTE_EXPORT prte_rml_peer_t *prte_rml_peer_lookup(const pmix_proc_t *name);

PRTE_EXPORT void prte_rml_peer_try_connect(int fd, short args, void *cbdata);
PRTE_EXPORT void prte_rml_peer_dump(prte_rml_peer_t *peer, const char *msg);
PRTE_EXPORT bool prte_rml_peer_accept(prte_rml_peer_t *peer);
PRTE_EXPORT void prte_rml_peer_complete_connect(prte_rml_peer_t *peer);
PRTE_EXPORT int prte_rml_peer_recv_connect_ack(prte_rml_peer_t *peer, int sd,
                                                          prte_rml_hdr_t *dhdr);
PRTE_EXPORT void prte_rml_peer_close(prte_rml_peer_t *peer);

#endif /* RML_BASE_COMMON_H_ */

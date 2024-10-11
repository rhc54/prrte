/*
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <fcntl.h>
#ifdef HAVE_NET_IF_H
#    include <net/if.h>
#endif
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif
#ifdef HAVE_NETDB_H
#    include <netdb.h>
#endif

#include "src/mca/base/pmix_mca_base_component_repository.h"
#include "src/mca/mca.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_parse_options.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/rml/rml_contact.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"

prte_rml_base_t prte_rml_base = {
    .rml_output = -1,
    .routed_output = -1,
    .max_retries = 0,
    .posted_recvs = PMIX_LIST_STATIC_INIT,
    .unmatched_msgs = PMIX_LIST_STATIC_INIT,
    .lifeline = PMIX_RANK_INVALID,
    .children = PMIX_LIST_STATIC_INIT,
    .radix = 64,
    .static_ports = false,
    .tcp_sndbuf = 0,
    .tcp_rcvbuf = 0,
    .disable_ipv4_family = false,
    .tcp_static_ports = NULL,
    .tcp_dyn_ports = NULL,
    .ipv4conns = NULL,
    .ipv4ports = NULL,
    .disable_ipv6_family = true,
    .tcp6_static_ports = NULL,
    .tcp6_dyn_ports = NULL,
    .ipv6conns = NULL,
    .ipv6ports = NULL,
    .peers = PMIX_LIST_STATIC_INIT,
    .peer_limit = 0,
    .local_ifs = PMIX_LIST_STATIC_INIT,
    .if_masks = NULL,
    .listeners = PMIX_LIST_STATIC_INIT,
    .listen_thread = {{0}},
    .listen_thread_active = false,
    .listen_thread_tv = {0, 0},
    .stop_thread = {0, 0},
    .keepalive_probes = 0,
    .keepalive_time = 0,
    .keepalive_intvl = 0,
    .retry_delay = 0,
    .max_recon_attempts = 0
};

static int verbosity = 0;
static int setup_interfaces(void);
static char **split_and_resolve(char **orig_str, char *name);
static void get_addr(void);

static char *static_port_string;
static char *dyn_port_string;
#if PRTE_ENABLE_IPV6
static char *static_port_string6;
#endif

void prte_rml_register(void)
{
    int ret;

    prte_rml_base.max_retries = 3;
    pmix_mca_base_var_register("prte", "rml", "base", "max_retries",
                               "Max #times to retry sending a message",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_rml_base.max_retries);

    verbosity = 0;
    pmix_mca_base_var_register("prte", "rml", "base", "verbose",
                               "Debug verbosity of the RML subsystem",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &verbosity);
    if (0 < verbosity) {
        prte_rml_base.rml_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_rml_base.rml_output, verbosity);
    }

    verbosity = 0;
    pmix_mca_base_var_register("prte", "routed", "base", "verbose",
                               "Debug verbosity of the Routed subsystem",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &verbosity);
    if (0 < verbosity) {
        prte_rml_base.routed_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_rml_base.routed_output, verbosity);
    }

    ret = pmix_mca_base_var_register("prte", "rml", "base", "radix",
                                     "Radix to be used for routing tree",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &prte_rml_base.radix);
    pmix_mca_base_var_register_synonym(ret, "prte", "routed", "radix", NULL,
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    prte_rml_base.peer_limit = -1;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "peer_limit",
                                     "Maximum number of peer connections to simultaneously maintain (-1 = infinite)",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &prte_rml_base.peer_limit);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "peer_limit",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    prte_rml_base.max_retries = 2;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "peer_retries",
                                     "Number of times to try shutting down a connection before giving up",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &prte_rml_base.max_retries);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "peer_retries",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    prte_rml_base.tcp_sndbuf = 0;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "sndbuf",
                                     "TCP socket send buffering size (in bytes, 0 => leave system default)",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &prte_rml_base.tcp_sndbuf);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "sndbuf",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    prte_rml_base.tcp_rcvbuf = 0;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "rcvbuf",
                                     "TCP socket receive buffering size (in bytes, 0 => leave system default)",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &prte_rml_base.tcp_rcvbuf);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "rcvbuf",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);


    static_port_string = NULL;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "static_ipv4_ports",
                                     "Static ports for daemons and procs (IPv4)",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &static_port_string);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "static_ipv4_ports",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    /* if ports were provided, parse the provided range */
    if (NULL != static_port_string) {
        pmix_util_parse_range_options(static_port_string, &prte_rml_base.tcp_static_ports);
        if (0 == strcmp(prte_rml_base.tcp_static_ports[0], "-1")) {
            PMIX_ARGV_FREE_COMPAT(prte_rml_base.tcp_static_ports);
            prte_rml_base.tcp_static_ports = NULL;
        }
    } else {
        prte_rml_base.tcp_static_ports = NULL;
    }

#if PRTE_ENABLE_IPV6
    static_port_string6 = NULL;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "static_ipv6_ports",
                                     "Static ports for daemons and procs (IPv6)",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &static_port_string6);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "static_ipv6_ports",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    /* if ports were provided, parse the provided range */
    if (NULL != static_port_string6) {
        pmix_util_parse_range_options(static_port_string6,
                                      &prte_rml_base.tcp6_static_ports);
        if (0 == strcmp(prte_rml_base.tcp6_static_ports[0], "-1")) {
            PMIX_ARGV_FREE_COMPAT(prte_rml_base.tcp6_static_ports);
            prte_rml_base.tcp6_static_ports = NULL;
        }
    } else {
        prte_rml_base.tcp6_static_ports = NULL;
    }
#endif // PRTE_ENABLE_IPV6

    if (NULL != prte_rml_base.tcp_static_ports
        || NULL != prte_rml_base.tcp6_static_ports) {
        prte_static_ports = true;
    }

    dyn_port_string = NULL;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "dynamic_ipv4_ports",
                                     "Range of ports to be dynamically used by daemons (IPv4)",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &dyn_port_string);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "dynamic_ipv4_ports",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    /* if ports were provided, parse the provided range */
    if (NULL != dyn_port_string) {
        /* can't have both static and dynamic ports! */
        if (prte_static_ports) {
            char *err = PMIX_ARGV_JOIN_COMPAT(prte_rml_base.tcp_static_ports, ',');
            pmix_show_help("help-oob-tcp.txt", "static-and-dynamic", true, err, dyn_port_string);
            free(err);
            return;
        }
        pmix_util_parse_range_options(dyn_port_string, &prte_rml_base.tcp_dyn_ports);
        if (0 == strcmp(prte_rml_base.tcp_dyn_ports[0], "-1")) {
            PMIX_ARGV_FREE_COMPAT(prte_rml_base.tcp_dyn_ports);
            prte_rml_base.tcp_dyn_ports = NULL;
        }
    } else {
        prte_rml_base.tcp_dyn_ports = NULL;
    }

#if PRTE_ENABLE_IPV6
    dyn_port_string6 = NULL;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "dynamic_ipv6_ports",
                                     "Range of ports to be dynamically used by daemons (IPv6)",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &dyn_port_string6);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "dynamic_ipv6_ports",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    /* if ports were provided, parse the provided range */
    if (NULL != dyn_port_string6) {
        /* can't have both static and dynamic ports! */
        if (prte_static_ports) {
            char *err4 = NULL, *err6 = NULL;
            if (NULL != prte_rml_base.tcp_static_ports) {
                err4 = PMIX_ARGV_JOIN_COMPAT(prte_rml_base.tcp_static_ports, ',');
            }
            if (NULL != prte_rml_base.tcp6_static_ports) {
                err6 = PMIX_ARGV_JOIN_COMPAT(prte_rml_base.tcp6_static_ports, ',');
            }
            pmix_show_help("help-oob-tcp.txt", "static-and-dynamic-ipv6", true,
                           (NULL == err4) ? "N/A" : err4, (NULL == err6) ? "N/A" : err6,
                           dyn_port_string6);
            if (NULL != err4) {
                free(err4);
            }
            if (NULL != err6) {
                free(err6);
            }
            return;
        }
        pmix_util_parse_range_options(dyn_port_string6, &prte_rml_base.tcp6_dyn_ports);
        if (0 == strcmp(prte_rml_base.tcp6_dyn_ports[0], "-1")) {
            PMIX_ARGV_FREE_COMPAT(prte_rml_base.tcp6_dyn_ports);
            prte_rml_base.tcp6_dyn_ports = NULL;
        }
    } else {
        prte_rml_base.tcp6_dyn_ports = NULL;
    }
#endif // PRTE_ENABLE_IPV6

    prte_rml_base.disable_ipv4_family = false;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "disable_ipv4_family",
                                     "Disable the IPv4 interfaces",
                                     PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                     &prte_rml_base.disable_ipv4_family);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "disable_ipv4_family",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

#if PRTE_ENABLE_IPV6
    prte_rml_base.disable_ipv6_family = false;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "disable_ipv6_family",
                                     "Disable the IPv6 interfaces",
                                     PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                     &prte_rml_base.disable_ipv6_family);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "disable_ipv6_family",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
#endif // PRTE_ENABLE_IPV6

    // Wait for this amount of time before sending the first keepalive probe
    prte_rml_base.keepalive_time = 300;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "keepalive_time",
                                     "Idle time in seconds before starting to send keepalives (keepalive_time <= 0 disables "
                                      "keepalive functionality)",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &prte_rml_base.keepalive_time);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "keepalive_time",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    // Resend keepalive probe every INT seconds
    prte_rml_base.keepalive_intvl = 20;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "keepalive_intvl",
                                     "Time between successive keepalive pings when peer has not responded, in seconds (ignored "
                                     "if keepalive_time <= 0)",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &prte_rml_base.keepalive_intvl);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "keepalive_intvl",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    // After sending PR probes every INT seconds consider the connection dead
    prte_rml_base.keepalive_probes = 9;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "keepalive_probes",
                                     "Number of keepalives that can be missed before "
                                     "declaring error (ignored if keepalive_time <= 0)",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &prte_rml_base.keepalive_probes);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "keepalive_probes",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    prte_rml_base.retry_delay = 0;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "retry_delay",
                                     "Time (in sec) to wait before trying to connect to peer again",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &prte_rml_base.retry_delay);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "retry_delay",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    prte_rml_base.max_recon_attempts = 10;
    ret = pmix_mca_base_var_register("prte", "rml", "base", "max_recon_attempts",
                                     "Max number of times to attempt connection before giving up (-1 -> never give up)",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &prte_rml_base.max_recon_attempts);
    pmix_mca_base_var_register_synonym(ret, "prte", "oob", "tcp", "max_recon_attempts",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

}

void prte_rml_close(void)
{
    int rc, i;

    // shutdown the listener thread
    if (prte_rml_base.listen_thread_active) {
        prte_rml_base.listen_thread_active = false;
        /* tell the thread to exit */
        rc = write(prte_rml_base.stop_thread[1], &i, sizeof(int));
        if (0 < rc) {
            pmix_thread_join(&prte_rml_base.listen_thread, NULL);
        }

        close(prte_rml_base.stop_thread[0]);
        close(prte_rml_base.stop_thread[1]);

    } else {
        pmix_output_verbose(2, prte_rml_base.rml_output,
                            "listener thread not active");
    }

    /* cleanup listen event list */
    PMIX_LIST_DESTRUCT(&prte_rml_base.listeners);

    pmix_output_verbose(2, prte_rml_base.rml_output, "%s TCP SHUTDOWN done",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    PMIX_LIST_DESTRUCT(&prte_rml_base.posted_recvs);
    PMIX_LIST_DESTRUCT(&prte_rml_base.unmatched_msgs);
    PMIX_LIST_DESTRUCT(&prte_rml_base.children);
    if (0 <= prte_rml_base.rml_output) {
        pmix_output_close(prte_rml_base.rml_output);
    }
}

int prte_rml_open(void)
{
    int rc;

    /* construct object for holding the active plugin modules */
    PMIX_CONSTRUCT(&prte_rml_base.posted_recvs, pmix_list_t);
    PMIX_CONSTRUCT(&prte_rml_base.unmatched_msgs, pmix_list_t);
    PMIX_CONSTRUCT(&prte_rml_base.children, pmix_list_t);

    /* compute the routing tree - only thing we need to know is the
     * number of daemons in the DVM */
    prte_rml_compute_routing_tree();

    // define our lifeline
    prte_rml_base.lifeline = PRTE_PROC_MY_PARENT->rank;

    if (NULL != prte_process_info.my_hnp_uri) {
        /* extract the HNP's name and update the routing table */
        rc = prte_rml_parse_uris(prte_process_info.my_hnp_uri,
                                 PRTE_PROC_MY_HNP,
                                 NULL);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    // get our address(es)
    rc = setup_interfaces();
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    // start listening for connections
    rc = prte_rml_start_listening();
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    // save our URI
    get_addr();

    return PRTE_SUCCESS;
}

static void get_addr(void)
{
    char *cptr = NULL, *tmp, *tp, *tm;

    if (!prte_rml_base.disable_ipv4_family &&
        NULL != prte_rml_base.ipv4conns) {
        tmp = PMIX_ARGV_JOIN_COMPAT(prte_rml_base.ipv4conns, ',');
        tp = PMIX_ARGV_JOIN_COMPAT(prte_rml_base.ipv4ports, ',');
        tm = PMIX_ARGV_JOIN_COMPAT(prte_rml_base.if_masks, ',');
        pmix_asprintf(&cptr, "tcp://%s:%s:%s", tmp, tp, tm);
        free(tmp);
        free(tp);
        free(tm);
    }
#if PRTE_ENABLE_IPV6
    if (!prte_rml_base.disable_ipv6_family &&
        NULL != prte_rml_base.ipv6conns) {
        char *tmp2;

        /* RFC 3986, section 3.2.2
         * The notation in that case is to encode the IPv6 IP number in square brackets:
         * "http://[2001:db8:1f70::999:de8:7648:6e8]:100/"
         * A host identified by an Internet Protocol literal address, version 6 [RFC3513]
         * or later, is distinguished by enclosing the IP literal within square brackets.
         * This is the only place where square bracket characters are allowed in the URI
         * syntax. In anticipation of future, as-yet-undefined IP literal address formats,
         * an implementation may use an optional version flag to indicate such a format
         * explicitly rather than rely on heuristic determination.
         */
        tmp = PMIX_ARGV_JOIN_COMPAT(prte_rml_base.ipv6conns, ',');
        tp = PMIX_ARGV_JOIN_COMPAT(prte_rml_base.ipv6ports, ',');
        tm = PMIX_ARGV_JOIN_COMPAT(prte_rml_base.if_masks, ',');
        if (NULL == cptr) {
            /* no ipv4 stuff */
            pmix_asprintf(&cptr, "tcp6://[%s]:%s:%s", tmp, tp, tm);
        } else {
            pmix_asprintf(&tmp2, "%s;tcp6://[%s]:%s:%s", cptr, tmp, tp, tm);
            free(cptr);
            cptr = tmp2;
        }
        free(tmp);
        free(tp);
        free(tm);
    }
#endif // PRTE_ENABLE_IPV6

    // store the result
    prte_process_info.my_uri = cptr;
}

void prte_rml_send_callback(int status, pmix_proc_t *peer,
                            pmix_data_buffer_t *buffer,
                            prte_rml_tag_t tag, void *cbdata)

{
    PRTE_HIDE_UNUSED_PARAMS(buffer, cbdata);

    if (PRTE_SUCCESS != status) {
        pmix_output_verbose(2, prte_rml_base.rml_output,
                            "%s UNABLE TO SEND MESSAGE TO %s TAG %d: %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(peer), tag,
                            PRTE_ERROR_NAME(status));
        if (PRTE_ERR_NO_PATH_TO_TARGET == status) {
            PRTE_ACTIVATE_PROC_STATE(peer, PRTE_PROC_STATE_NO_PATH_TO_TARGET);
        } else if (PRTE_ERR_ADDRESSEE_UNKNOWN == status) {
            PRTE_ACTIVATE_PROC_STATE(peer, PRTE_PROC_STATE_PEER_UNKNOWN);
        } else {
            PRTE_ACTIVATE_PROC_STATE(peer, PRTE_PROC_STATE_UNABLE_TO_SEND_MSG);
        }
    }
}

static int setup_interfaces(void)
{
    pmix_pif_t *copied_interface, *selected_interface;
    struct sockaddr_storage my_ss;
    /* Larger than necessary, used for copying mask */
    char string[50], **interfaces = NULL;
    int kindex;
    int i, rc;
    bool keeploopback = false;
    bool including = false;

    pmix_output_verbose(5, prte_rml_base.rml_output,
                        "%s: setup_interfaces called",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* if interface include was given, construct a list
     * of those interfaces which match the specifications - remember,
     * the includes could be given as named interfaces, IP addrs, or
     * subnet+mask
     */
    if (NULL != prte_if_include) {
        interfaces = split_and_resolve(&prte_if_include,
                                       "include");
        including = true;
    } else if (NULL != prte_if_exclude) {
        interfaces = split_and_resolve(&prte_if_exclude,
                                       "exclude");
    }

    /* if we are the master, then check the interfaces for loopbacks
     * and keep loopbacks only if no non-loopback interface exists */
    if (PRTE_PROC_IS_MASTER) {
        keeploopback = true;
        PMIX_LIST_FOREACH(selected_interface, &pmix_if_list, pmix_pif_t)
        {
            if (!(selected_interface->if_flags & IFF_LOOPBACK)) {
                keeploopback = false;
                break;
            }
        }
    }

    /* look at all available interfaces */
    PMIX_LIST_FOREACH(selected_interface, &pmix_if_list, pmix_pif_t)
    {
        if ((selected_interface->if_flags & IFF_LOOPBACK) &&
            !keeploopback) {
            continue;
        }


        i = selected_interface->if_index;
        kindex = selected_interface->if_kernel_index;
        memcpy((struct sockaddr *) &my_ss, &selected_interface->if_addr,
               MIN(sizeof(struct sockaddr_storage), sizeof(selected_interface->if_addr)));

        /* ignore non-ip4/6 interfaces */
        if (AF_INET != my_ss.ss_family
#if PRTE_ENABLE_IPV6
            && AF_INET6 != my_ss.ss_family
#endif
            ) {
            continue;
        }

        /* ignore any virtual interfaces */
        if (0 == strncmp(selected_interface->if_name, "vir", 3)) {
            continue;
        }

        /* handle include/exclude directives */
        if (NULL != interfaces) {
            /* check for match */
            rc = pmix_ifmatches(kindex, interfaces);
            /* if one of the network specifications isn't parseable, then
             * error out as we can't do what was requested
             */
            if (PRTE_ERR_NETWORK_NOT_PARSEABLE == rc) {
                pmix_show_help("help-oob-tcp.txt", "not-parseable", true);
                PMIX_ARGV_FREE_COMPAT(interfaces);
                return PRTE_ERR_BAD_PARAM;
            }
            /* if we are including, then ignore this if not present */
            if (including) {
                if (PMIX_SUCCESS != rc) {
                    pmix_output_verbose(20, prte_rml_base.rml_output,
                                        "%s oob:tcp:init rejecting interface %s (not in include list)",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), selected_interface->if_name);
                    continue;
                }
            } else {
                /* we are excluding, so ignore if present */
                if (PMIX_SUCCESS == rc) {
                    pmix_output_verbose(20, prte_rml_base.rml_output,
                                        "%s oob:tcp:init rejecting interface %s (in exclude list)",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), selected_interface->if_name);
                    continue;
                }
            }
        }

        /* Refs ticket #3019
         * it would probably be worthwhile to print out a warning if PRRTE detects multiple
         * IP interfaces that are "up" on the same subnet (because that's a Bad Idea). Note
         * that we should only check for this after applying the relevant include/exclude
         * list MCA params. If we detect redundant ports, we can also automatically ignore
         * them so that applications won't hang.
         */

        /* add this address to our connections */
        if (AF_INET == my_ss.ss_family) {
            pmix_output_verbose(10, prte_rml_base.rml_output,
                                "%s oob:tcp:init adding %s to our list of %s connections",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                pmix_net_get_hostname((struct sockaddr *) &my_ss),
                                (AF_INET == my_ss.ss_family) ? "V4" : "V6");
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&prte_rml_base.ipv4conns,
                                    pmix_net_get_hostname((struct sockaddr *) &my_ss));
        } else if (AF_INET6 == my_ss.ss_family) {
#if PRTE_ENABLE_IPV6
            pmix_output_verbose(10, prte_rml_base.rml_output,
                                "%s oob:tcp:init adding %s to our list of %s connections",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                pmix_net_get_hostname((struct sockaddr *) &my_ss),
                                (AF_INET == my_ss.ss_family) ? "V4" : "V6");
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&prte_rml_base.ipv6conns,
                                    pmix_net_get_hostname((struct sockaddr *) &my_ss));
#endif // PRTE_ENABLE_IPV6
        } else {
            pmix_output_verbose(10, prte_rml_base.rml_output,
                                "%s oob:tcp:init ignoring %s from out list of connections",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                pmix_net_get_hostname((struct sockaddr *) &my_ss));
            continue;
        }
        copied_interface = PMIX_NEW(pmix_pif_t);
        if (NULL == copied_interface) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        pmix_string_copy(copied_interface->if_name, selected_interface->if_name, PMIX_IF_NAMESIZE);
        copied_interface->if_index = i;
        copied_interface->if_kernel_index = kindex;
        copied_interface->af_family = my_ss.ss_family;
        copied_interface->if_flags = selected_interface->if_flags;
        copied_interface->if_speed = selected_interface->if_speed;
        memcpy(&copied_interface->if_addr, &selected_interface->if_addr,
               sizeof(struct sockaddr_storage));
        copied_interface->if_mask = selected_interface->if_mask;
        /* If bandwidth is not found, set to arbitrary non zero value */
        copied_interface->if_bandwidth = selected_interface->if_bandwidth > 0
                                             ? selected_interface->if_bandwidth
                                             : 1;
        memcpy(&copied_interface->if_mac, &selected_interface->if_mac,
               sizeof(copied_interface->if_mac));
        copied_interface->ifmtu = selected_interface->ifmtu;
        /* Add the if_mask to the list */
        sprintf(string, "%d", selected_interface->if_mask);
        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&prte_rml_base.if_masks, string);
        pmix_list_append(&prte_rml_base.local_ifs, &(copied_interface->super));
    }

    if (0 == PMIX_ARGV_COUNT_COMPAT(prte_rml_base.ipv4conns)
#if PRTE_ENABLE_IPV6
        && 0 == PMIX_ARGV_COUNT_COMPAT(prte_rml_base.ipv6conns)
#endif
    ) {
        return PRTE_ERR_NOT_AVAILABLE;
    }

    return PRTE_SUCCESS;
}

/*
 * Go through a list of argv; if there are any subnet specifications
 * (a.b.c.d/e), resolve them to an interface name (Currently only
 * supporting IPv4).  If unresolvable, warn and remove.
 */
static char **split_and_resolve(char **orig_str, char *name)
{
    pmix_pif_t *selected_interface;
    int i, n, ret, match_count, interface_count;
    char **argv, **interfaces, *str, *tmp;
    char if_name[IF_NAMESIZE];
    struct sockaddr_storage argv_inaddr, if_inaddr;
    uint32_t argv_prefix;

    /* Sanity check */
    if (NULL == orig_str || NULL == *orig_str) {
        return NULL;
    }

    argv = PMIX_ARGV_SPLIT_COMPAT(*orig_str, ',');
    if (NULL == argv) {
        return NULL;
    }
    interface_count = 0;
    interfaces = NULL;
    for (i = 0; NULL != argv[i]; ++i) {
        if (isalpha(argv[i][0])) {
            /* This is an interface name. If not already in the interfaces array, add it */
            for (n = 0; n < interface_count; n++) {
                if (0 == strcmp(argv[i], interfaces[n])) {
                    break;
                }
            }
            if (n == interface_count) {
                pmix_output_verbose(20,
                                    prte_rml_base.rml_output,
                                    "oob:tcp: Using interface: %s ", argv[i]);
                PMIX_ARGV_APPEND_NOSIZE_COMPAT(&interfaces, argv[i]);
                ++interface_count;
            }
            continue;
        }

        /* Found a subnet notation.  Convert it to an IP
           address/netmask.  Get the prefix first. */
        argv_prefix = 0;
        tmp = strdup(argv[i]);
        str = strchr(argv[i], '/');
        if (NULL == str) {
            pmix_show_help("help-oob-tcp.txt", "invalid if_inexclude",
                           true, name, prte_process_info.nodename,
                           tmp, "Invalid specification (missing \"/\")");
            free(argv[i]);
            free(tmp);
            continue;
        }
        *str = '\0';
        argv_prefix = atoi(str + 1);

        /* Now convert the IPv4 address */
        ((struct sockaddr*) &argv_inaddr)->sa_family = AF_INET;
        ret = inet_pton(AF_INET, argv[i],
                        &((struct sockaddr_in*) &argv_inaddr)->sin_addr);
        free(argv[i]);

        if (1 != ret) {
            pmix_show_help("help-oob-tcp.txt", "invalid if_inexclude",
                           true, name, prte_process_info.nodename, tmp,
                           "Invalid specification (inet_pton() failed)");
            free(tmp);
            continue;
        }
        pmix_output_verbose(20, prte_rml_base.rml_output,
                            "%s oob:tcp: Searching for %s address+prefix: %s / %u",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            name,
                            pmix_net_get_hostname((struct sockaddr*) &argv_inaddr),
                            argv_prefix);

        /* Go through all interfaces and see if we can find a match */
        match_count = 0;
        PMIX_LIST_FOREACH(selected_interface, &pmix_if_list, pmix_pif_t) {
            pmix_ifindextoaddr(selected_interface->if_kernel_index,
                               (struct sockaddr*) &if_inaddr,
                               sizeof(if_inaddr));
            if (pmix_net_samenetwork((struct sockaddr_storage*) &argv_inaddr,
                                     (struct sockaddr_storage*) &if_inaddr,
                                     argv_prefix)) {
                /* We found a match. If it's not already in the interfaces array,
                   add it. If it's already in the array, treat it as a match */
                match_count = match_count + 1;
                pmix_ifindextoname(selected_interface->if_kernel_index, if_name, sizeof(if_name));
                for (n = 0; n < interface_count; n++) {
                    if (0 == strcmp(if_name, interfaces[n])) {
                        break;
                    }
                }
                if (n == interface_count) {
                    pmix_output_verbose(20,
                                        prte_rml_base.rml_output,
                                        "oob:tcp: Found match: %s (%s)",
                                        pmix_net_get_hostname((struct sockaddr*) &if_inaddr),
                                        if_name);
                    PMIX_ARGV_APPEND_NOSIZE_COMPAT(&interfaces, if_name);
                    ++interface_count;
                }
            }
        }
        /* If we didn't find a match, keep trying */
        if (0 == match_count) {
            pmix_show_help("help-oob-tcp.txt", "invalid if_inexclude",
                           true, name, prte_process_info.nodename, tmp,
                           "Did not find interface matching this subnet");
            free(tmp);
            continue;
        }

        free(tmp);
    }

    /* Mark the end of the interface name array with NULL */
    if (NULL != interfaces) {
        interfaces[interface_count] = NULL;
    }
    free(argv);
    free(*orig_str);
    *orig_str = PMIX_ARGV_JOIN_COMPAT(interfaces, ',');
    return interfaces;
}

/***   RML CLASS INSTANCES   ***/
static void send_cons(prte_rml_send_t *ptr)
{
    ptr->retries = 0;
    ptr->cbdata = NULL;
    ptr->dbuf = NULL;
    ptr->seq_num = 0xFFFFFFFF;
}
static void send_des(prte_rml_send_t *ptr)
{
    if (ptr->dbuf != NULL)
        PMIX_DATA_BUFFER_RELEASE(ptr->dbuf);
}
PMIX_CLASS_INSTANCE(prte_rml_send_t, pmix_list_item_t, send_cons, send_des);

static void send_req_cons(prte_rml_send_request_t *ptr)
{
    PMIX_CONSTRUCT(&ptr->send, prte_rml_send_t);
}
static void send_req_des(prte_rml_send_request_t *ptr)
{
    PMIX_DESTRUCT(&ptr->send);
}
PMIX_CLASS_INSTANCE(prte_rml_send_request_t, pmix_object_t, send_req_cons, send_req_des);

static void recv_cons(prte_rml_recv_t *ptr)
{
    ptr->dbuf = NULL;
}
static void recv_des(prte_rml_recv_t *ptr)
{
    if (ptr->dbuf != NULL)
        PMIX_DATA_BUFFER_RELEASE(ptr->dbuf);
}
PMIX_CLASS_INSTANCE(prte_rml_recv_t, pmix_list_item_t, recv_cons, recv_des);

static void rcv_cons(prte_rml_recv_cb_t *ptr)
{
    PMIX_DATA_BUFFER_CONSTRUCT(&ptr->data);
    ptr->active = false;
}
static void rcv_des(prte_rml_recv_cb_t *ptr)
{
    PMIX_DATA_BUFFER_DESTRUCT(&ptr->data);
}
PMIX_CLASS_INSTANCE(prte_rml_recv_cb_t, pmix_object_t, rcv_cons, rcv_des);

static void prcv_cons(prte_rml_posted_recv_t *ptr)
{
    ptr->cbdata = NULL;
}
PMIX_CLASS_INSTANCE(prte_rml_posted_recv_t, pmix_list_item_t, prcv_cons, NULL);

static void prq_cons(prte_rml_recv_request_t *ptr)
{
    ptr->cancel = false;
    ptr->post = PMIX_NEW(prte_rml_posted_recv_t);
}
static void prq_des(prte_rml_recv_request_t *ptr)
{
    if (NULL != ptr->post) {
        PMIX_RELEASE(ptr->post);
    }
}
PMIX_CLASS_INSTANCE(prte_rml_recv_request_t, pmix_object_t, prq_cons, prq_des);

static void rtcon(prte_routed_tree_t *rt)
{
    rt->rank = PMIX_RANK_INVALID;
    PMIX_CONSTRUCT(&rt->relatives, pmix_bitmap_t);
}
static void rtdes(prte_routed_tree_t *rt)
{
    PMIX_DESTRUCT(&rt->relatives);
}
PMIX_CLASS_INSTANCE(prte_routed_tree_t,
                    pmix_list_item_t,
                    rtcon, rtdes);

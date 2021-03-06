/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2016 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2016      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "wait_sync.h"

static prte_mutex_t wait_sync_lock = PRTE_MUTEX_STATIC_INIT;
static prte_wait_sync_t *wait_sync_list = NULL;

#define PRTE_WAIT_SYNC_PASS_OWNERSHIP(who)      \
    do {                                        \
        pthread_mutex_lock(&(who)->lock);       \
        pthread_cond_signal(&(who)->condition); \
        pthread_mutex_unlock(&(who)->lock);     \
    } while (0)

int prte_sync_wait_mt(prte_wait_sync_t *sync)
{
    /* Don't stop if the waiting synchronization is completed. We avoid the
     * race condition around the release of the synchronization using the
     * signaling field.
     */
    if (sync->count <= 0)
        return (0 == sync->status) ? PRTE_SUCCESS : PRTE_ERROR;

    /* lock so nobody can signal us during the list updating */
    pthread_mutex_lock(&sync->lock);

    /* Now that we hold the lock make sure another thread has not already
     * call cond_signal.
     */
    if (sync->count <= 0) {
        pthread_mutex_unlock(&sync->lock);
        return (0 == sync->status) ? PRTE_SUCCESS : PRTE_ERROR;
    }

    /* Insert sync on the list of pending synchronization constructs */
    prte_mutex_lock(&wait_sync_lock);
    if (NULL == wait_sync_list) {
        sync->next = sync->prev = sync;
        wait_sync_list = sync;
    } else {
        sync->prev = wait_sync_list->prev;
        sync->prev->next = sync;
        sync->next = wait_sync_list;
        wait_sync_list->prev = sync;
    }
    prte_mutex_unlock(&wait_sync_lock);

    /**
     * If we are not responsible for progresing, go silent until something worth noticing happen:
     *  - this thread has been promoted to take care of the progress
     *  - our sync has been triggered.
     */
check_status:
    if (sync != wait_sync_list) {
        pthread_cond_wait(&sync->condition, &sync->lock);

        /**
         * At this point either the sync was completed in which case
         * we should remove it from the wait list, or/and I was
         * promoted as the progress manager.
         */

        if (sync->count <= 0) { /* Completed? */
            pthread_mutex_unlock(&sync->lock);
            goto i_am_done;
        }
        /* either promoted, or spurious wakeup ! */
        goto check_status;
    }

    pthread_mutex_unlock(&sync->lock);
    while (sync->count > 0) { /* progress till completion */
    }
    assert(sync == wait_sync_list);

i_am_done:
    /* My sync is now complete. Trim the list: remove self, wake next */
    prte_mutex_lock(&wait_sync_lock);
    sync->prev->next = sync->next;
    sync->next->prev = sync->prev;
    /* In case I am the progress manager, pass the duties on */
    if (sync == wait_sync_list) {
        wait_sync_list = (sync == sync->next) ? NULL : sync->next;
        if (NULL != wait_sync_list)
            PRTE_WAIT_SYNC_PASS_OWNERSHIP(wait_sync_list);
    }
    prte_mutex_unlock(&wait_sync_lock);

    return (0 == sync->status) ? PRTE_SUCCESS : PRTE_ERROR;
}

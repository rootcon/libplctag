/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever     *
 * you choose.                                                             *
 *                                                                         *
 * MPL 2.0:                                                                *
 *                                                                         *
 *   This Source Code Form is subject to the terms of the Mozilla Public   *
 *   License, v. 2.0. If a copy of the MPL was not distributed with this   *
 *   file, You can obtain one at http://mozilla.org/MPL/2.0/.              *
 *                                                                         *
 *                                                                         *
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#define LIBPLCTAGDLL_EXPORTS (1)

#include <libplctag/lib/libplctag.h>

#include <ctype.h>
#include <float.h>
#include <inttypes.h>
#include <libplctag/lib/init.h>
#include <libplctag/lib/tag.h>
#include <libplctag/lib/version.h>
#include <libplctag/protocols/ab/ab.h>
#include <libplctag/protocols/mb/modbus.h>
#include <limits.h>
#include <platform.h>
#include <stdlib.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/hash.h>
#include <utils/hashtable.h>
#include <utils/random_utils.h>
#include <utils/rc.h>
#include <utils/vector.h>


#define INITIAL_TAG_TABLE_SIZE (201)

#define TAG_ID_MASK (0xFFFFFFF)

#define MAX_TAG_MAP_ATTEMPTS (50)

/* these are only internal to the file */

static volatile int32_t next_tag_id = 10; /* MAGIC */
static volatile hashtable_p tags = NULL;
static mutex_p tag_lookup_mutex = NULL;

atomic_bool library_terminating = false;

static thread_p tag_tickler_thread = NULL;
static cond_p tag_tickler_wait = NULL;
#define TAG_TICKLER_TIMEOUT_MS (100)
#define TAG_TICKLER_TIMEOUT_MIN_MS (10)
static int64_t tag_tickler_wait_timeout_end = 0;

// static mutex_p global_library_mutex = NULL;


/* helper functions. */
static plc_tag_p lookup_tag(int32_t id);
static int add_tag_lookup(plc_tag_p tag);
static int tag_id_inc(int id);
static THREAD_FUNC(tag_tickler_func);
static int plc_tag_abort_impl(plc_tag_p tag);
static int set_tag_byte_order(plc_tag_p tag, attr attribs);
static int check_byte_order_str(const char *byte_order, int length);
static int get_string_total_length_unsafe(plc_tag_p tag, int string_start_offset);
static int get_string_length_unsafe(plc_tag_p tag, int offset);
static int resize_tag_buffer_at_offset_unsafe(plc_tag_p tag, int old_split_index, int new_split_index);
static int resize_tag_buffer_unsafe(plc_tag_p tag, int new_size);
static int get_new_string_total_length_unsafe(plc_tag_p tag, const char *string_val);


#ifdef LIPLCTAGDLL_EXPORTS
#    if defined(_WIN32) || (defined(_WIN64)
#        include <process.h>
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch(fdwReason) {
        case DLL_PROCESS_ATTACH:
            // fprintf(stderr, "DllMain called with DLL_PROCESS_ATTACH\n");
            break;

        case DLL_PROCESS_DETACH:
            // fprintf(stderr, "DllMain called with DLL_PROCESS_DETACH\n");
            plc_tag_shutdown();
            break;

        case DLL_THREAD_ATTACH:
            // fprintf(stderr, "DllMain called with DLL_THREAD_ATTACH\n");
            break;

        case DLL_THREAD_DETACH:
            // fprintf(stderr, "DllMain called with DLL_THREAD_DETACH\n");
            break;

        default:
            // fprintf(stderr, "DllMain called with unexpected code %d!\n", fdwReason);
            break;
    }

    return TRUE;
}
#    endif
#endif

/*
 * Initialize the library.  This is called in a threadsafe manner and
 * only called once.
 */

int lib_init(void) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    atomic_set_bool(&library_terminating, false);

    pdebug(DEBUG_INFO, "Setting up global library data.");

    pdebug(DEBUG_INFO, "Creating tag hashtable.");
    if((tags = hashtable_create(INITIAL_TAG_TABLE_SIZE)) == NULL) { /* MAGIC */
        pdebug(DEBUG_ERROR, "Unable to create tag hashtable!");
        return PLCTAG_ERR_NO_MEM;
    }

    pdebug(DEBUG_INFO, "Creating tag hashtable mutex.");
    rc = mutex_create((mutex_p *)&tag_lookup_mutex);
    if(rc != PLCTAG_STATUS_OK) { pdebug(DEBUG_ERROR, "Unable to create tag hashtable mutex!"); }

    pdebug(DEBUG_INFO, "Creating tag condition variable.");
    rc = cond_create((cond_p *)&tag_tickler_wait);
    if(rc != PLCTAG_STATUS_OK) { pdebug(DEBUG_ERROR, "Unable to create tag condition var!"); }

    pdebug(DEBUG_INFO, "Creating tag tickler thread.");
    rc = thread_create(&tag_tickler_thread, tag_tickler_func, 32 * 1024, NULL);
    if(rc != PLCTAG_STATUS_OK) { pdebug(DEBUG_ERROR, "Unable to create tag tickler thread!"); }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


void lib_teardown(void) {
    pdebug(DEBUG_INFO, "Tearing down library.");

    atomic_set_bool(&library_terminating, true);

    if(tag_tickler_wait) {
        pdebug(DEBUG_INFO, "Signaling tag tickler condition var.");
        cond_signal(tag_tickler_wait);
    }

    if(tag_tickler_thread) {
        pdebug(DEBUG_INFO, "Tearing down tag tickler thread.");
        thread_join(tag_tickler_thread);
        thread_destroy(&tag_tickler_thread);
        tag_tickler_thread = NULL;
    }

    if(tag_tickler_wait) {
        pdebug(DEBUG_INFO, "Tearing down tag tickler condition var.");
        cond_destroy(&tag_tickler_wait);
        tag_tickler_wait = NULL;
    }

    if(tag_lookup_mutex) {
        pdebug(DEBUG_INFO, "Tearing down tag lookup mutex.");
        mutex_destroy(&tag_lookup_mutex);
        tag_lookup_mutex = NULL;
    }

    if(tags) {
        pdebug(DEBUG_INFO, "Destroying tag hashtable.");
        hashtable_destroy(tags);
        tags = NULL;
    }

    atomic_set_bool(&library_terminating, false);

    pdebug(DEBUG_INFO, "Done.");
}


int plc_tag_tickler_wake_impl(const char *func, int line_num) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting. Called from %s:%d.", func, line_num);

    if(!tag_tickler_wait) {
        pdebug(DEBUG_WARN, "Called from %s:%d when tag tickler condition var is NULL!", func, line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = cond_signal(tag_tickler_wait);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error %s trying to signal condition variable in call from %s:%d", plc_tag_decode_error(rc), func,
               line_num);
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done. Called from %s:%d.", func, line_num);

    return rc;
}


int plc_tag_generic_wake_tag_impl(const char *func, int line_num, plc_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting. Called from %s:%d.", func, line_num);

    if(!tag) {
        pdebug(DEBUG_WARN, "Called from %s:%d when tag is NULL!", func, line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!tag->tag_cond_wait) {
        pdebug(DEBUG_WARN, "Called from %s:%d when tag condition var is NULL!", func, line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = cond_signal(tag->tag_cond_wait);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error %s trying to signal condition variable in call from %s:%d", plc_tag_decode_error(rc), func,
               line_num);
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done. Called from %s:%d.", func, line_num);

    return rc;
}


/*
 * plc_tag_generic_tickler
 *
 * This implements the protocol-independent tickling functions such as handling
 * automatic tag operations and callbacks.
 */

void plc_tag_generic_tickler(plc_tag_p tag) {
    if(tag) {
        debug_set_tag_id(tag->tag_id);

        pdebug(DEBUG_DETAIL, "Tickling tag %d.", tag->tag_id);

        /* first check for aborts */
        if(atomic_get_bool(&tag->abort_requested)) {
            if(tag->vtable && tag->vtable->abort) { tag->vtable->abort(tag); }

            pdebug(DEBUG_DETAIL, "Aborting ongoing operation if any!");

            tag->read_complete = 0;
            tag->read_in_flight = 0;
            tag->write_complete = 0;
            tag->write_in_flight = 0;

            /* clear the flag so we do not do this again. */
            atomic_set_bool(&tag->abort_requested, false);

            tag_raise_event(tag, PLCTAG_EVENT_ABORTED, PLCTAG_ERR_ABORT);

            /* do not do anything else. */
            return;
        }

        /* if this tag has automatic writes, then there are many things we should check */
        if(tag->auto_sync_write_ms > 0) {
            /* has the tag been written to? */
            if(tag->tag_is_dirty) {
                /* abort any in flight read if the tag is dirty. */
                if(tag->read_in_flight) {
                    if(tag->vtable && tag->vtable->abort) { tag->vtable->abort(tag); }

                    pdebug(DEBUG_DETAIL, "Aborting in-flight automatic read!");

                    tag->read_complete = 0;
                    tag->read_in_flight = 0;

                    /* TODO - should we report an ABORT event here? */
                    // tag->event_operation_aborted = 1;
                    tag_raise_event(tag, PLCTAG_EVENT_ABORTED, PLCTAG_ERR_ABORT);
                }

                /* have we already done something about automatic reads? */
                if(!tag->auto_sync_next_write) {
                    /* we need to queue up a new write. */
                    tag->auto_sync_next_write = time_ms() + tag->auto_sync_write_ms;

                    pdebug(DEBUG_DETAIL, "Queueing up automatic write in %dms.", tag->auto_sync_write_ms);
                } else if(!tag->write_in_flight && tag->auto_sync_next_write <= time_ms()) {
                    pdebug(DEBUG_DETAIL, "Triggering automatic write start.");

                    /* clear out any outstanding reads. */
                    if(tag->read_in_flight) {
                        if(tag->vtable && tag->vtable->abort) { tag->vtable->abort(tag); }

                        tag->read_in_flight = 0;
                    }

                    tag->tag_is_dirty = 0;
                    tag->write_in_flight = 1;
                    tag->auto_sync_next_write = 0;

                    if(tag->vtable && tag->vtable->write) { tag->status = (int8_t)tag->vtable->write(tag); }

                    // tag->event_write_started = 1;
                    tag_raise_event(tag, PLCTAG_EVENT_WRITE_STARTED, tag->status);
                }
            }
        }

        /* if this tag has automatic reads, we need to check that state too. */
        if(tag->auto_sync_read_ms > 0) {
            int64_t current_time = time_ms();

            // /* spread these out randomly to avoid too much clustering. */
            // if(tag->auto_sync_next_read == 0) {
            //     tag->auto_sync_next_read = current_time - (rand() % tag->auto_sync_read_ms);
            // }

            /* do we need to read? */
            if(tag->auto_sync_next_read < current_time) {
                /* make sure that we do not have an outstanding read or write. */
                if(!tag->read_in_flight && !tag->tag_is_dirty && !tag->write_in_flight) {
                    int64_t periods = 0;

                    pdebug(DEBUG_DETAIL, "Triggering automatic read start.");

                    tag->read_in_flight = 1;

                    if(tag->vtable && tag->vtable->read) { tag->status = (int8_t)tag->vtable->read(tag); }

                    // tag->event_read_started = 1;
                    tag_raise_event(tag, PLCTAG_EVENT_READ_STARTED, tag->status);

                    /*
                     * schedule the next read.
                     *
                     * Note that there will be some jitter.  In that case we want to skip
                     * to the next read time that is a whole multiple of the read period.
                     *
                     * This keeps the jitter from slowly moving the polling cycle.
                     *
                     * Round up to the next period.
                     */
                    periods = (current_time - tag->auto_sync_next_read + (tag->auto_sync_read_ms - 1)) / tag->auto_sync_read_ms;

                    /* warn if we need to skip more than one period. */
                    if(periods > 1) {
                        pdebug(DEBUG_WARN, "Skipping %" PRId64 " periods of %" PRId32 "ms.", periods, tag->auto_sync_read_ms);
                    }

                    tag->auto_sync_next_read += (periods * tag->auto_sync_read_ms);
                    pdebug(DEBUG_DETAIL, "Scheduling next read at time %" PRId64 ".", tag->auto_sync_next_read);
                } else {
                    pdebug(DEBUG_SPEW,
                           "Unable to start auto read tag->read_in_flight=%d, tag->tag_is_dirty=%d, tag->write_in_flight=%d!",
                           tag->read_in_flight, tag->tag_is_dirty, tag->write_in_flight);
                }
            }
        }
    } else {
        pdebug(DEBUG_WARN, "Called with null tag pointer!");
    }

    pdebug(DEBUG_DETAIL, "Done.");

    debug_set_tag_id(0);
}


void plc_tag_generic_handle_event_callbacks(plc_tag_p tag) {
    critical_block(tag->api_mutex) {
        /* call the callbacks outside the API mutex. */
        if(tag && tag->callback) {
            debug_set_tag_id(tag->tag_id);

            /* trigger this if there is any other event. Only once. */
            if(tag->event_creation_complete) {
                pdebug(DEBUG_DETAIL, "Tag creation complete with status %s.",
                       plc_tag_decode_error(tag->event_creation_complete_status));
                tag->callback(tag->tag_id, PLCTAG_EVENT_CREATED, tag->event_creation_complete_status, tag->userdata);
                tag->event_creation_complete = 0;
                tag->event_creation_complete_status = PLCTAG_STATUS_OK;
            }

            /* was there a read start? */
            if(tag->event_read_started) {
                pdebug(DEBUG_DETAIL, "Tag read started with status %s.", plc_tag_decode_error(tag->event_read_started_status));
                tag->callback(tag->tag_id, PLCTAG_EVENT_READ_STARTED, tag->event_read_started_status, tag->userdata);
                tag->event_read_started = 0;
                tag->event_read_started_status = PLCTAG_STATUS_OK;
            }

            /* was there a write start? */
            if(tag->event_write_started) {
                pdebug(DEBUG_DETAIL, "Tag write started with status %s.", plc_tag_decode_error(tag->event_write_started_status));
                tag->callback(tag->tag_id, PLCTAG_EVENT_WRITE_STARTED, tag->event_write_started_status, tag->userdata);
                tag->event_write_started = 0;
                tag->event_write_started_status = PLCTAG_STATUS_OK;
            }

            /* was there an abort? */
            if(tag->event_operation_aborted) {
                pdebug(DEBUG_DETAIL, "Tag operation aborted with status %s.",
                       plc_tag_decode_error(tag->event_operation_aborted_status));
                tag->callback(tag->tag_id, PLCTAG_EVENT_ABORTED, tag->event_operation_aborted_status, tag->userdata);
                tag->event_operation_aborted = 0;
                tag->event_operation_aborted_status = PLCTAG_STATUS_OK;
            }

            /* was there a read completion? */
            if(tag->event_read_complete) {
                pdebug(DEBUG_DETAIL, "Tag read completed with status %s.", plc_tag_decode_error(tag->event_read_complete_status));
                tag->callback(tag->tag_id, PLCTAG_EVENT_READ_COMPLETED, tag->event_read_complete_status, tag->userdata);
                tag->event_read_complete = 0;
                tag->event_read_complete_status = PLCTAG_STATUS_OK;
            }

            /* was there a write completion? */
            if(tag->event_write_complete) {
                pdebug(DEBUG_DETAIL, "Tag write completed with status %s.",
                       plc_tag_decode_error(tag->event_write_complete_status));
                tag->callback(tag->tag_id, PLCTAG_EVENT_WRITE_COMPLETED, tag->event_write_complete_status, tag->userdata);
                tag->event_write_complete = 0;
                tag->event_write_complete_status = PLCTAG_STATUS_OK;
            }

            /* do this last so that we raise all other events first. we only start deletion events. */
            if(tag->event_deletion_started) {
                pdebug(DEBUG_DETAIL, "Tag deletion started with status %s.",
                       plc_tag_decode_error(tag->event_creation_complete_status));
                tag->callback(tag->tag_id, PLCTAG_EVENT_DESTROYED, tag->event_deletion_started_status, tag->userdata);
                tag->event_deletion_started = 0;
                tag->event_deletion_started_status = PLCTAG_STATUS_OK;
            }

            debug_set_tag_id(0);
        }
    } /* end of API mutex critical area. */
}


int plc_tag_generic_init_tag(plc_tag_p tag, attr attribs,
                             void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata), void *userdata) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* get the connection group ID here rather than in each PLC specific tag type. */
    tag->connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    if(tag->connection_group_id < 0 || tag->connection_group_id > 32767) {
        pdebug(DEBUG_WARN, "Connection group ID must be between 0 and 32767, inclusive, but was %d!", tag->connection_group_id);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    rc = mutex_create(&(tag->ext_mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create tag external mutex!");
        return PLCTAG_ERR_CREATE;
    }

    rc = mutex_create(&(tag->api_mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create tag API mutex!");
        return PLCTAG_ERR_CREATE;
    }

    rc = cond_create(&(tag->tag_cond_wait));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create tag condition variable!");
        return PLCTAG_ERR_CREATE;
    }

    /* do this early so that events can be raised early. */
    tag->callback = tag_callback_func;
    tag->userdata = userdata;

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


THREAD_FUNC(tag_tickler_func) {
    (void)arg;

    debug_set_tag_id(0);

    pdebug(DEBUG_INFO, "Starting.");

    while(!atomic_get_bool(&library_terminating)) {
        int max_index = 0;
        int64_t timeout_wait_ms = TAG_TICKLER_TIMEOUT_MS;

        /* what is the maximum time we will wait until */
        tag_tickler_wait_timeout_end = time_ms() + timeout_wait_ms;

        critical_block(tag_lookup_mutex) { max_index = hashtable_capacity(tags); }

        for(int i = 0; i < max_index; i++) {
            plc_tag_p tag = NULL;

            critical_block(tag_lookup_mutex) {
                /* look up the max index again. it may have changed. */
                max_index = hashtable_capacity(tags);

                if(i < max_index) {
                    tag = hashtable_get_index(tags, i);

                    if(tag) {
                        debug_set_tag_id(tag->tag_id);
                        pdebug(DEBUG_SPEW, "rc_inc: Acquiring reference to tag %" PRId32 ".", tag->tag_id);
                        tag = rc_inc(tag);
                    }
                } else {
                    debug_set_tag_id(0);
                    tag = NULL;
                }
            }

            if(tag) {
                debug_set_tag_id(tag->tag_id);

                if(!tag->skip_tickler) {
                    pdebug(DEBUG_DETAIL, "Tickling tag %d.", tag->tag_id);

                    /* try to hold the tag API mutex while all this goes on. */
                    if(mutex_try_lock(tag->api_mutex) == PLCTAG_STATUS_OK) {
                        plc_tag_generic_tickler(tag);

                        /* call the tickler function if we can. */
                        if(tag->vtable && tag->vtable->tickler) {
                            /* call the tickler on the tag. */
                            tag->vtable->tickler(tag);

                            if(tag->read_complete) {
                                tag->read_complete = 0;
                                tag->read_in_flight = 0;

                                // tag->event_read_complete = 1;
                                tag_raise_event(tag, PLCTAG_EVENT_READ_COMPLETED, tag->status);

                                /* wake immediately */
                                plc_tag_tickler_wake();
                                cond_signal(tag->tag_cond_wait);
                            }

                            if(tag->write_complete) {
                                tag->write_complete = 0;
                                tag->write_in_flight = 0;
                                tag->auto_sync_next_write = 0;

                                // tag->event_write_complete = 1;
                                tag_raise_event(tag, PLCTAG_EVENT_WRITE_COMPLETED, tag->status);

                                /* wake immediately */
                                plc_tag_tickler_wake();
                                cond_signal(tag->tag_cond_wait);
                            }
                        }

                        /* wake up earlier if the time until the next write wake up is sooner. */
                        if(tag->auto_sync_next_write && tag->auto_sync_next_write < tag_tickler_wait_timeout_end) {
                            tag_tickler_wait_timeout_end = tag->auto_sync_next_write;
                        }

                        /* wake up earlier if the time until the next read wake up is sooner. */
                        if(tag->auto_sync_next_read && tag->auto_sync_next_read < tag_tickler_wait_timeout_end) {
                            tag_tickler_wait_timeout_end = tag->auto_sync_next_read;
                        }

                        /* we are done with the tag API mutex now. */
                        mutex_unlock(tag->api_mutex);

                        /* call callbacks */
                        plc_tag_generic_handle_event_callbacks(tag);
                    } else {
                        pdebug(DEBUG_DETAIL, "Skipping tag as it is already locked.");
                    }

                } else {
                    pdebug(DEBUG_DETAIL, "Tag has its own tickler.");
                }

                // pdebug(DEBUG_DETAIL, "Current time %" PRId64 ".", time_ms());
                // pdebug(DEBUG_DETAIL, "Time to wake %" PRId64 ".", tag_tickler_wait_timeout_end);
                // pdebug(DEBUG_DETAIL, "Auto read time %" PRId64 ".", tag->auto_sync_next_read);
                // pdebug(DEBUG_DETAIL, "Auto write time %" PRId64 ".", tag->auto_sync_next_write);

                debug_set_tag_id(0);
            }

            if(tag) { rc_dec(tag); }

            debug_set_tag_id(0);
        }

        if(tag_tickler_wait) {
            int64_t time_to_wait = tag_tickler_wait_timeout_end - time_ms();
            int wait_rc = PLCTAG_STATUS_OK;

            if(time_to_wait < TAG_TICKLER_TIMEOUT_MIN_MS) { time_to_wait = TAG_TICKLER_TIMEOUT_MIN_MS; }

            if(time_to_wait > 0) {
                wait_rc = cond_wait(tag_tickler_wait, (int)time_to_wait);
                if(wait_rc == PLCTAG_ERR_TIMEOUT) {
                    pdebug(DEBUG_DETAIL, "Tag tickler thread timed out waiting for something to do.");
                }
            } else {
                pdebug(DEBUG_DETAIL, "Not waiting as time to wake is in the past.");
            }
        }
    }

    debug_set_tag_id(0);

    pdebug(DEBUG_INFO, "Terminating.");

    THREAD_RETURN(0);
}


/* must be called with a valid tag pointer and the API mutex held!*/
static int plc_tag_abort_impl(plc_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /*
     * flag an abort request before we wait on the mutex.
     * Otherwise we will wait until any long running operation
     * completes (such as a plc_tag_read() call with a wait).
     */

    atomic_set_bool(&tag->abort_requested, true);

    /* if the tag does not use a tickler, then wake the PLC thread */
    if(tag->vtable && tag->vtable->wake_plc) { rc = tag->vtable->wake_plc(tag); }

    /* wake the tag tickler */
    plc_tag_tickler_wake();

    /*
     * this blocks until we get the mutex by which time any
     * long running operation is done.
     */
    critical_block(tag->api_mutex) {
        tag->read_cache_expire = (uint64_t)0;

        /* Is the abort flag still set? This may be synchronous. */
        if(atomic_get_bool(&tag->abort_requested) && tag->vtable && tag->vtable->abort) {
            rc = tag->vtable->abort(tag);

            /* release the kraken... or tickler */
            plc_tag_tickler_wake();
        } else {
            pdebug(DEBUG_WARN, "Tag does not have an abort function.");
            rc = PLCTAG_ERR_NOT_IMPLEMENTED;
        }

        tag->read_in_flight = 0;
        tag->read_complete = 0;
        tag->write_in_flight = 0;
        tag->write_complete = 0;

        tag_raise_event(tag, PLCTAG_EVENT_ABORTED, PLCTAG_ERR_ABORT);
    }

    plc_tag_generic_handle_event_callbacks(tag);

    return rc;
}


static int plc_tag_status_impl(plc_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    critical_block(tag->api_mutex) {
        if(tag->vtable && tag->vtable->tickler) { tag->vtable->tickler(tag); }

        if(tag->vtable && tag->vtable->status) {
            rc = tag->vtable->status(tag);
        } else {
            rc = PLCTAG_ERR_NOT_IMPLEMENTED;
        }

        if(rc == PLCTAG_STATUS_OK) {
            if(tag->read_in_flight || tag->write_in_flight) { rc = PLCTAG_STATUS_PENDING; }
        }
    }

    return rc;
}


/**************************************************************************
 ***************************  API Functions  ******************************
 **************************************************************************/


/*
 * plc_tag_decode_error()
 *
 * This takes an integer error value and turns it into a printable string.
 *
 * TODO - this should produce better errors than this!
 */


LIB_EXPORT const char *plc_tag_decode_error(int rc) {
    switch(rc) {
        case PLCTAG_STATUS_PENDING: return "PLCTAG_STATUS_PENDING";
        case PLCTAG_STATUS_OK: return "PLCTAG_STATUS_OK";
        case PLCTAG_ERR_ABORT: return "PLCTAG_ERR_ABORT";
        case PLCTAG_ERR_BAD_CONFIG: return "PLCTAG_ERR_BAD_CONFIG";
        case PLCTAG_ERR_BAD_CONNECTION: return "PLCTAG_ERR_BAD_CONNECTION";
        case PLCTAG_ERR_BAD_DATA: return "PLCTAG_ERR_BAD_DATA";
        case PLCTAG_ERR_BAD_DEVICE: return "PLCTAG_ERR_BAD_DEVICE";
        case PLCTAG_ERR_BAD_GATEWAY: return "PLCTAG_ERR_BAD_GATEWAY";
        case PLCTAG_ERR_BAD_PARAM: return "PLCTAG_ERR_BAD_PARAM";
        case PLCTAG_ERR_BAD_REPLY: return "PLCTAG_ERR_BAD_REPLY";
        case PLCTAG_ERR_BAD_STATUS: return "PLCTAG_ERR_BAD_STATUS";
        case PLCTAG_ERR_CLOSE: return "PLCTAG_ERR_CLOSE";
        case PLCTAG_ERR_CREATE: return "PLCTAG_ERR_CREATE";
        case PLCTAG_ERR_DUPLICATE: return "PLCTAG_ERR_DUPLICATE";
        case PLCTAG_ERR_ENCODE: return "PLCTAG_ERR_ENCODE";
        case PLCTAG_ERR_MUTEX_DESTROY: return "PLCTAG_ERR_MUTEX_DESTROY";
        case PLCTAG_ERR_MUTEX_INIT: return "PLCTAG_ERR_MUTEX_INIT";
        case PLCTAG_ERR_MUTEX_LOCK: return "PLCTAG_ERR_MUTEX_LOCK";
        case PLCTAG_ERR_MUTEX_UNLOCK: return "PLCTAG_ERR_MUTEX_UNLOCK";
        case PLCTAG_ERR_NOT_ALLOWED: return "PLCTAG_ERR_NOT_ALLOWED";
        case PLCTAG_ERR_NOT_FOUND: return "PLCTAG_ERR_NOT_FOUND";
        case PLCTAG_ERR_NOT_IMPLEMENTED: return "PLCTAG_ERR_NOT_IMPLEMENTED";
        case PLCTAG_ERR_NO_DATA: return "PLCTAG_ERR_NO_DATA";
        case PLCTAG_ERR_NO_MATCH: return "PLCTAG_ERR_NO_MATCH";
        case PLCTAG_ERR_NO_MEM: return "PLCTAG_ERR_NO_MEM";
        case PLCTAG_ERR_NO_RESOURCES: return "PLCTAG_ERR_NO_RESOURCES";
        case PLCTAG_ERR_NULL_PTR: return "PLCTAG_ERR_NULL_PTR";
        case PLCTAG_ERR_OPEN: return "PLCTAG_ERR_OPEN";
        case PLCTAG_ERR_OUT_OF_BOUNDS: return "PLCTAG_ERR_OUT_OF_BOUNDS";
        case PLCTAG_ERR_READ: return "PLCTAG_ERR_READ";
        case PLCTAG_ERR_REMOTE_ERR: return "PLCTAG_ERR_REMOTE_ERR";
        case PLCTAG_ERR_THREAD_CREATE: return "PLCTAG_ERR_THREAD_CREATE";
        case PLCTAG_ERR_THREAD_JOIN: return "PLCTAG_ERR_THREAD_JOIN";
        case PLCTAG_ERR_TIMEOUT: return "PLCTAG_ERR_TIMEOUT";
        case PLCTAG_ERR_TOO_LARGE: return "PLCTAG_ERR_TOO_LARGE";
        case PLCTAG_ERR_TOO_SMALL: return "PLCTAG_ERR_TOO_SMALL";
        case PLCTAG_ERR_UNSUPPORTED: return "PLCTAG_ERR_UNSUPPORTED";
        case PLCTAG_ERR_WINSOCK: return "PLCTAG_ERR_WINSOCK";
        case PLCTAG_ERR_WRITE: return "PLCTAG_ERR_WRITE";
        case PLCTAG_ERR_PARTIAL: return "PLCTAG_ERR_PARTIAL";
        case PLCTAG_ERR_BUSY: return "PLCTAG_ERR_BUSY";

        default: return "Unknown error."; break;
    }

    return "Unknown error.";
}


/*
 * Set the debug level.
 *
 * This function takes values from the defined debug levels.  It sets
 * the debug level to the passed value.  Higher numbers output increasing amounts
 * of information.   Input values not defined will be ignored.
 */

LIB_EXPORT void plc_tag_set_debug_level(int debug_level) {
    if(debug_level >= PLCTAG_DEBUG_NONE && debug_level <= PLCTAG_DEBUG_SPEW) { set_debug_level(debug_level); }
}


/*
 * Check that the library supports the required API version.
 *
 * PLCTAG_STATUS_OK is returned if the version matches.  If it does not,
 * PLCTAG_ERR_UNSUPPORTED is returned.
 */

LIB_EXPORT int plc_tag_check_lib_version(int req_major, int req_minor, int req_patch) {
    /* encode these with 16-bits per version part. */
    uint64_t lib_encoded_version =
        (((uint64_t)version_major) << 32u) + (((uint64_t)version_minor) << 16u) + (uint64_t)version_patch;

    uint64_t req_encoded_version = (((uint64_t)req_major) << 32u) + (((uint64_t)req_minor) << 16u) + (uint64_t)req_patch;

    if(version_major == (uint64_t)req_major && lib_encoded_version >= req_encoded_version) {
        return PLCTAG_STATUS_OK;
    } else {
        return PLCTAG_ERR_UNSUPPORTED;
    }
}


/*
 * plc_tag_create()
 *
 * This is where the dispatch occurs to the protocol specific implementation.
 */

LIB_EXPORT int32_t plc_tag_create(const char *attrib_str, int timeout) {
    return plc_tag_create_ex(attrib_str, NULL, NULL, timeout);
}


LIB_EXPORT int32_t plc_tag_create_ex(const char *attrib_str,
                                     void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                     void *userdata, int timeout) {
    plc_tag_p tag = PLC_TAG_P_NULL;
    int id = PLCTAG_ERR_OUT_OF_BOUNDS;
    attr attribs = NULL;
    int rc = PLCTAG_STATUS_OK;
    int read_cache_ms = 0;
    tag_create_function tag_constructor;
    int debug_level = -1;

    /* we are creating a tag, there is no ID yet. */
    debug_set_tag_id(0);

    pdebug(DEBUG_INFO, "Starting");

    /* check to see if the library is terminating. */
    if(atomic_get_bool(&library_terminating)) {
        pdebug(DEBUG_WARN, "The plctag library is in the process of shutting down!");
        return PLCTAG_ERR_NOT_ALLOWED;
    }

    /* make sure that all modules are initialized. */
    if((rc = initialize_modules()) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to initialize the internal library state!");
        return rc;
    }

    /* check the arguments */

    if(timeout < 0) {
        pdebug(DEBUG_WARN, "Timeout must not be negative!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!attrib_str || str_length(attrib_str) == 0) {
        pdebug(DEBUG_WARN, "Tag attribute string is null or zero length!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    attribs = attr_create_from_str(attrib_str);
    if(!attribs) {
        pdebug(DEBUG_WARN, "Unable to parse attribute string!");
        return PLCTAG_ERR_BAD_DATA;
    }

    /* set debug level */
    debug_level = attr_get_int(attribs, "debug", -1);
    if(debug_level > DEBUG_NONE) { set_debug_level(debug_level); }

    /*
     * create the tag, this is protocol specific.
     *
     * If this routine wants to keep the attributes around, it needs
     * to clone them.
     */
    tag_constructor = find_tag_create_func(attribs);

    if(!tag_constructor) {
        pdebug(DEBUG_WARN, "Tag creation failed, no tag constructor found for tag type!");
        attr_destroy(attribs);
        return PLCTAG_ERR_BAD_PARAM;
    }

    tag = tag_constructor(attribs, tag_callback_func, userdata);

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag creation failed, skipping mutex creation and other generic setup.");
        attr_destroy(attribs);
        return PLCTAG_ERR_CREATE;
    }

    if(tag->status != PLCTAG_STATUS_OK && tag->status != PLCTAG_STATUS_PENDING) {
        int tag_status = tag->status;

        pdebug(DEBUG_WARN, "Warning, %s error found while creating tag!", plc_tag_decode_error(tag_status));

        attr_destroy(attribs);
        rc_dec(tag);

        return tag_status;
    }

    /* set up the read cache config. */
    read_cache_ms = attr_get_int(attribs, "read_cache_ms", 0);
    if(read_cache_ms < 0) {
        pdebug(DEBUG_WARN, "read_cache_ms value must be positive, using zero.");
        read_cache_ms = 0;
    }

    tag->read_cache_expire = (int64_t)0;
    tag->read_cache_ms = (int64_t)read_cache_ms;

    /* set up any automatic read/write */
    tag->auto_sync_read_ms = attr_get_int(attribs, "auto_sync_read_ms", 0);
    if(tag->auto_sync_read_ms < 0) {
        pdebug(DEBUG_WARN, "auto_sync_read_ms value must be positive!");
        attr_destroy(attribs);
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    } else if(tag->auto_sync_read_ms > 0) {
        /* how many periods did we already pass? */
        // int64_t periods = (time_ms() / tag->auto_sync_read_ms);
        // tag->auto_sync_next_read = (periods + 1) * tag->auto_sync_read_ms;
        /* start some time in the future, but with random jitter. */
        tag->auto_sync_next_read = time_ms() + (int64_t)(random_u64((uint64_t)tag->auto_sync_read_ms));
    }

    tag->auto_sync_write_ms = attr_get_int(attribs, "auto_sync_write_ms", 0);
    if(tag->auto_sync_write_ms < 0) {
        pdebug(DEBUG_WARN, "auto_sync_write_ms value must be positive!");
        attr_destroy(attribs);
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    } else {
        tag->auto_sync_next_write = 0;
    }

    /* See if we are allowed to resize fields */
    tag->allow_field_resize = (uint8_t)(attr_get_int(attribs, "allow_field_resize", 0) ? 1 : 0);

    /* set up the tag byte order if there are any overrides. */
    rc = set_tag_byte_order(tag, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to correctly set tag data byte order: %s!", plc_tag_decode_error(rc));
        attr_destroy(attribs);
        rc_dec(tag);
        return rc;
    }

    /*
     * Release memory for attributes
     */
    attr_destroy(attribs);

    /* map the tag to a tag ID */
    id = add_tag_lookup(tag);

    /* if the mapping failed, then punt */
    if(id < 0) {
        pdebug(DEBUG_ERROR, "Unable to map tag %p to lookup table entry, rc=%s", tag, plc_tag_decode_error(id));
        rc_dec(tag);
        return id;
    }

    /* save this for later. */
    tag->tag_id = id;

    debug_set_tag_id(id);

    pdebug(DEBUG_INFO, "Returning mapped tag ID %d", id);

    /* wake up tag's PLC here. */
    if(tag->vtable && tag->vtable->wake_plc) { tag->vtable->wake_plc(tag); }

    /* get the tag status. */
    if(tag->vtable && tag->vtable->status) { rc = tag->vtable->status(tag); }

    /* check to see if there was an error during tag creation. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_WARN, "Error %s while trying to create tag!", plc_tag_decode_error(rc));
        if(tag->vtable && tag->vtable->abort) { tag->vtable->abort(tag); }

        /* remove the tag from the hashtable. */
        critical_block(tag_lookup_mutex) { hashtable_remove(tags, (int64_t)tag->tag_id); }

        rc_dec(tag);
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Tag status after creation is %s.", plc_tag_decode_error(rc));

    /*
     * if there is a timeout, then wait until we get
     * an error or we timeout.
     */
    if(timeout > 0 && rc == PLCTAG_STATUS_PENDING) {
        int64_t start_time = time_ms();
        int64_t end_time = start_time + timeout;

        /* wake up the tickler in case it is needed to create the tag. */
        plc_tag_tickler_wake();

        /* we loop as long as we have time left to wait. */
        do {
            int64_t timeout_left = end_time - time_ms();

            /* clamp the timeout left to non-negative int range. */
            if(timeout_left < 0) { timeout_left = 0; }

            if(timeout_left > INT_MAX) { timeout_left = 100; /* MAGIC, only wait 100ms in this weird case. */ }

            /* wait for something to happen */
            rc = cond_wait(tag->tag_cond_wait, (int)timeout_left);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error %s while waiting for tag creation to complete!", plc_tag_decode_error(rc));
                if(tag->vtable && tag->vtable->abort) { tag->vtable->abort(tag); }

                /* remove the tag from the hashtable. */
                critical_block(tag_lookup_mutex) { hashtable_remove(tags, (int64_t)tag->tag_id); }

                rc_dec(tag);
                return rc;
            }

            /* get the tag status. */
            if(tag->vtable && tag->vtable->status) {
                rc = tag->vtable->status(tag);
            } else {
                pdebug(DEBUG_WARN, "Tag does not have a status function!");
            }

            /* check to see if there was an error during tag creation. */
            if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                pdebug(DEBUG_WARN, "Error %s while trying to create tag!", plc_tag_decode_error(rc));
                if(tag->vtable && tag->vtable->abort) { tag->vtable->abort(tag); }

                /* remove the tag from the hashtable. */
                critical_block(tag_lookup_mutex) { hashtable_remove(tags, (int64_t)tag->tag_id); }

                rc_dec(tag);
                return rc;
            }
        } while(rc == PLCTAG_STATUS_PENDING && time_ms() > end_time);

        /* clear up any remaining flags.  This should be refactored. */
        tag->read_in_flight = 0;
        tag->write_in_flight = 0;

        /* raise create event. */
        tag_raise_event(tag, PLCTAG_EVENT_CREATED, (int8_t)rc);

        pdebug(DEBUG_INFO, "tag set up elapsed time %" PRId64 "ms", (time_ms() - start_time));
    }

    /* dispatch any outstanding events. */
    plc_tag_generic_handle_event_callbacks(tag);

    pdebug(DEBUG_INFO, "Done.");

    return id;
}


/*
 * plc_tag_shutdown
 *
 * Some systems may not be able to call atexit() handlers.  In those cases, wrappers should
 * call this function before unloading the library or terminating.   Most OSes will cleanly
 * recover all system resources when a process is terminated and this will not be necessary.
 */

LIB_EXPORT void plc_tag_shutdown(void) {
    int tag_table_entries = 0;

    debug_set_tag_id(0);

    pdebug(DEBUG_INFO, "Starting.");

    /* terminate anything waiting on the library and prevent any tags from being created. */
    atomic_set_bool(&library_terminating, true);

    /* close all tags. */
    pdebug(DEBUG_INFO, "Closing all tags.");

    critical_block(tag_lookup_mutex) { tag_table_entries = hashtable_capacity(tags); }

    for(int i = 0; i < tag_table_entries; i++) {
        plc_tag_p tag = NULL;

        critical_block(tag_lookup_mutex) {
            tag_table_entries = hashtable_capacity(tags);

            if(i < tag_table_entries && tag_table_entries >= 0) {
                tag = hashtable_get_index(tags, i);

                /* make sure the tag does not go away while we are using the pointer. */
                if(tag) {
                    /* this returns NULL if the existing ref-count is zero. */
                    pdebug(DEBUG_DETAIL, "rc_inc: Acquiring reference to tag %" PRId32 ".", tag->tag_id);
                    tag = rc_inc(tag);
                }
            }
        }

        /* do this outside the mutex. */
        if(tag) {
            debug_set_tag_id(tag->tag_id);
            pdebug(DEBUG_INFO, "Destroying tag %" PRId32 ".", tag->tag_id);
            plc_tag_destroy(tag->tag_id);
            pdebug(DEBUG_INFO, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
            rc_dec(tag);
        }
    }

    pdebug(DEBUG_INFO, "All tags closed.");

    pdebug(DEBUG_INFO, "Cleaning up library resources.");

    destroy_modules();

    /* Clear the termination flag in case we want to start up again. */
    atomic_set_bool(&library_terminating, false);

    pdebug(DEBUG_INFO, "Done.");
}


/*
 * plc_tag_register_callback
 *
 * This function registers the passed callback function with the tag.  Only one callback function
 * may be registered on a tag at a time!
 *
 * Once registered, any of the following operations on or in the tag will result in the callback
 * being called:
 *
 *      * a tag handle finishing creation.
 *      * starting a tag read operation.
 *      * a tag read operation ending.
 *      * a tag read being aborted.
 *      * starting a tag write operation.
 *      * a tag write operation ending.
 *      * a tag write being aborted.
 *      * a tag being destroyed
 *
 * The callback is called outside of the internal tag mutex so it can call any tag functions safely.   However,
 * the callback is called in the context of the internal tag helper thread and not the client library thread(s).
 * This means that YOU are responsible for making sure that all client application data structures the callback
 * function touches are safe to access by the callback!
 *
 * Do not do any operations in the callback that block for any significant time.   This will cause library
 * performance to be poor or even to start failing!
 *
 * When the callback is called with the PLCTAG_EVENT_DESTROY_STARTED, do not call any tag functions.  It is
 * not guaranteed that they will work and they will possibly hang or fail.
 *
 * Return values:
 *void (*tag_callback_func)(int32_t tag_id, uint32_t event, int status)
 * If there is already a callback registered, the function will return PLCTAG_ERR_DUPLICATE.   Only one callback
 * function may be registered at a time on each tag.
 *
 * If all is successful, the function will return PLCTAG_STATUS_OK.
 *
 * Also see plc_tag_register_callback_ex.
 */


/* there needs to be a better way to make the cast clean than this! */
#ifndef _MSC_VER
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

LIB_EXPORT int plc_tag_register_callback(int32_t tag_id, tag_callback_func callback_func) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    rc = plc_tag_register_callback_ex(tag_id, (tag_extended_callback_func)callback_func, NULL);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

#ifndef _MSC_VER
#    pragma GCC diagnostic pop
#endif

/*
 * plc_tag_register_callback_ex
 *
 * This function registers the passed callback function and user data with the tag.  Only one callback function
 * may be registered on a tag at a time!
 *
 * Once registered, any of the following operations on or in the tag will result in the callback
 * being called:
 *
 *      * a tag handle finishing creation.
 *      * starting a tag read operation.
 *      * a tag read operation ending.
 *      * a tag read being aborted.
 *      * starting a tag write operation.
 *      * a tag write operation ending.
 *      * a tag write being aborted.
 *      * a tag being destroyed
 *
 * The callback is called outside of the internal tag mutex so it can call any tag functions safely.   However,
 * the callback is called in the context of the internal tag helper thread and not the client library thread(s).
 * This means that YOU are responsible for making sure that all client application data structures the callback
 * function touches are safe to access by the callback!
 *
 * Do not do any operations in the callback that block for any significant time.   This will cause library
 * performance to be poor or even to start failing!
 *
 * When the callback is called with the PLCTAG_EVENT_DESTROY_STARTED, do not call any tag functions.  It is
 * not guaranteed that they will work and they will possibly hang or fail.
 *
 * Return values:
 *
 * If there is already a callback registered, the function will return PLCTAG_ERR_DUPLICATE.   Only one callback
 * function may be registered at a time on each tag.
 *
 * If all is successful, the function will return PLCTAG_STATUS_OK.
 *
 * Also see plc_tag_register_callback.
 */

LIB_EXPORT int plc_tag_register_callback_ex(int32_t tag_id, tag_extended_callback_func callback_func, void *userdata) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(tag_id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        if(tag->callback) {
            rc = PLCTAG_ERR_DUPLICATE;
        } else {
            if(callback_func) {
                tag->callback = callback_func;
                tag->userdata = userdata;
            } else {
                tag->callback = NULL;
                tag->userdata = NULL;
            }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/*
 * plc_tag_unregister_callback
 *
 * This function removes the callback already registered on the tag.
 *
 * Return values:
 *
 * The function returns PLCTAG_STATUS_OK if there was a registered callback and removing it went well.
 * An error of PLCTAG_ERR_NOT_FOUND is returned if there was no registered callback.
 */

LIB_EXPORT int plc_tag_unregister_callback(int32_t tag_id) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(tag_id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        if(tag->callback) {
            rc = PLCTAG_STATUS_OK;
            tag->callback = NULL;
            tag->userdata = NULL;
        } else {
            rc = PLCTAG_ERR_NOT_FOUND;
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/*
 * plc_tag_register_logger
 *
 * This function registers the passed callback function with the library.  Only one callback function
 * may be registered with the library at a time!
 *
 * Once registered, the function will be called with any logging message that is normally printed due
 * to the current log level setting.
 *
 * WARNING: the callback will usually be called when the internal tag API mutex is held.   You cannot
 * call any tag functions within the callback!
 *
 * Return values:
 *
 * If there is already a callback registered, the function will return PLCTAG_ERR_DUPLICATE.   Only one callback
 * function may be registered at a time on each tag.
 *
 * If all is successful, the function will return PLCTAG_STATUS_OK.
 */

LIB_EXPORT int plc_tag_register_logger(void (*log_callback_func)(int32_t tag_id, int debug_level, const char *message)) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    rc = debug_register_logger(log_callback_func);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


/*
 * plc_tag_unregister_logger
 *
 * This function removes the logger callback already registered for the library.
 *
 * Return values:
 *
 * The function returns PLCTAG_STATUS_OK if there was a registered callback and removing it went well.
 * An error of PLCTAG_ERR_NOT_FOUND is returned if there was no registered callback.
 */

LIB_EXPORT int plc_tag_unregister_logger(void) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting");

    rc = debug_unregister_logger();

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


/*
 * plc_tag_lock
 *
 * Lock the tag against use by other threads.  Because operations on a tag are
 * very much asynchronous, actions like getting and extracting the data from
 * a tag take more than one API call.  If more than one thread is using the same tag,
 * then the internal state of the tag will get broken and you will probably experience
 * a crash.
 *
 * This should be used to initially lock a tag when starting operations with it
 * followed by a call to plc_tag_unlock when you have everything you need from the tag.
 */

LIB_EXPORT int plc_tag_lock(int32_t id) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* FIXME - there has to be a better way that this! */
    /* we cannot nest the mutexes otherwise we will deadlock. */
    do {
        critical_block(tag->api_mutex) { rc = mutex_try_lock(tag->ext_mutex); }

        /* if the mutex is already locked then we get a mutex lock error. */
        if(rc == PLCTAG_ERR_MUTEX_LOCK) {
            pdebug(DEBUG_SPEW, "Mutex already locked, wait and retry.");
            sleep_ms(10);
        }
    } while(rc == PLCTAG_ERR_MUTEX_LOCK);

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_SPEW, "External mutex locked.");
    } else {
        pdebug(DEBUG_WARN, "Error %s trying to lock external mutex!", plc_tag_decode_error(rc));
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/*
 * plc_tag_unlock
 *
 * The opposite action of plc_tag_unlock.  This allows other threads to access the
 * tag.
 */

LIB_EXPORT int plc_tag_unlock(int32_t id) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) { rc = mutex_unlock(tag->ext_mutex); }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/*
 * plc_tag_abort()
 *
 * This function calls through the vtable in the passed tag to call
 * the protocol-specific implementation.
 *
 * The implementation must do whatever is necessary to abort any
 * ongoing IO.
 *
 * The status of the operation is returned.
 */

LIB_EXPORT int plc_tag_abort(int32_t id) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    rc = plc_tag_abort_impl(tag);

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/*
 * plc_tag_destroy()
 *
 * Remove all implementation specific details about a tag and clear its
 * memory.
 */


LIB_EXPORT int plc_tag_destroy(int32_t tag_id) {
    plc_tag_p tag = NULL;

    debug_set_tag_id((int)tag_id);

    pdebug(DEBUG_INFO, "Starting.");

    if(tag_id <= 0 || tag_id >= TAG_ID_MASK) {
        pdebug(DEBUG_WARN, "Called with zero or invalid tag!");
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(tag_lookup_mutex) { tag = hashtable_remove(tags, tag_id); }

    if(!tag) {
        pdebug(DEBUG_WARN, "Called with non-existent tag!");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* abort anything in flight */
    pdebug(DEBUG_DETAIL, "Aborting any in-flight operations.");

    plc_tag_abort_impl(tag);

    critical_block(tag->api_mutex) { tag_raise_event(tag, PLCTAG_EVENT_DESTROYED, PLCTAG_STATUS_OK); }

    /* wake the tickler */
    plc_tag_tickler_wake();

    plc_tag_generic_handle_event_callbacks(tag);

    /* release the reference outside the mutex. */
    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 " and tag mutex not locked.", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    debug_set_tag_id(0);

    return PLCTAG_STATUS_OK;
}


/*
 * plc_tag_read()
 *
 * This function calls through the vtable in the passed tag to call
 * the protocol-specific implementation.  That starts the read operation.
 * If there is a timeout passed, then this routine waits for either
 * a timeout or an error.
 *
 * The status of the operation is returned.
 */

LIB_EXPORT int plc_tag_read(int32_t id, int timeout) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    int is_done = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    if(timeout < 0) {
        pdebug(DEBUG_WARN, "Timeout must not be negative!");
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    }

    critical_block(tag->api_mutex) {
        tag_raise_event(tag, PLCTAG_EVENT_READ_STARTED, PLCTAG_STATUS_OK);
        plc_tag_generic_handle_event_callbacks(tag);

        /* check read cache, if not expired, return existing data. */
        if(tag->read_cache_expire > time_ms()) {
            pdebug(DEBUG_INFO, "Returning cached data.");
            rc = PLCTAG_STATUS_OK;
            is_done = 1;
            break;
        }

        if(tag->read_in_flight || tag->write_in_flight) {
            pdebug(DEBUG_WARN, "An operation is already in flight!");
            rc = PLCTAG_ERR_BUSY;
            is_done = 1;
            break;
        }

        if(tag->tag_is_dirty) {
            pdebug(DEBUG_WARN, "Tag has locally updated data that will be overwritten!");
            rc = PLCTAG_ERR_BUSY;
            is_done = 1;
            break;
        }

        tag->read_in_flight = 1;
        tag->status = PLCTAG_STATUS_PENDING;

        /* clear the condition var */
        cond_clear(tag->tag_cond_wait);

        /* the protocol implementation does not do the timeout. */
        if(tag->vtable && tag->vtable->read) {
            rc = tag->vtable->read(tag);
        } else {
            pdebug(DEBUG_WARN, "Attempt to call read on a tag that does not support reads.");
            rc = PLCTAG_ERR_NOT_IMPLEMENTED;
        }

        /* if not pending then check for success or error. */
        if(rc != PLCTAG_STATUS_PENDING) {
            if(rc != PLCTAG_STATUS_OK) {
                /* not pending and not OK, so error. Abort and clean up. */

                pdebug(DEBUG_WARN, "Response from read command returned error %s!", plc_tag_decode_error(rc));

                rc = plc_tag_abort_impl(tag);
            }

            tag->read_in_flight = 0;
            is_done = 1;
            break;
        }
    }

    /*
     * if there is a timeout, then wait until we get
     * an error or we timeout.
     */
    if(!is_done && timeout > 0) {
        int64_t start_time = time_ms();
        int64_t end_time = start_time + timeout;

        /* wake up the tickler in case it is needed to read the tag. */
        plc_tag_tickler_wake();

        /* we loop as long as we have time left to wait. */
        do {
            int64_t timeout_left = end_time - time_ms();

            /* clamp the timeout left to non-negative int range. */
            if(timeout_left < 0) { timeout_left = 0; }

            if(timeout_left > INT_MAX) { timeout_left = 100; /* MAGIC, only wait 100ms in this weird case. */ }

            /* wait for something to happen */
            rc = cond_wait(tag->tag_cond_wait, (int)timeout_left);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error %s while waiting for tag read to complete!", plc_tag_decode_error(rc));
                plc_tag_abort_impl(tag);

                break;
            }

            /* get the tag status. */
            rc = plc_tag_status_impl(tag);

            /* check to see if there was an error during tag read. */
            if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                pdebug(DEBUG_WARN, "Error %s while trying to read tag!", plc_tag_decode_error(rc));
                plc_tag_abort_impl(tag);
            }
        } while(rc == PLCTAG_STATUS_PENDING && time_ms() < end_time);

        /* the read is not in flight anymore. */
        critical_block(tag->api_mutex) {
            tag->read_in_flight = 0;
            tag->read_complete = 0;
            /* is_done = 1; */
            tag_raise_event(tag, PLCTAG_EVENT_READ_COMPLETED, (int8_t)rc);
        }

        pdebug(DEBUG_INFO, "elapsed time %" PRId64 "ms", (time_ms() - start_time));
    }

    if(rc == PLCTAG_STATUS_OK) {
        /* set up the cache time.  This works when read_cache_ms is zero as it is already expired. */
        tag->read_cache_expire = time_ms() + tag->read_cache_ms;
    }

    /* fire any events that are pending. */
    plc_tag_generic_handle_event_callbacks(tag);

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done");

    return rc;
}


/*
 * plc_tag_status
 *
 * Return the current status of the tag.  This will be PLCTAG_STATUS_PENDING if there is
 * an uncompleted IO operation.  It will be PLCTAG_STATUS_OK if everything is fine.  Other
 * errors will be returned as appropriate.
 *
 * This is a function provided by the underlying protocol implementation.
 */

LIB_EXPORT int plc_tag_status(int32_t id) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    /* check the ID.  It might be an error status from creating the tag. */
    if(!tag) {
        if(id < 0) {
            pdebug(DEBUG_WARN, "Called with an error status %s!", plc_tag_decode_error(id));
            return id;
        } else {
            pdebug(DEBUG_WARN, "Tag not found.");
            return PLCTAG_ERR_NOT_FOUND;
        }
    }

    rc = plc_tag_status_impl(tag);

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done with rc=%s.", plc_tag_decode_error(rc));

    return rc;
}


/*
 * plc_tag_write()
 *
 * This function calls through the vtable in the passed tag to call
 * the protocol-specific implementation.  That starts the write operation.
 * If there is a timeout passed, then this routine waits for either
 * a timeout or an error.
 *
 * The status of the operation is returned.
 */

LIB_EXPORT int plc_tag_write(int32_t id, int timeout) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    int is_done = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    if(timeout < 0) {
        pdebug(DEBUG_WARN, "Timeout must not be negative!");
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    }

    critical_block(tag->api_mutex) {
        if(tag->read_in_flight || tag->write_in_flight) {
            pdebug(DEBUG_WARN, "Tag already has an operation in flight!");
            is_done = 1;
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        /* a write is now in flight. */
        tag->write_in_flight = 1;
        tag->status = PLCTAG_STATUS_OK;

        /*
         * This needs to be done before we raise the event below in case the user code
         * tries to do something tricky like abort the write.   In that case, the condition
         * variable will be set by the abort.   So we have to clear it here and then see
         * if it gets raised afterward.
         */
        cond_clear(tag->tag_cond_wait);

        /*
         * This must be raised _before_ we start the write to enable
         * application code to fill in the tag data buffer right before
         * we start the write process.
         */
        tag_raise_event(tag, PLCTAG_EVENT_WRITE_STARTED, tag->status);
        plc_tag_generic_handle_event_callbacks(tag);

        /* the protocol implementation does not do the timeout. */
        if(tag->vtable && tag->vtable->write) {
            rc = tag->vtable->write(tag);
        } else {
            pdebug(DEBUG_WARN, "Attempt to call write on a tag that does not support writes.");
            rc = PLCTAG_ERR_NOT_IMPLEMENTED;
        }

        /* if not pending then check for success or error. */
        if(rc != PLCTAG_STATUS_PENDING) {
            if(rc != PLCTAG_STATUS_OK) {
                /* not pending and not OK, so error. Abort and clean up. */

                pdebug(DEBUG_WARN, "Response from write command returned error %s!", plc_tag_decode_error(rc));

                if(tag->vtable && tag->vtable->abort) { tag->vtable->abort(tag); }
            }

            tag->write_in_flight = 0;
            is_done = 1;
            break;
        }
    } /* end of api mutex block */

    /*
     * if there is a timeout, then wait until we get
     * an error or we timeout.
     */
    if(!is_done && timeout > 0) {
        int64_t start_time = time_ms();
        int64_t end_time = start_time + timeout;

        /* wake up the tickler in case it is needed to write the tag. */
        plc_tag_tickler_wake();

        /* we loop as long as we have time left to wait. */
        do {
            int64_t timeout_left = end_time - time_ms();

            /* clamp the timeout left to non-negative int range. */
            if(timeout_left < 0) { timeout_left = 0; }

            if(timeout_left > INT_MAX) { timeout_left = 100; /* MAGIC, only wait 100ms in this weird case. */ }

            /* wait for something to happen */
            rc = cond_wait(tag->tag_cond_wait, (int)timeout_left);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error %s while waiting for tag write to complete!", plc_tag_decode_error(rc));
                plc_tag_abort_impl(tag);

                break;
            }

            /* get the tag status. */
            rc = plc_tag_status_impl(tag);

            /* check to see if there was an error during tag creation. */
            if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                pdebug(DEBUG_WARN, "Error %s while trying to write tag!", plc_tag_decode_error(rc));
                plc_tag_abort_impl(tag);
            }
        } while(rc == PLCTAG_STATUS_PENDING && time_ms() < end_time);

        /* the write is not in flight anymore. */
        critical_block(tag->api_mutex) {
            tag->write_in_flight = 0;
            tag->write_complete = 0;
            is_done = 1;
        }

        pdebug(DEBUG_INFO, "Write finshed with elapsed time %" PRId64 "ms", (time_ms() - start_time));
    }

    if(is_done) {
        critical_block(tag->api_mutex) { tag_raise_event(tag, PLCTAG_EVENT_WRITE_COMPLETED, (int8_t)rc); }
    }

    /* fire any events that are pending. */
    plc_tag_generic_handle_event_callbacks(tag);

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done: status = %s.", plc_tag_decode_error(rc));

    return rc;
}


/*
 * Tag data accessors.
 */


LIB_EXPORT int plc_tag_get_int_attribute(int32_t id, const char *attrib_name, int default_value) {
    int res = default_value;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    /* FIXME - this should set the tag status if there is a tag. */
    if(!attrib_name || str_length(attrib_name) == 0) {
        pdebug(DEBUG_WARN, "Attribute name must not be null or zero-length!");
        return default_value;
    }

    /* get library attributes */
    if(id == 0) {
        if(str_cmp_i(attrib_name, "version_major") == 0) {
            res = (int)version_major;
        } else if(str_cmp_i(attrib_name, "version_minor") == 0) {
            res = (int)version_minor;
        } else if(str_cmp_i(attrib_name, "version_patch") == 0) {
            res = (int)version_patch;
        } else if(str_cmp_i(attrib_name, "debug") == 0) {
            res = (int)get_debug_level();
        } else if(str_cmp_i(attrib_name, "debug_level") == 0) {
            pdebug(DEBUG_WARN, "Deprecated attribute \"debug_level\" used, use \"debug\" instead.");
            res = (int)get_debug_level();
        } else {
            pdebug(DEBUG_WARN, "Attribute \"%s\" is not supported at the library level!");
            res = default_value;
        }
    } else {
        tag = lookup_tag(id);

        if(!tag) {
            pdebug(DEBUG_WARN, "Tag not found.");
            return default_value;
        }

        critical_block(tag->api_mutex) {
            /* match the generic ones first. */
            if(str_cmp_i(attrib_name, "size") == 0) {
                tag->status = PLCTAG_STATUS_OK;
                res = (int)tag->size;
            } else if(str_cmp_i(attrib_name, "read_cache_ms") == 0) {
                /* FIXME - what happens if this overflows? */
                tag->status = PLCTAG_STATUS_OK;
                res = (int)tag->read_cache_ms;
            } else if(str_cmp_i(attrib_name, "auto_sync_read_ms") == 0) {
                tag->status = PLCTAG_STATUS_OK;
                res = (int)tag->auto_sync_read_ms;
            } else if(str_cmp_i(attrib_name, "auto_sync_write_ms") == 0) {
                tag->status = PLCTAG_STATUS_OK;
                res = (int)tag->auto_sync_write_ms;
            } else if(str_cmp_i(attrib_name, "bit_num") == 0) {
                tag->status = PLCTAG_STATUS_OK;
                res = (int)(unsigned int)(tag->bit);
            } else if(str_cmp_i(attrib_name, "connection_group_id") == 0) {
                pdebug(DEBUG_DETAIL, "Getting the connection_group_id for tag %" PRId32 ".", id);
                tag->status = PLCTAG_STATUS_OK;
                res = tag->connection_group_id;
            } else {
                if(tag->vtable && tag->vtable->get_int_attrib) {
                    res = tag->vtable->get_int_attrib(tag, attrib_name, default_value);
                } else {
                    res = default_value;
                    tag->status = PLCTAG_ERR_NOT_IMPLEMENTED;
                }
            }
        }

        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return res;
}


LIB_EXPORT int plc_tag_set_int_attribute(int32_t id, const char *attrib_name, int new_value) {
    int res = PLCTAG_ERR_NOT_FOUND;
    plc_tag_p tag = NULL;

    if(!attrib_name || str_length(attrib_name) == 0) {
        pdebug(DEBUG_WARN, "Attribute name must not be null or zero-length!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_DETAIL, "Starting for int attribute %s.", attrib_name);

    /* get library attributes */
    if(id == 0) {
        if(str_cmp_i(attrib_name, "debug") == 0) {
            if(new_value >= DEBUG_ERROR && new_value < DEBUG_SPEW) {
                set_debug_level(new_value);
                res = PLCTAG_STATUS_OK;
            } else {
                res = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        } else if(str_cmp_i(attrib_name, "debug_level") == 0) {
            pdebug(DEBUG_WARN, "Deprecated attribute \"debug_level\" used, use \"debug\" instead.");
            if(new_value >= DEBUG_ERROR && new_value < DEBUG_SPEW) {
                set_debug_level(new_value);
                res = PLCTAG_STATUS_OK;
            } else {
                res = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        } else {
            pdebug(DEBUG_WARN, "Attribute \"%s\" is not support at the library level!", attrib_name);
            return PLCTAG_ERR_UNSUPPORTED;
        }
    } else {
        tag = lookup_tag(id);

        if(!tag) {
            pdebug(DEBUG_WARN, "Tag not found.");
            return PLCTAG_ERR_NOT_FOUND;
        }

        critical_block(tag->api_mutex) {
            /* match the generic ones first. */
            if(str_cmp_i(attrib_name, "read_cache_ms") == 0) {
                if(new_value >= 0) {
                    /* expire the cache. */
                    tag->read_cache_expire = (int64_t)0;
                    tag->read_cache_ms = (int64_t)new_value;
                    tag->status = PLCTAG_STATUS_OK;
                    res = PLCTAG_STATUS_OK;
                } else {
                    tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                    res = PLCTAG_ERR_OUT_OF_BOUNDS;
                }
            } else if(str_cmp_i(attrib_name, "auto_sync_read_ms") == 0) {
                if(new_value >= 0) {
                    tag->auto_sync_read_ms = new_value;
                    tag->status = PLCTAG_STATUS_OK;
                    res = PLCTAG_STATUS_OK;
                } else {
                    pdebug(DEBUG_WARN, "auto_sync_read_ms must be greater than or equal to zero!");
                    tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                    res = PLCTAG_ERR_OUT_OF_BOUNDS;
                }
            } else if(str_cmp_i(attrib_name, "auto_sync_write_ms") == 0) {
                if(new_value >= 0) {
                    tag->auto_sync_write_ms = new_value;
                    tag->status = PLCTAG_STATUS_OK;
                    res = PLCTAG_STATUS_OK;
                } else {
                    pdebug(DEBUG_WARN, "auto_sync_write_ms must be greater than or equal to zero!");
                    tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                    res = PLCTAG_ERR_OUT_OF_BOUNDS;
                }
            } else if(str_cmp_i(attrib_name, "allow_field_resize") == 0) {
                tag->allow_field_resize = (new_value > 0 ? 1 : 0);
                tag->status = PLCTAG_STATUS_OK;
                res = PLCTAG_STATUS_OK;
            } else {
                if(tag->vtable && tag->vtable->set_int_attrib) {
                    res = tag->vtable->set_int_attrib(tag, attrib_name, new_value);
                    tag->status = (int8_t)res;
                } else {
                    tag->status = PLCTAG_ERR_NOT_IMPLEMENTED;
                }
            }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return res;
}


LIB_EXPORT int plc_tag_get_byte_array_attribute(int32_t id, const char *attrib_name, uint8_t *buffer, int buffer_length) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!attrib_name || str_length(attrib_name) == 0) {
        pdebug(DEBUG_WARN, "Attribute name must not be null or zero-length!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!buffer) {
        pdebug(DEBUG_WARN, "Host data buffer pointer must not be null!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(buffer_length <= 0) {
        pdebug(DEBUG_WARN, "Buffer length must not be negative or zero!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    tag = lookup_tag(id);

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        if(tag->vtable && tag->vtable->get_byte_array_attrib) {
            rc = tag->vtable->get_byte_array_attrib(tag, attrib_name, buffer, buffer_length);
        } else {
            rc = PLCTAG_ERR_NOT_IMPLEMENTED;
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


LIB_EXPORT int plc_tag_get_size(int32_t id) {
    int result = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        result = tag->size;
        tag->status = PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return result;
}


LIB_EXPORT int plc_tag_set_size(int32_t id, int new_size) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_DETAIL, "Starting with new size %d.", new_size);

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    if(new_size < 0) {
        pdebug(DEBUG_WARN, "Illegal new size %d bytes for tag is illegal.  Tag size must be positive.");
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_BAD_PARAM;
    }

    critical_block(tag->api_mutex) { rc = resize_tag_buffer_unsafe(tag, new_size); }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    if(rc >= 0) {
        pdebug(DEBUG_DETAIL, "Done with old size %d.", rc);
    } else {
        pdebug(DEBUG_WARN, "Tag buffer resize failed with error %s!", plc_tag_decode_error(rc));
    }

    return rc;
}


static int plc_tag_get_bit_impl(plc_tag_p tag, int offset_bit) {
    int res = 0;
    int real_offset = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    do {
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* if this is a single bit, then make sure the offset is the tag bit. */
        if(tag->is_bit) {
            real_offset = tag->bit;
        } else {
            real_offset = offset_bit;
        }

        pdebug(DEBUG_SPEW, "selecting bit %d with offset %d in byte %d (%x).", real_offset, (real_offset % 8), (real_offset / 8),
               tag->data[real_offset / 8]);

        if((real_offset >= 0) && ((real_offset / 8) < tag->size)) {
            res = !!(((1 << (real_offset % 8)) & 0xFF) & (tag->data[real_offset / 8]));
            tag->status = PLCTAG_STATUS_OK;
            break;
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            res = PLCTAG_ERR_OUT_OF_BOUNDS;
            tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }
    } while(0);

    pdebug(DEBUG_SPEW, "Done.");

    return res;
}


LIB_EXPORT int plc_tag_get_bit(int32_t id, int offset_bit) {
    int res = PLCTAG_ERR_OUT_OF_BOUNDS;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) { res = plc_tag_get_bit_impl(tag, offset_bit); }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}

static int plc_tag_set_bit_impl(plc_tag_p tag, int offset_bit, int val) {
    int res = PLCTAG_STATUS_OK;
    int real_offset = offset_bit;

    pdebug(DEBUG_SPEW, "Starting.");

    do {
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* if this is a single bit, then make sure the offset is the tag bit. */
        if(tag->is_bit) {
            real_offset = tag->bit;
        } else {
            real_offset = offset_bit;
        }

        pdebug(DEBUG_SPEW, "Setting bit %d with offset %d in byte %d (%x).", real_offset, (real_offset % 8), (real_offset / 8),
               tag->data[real_offset / 8]);

        if((real_offset >= 0) && ((real_offset / 8) < tag->size)) {
            if(tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

            if(val) {
                tag->data[real_offset / 8] |= (uint8_t)(1 << (real_offset % 8));
            } else {
                tag->data[real_offset / 8] &= (uint8_t)(~(1 << (real_offset % 8)));
            }

            tag->status = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            res = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }
    } while(0);

    pdebug(DEBUG_SPEW, "Done.");

    return res;
}


LIB_EXPORT int plc_tag_set_bit(int32_t id, int offset_bit, int val) {
    int res = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) { res = plc_tag_set_bit_impl(tag, offset_bit, val); }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}


LIB_EXPORT uint64_t plc_tag_get_uint64(int32_t id, int offset) {
    uint64_t res = UINT64_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = UINT64_MAX;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(uint64_t)) <= tag->size)) {
                res = ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[0]]) << 0)
                      + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[1]]) << 8)
                      + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[2]]) << 16)
                      + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[3]]) << 24)
                      + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[4]]) << 32)
                      + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[5]]) << 40)
                      + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[6]]) << 48)
                      + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[7]]) << 56);

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        } else {
            int rc = plc_tag_get_bit_impl(tag, tag->bit);

            /* make sure the response is good. */
            if(rc >= 0) { res = (unsigned int)rc; }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}


LIB_EXPORT int plc_tag_set_uint64(int32_t id, int offset, uint64_t val) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(uint64_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

                tag->data[offset + tag->byte_order->int64_order[0]] = (uint8_t)((val >> 0) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[1]] = (uint8_t)((val >> 8) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[2]] = (uint8_t)((val >> 16) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[3]] = (uint8_t)((val >> 24) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[4]] = (uint8_t)((val >> 32) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[5]] = (uint8_t)((val >> 40) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[6]] = (uint8_t)((val >> 48) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[7]] = (uint8_t)((val >> 56) & 0xFF);

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        } else {
            if(!val) {
                rc = plc_tag_set_bit_impl(tag, 0, 0);
            } else {
                rc = plc_tag_set_bit_impl(tag, 0, 1);
            }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}


LIB_EXPORT int64_t plc_tag_get_int64(int32_t id, int offset) {
    int64_t res = INT64_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(!tag->is_bit) {
            critical_block(tag->api_mutex) {
                if((offset >= 0) && (offset + ((int)sizeof(int64_t)) <= tag->size)) {
                    res = (int64_t)(((uint64_t)(tag->data[offset + tag->byte_order->int64_order[0]]) << 0)
                                    + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[1]]) << 8)
                                    + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[2]]) << 16)
                                    + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[3]]) << 24)
                                    + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[4]]) << 32)
                                    + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[5]]) << 40)
                                    + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[6]]) << 48)
                                    + ((uint64_t)(tag->data[offset + tag->byte_order->int64_order[7]]) << 56));

                    tag->status = PLCTAG_STATUS_OK;
                } else {
                    pdebug(DEBUG_WARN, "Data offset out of bounds!");
                    tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                }
            }
        } else {
            int rc = plc_tag_get_bit_impl(tag, tag->bit);

            /* make sure the response is good. */
            if(rc >= 0) { res = rc; }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}


LIB_EXPORT int plc_tag_set_int64(int32_t id, int offset, int64_t ival) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    uint64_t val = (uint64_t)(ival);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            rc = PLCTAG_ERR_NO_DATA;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(int64_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

                tag->data[offset + tag->byte_order->int64_order[0]] = (uint8_t)((val >> 0) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[1]] = (uint8_t)((val >> 8) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[2]] = (uint8_t)((val >> 16) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[3]] = (uint8_t)((val >> 24) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[4]] = (uint8_t)((val >> 32) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[5]] = (uint8_t)((val >> 40) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[6]] = (uint8_t)((val >> 48) & 0xFF);
                tag->data[offset + tag->byte_order->int64_order[7]] = (uint8_t)((val >> 56) & 0xFF);

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        } else {
            if(!val) {
                rc = plc_tag_set_bit_impl(tag, 0, 0);
            } else {
                rc = plc_tag_set_bit_impl(tag, 0, 1);
            }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}


LIB_EXPORT uint32_t plc_tag_get_uint32(int32_t id, int offset) {
    uint32_t res = UINT32_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = UINT32_MAX;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(uint32_t)) <= tag->size)) {
                res = ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[0]]) << 0)
                      + ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[1]]) << 8)
                      + ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[2]]) << 16)
                      + ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[3]]) << 24);

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        } else {
            int rc = plc_tag_get_bit_impl(tag, tag->bit);

            /* make sure the response is good. */
            if(rc >= 0) { res = (unsigned int)rc; }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}

LIB_EXPORT int plc_tag_set_uint32(int32_t id, int offset, uint32_t val) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(uint32_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

                tag->data[offset + tag->byte_order->int32_order[0]] = (uint8_t)((val >> 0) & 0xFF);
                tag->data[offset + tag->byte_order->int32_order[1]] = (uint8_t)((val >> 8) & 0xFF);
                tag->data[offset + tag->byte_order->int32_order[2]] = (uint8_t)((val >> 16) & 0xFF);
                tag->data[offset + tag->byte_order->int32_order[3]] = (uint8_t)((val >> 24) & 0xFF);

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        } else {
            if(!val) {
                rc = plc_tag_set_bit_impl(tag, 0, 0);
            } else {
                rc = plc_tag_set_bit_impl(tag, 0, 1);
            }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}

LIB_EXPORT int32_t plc_tag_get_int32(int32_t id, int offset) {
    int32_t res = INT32_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(int32_t)) <= tag->size)) {
                res = (int32_t)(((uint32_t)(tag->data[offset + tag->byte_order->int32_order[0]]) << 0)
                                + ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[1]]) << 8)
                                + ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[2]]) << 16)
                                + ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[3]]) << 24));

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        } else {
            int rc = plc_tag_get_bit_impl(tag, tag->bit);

            /* make sure the response is good. */
            if(rc >= 0) { res = (int32_t)rc; }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}

LIB_EXPORT int plc_tag_set_int32(int32_t id, int offset, int32_t ival) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    uint32_t val = (uint32_t)ival;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(int32_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

                tag->data[offset + tag->byte_order->int32_order[0]] = (uint8_t)((val >> 0) & 0xFF);
                tag->data[offset + tag->byte_order->int32_order[1]] = (uint8_t)((val >> 8) & 0xFF);
                tag->data[offset + tag->byte_order->int32_order[2]] = (uint8_t)((val >> 16) & 0xFF);
                tag->data[offset + tag->byte_order->int32_order[3]] = (uint8_t)((val >> 24) & 0xFF);

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        } else {
            if(!val) {
                rc = plc_tag_set_bit_impl(tag, 0, 0);
            } else {
                rc = plc_tag_set_bit_impl(tag, 0, 1);
            }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}

LIB_EXPORT uint16_t plc_tag_get_uint16(int32_t id, int offset) {
    uint16_t res = UINT16_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = UINT16_MAX;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(uint16_t)) <= tag->size)) {
                res = (uint16_t)(((uint16_t)(tag->data[offset + tag->byte_order->int16_order[0]]) << 0)
                                 + ((uint16_t)(tag->data[offset + tag->byte_order->int16_order[1]]) << 8));

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        } else {
            int rc = plc_tag_get_bit_impl(tag, tag->bit);

            /* make sure the response is good. */
            if(rc >= 0) { res = (uint16_t)(unsigned int)rc; }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}


LIB_EXPORT int plc_tag_set_uint16(int32_t id, int offset, uint16_t val) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(uint16_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

                tag->data[offset + tag->byte_order->int16_order[0]] = (uint8_t)((val >> 0) & 0xFF);
                tag->data[offset + tag->byte_order->int16_order[1]] = (uint8_t)((val >> 8) & 0xFF);

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        } else {
            if(!val) {
                rc = plc_tag_set_bit_impl(tag, 0, 0);
            } else {
                rc = plc_tag_set_bit_impl(tag, 0, 1);
            }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}


LIB_EXPORT int16_t plc_tag_get_int16(int32_t id, int offset) {
    int16_t res = INT16_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(int16_t)) <= tag->size)) {
                res = (int16_t)(((uint16_t)(tag->data[offset + tag->byte_order->int16_order[0]]) << 0)
                                + ((uint16_t)(tag->data[offset + tag->byte_order->int16_order[1]]) << 8));
                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        } else {
            int rc = plc_tag_get_bit_impl(tag, tag->bit);

            /* make sure the response is good. */
            if(rc >= 0) { res = (int16_t)(unsigned int)rc; }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}


LIB_EXPORT int plc_tag_set_int16(int32_t id, int offset, int16_t ival) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    uint16_t val = (uint16_t)ival;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(int16_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

                tag->data[offset + tag->byte_order->int16_order[0]] = (uint8_t)((val >> 0) & 0xFF);
                tag->data[offset + tag->byte_order->int16_order[1]] = (uint8_t)((val >> 8) & 0xFF);

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        } else {
            if(!val) {
                rc = plc_tag_set_bit_impl(tag, 0, 0);
            } else {
                rc = plc_tag_set_bit_impl(tag, 0, 1);
            }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}


LIB_EXPORT uint8_t plc_tag_get_uint8(int32_t id, int offset) {
    uint8_t res = UINT8_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = UINT8_MAX;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(uint8_t)) <= tag->size)) {
                res = tag->data[offset];
                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        } else {
            int rc = plc_tag_get_bit_impl(tag, tag->bit);

            /* make sure the response is good. */
            if(rc >= 0) { res = (uint8_t)(unsigned int)rc; }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}


LIB_EXPORT int plc_tag_set_uint8(int32_t id, int offset, uint8_t val) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(uint8_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

                tag->data[offset] = val;

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        } else {
            if(!val) {
                rc = plc_tag_set_bit_impl(tag, 0, 0);
            } else {
                rc = plc_tag_set_bit_impl(tag, 0, 1);
            }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}


LIB_EXPORT int8_t plc_tag_get_int8(int32_t id, int offset) {
    int8_t res = INT8_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(uint8_t)) <= tag->size)) {
                res = (int8_t)tag->data[offset];
                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        } else {
            int rc = plc_tag_get_bit_impl(tag, tag->bit);

            /* make sure the response is good. */
            if(rc >= 0) { res = (int8_t)(unsigned int)rc; }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}


LIB_EXPORT int plc_tag_set_int8(int32_t id, int offset, int8_t ival) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);
    uint8_t val = (uint8_t)ival;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(!tag->is_bit) {
            if((offset >= 0) && (offset + ((int)sizeof(int8_t)) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

                tag->data[offset] = val;

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        } else {
            if(!val) {
                rc = plc_tag_set_bit_impl(tag, 0, 0);
            } else {
                rc = plc_tag_set_bit_impl(tag, 0, 1);
            }
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}

LIB_EXPORT double plc_tag_get_float64(int32_t id, int offset) {
    double res = DBL_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = DBL_MIN;
            break;
        }

        if(tag->is_bit) {
            pdebug(DEBUG_WARN, "Getting float64 value is unsupported on a bit tag!");
            tag->status = PLCTAG_ERR_UNSUPPORTED;
            res = DBL_MIN;
            break;
        }

        if((offset >= 0) && (offset + ((int)sizeof(double)) <= tag->size)) {
            uint64_t ures = ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[0]]) << 0)
                            + ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[1]]) << 8)
                            + ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[2]]) << 16)
                            + ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[3]]) << 24)
                            + ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[4]]) << 32)
                            + ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[5]]) << 40)
                            + ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[6]]) << 48)
                            + ((uint64_t)(tag->data[offset + tag->byte_order->float64_order[7]]) << 56);

            /* copy the data */
            mem_copy(&res, &ures, sizeof(res));

            tag->status = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            res = DBL_MIN;
            break;
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}


LIB_EXPORT int plc_tag_set_float64(int32_t id, int offset, double fval) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(tag->is_bit) {
            pdebug(DEBUG_WARN, "Setting float64 value is unsupported on a bit tag!");
            tag->status = PLCTAG_ERR_UNSUPPORTED;
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;
        }

        if((offset >= 0) && (offset + ((int)sizeof(double)) <= tag->size)) {
            if(tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

            uint64_t val;
            /* copy the data into the uint64 value */
            mem_copy(&val, &fval, sizeof(val));

            tag->data[offset + tag->byte_order->float64_order[0]] = (uint8_t)((val >> 0) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[1]] = (uint8_t)((val >> 8) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[2]] = (uint8_t)((val >> 16) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[3]] = (uint8_t)((val >> 24) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[4]] = (uint8_t)((val >> 32) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[5]] = (uint8_t)((val >> 40) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[6]] = (uint8_t)((val >> 48) & 0xFF);
            tag->data[offset + tag->byte_order->float64_order[7]] = (uint8_t)((val >> 56) & 0xFF);

            tag->status = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}


LIB_EXPORT float plc_tag_get_float32(int32_t id, int offset) {
    float res = FLT_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            res = FLT_MIN;
            break;
        }

        if(tag->is_bit) {
            pdebug(DEBUG_WARN, "Getting float32 value is unsupported on a bit tag!");
            tag->status = PLCTAG_ERR_UNSUPPORTED;
            res = FLT_MIN;
            break;
        }

        if((offset >= 0) && (offset + ((int)sizeof(float)) <= tag->size)) {
            uint32_t ures = (uint32_t)(((uint32_t)(tag->data[offset + tag->byte_order->float32_order[0]]) << 0)
                                       + ((uint32_t)(tag->data[offset + tag->byte_order->float32_order[1]]) << 8)
                                       + ((uint32_t)(tag->data[offset + tag->byte_order->float32_order[2]]) << 16)
                                       + ((uint32_t)(tag->data[offset + tag->byte_order->float32_order[3]]) << 24));

            /* copy the data */
            mem_copy(&res, &ures, sizeof(res));

            tag->status = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            res = FLT_MIN;
            break;
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return res;
}


LIB_EXPORT int plc_tag_set_float32(int32_t id, int offset, float fval) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN, "Tag has no data!");
            tag->status = PLCTAG_ERR_NO_DATA;
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        if(tag->is_bit) {
            pdebug(DEBUG_WARN, "Setting float32 value is unsupported on a bit tag!");
            tag->status = PLCTAG_ERR_UNSUPPORTED;
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;
        }

        if((offset >= 0) && (offset + ((int)sizeof(float)) <= tag->size)) {
            if(tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

            uint32_t val;
            /* copy the data into the uint32 value */
            mem_copy(&val, &fval, sizeof(val));

            tag->data[offset + tag->byte_order->float32_order[0]] = (uint8_t)((val >> 0) & 0xFF);
            tag->data[offset + tag->byte_order->float32_order[1]] = (uint8_t)((val >> 8) & 0xFF);
            tag->data[offset + tag->byte_order->float32_order[2]] = (uint8_t)((val >> 16) & 0xFF);
            tag->data[offset + tag->byte_order->float32_order[3]] = (uint8_t)((val >> 24) & 0xFF);

            tag->status = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}


LIB_EXPORT int plc_tag_get_string(int32_t tag_id, int string_start_offset, char *buffer, int buffer_length) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(tag_id);
    int max_len = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        pdebug(DEBUG_WARN, "Tag has no definitions for strings!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_WARN, "Tag has no data!");
        tag->status = PLCTAG_ERR_NO_DATA;
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        pdebug(DEBUG_WARN, "Getting a string value from a bit tag is not supported!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* set all buffer bytes to zero. */
    mem_set(buffer, 0, buffer_length);

    critical_block(tag->api_mutex) {
        int string_length = get_string_length_unsafe(tag, string_start_offset);

        /* determine the maximum number of characters/bytes to copy. */
        if(buffer_length < string_length) {
            pdebug(DEBUG_WARN, "Buffer length, %d, is less than the string length, %d!", buffer_length, string_length);
            max_len = buffer_length;
        } else {
            max_len = string_length;
        }

        /* check the amount of space. */
        if(string_start_offset + (int)tag->byte_order->str_count_word_bytes + max_len <= tag->size) {
            for(int i = 0; i < max_len && i < tag->size; i++) {
                size_t char_index =
                    (((size_t)(unsigned int)i) ^ (tag->byte_order->str_is_byte_swapped)) /* byte swap if necessary */
                    + (size_t)(unsigned int)string_start_offset + (size_t)(unsigned int)(tag->byte_order->str_count_word_bytes);

                if(char_index < (size_t)tag->size) {
                    buffer[i] = (char)tag->data[char_index];
                } else {
                    pdebug(DEBUG_WARN, "Out of bounds index, %zu, generated!", char_index);
                    rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                    break;
                }
            }

            if(rc != PLCTAG_STATUS_OK) { break; }

            tag->status = PLCTAG_STATUS_OK;
            rc = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_WARN, "Data offset out of bounds!");
            tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        }
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


LIB_EXPORT int plc_tag_set_string(int32_t tag_id, int string_start_offset, const char *string_val) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(tag_id);
    unsigned int string_length = 0;
    unsigned int string_data_start_offset = (unsigned int)string_start_offset;

    pdebug(DEBUG_DETAIL, "Starting with string %s.", string_val);

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        pdebug(DEBUG_WARN, "Tag has no definitions for strings!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(!string_val) {
        pdebug(DEBUG_WARN, "New string value pointer is null!");
        tag->status = PLCTAG_ERR_NULL_PTR;
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_NULL_PTR;
    }

    /* note that passing a zero-length string is valid. */

    if(tag->is_bit) {
        pdebug(DEBUG_WARN, "Setting a string value on a bit tag is not supported!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return PLCTAG_ERR_UNSUPPORTED;
    }

    string_length = (unsigned int)str_length(string_val);

    /* will the string fit in the space on the PLC?  If we have a max capacity we check. */
    if(tag->byte_order->str_max_capacity && string_length > tag->byte_order->str_max_capacity) {
        pdebug(DEBUG_WARN, "String is longer, %u bytes, than the maximum capacity, %u!", string_length,
               tag->byte_order->str_max_capacity);
        rc = PLCTAG_ERR_TOO_LARGE;
        tag->status = (int8_t)rc;
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return rc;
    }

    /* we may be changing things, so take the mutex */
    critical_block(tag->api_mutex) {
        int old_string_size_in_buffer = 0;
        int new_string_size_in_buffer = 0;

        old_string_size_in_buffer = get_string_total_length_unsafe(tag, string_start_offset);
        if(old_string_size_in_buffer < 0) {
            pdebug(DEBUG_WARN, "Error getting existing string size in the tag buffer!");
            rc = old_string_size_in_buffer;
            break;
        }

        new_string_size_in_buffer = get_new_string_total_length_unsafe(tag, string_val);
        if(new_string_size_in_buffer < 0) {
            pdebug(DEBUG_WARN, "Error getting new string size!");
            rc = new_string_size_in_buffer;
            break;
        }

        pdebug(DEBUG_DETAIL,
               "allow_field_resize=%d, old_string_size_in_buffer=%" PRId32 ", new_string_size_in_buffer=%" PRId32 ".",
               tag->allow_field_resize, old_string_size_in_buffer, new_string_size_in_buffer);

        if(!tag->allow_field_resize && (new_string_size_in_buffer != old_string_size_in_buffer)) {
            pdebug(DEBUG_DETAIL, "This tag does not allow resizing of fields.");
            rc = PLCTAG_ERR_NOT_ALLOWED;
            break;
        }

        rc = resize_tag_buffer_at_offset_unsafe(tag, string_start_offset + old_string_size_in_buffer,
                                                string_start_offset + new_string_size_in_buffer);
        if(rc != PLCTAG_STATUS_OK) { break; }

        /* zero out the string data in the buffer. */
        pdebug(DEBUG_DETAIL, "Zeroing out the string data in the buffer.");
        for(unsigned int i = (unsigned int)string_start_offset;
            i < (unsigned int)(string_start_offset + new_string_size_in_buffer) && i < (unsigned int)tag->size; i++) {
            tag->data[i] = 0;
        }

        /* if the string is counted, set the length */
        pdebug(DEBUG_DETAIL, "Set count word if the string is counted.");
        if(tag->byte_order->str_is_counted) {
            int last_count_word_index = string_start_offset + (int)(unsigned int)tag->byte_order->str_count_word_bytes;

            if(last_count_word_index > (int)(tag->size)) {
                pdebug(DEBUG_WARN, "Unable to write valid count word as count word would go past the end of the tag buffer!");
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                tag->status = (int8_t)rc;
                break;
            }

            /* move the index of the string data start past the count word. */
            string_data_start_offset += tag->byte_order->str_count_word_bytes;

            switch(tag->byte_order->str_count_word_bytes) {
                case 1:
                    if(string_length > UINT8_MAX) {
                        pdebug(DEBUG_WARN, "String length, %u, is greater than can be expressed in a one-byte count word!",
                               string_length);
                        rc = PLCTAG_ERR_TOO_LARGE;
                        break;
                    }

                    tag->data[string_start_offset] = (uint8_t)(unsigned int)string_length;
                    break;

                case 2:
                    if(string_length > UINT16_MAX) {
                        pdebug(DEBUG_WARN, "String length, %u, is greater than can be expressed in a two-byte count word!",
                               string_length);
                        rc = PLCTAG_ERR_TOO_LARGE;
                        break;
                    }

                    tag->data[string_start_offset + tag->byte_order->int16_order[0]] =
                        (uint8_t)((((unsigned int)string_length) >> 0) & 0xFF);
                    tag->data[string_start_offset + tag->byte_order->int16_order[1]] =
                        (uint8_t)((((unsigned int)string_length) >> 8) & 0xFF);
                    break;

                case 4:
                    if(string_length > UINT32_MAX) {
                        pdebug(DEBUG_WARN, "String length, %u, is greater than can be expressed in a four-byte count word!",
                               string_length);
                        rc = PLCTAG_ERR_TOO_LARGE;
                        break;
                    }

                    tag->data[string_start_offset + tag->byte_order->int32_order[0]] =
                        (uint8_t)((((unsigned int)string_length) >> 0) & 0xFF);
                    tag->data[string_start_offset + tag->byte_order->int32_order[1]] =
                        (uint8_t)((((unsigned int)string_length) >> 8) & 0xFF);
                    tag->data[string_start_offset + tag->byte_order->int32_order[2]] =
                        (uint8_t)((((unsigned int)string_length) >> 16) & 0xFF);
                    tag->data[string_start_offset + tag->byte_order->int32_order[3]] =
                        (uint8_t)((((unsigned int)string_length) >> 24) & 0xFF);
                    break;

                default:
                    pdebug(DEBUG_WARN, "Unsupported string count size, %d!", tag->byte_order->str_count_word_bytes);
                    rc = PLCTAG_ERR_UNSUPPORTED;
                    tag->status = (int8_t)rc;
                    break;
            }
        }

        /* if status is bad, punt out of the critical block */
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s (%d) trying to set the count word!", plc_tag_decode_error(rc), rc);
            tag->status = (int8_t)rc;
            break;
        }

        /* copy the string data into the tag. */
        pdebug(DEBUG_DETAIL, "Copying %u bytes of the string into the tag data buffer.", string_length);
        for(unsigned int i = 0; i < string_length; i++) {
            size_t char_index = 0;

            if(tag->byte_order->str_is_byte_swapped) {
                char_index = string_data_start_offset + ((i & 0x01) ? i - 1 : i + 1);
            } else {
                char_index = string_data_start_offset + i;
            }

            if(char_index < (size_t)(uint32_t)tag->size) {
                tag->data[char_index] = (uint8_t)string_val[i];
            } else {
                pdebug(DEBUG_WARN, "Out of bounds index, %zu, generated during string copy!  Tag size is %" PRId32 ".",
                       char_index, tag->size);
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;

                /* note: only breaks out of the for loop, we need another break. */
                break;
            }
        }

        /* break out of the critical block if bad status. */
        if(rc != PLCTAG_STATUS_OK) {
            tag->status = (int8_t)rc;
            break;
        }

        pdebug(DEBUG_DETAIL, "If string is nul terminated we need to set the termination byte.");
        if(tag->byte_order->str_is_zero_terminated) {
            pdebug(DEBUG_DETAIL, "Setting the nul termination byte.");

            if(string_data_start_offset + string_length < (unsigned int)tag->size) {
                tag->data[string_data_start_offset + string_length] = (uint8_t)0;
            } else {
                pdebug(DEBUG_WARN, "Index of nul termination byte, %u, is outside of the tag data of %u bytes!",
                       string_data_start_offset + string_length, tag->size);
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                break;
            }
        }

        pdebug(DEBUG_DETAIL, "String data in buffer:");
        pdebug_dump_bytes(DEBUG_DETAIL, tag->data + string_start_offset, new_string_size_in_buffer);

        /* if this is an auto-write tag, set the dirty flag to eventually trigger a write */
        if(rc == PLCTAG_STATUS_OK && tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

        /* set the return and tag status. */
        rc = PLCTAG_STATUS_OK;
        tag->status = (int8_t)rc;
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_DETAIL, "Done with status %s (%d).", plc_tag_decode_error(rc), rc);

    return rc;
}


LIB_EXPORT int plc_tag_get_string_capacity(int32_t id, int string_start_offset) {
    int string_capacity = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Tag has no definitions for strings!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Tag has no data!");
        tag->status = PLCTAG_ERR_NO_DATA;
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Getting string capacity from a bit tag is not supported!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* FIXME - there is no capacity that is valid if it str_max_capacity is not set.  Should return 0 or an error. */
    critical_block(tag->api_mutex) {
        string_capacity = (tag->byte_order->str_max_capacity ? (int)(tag->byte_order->str_max_capacity) :
                                                               get_string_length_unsafe(tag, string_start_offset));
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return string_capacity;
}


LIB_EXPORT int plc_tag_get_string_length(int32_t id, int string_start_offset) {
    int string_length = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Tag has no definitions for strings!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Tag has no data!");
        tag->status = PLCTAG_ERR_NO_DATA;
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Getting string length from a bit tag is not supported!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    critical_block(tag->api_mutex) { string_length = get_string_length_unsafe(tag, string_start_offset); }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return string_length;
}


LIB_EXPORT int plc_tag_get_string_total_length(int32_t id, int string_start_offset) {
    int total_length = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* are strings defined for this tag? */
    if(!tag->byte_order || !tag->byte_order->str_is_defined) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Tag has no definitions for strings!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Tag has no data!");
        tag->status = PLCTAG_ERR_NO_DATA;
        return PLCTAG_ERR_NO_DATA;
    }

    if(tag->is_bit) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Getting a string total length from a bit tag is not supported!");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* FIXME - what about byte swapping?  If the string length is not even, what happens? */
    critical_block(tag->api_mutex) { total_length = get_string_total_length_unsafe(tag, string_start_offset); }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return total_length;
}


LIB_EXPORT int plc_tag_set_raw_bytes(int32_t id, int offset, uint8_t *buffer, int buffer_size) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Tag has no data!");
        tag->status = PLCTAG_ERR_NO_DATA;
        return PLCTAG_ERR_NO_DATA;
    }

    if(!buffer) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Buffer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(buffer_size <= 0) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "The buffer must have some capacity for data.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && ((offset + buffer_size) <= tag->size)) {
                if(tag->auto_sync_write_ms > 0) { tag->tag_is_dirty = 1; }

                int i;
                for(i = 0; i < buffer_size; i++) { tag->data[offset + i] = buffer[i]; }

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        }
    } else {
        pdebug(DEBUG_WARN, "Trying to write a list of values on a Tag bit.");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        rc = PLCTAG_ERR_UNSUPPORTED;
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}


LIB_EXPORT int plc_tag_get_raw_bytes(int32_t id, int offset, uint8_t *buffer, int buffer_size) {
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* is there data? */
    if(!tag->data) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Tag has no data!");
        tag->status = PLCTAG_ERR_NO_DATA;
        return PLCTAG_ERR_NO_DATA;
    }

    if(!buffer) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "Buffer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(buffer_size <= 0) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        pdebug(DEBUG_WARN, "The buffer must have some capacity for data.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!tag->is_bit) {
        critical_block(tag->api_mutex) {
            if((offset >= 0) && ((offset + buffer_size) <= tag->size)) {
                int i;
                for(i = 0; i < buffer_size; i++) { buffer[i] = tag->data[offset + i]; }

                tag->status = PLCTAG_STATUS_OK;
            } else {
                pdebug(DEBUG_WARN, "Data offset out of bounds!");
                tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
                rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        }
    } else {
        pdebug(DEBUG_WARN, "Trying to read a list of values from a Tag bit.");
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        rc = PLCTAG_ERR_UNSUPPORTED;
    }

    pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
    rc_dec(tag);

    return rc;
}


/*****************************************************************************************************
 *****************************  Support routines for extra indirection *******************************
 ****************************************************************************************************/

int set_tag_byte_order(plc_tag_p tag, attr attribs)

{
    int use_default = 1;

    pdebug(DEBUG_INFO, "Starting.");

    /* the default values are already set in the tag. */

    /* check for overrides. */
    if(attr_get_str(attribs, "int16_byte_order", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "int32_byte_order", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "int64_byte_order", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "float32_byte_order", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "float64_byte_order", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_is_counted", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_is_fixed_length", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_is_zero_terminated", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_is_byte_swapped", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_count_word_bytes", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_max_capacity", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_total_length", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_pad_bytes", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_pad_to_multiple_bytes_EXPERIMENTAL", NULL) != NULL) { use_default = 0; }

    /* if we need to override something, build a new byte order structure. */
    if(!use_default) {
        const char *byte_order_str = NULL;
        int str_param = 0;
        int rc = PLCTAG_STATUS_OK;
        tag_byte_order_t *new_byte_order = mem_alloc((int)(unsigned int)sizeof(*(tag->byte_order)));

        if(!new_byte_order) {
            pdebug(DEBUG_WARN, "Unable to allocate byte order struct for tag!");
            return PLCTAG_ERR_NO_MEM;
        }

        /* copy the defaults. */
        *new_byte_order = *(tag->byte_order);

        /* replace the old byte order. */
        tag->byte_order = new_byte_order;

        /* mark it as allocated so that we free it later. */
        tag->byte_order->is_allocated = 1;

        /* 16-bit ints. */
        byte_order_str = attr_get_str(attribs, "int16_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_DETAIL, "Override byte order int16_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 2);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Byte order string int16_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            /* strange gyrations to make the compiler happy.   MSVC will probably complain. */
            tag->byte_order->int16_order[0] = (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x01);
            tag->byte_order->int16_order[1] = (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x01);
        }

        /* 32-bit ints. */
        byte_order_str = attr_get_str(attribs, "int32_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_DETAIL, "Override byte order int32_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 4);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Byte order string int32_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            tag->byte_order->int32_order[0] = (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x03);
            tag->byte_order->int32_order[1] = (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x03);
            tag->byte_order->int32_order[2] = (int)(unsigned int)(((unsigned int)byte_order_str[2] - (unsigned int)('0')) & 0x03);
            tag->byte_order->int32_order[3] = (int)(unsigned int)(((unsigned int)byte_order_str[3] - (unsigned int)('0')) & 0x03);
        }

        /* 64-bit ints. */
        byte_order_str = attr_get_str(attribs, "int64_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_DETAIL, "Override byte order int64_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 8);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Byte order string int64_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            tag->byte_order->int64_order[0] = (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[1] = (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[2] = (int)(unsigned int)(((unsigned int)byte_order_str[2] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[3] = (int)(unsigned int)(((unsigned int)byte_order_str[3] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[4] = (int)(unsigned int)(((unsigned int)byte_order_str[4] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[5] = (int)(unsigned int)(((unsigned int)byte_order_str[5] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[6] = (int)(unsigned int)(((unsigned int)byte_order_str[6] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[7] = (int)(unsigned int)(((unsigned int)byte_order_str[7] - (unsigned int)('0')) & 0x07);
        }

        /* 32-bit floats. */
        byte_order_str = attr_get_str(attribs, "float32_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_DETAIL, "Override byte order float32_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 4);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Byte order string float32_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            tag->byte_order->float32_order[0] =
                (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x03);
            tag->byte_order->float32_order[1] =
                (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x03);
            tag->byte_order->float32_order[2] =
                (int)(unsigned int)(((unsigned int)byte_order_str[2] - (unsigned int)('0')) & 0x03);
            tag->byte_order->float32_order[3] =
                (int)(unsigned int)(((unsigned int)byte_order_str[3] - (unsigned int)('0')) & 0x03);
        }

        /* 64-bit floats */
        byte_order_str = attr_get_str(attribs, "float64_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_DETAIL, "Override byte order float64_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 8);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Byte order string float64_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            tag->byte_order->float64_order[0] =
                (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[1] =
                (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[2] =
                (int)(unsigned int)(((unsigned int)byte_order_str[2] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[3] =
                (int)(unsigned int)(((unsigned int)byte_order_str[3] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[4] =
                (int)(unsigned int)(((unsigned int)byte_order_str[4] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[5] =
                (int)(unsigned int)(((unsigned int)byte_order_str[5] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[6] =
                (int)(unsigned int)(((unsigned int)byte_order_str[6] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[7] =
                (int)(unsigned int)(((unsigned int)byte_order_str[7] - (unsigned int)('0')) & 0x07);
        }

        /* string information. */

        /* is the string counted? */
        if(attr_get_str(attribs, "str_is_counted", NULL)) {
            str_param = attr_get_int(attribs, "str_is_counted", 0);
            if(str_param == 1 || str_param == 0) {
                tag->byte_order->str_is_counted = (str_param ? 1 : 0);
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_is_counted must be missing, zero (0) or one (1)!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* is the string a fixed length? */
        if(attr_get_str(attribs, "str_is_fixed_length", NULL)) {
            str_param = attr_get_int(attribs, "str_is_fixed_length", 0);
            if(str_param == 1 || str_param == 0) {
                tag->byte_order->str_is_fixed_length = (str_param ? 1 : 0);
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_is_fixed_length must be missing, zero (0) or one (1)!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* is the string zero terminated? */
        if(attr_get_str(attribs, "str_is_zero_terminated", NULL)) {
            str_param = attr_get_int(attribs, "str_is_zero_terminated", 0);
            if(str_param == 1 || str_param == 0) {
                tag->byte_order->str_is_zero_terminated = (str_param ? 1 : 0);
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_is_zero_terminated must be missing, zero (0) or one (1)!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* is the string byteswapped like PLC/5? */
        if(attr_get_str(attribs, "str_is_byte_swapped", NULL)) {
            str_param = attr_get_int(attribs, "str_is_byte_swapped", 0);
            if(str_param == 1 || str_param == 0) {
                tag->byte_order->str_is_byte_swapped = (str_param ? 1 : 0);
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_is_byte_swapped must be missing, zero (0) or one (1)!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* main string parameters. */

        /* how many bytes is the string count word? */
        if(attr_get_str(attribs, "str_count_word_bytes", NULL)) {
            str_param = attr_get_int(attribs, "str_count_word_bytes", 0);
            if(str_param == 0 || str_param == 1 || str_param == 2 || str_param == 4 || str_param == 8) {
                tag->byte_order->str_count_word_bytes = (unsigned int)str_param;
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_count_word_bytes must be missing, 0, 1, 2, 4, or 8!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* What is the string maximum capacity */
        if(attr_get_str(attribs, "str_max_capacity", NULL)) {
            str_param = attr_get_int(attribs, "str_max_capacity", 0);
            if(str_param >= 0) {
                tag->byte_order->str_max_capacity = (unsigned int)str_param;
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_max_capacity must be missing, 0, or positive!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* What is the string total length */
        if(attr_get_str(attribs, "str_total_length", NULL)) {
            str_param = attr_get_int(attribs, "str_total_length", 0);
            if(str_param >= 0) {
                tag->byte_order->str_total_length = (unsigned int)str_param;
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_total_length must be missing, 0, or positive!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* What is the string padding length */
        if(attr_get_str(attribs, "str_pad_bytes", NULL)) {
            str_param = attr_get_int(attribs, "str_pad_bytes", 0);
            if(str_param >= 0) {
                tag->byte_order->str_pad_bytes = (unsigned int)str_param;
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_pad_bytes must be missing, 0, or positive!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* Should we pad the string to a multiple of 1 (no padding), 2, or 4 bytes. Adding padding causes issues when writing
           OmronNJ strings, 2 byte padding is required for certain AB PLCs*/
        if(attr_get_str(attribs, "str_pad_to_multiple_bytes_EXPERIMENTAL", NULL)) {
            str_param = attr_get_int(attribs, "str_pad_to_multiple_bytes_EXPERIMENTAL", 0);
            if(str_param == 0 || str_param == 1 || str_param == 2 || str_param == 4) {
                if(str_param == 0) {
                    str_param = 1;
                } /* Padding to 0 bytes doesnt make much sense, so we overwride to 1 byte which means no padding */
                tag->byte_order->str_pad_to_multiple_bytes = (unsigned int)str_param;
            } else {
                pdebug(DEBUG_WARN, "Tag string attribute str_pad_to_multiple_bytes must be missing, 1, 2 or 4!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* now make sure that the combination of settings works. */

        /* if we have a counted string, we need the count! */
        if(tag->byte_order->str_is_counted) {
            if(tag->byte_order->str_count_word_bytes == 0) {
                pdebug(
                    DEBUG_WARN,
                    "If a string definition is counted, you must use both \"str_is_counted\" and \"str_count_word_bytes\" parameters!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* if we have a fixed length string, we need to know what the length is! */
        if(tag->byte_order->str_is_fixed_length) {
            if(tag->byte_order->str_total_length == 0) {
                pdebug(
                    DEBUG_WARN,
                    "If a string definition is fixed length, you must use both \"str_is_fixed_length\" and \"str_total_length\" parameters!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* check the total length. */
        if(tag->byte_order->str_total_length > 0
           && (tag->byte_order->str_is_zero_terminated + tag->byte_order->str_max_capacity + tag->byte_order->str_count_word_bytes
               + tag->byte_order->str_pad_bytes)
                  > tag->byte_order->str_total_length) {
            pdebug(DEBUG_WARN, "Tag string total length, %d bytes, must be at least the sum, %d, of the other string components!",
                   tag->byte_order->str_total_length,
                   tag->byte_order->str_is_zero_terminated + tag->byte_order->str_max_capacity
                       + tag->byte_order->str_count_word_bytes + tag->byte_order->str_pad_bytes);
            pdebug(DEBUG_DETAIL, "str_is_zero_terminated=%d, str_max_capacity=%d, str_count_word_bytes=%d, str_pad_bytes=%d",
                   tag->byte_order->str_is_zero_terminated, tag->byte_order->str_max_capacity,
                   tag->byte_order->str_count_word_bytes, tag->byte_order->str_pad_bytes);
            return PLCTAG_ERR_BAD_PARAM;
        }

        /* Do we have enough of a definition for a string? */
        /* FIXME - This is probably not enough checking! */
        if(tag->byte_order->str_is_counted || tag->byte_order->str_is_zero_terminated) {
            tag->byte_order->str_is_defined = 1;
        } else {
            pdebug(DEBUG_WARN, "Insufficient definitions found to support strings!");
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}

int check_byte_order_str(const char *byte_order, int length) {
    int taken[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int byte_order_len = str_length(byte_order);

    pdebug(DEBUG_DETAIL, "Starting.");

    /* check the size. */
    if(byte_order_len != length) {
        pdebug(DEBUG_WARN, "Byte order string, \"%s\", must be %d characters long!", byte_order, length);
        return (byte_order_len < length ? PLCTAG_ERR_TOO_SMALL : PLCTAG_ERR_TOO_LARGE);
    }

    /* check each character. */
    for(int i = 0; i < byte_order_len; i++) {
        int val = 0;

        if(!isdigit(byte_order[i]) || byte_order[i] < '0' || byte_order[i] > '7') {
            pdebug(DEBUG_WARN, "Byte order string, \"%s\", must be only characters from '0' to '7'!", byte_order);
            return PLCTAG_ERR_BAD_DATA;
        }

        /* get the numeric value. */
        val = byte_order[i] - '0';

        if(val < 0 || val > (length - 1)) {
            pdebug(DEBUG_WARN, "Byte order string, \"%s\", must only values from 0 to %d!", byte_order, (length - 1));
            return PLCTAG_ERR_BAD_DATA;
        }

        if(taken[val]) {
            pdebug(DEBUG_WARN, "Byte order string, \"%s\", must use each digit exactly once!", byte_order);
            return PLCTAG_ERR_BAD_DATA;
        }

        taken[val] = 1;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


plc_tag_p lookup_tag(int32_t tag_id) {
    plc_tag_p tag = NULL;

    critical_block(tag_lookup_mutex) {
        tag = hashtable_get(tags, (int64_t)tag_id);

        if(tag) {
            debug_set_tag_id(tag->tag_id);
        } else {
            /* TODO - remove this. */
            pdebug(DEBUG_WARN, "Tag with ID %d not found.", tag_id);
        }

        if(tag && tag->tag_id == tag_id) {
            pdebug(DEBUG_SPEW, "Found tag %p with id %d.", tag, tag->tag_id);
            pdebug(DEBUG_DETAIL, "rc_inc: Acquiring reference to tag %" PRId32 ".", tag->tag_id);
            tag = rc_inc(tag);
        } else {
            debug_set_tag_id(0);
            tag = NULL;
        }
    }

    return tag;
}


int tag_id_inc(int id) {
    if(id <= 0) {
        pdebug(DEBUG_ERROR, "Incoming ID is not valid! Got %d", id);
        /* try to correct. */
        id = (TAG_ID_MASK / 2);
    }

    id = (id + 1) & TAG_ID_MASK;

    if(id == 0) { id = 1; /* skip zero intentionally! Can't return an ID of zero because it looks like a NULL pointer */ }

    return id;
}


int add_tag_lookup(plc_tag_p tag) {
    int rc = PLCTAG_ERR_NOT_FOUND;
    int new_id = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(tag_lookup_mutex) {
        int attempts = 0;

        /* only get this when we hold the mutex. */
        new_id = next_tag_id;

        do {
            new_id = tag_id_inc(new_id);

            if(new_id <= 0) {
                pdebug(DEBUG_WARN, "ID %d is illegal!", new_id);
                attempts = MAX_TAG_MAP_ATTEMPTS;
                break;
            }

            pdebug(DEBUG_SPEW, "Trying new ID %d.", new_id);

            if(!hashtable_get(tags, (int64_t)new_id)) {
                pdebug(DEBUG_DETAIL, "Found unused ID %d", new_id);
                break;
            }

            attempts++;
        } while(attempts < MAX_TAG_MAP_ATTEMPTS);

        if(attempts < MAX_TAG_MAP_ATTEMPTS) {
            rc = hashtable_put(tags, (int64_t)new_id, tag);
        } else {
            rc = PLCTAG_ERR_NO_RESOURCES;
        }

        next_tag_id = new_id;
    }

    if(rc != PLCTAG_STATUS_OK) { new_id = rc; }

    pdebug(DEBUG_DETAIL, "Done.");

    return new_id;
}


/**
 * @brief Get the total length of the string currently in the tag.
 *
 * @param tag
 * @param string_start_offset
 * @return int
 */
int get_string_total_length_unsafe(plc_tag_p tag, int string_start_offset) {
    int total_length = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    total_length = (int)(tag->byte_order->str_count_word_bytes)
                   + (tag->byte_order->str_is_fixed_length ? (int)(tag->byte_order->str_max_capacity) :
                                                             get_string_length_unsafe(tag, string_start_offset))
                   + (tag->byte_order->str_is_zero_terminated ? (int)1 : (int)0) + (int)(tag->byte_order->str_pad_bytes);

    pdebug(DEBUG_DETAIL, "Done with length %d.", total_length);

    return total_length;
}


/*
 * get the string length depending on the PLC string type.
 *
 * This is called in other functions so is separated out.
 *
 * This must be called with the tag API mutex held!
 */

int get_string_length_unsafe(plc_tag_p tag, int offset) {
    int string_length = 0;

    if(tag->byte_order->str_is_counted) {
        switch(tag->byte_order->str_count_word_bytes) {
            case 1: string_length = (int)(unsigned int)(tag->data[offset]); break;

            case 2:
                string_length = (int16_t)(uint16_t)(((uint16_t)(tag->data[offset + tag->byte_order->int16_order[0]]) << 0)
                                                    + ((uint16_t)(tag->data[offset + tag->byte_order->int16_order[1]]) << 8));
                break;

            case 4:
                string_length = (int32_t)(((uint32_t)(tag->data[offset + tag->byte_order->int32_order[0]]) << 0)
                                          + ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[1]]) << 8)
                                          + ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[2]]) << 16)
                                          + ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[3]]) << 24));
                break;

            default:
                pdebug(DEBUG_WARN, "Unsupported string count word size, %d bytes!", tag->byte_order->str_count_word_bytes);
                return 0; /* FIXME - this should be an error code. */
                break;
        }

    } else {
        if(tag->byte_order->str_is_zero_terminated) {
            /* slow, but hopefully correct. */

            /*
             * note that this will count the correct length of a string that runs up against
             * the end of the tag buffer.
             */
            for(int i = offset + (int)(tag->byte_order->str_count_word_bytes); i < tag->size; i++) {
                size_t char_index =
                    (((size_t)(unsigned int)string_length) ^ (tag->byte_order->str_is_byte_swapped)) /* byte swap if necessary */
                    + (size_t)(unsigned int)offset + (size_t)(unsigned int)(tag->byte_order->str_count_word_bytes);

                if(tag->data[char_index] == (uint8_t)0) {
                    /* found the end. */
                    break;
                }

                string_length++;
            }
        } else {
            /* it is not counted or zero terminated, so it is not supported. */
            pdebug(DEBUG_WARN, "Unsupported string length type.   Must be counted or zero-terminated!");
            return 0; /* FIXME this should be an error code. */
        }
    }

    return string_length;
}


int get_new_string_total_length_unsafe(plc_tag_p tag, const char *string_val) {
    int rc = PLCTAG_STATUS_OK;
    unsigned int string_size_in_buffer = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        int string_length = str_length(string_val);

        /* if this is a fixed-size string, use that data. */
        if(tag->byte_order->str_is_fixed_length) {
            if(tag->byte_order->str_total_length) {
                string_size_in_buffer = tag->byte_order->str_total_length;
                pdebug(DEBUG_DETAIL, "String is fixed size, so use the total length %d as the size in the buffer.",
                       tag->byte_order->str_total_length);
                break;
            } else {
                pdebug(
                    DEBUG_WARN,
                    "Unsupported configuration.  You must set the total string length if you set the flag for string is fixed size!");
                rc = PLCTAG_ERR_BAD_CONFIG;
                break;
            }
        }

        /* add the incoming string size. */
        string_size_in_buffer = (unsigned int)string_length;
        pdebug(DEBUG_DETAIL, "String size in buffer is at least %u after the incoming string length %u.", string_size_in_buffer,
               string_length);

        /* OK the string will fit, now lets add the count word if any. */
        if(tag->byte_order->str_count_word_bytes) {
            string_size_in_buffer += tag->byte_order->str_count_word_bytes;
            pdebug(DEBUG_DETAIL, "String size in buffer is %u after adding count word size, %u.", string_size_in_buffer,
                   tag->byte_order->str_count_word_bytes);
        }

        /* any terminator byte? */
        if(tag->byte_order->str_is_zero_terminated) {
            string_size_in_buffer += 1;
            pdebug(DEBUG_DETAIL, "String is zero terminated so the string size in the tag buffer is at least %u.",
                   string_size_in_buffer);
        }

        /* any pad bytes? */
        if(tag->byte_order->str_pad_bytes) {
            string_size_in_buffer += tag->byte_order->str_pad_bytes;
            pdebug(DEBUG_DETAIL, "String has %u padding bytes so the string size in the tag buffer is at least %u.",
                   tag->byte_order->str_pad_bytes, string_size_in_buffer);
        }

        /* bytes reordered?  If so, we need an even string size in the buffer. */
        if(tag->byte_order->str_is_byte_swapped) {
            if(string_length & 0x01) {
                string_size_in_buffer += 1;
                pdebug(DEBUG_DETAIL, "String is byte swapped so length is now %u.", string_size_in_buffer);
            }
        }

        pdebug(DEBUG_DETAIL, "Final string size in the tag buffer is %u bytes.", string_size_in_buffer);

    } while(0);

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Done with size %d.", string_size_in_buffer);
        return (int)string_size_in_buffer;
    } else {
        pdebug(DEBUG_WARN, "Error %s found while calculating the new string size in the tag buffer.", plc_tag_decode_error(rc));
        return rc;
    }
}


int resize_tag_buffer_unsafe(plc_tag_p tag, int new_size) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        uint8_t *new_data = NULL;

        pdebug(DEBUG_INFO, "Changing the tag buffer size from %d to %d.", tag->size, new_size);

        new_data = mem_realloc(tag->data, (int)new_size);
        if(!new_data) {
            pdebug(DEBUG_WARN, "Unable to allocate new tag data buffer!");
            rc = PLCTAG_ERR_NO_MEM;
            tag->status = (int8_t)rc;
            break;
        }

        tag->data = new_data;
        tag->size = new_size;
    } while(0);

    pdebug(DEBUG_DETAIL, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}


int resize_tag_buffer_at_offset_unsafe(plc_tag_p tag, int old_split_index, int new_split_index) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        pdebug(DEBUG_DETAIL, "Move old index %d to new index %d.", old_split_index, new_split_index);

        /* double check the data. */
        if(old_split_index < 0 || old_split_index > tag->size) {
            /* not good. */
            pdebug(DEBUG_WARN, "Old split index %d is outside tag data, %d bytes!", old_split_index, tag->size);
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        if(new_split_index < 0) {
            /* not good. */
            pdebug(DEBUG_WARN, "New split index %d is outside tag data!", old_split_index);
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        if(new_split_index == old_split_index) {
            /* nothing to do! */
            pdebug(DEBUG_INFO, "Tag new size is the same as the tag old size so nothing to do.");
            break;
        }

        /* are we shrinking or growing? */
        if(new_split_index < old_split_index) {
            pdebug(DEBUG_DETAIL, "Shrinking tag buffer by %d bytes", old_split_index - new_split_index);

            /* shrinking.  We must move the existing data down to the new end point of the string. */
            void *old_split_ptr = tag->data + old_split_index;
            void *new_split_ptr = tag->data + new_split_index;
            int amount_to_move = tag->size - old_split_index;
            int new_tag_size = tag->size - (old_split_index - new_split_index);

            /* amount_to_move will be positive or zero because of the above if check. */
            mem_move(new_split_ptr, old_split_ptr, amount_to_move);

            rc = resize_tag_buffer_unsafe(tag, new_tag_size);
            break;
        }

        /* are we shrinking or growing? */
        if(new_split_index > old_split_index) {
            void *old_split_ptr = NULL;
            void *new_split_ptr = NULL;

            pdebug(DEBUG_DETAIL, "Growing tag buffer by %d bytes", new_split_index - old_split_index);

            /* growing.  We must move the existing data up to the new end point of the string. */
            int amount_to_move = tag->size - old_split_index;
            int new_tag_size = tag->size + (new_split_index - old_split_index);

            /* resize the buffer now. */
            rc = resize_tag_buffer_unsafe(tag, new_tag_size);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to resize the tag buffer!");
                break;
            }

            /* the tag data pointer may have changed, now calculate the two pointers. */
            old_split_ptr = tag->data + old_split_index;
            new_split_ptr = tag->data + new_split_index;

            /* amount_to_move will be positive or zero because of the above if check. */
            mem_move(new_split_ptr, old_split_ptr, amount_to_move);
        }
    } while(0);

    pdebug(DEBUG_DETAIL, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}

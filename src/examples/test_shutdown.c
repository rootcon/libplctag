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


#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define REQUIRED_VERSION 2, 5, 5
#define TAG_ATTRIBS_TMPL \
    "protocol=ab_eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_type=DINT&elem_count=1&name=TestBigArray[%d]&auto_sync_read_ms=200&auto_sync_write_ms=20"
#define DATA_TIMEOUT (5000)
#define RUN_PERIOD (10000)
#define READ_SLEEP_MS (100)
#define WRITE_SLEEP_MS (300)

#define READ_PERIOD_MS (200)

static volatile int read_start_count = 0;
static volatile int read_complete_count = 0;
static volatile int write_start_count = 0;
static volatile int write_complete_count = 0;


static void *reader_function(void *tag_arg);
static void *writer_function(void *tag_arg);
static void tag_callback(int32_t tag_id, int event, int status, void *not_used);


#define NUM_TAGS (10)


int main(void) {
    int rc = PLCTAG_STATUS_OK;
    char tag_attr_str[sizeof(TAG_ATTRIBS_TMPL) + 10] = {0};
    compat_thread_t read_threads[NUM_TAGS];
    compat_thread_t write_threads[NUM_TAGS];
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        // NOLINTNEXTLINE
        fprintf(stderr, "Available library version is %d.%d.%d.\n", version_major, version_minor, version_patch);
        exit(1);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    /* create all the tags. */
    fprintf(stderr, "Creating tag handles ");

    for(int i = 0; i < NUM_TAGS; i++) {
        int32_t tag_id = PLCTAG_ERR_CREATE;

        // NOLINTNEXTLINE
        snprintf(tag_attr_str, sizeof(tag_attr_str), TAG_ATTRIBS_TMPL, (int32_t)i);
        tag_id = plc_tag_create_ex(tag_attr_str, tag_callback, NULL, DATA_TIMEOUT);

        if(tag_id <= 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Error %s trying to create tag %d!\n", plc_tag_decode_error(tag_id), i);
            plc_tag_shutdown();
            return 1;
        }

        /* create read and write thread for this tag. */
        /* FIXME - check error returns! */
        compat_thread_create(&read_threads[i], reader_function, (void *)(intptr_t)tag_id);
        compat_thread_create(&write_threads[i], writer_function, (void *)(intptr_t)tag_id);

        fprintf(stderr, ".");
    }

    fprintf(stderr, "\nWaiting for threads to stabilize %dms.\n", RUN_PERIOD / 2);

    /* let everything run for a while */
    compat_sleep_ms(RUN_PERIOD / 2, NULL);

    /* forcible shut down the entire library. */
    // NOLINTNEXTLINE
    fprintf(stderr, "Forcing library shutdown.\n");

    plc_tag_set_debug_level(PLCTAG_DEBUG_INFO);

    plc_tag_shutdown();

    // NOLINTNEXTLINE
    fprintf(stderr, "Waiting for threads to quit.\n");

    for(int i = 0; i < NUM_TAGS; i++) {
        compat_thread_join(read_threads[i], NULL);
        compat_thread_join(write_threads[i], NULL);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Done.\n");

    return rc;
}


void *reader_function(void *tag_arg) {
    int32_t tag_id = (int32_t)(intptr_t)tag_arg;
    int64_t start_time = compat_time_ms();
    int64_t run_until = start_time + RUN_PERIOD;
    int iteration = 1;

    while(run_until > compat_time_ms()) {
        int status = plc_tag_status(tag_id);
        int32_t val = plc_tag_get_int32(tag_id, 0);

        if(status < 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %" PRId32 " has error status %s, terminating!\n", tag_id, plc_tag_decode_error(status));
            break;
        }

        // NOLINTNEXTLINE
        fprintf(stderr, "READER: Tag %" PRId32 " iteration %d, got value: %d at time %" PRId64 "\n", tag_id, iteration++, val,
                compat_time_ms() - start_time);

        compat_sleep_ms(READ_SLEEP_MS, NULL);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Reader thread for tag ID %" PRId32 " exiting.\n", tag_id);

    return 0;
}


void *writer_function(void *tag_arg) {
    int32_t tag_id = (int32_t)(intptr_t)tag_arg;
    int64_t start_time = compat_time_ms();
    int64_t run_until = start_time + RUN_PERIOD;
    int iteration = 1;

    compat_sleep_ms(WRITE_SLEEP_MS, NULL);

    while(run_until > compat_time_ms()) {
        int32_t val = plc_tag_get_int32(tag_id, 0);
        int32_t new_val = ((val + 1) > 499) ? 0 : (val + 1);
        int status = plc_tag_status(tag_id);

        if(status < 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %" PRId32 " has error status %s, terminating!\n", tag_id, plc_tag_decode_error(status));
            break;
        }

        /* write the value */
        plc_tag_set_int32(tag_id, 0, new_val);

        // NOLINTNEXTLINE
        fprintf(stderr, "WRITER: Tag %" PRId32 " iteration %d, wrote value: %d at time %" PRId64 "\n", tag_id, iteration++,
                new_val, compat_time_ms() - start_time);

        compat_sleep_ms(WRITE_SLEEP_MS, NULL);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Writer thread for tag ID %" PRId32 " exiting.\n", tag_id);

    return 0;
}


void tag_callback(int32_t tag_id, int event, int status, void *not_used) {
    (void)not_used;

    /* handle the events. */
    switch(event) {
        case PLCTAG_EVENT_CREATED:
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %" PRId32 " created.\n", tag_id);
            break;

        case PLCTAG_EVENT_ABORTED:
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %" PRId32 " automatic operation was aborted!\n", tag_id);
            break;

        case PLCTAG_EVENT_DESTROYED:
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %" PRId32 " was destroyed.\n", tag_id);
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            read_complete_count++;
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %" PRId32 " automatic read operation completed with status %s.\n", tag_id,
                    plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_READ_STARTED:
            read_start_count++;
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %" PRId32 " automatic read operation started with status %s.\n", tag_id,
                    plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_WRITE_COMPLETED:
            write_complete_count++;
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %" PRId32 " automatic write operation completed with status %s.\n", tag_id,
                    plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_WRITE_STARTED:
            write_start_count++;
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %" PRId32 " automatic write operation started with status %s.\n", tag_id,
                    plc_tag_decode_error(status));

            break;

        default:
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %" PRId32 " unexpected event %d!\n", tag_id, event);
            break;
    }
}

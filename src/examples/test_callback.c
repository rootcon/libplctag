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
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRED_VERSION 2, 5, 0

#define TAG_PATH "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=10&name=TestBigArray"
#define DATA_TIMEOUT 5000

typedef int32_t DINT;

static volatile DINT *TestDINTArray = NULL;

void tag_callback(int32_t tag_id, int event, int status) {
    /* handle the events. */
    switch(event) {
        case PLCTAG_EVENT_ABORTED: printf("Tag operation was aborted with status %s!\n", plc_tag_decode_error(status)); break;

        case PLCTAG_EVENT_CREATED: printf("Tag created with status %s.\n", plc_tag_decode_error(status)); break;

        case PLCTAG_EVENT_DESTROYED:
            if(TestDINTArray) {
                free((void *)TestDINTArray);
                TestDINTArray = NULL;
            }
            printf("Tag was destroyed with status %s.\n", plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            if(status == PLCTAG_STATUS_OK && TestDINTArray) {
                int elem_count = plc_tag_get_int_attribute(tag_id, "elem_count", -1);
                int elem_size = plc_tag_get_int_attribute(tag_id, "elem_size", 0);

                for(int i = 0; i < elem_count; i++) { TestDINTArray[i] = plc_tag_get_int32(tag_id, (i * elem_size)); }
            }

            printf("Tag read operation completed with status %s.\n", plc_tag_decode_error(status));

            break;

        case PLCTAG_EVENT_READ_STARTED:
            printf("Tag read operation started with status %s.\n", plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_WRITE_COMPLETED:
            printf("Tag write operation completed with status %s!\n", plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_WRITE_STARTED:
            if(status == PLCTAG_STATUS_OK && TestDINTArray) {
                int elem_count = plc_tag_get_int_attribute(tag_id, "elem_count", -1);
                int elem_size = plc_tag_get_int_attribute(tag_id, "elem_size", 0);

                for(int i = 0; i < elem_count; i++) { plc_tag_set_int32(tag_id, (i * elem_size), TestDINTArray[i]); }
            }

            printf("Tag write operation started with status %s.\n", plc_tag_decode_error(status));

            break;

        default: printf("Unexpected event %d!\n", event); break;
    }
}


void log_callback(int32_t tag_id, int debug_level, const char *message) {
    // NOLINTNEXTLINE
    fprintf(stderr, "Log message of level %d for tag %d: %s", debug_level, tag_id, message);
}

int main(void) {
    int32_t tag = 0;
    int rc;
    int i;
    int elem_count = 0;
    int elem_size = 0;
    int64_t start = 0;
    int64_t end = 0;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION, version_major,
               version_minor, version_patch);
        return 1;
    }

    printf("Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    /* set up the logger. */
    printf("Setting up logger callback.\n");
    rc = plc_tag_register_logger(log_callback);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: %s: Could not register log callback!\n", plc_tag_decode_error(rc));
        return 1;
    }

    /* try again to see if we get the right error. */
    printf("Testing duplicate logger callback registration.\n");
    rc = plc_tag_register_logger(log_callback);
    if(rc != PLCTAG_ERR_DUPLICATE) {
        printf("ERROR: %s: Did not get PLCTAG_ERR_DUPLICATE when registering the logger again!\n", plc_tag_decode_error(rc));
        return 1;
    }

    /* Remove the logger. */
    printf("Testing logger callback unregister.\n");
    rc = plc_tag_unregister_logger();
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: %s: Got error when unregistering the log callback!\n", plc_tag_decode_error(rc));
        return 1;
    }

    /* Remove the logger again. */
    printf("Testing duplicate logger callback unregistration.\n");
    rc = plc_tag_unregister_logger();
    if(rc != PLCTAG_ERR_NOT_FOUND) {
        printf("ERROR: %s: Did not get PLCTAG_ERR_NOT_FOUND when unregistering the logger again!\n", plc_tag_decode_error(rc));
        return 1;
    }

    /* set up the logger again. */
    rc = plc_tag_register_logger(log_callback);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: %s: Could not register log callback after removing it!\n", plc_tag_decode_error(rc));
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* create the tag */
    tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT);
    if(tag < 0) {
        printf("ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return 1;
    }

    printf("Removing logger callback.  Should see regular logging output now.\n");
    rc = plc_tag_unregister_logger();
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: %s: Got error when removing the logger callback!\n", plc_tag_decode_error(rc));
        return 1;
    }

    /* set up the local array. */
    elem_count = plc_tag_get_int_attribute(tag, "elem_count", -1);
    elem_size = plc_tag_get_int_attribute(tag, "elem_size", -1);

    if(elem_size == -1 || elem_count == -1) {
        printf("Unable to get elem_count (%d) or elem_size (%d)!\n", elem_count, elem_size);
        plc_tag_destroy(tag);
        return 1;
    }

    TestDINTArray = calloc((size_t)elem_count, (size_t)elem_size);
    if(!TestDINTArray) {
        printf("Unable to allocate memory for tag array!\n");
        plc_tag_destroy(tag);
        return 1;
    }

    /* test registering the callback  */
    rc = plc_tag_register_callback(tag, tag_callback);
    if(rc != PLCTAG_STATUS_OK) {
        printf("Got incorrect status when registering callback %s!\n", plc_tag_decode_error(rc));
        free((void *)TestDINTArray);
        TestDINTArray = NULL;
        plc_tag_destroy(tag);
        return 1;
    }

    /* test registering the callback again, should be an error */
    rc = plc_tag_register_callback(tag, tag_callback);
    if(rc != PLCTAG_ERR_DUPLICATE) {
        printf("Got incorrect status when registering callback twice %s!\n", plc_tag_decode_error(rc));

        /*
         * we do not need to free the array TestDINTArray because the callback will do
         * it when the tag is destroyed.
         */

        plc_tag_destroy(tag);
        return 1;
    }


    /* get the data */
    rc = plc_tag_read(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: Unable to read the data! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);

        return 1;
    }

    /* print out the data */
    for(i = 0; i < elem_count; i++) { printf("data[%d]=%d\n", i, TestDINTArray[i]); }

    /* now test a write */
    for(i = 0; i < elem_count; i++) { TestDINTArray[i]++; }

    printf("Turn off logging.\n");
    plc_tag_set_debug_level(PLCTAG_DEBUG_NONE);

    rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: Unable to read the data! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);

        return 1;
    }

    /* get the data again*/
    rc = plc_tag_read(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: Unable to read the data! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);

        return 1;
    }

    /* print out the data */
    for(i = 0; i < elem_count; i++) { printf("data[%d]=%d\n", i, TestDINTArray[i]); }

    /* test a timeout. */
    printf("Testing timeout behavior.\n");
    start = compat_time_ms();
    rc = plc_tag_read(tag, 1);
    end = compat_time_ms();

    if(rc != PLCTAG_ERR_TIMEOUT) {
        printf("Expected PLCTAG_ERR_TIMEOUT, got %s in %dms!\n", plc_tag_decode_error(rc), (int)(end - start));
        plc_tag_destroy(tag);

        return 1;
    }

    /* test an abort. */
    printf("Testing abort behavior.\n");
    rc = plc_tag_read(tag, 0);
    if(rc != PLCTAG_STATUS_PENDING) {
        printf("ERROR: Unable to read the data! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);

        return 1;
    }

    rc = plc_tag_abort(tag);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: Unable to abort the read, error %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);

        return 1;
    }

    /*
     * we do not need to free the array TestDINTArray because the callback will do
     * it when the tag is destroyed.
     */
    plc_tag_destroy(tag);

    printf("SUCCESS!\n");

    return 0;
}

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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*

This example shows the use of standard strings for a ControlLogix and for a PLC-5.   The string definitions are
the standard string types:

- for ControlLogix, this is a 4-byte count word followed by 82 bytes of character data followed by 2 bytes of padding.
- for PLC-5 this is a 2-byte count word followed by 82 bytes of character data and no padding.

DO NOT USE THIS CODE FOR NON-STANDARD STRINGS!!!

If you have a ControlLogix program that has some special UDT you use instead of the standard STRING type, this is NOT
correct code and will be dangerous to use.   You can overwrite parts of larger UDTs or have other failures if you
do so.   You MUST use the optional string definitions attributes to tell the library what your string definitions are.

AGAIN: DO NOT USE THIS CODE FOR NON-STANDARD STRINGS!

For PLC-5, SLC 500 and MicroLogix, the defaults here are fine.  For Control/CompactLogix, make sure that you are using
the standard STRING type.   If you are not, this code WILL NOT WORK.

*/


#define REQUIRED_VERSION 2, 2, 0

static const char *tag_strings[] = {
    "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=88&elem_count=11&name=barcodes",
    "protocol=ab_eip&gateway=10.206.1.38&plc=plc5&elem_count=2&name=ST18:0"};

#define DATA_TIMEOUT 5000


int main(void) {
    int32_t tag = 0;
    int rc;
    int str_num = 1;
    int offset = 0;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        return 1;
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Using library version %d.%d.%d.\n", plc_tag_get_int_attribute(0, "version_major", -1),
            plc_tag_get_int_attribute(0, "version_minor", -1), plc_tag_get_int_attribute(0, "version_patch", -1));

    /* turn off debugging output. */
    plc_tag_set_debug_level(PLCTAG_DEBUG_NONE);

    /* loop over the tag strings. */
    for(int i = 0; i < (int)(unsigned int)(sizeof(tag_strings) / sizeof(tag_strings[0])); i++) {
        tag = plc_tag_create(tag_strings[i], DATA_TIMEOUT);

        /* everything OK? */
        if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Error creating tag %d! Error %s\n", i, plc_tag_decode_error(rc));
            plc_tag_destroy(tag);
            return rc;
        }

        /* get the data */
        rc = plc_tag_read(tag, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR: Unable to read the data for tag %d! Got error code %d: %s\n", i, rc,
                    plc_tag_decode_error(rc));
            plc_tag_destroy(tag);
            return rc;
        }

        /* print out the data */
        offset = 0;
        str_num = 1;
        while(offset < plc_tag_get_size(tag)) {
            char *str = NULL;
            int str_cap = plc_tag_get_string_length(tag, offset) + 1; /* +1 for the zero termination. */

            str = malloc((size_t)(unsigned int)str_cap);
            if(!str) {
                // NOLINTNEXTLINE
                fprintf(stderr, "Unable to allocate memory for the string %d of tag %d!\n", str_num, i);
                plc_tag_destroy(tag);
                return PLCTAG_ERR_NO_MEM;
            }

            rc = plc_tag_get_string(tag, offset, str, str_cap);
            if(rc != PLCTAG_STATUS_OK) {
                // NOLINTNEXTLINE
                fprintf(stderr, "Unable to get string %d of tag %d, got error %s!\n", str_num, i, plc_tag_decode_error(rc));
                free(str);
                plc_tag_destroy(tag);
                return rc;
            }

            // NOLINTNEXTLINE
            fprintf(stderr, "tag %d string %d (%u chars) = '%s'\n", i, str_num, (unsigned int)strlen(str), str);

            free(str);

            str_num++;

            offset += plc_tag_get_string_total_length(tag, offset);
        }

        /* we are done */
        plc_tag_destroy(tag);
    }

    return 0;
}

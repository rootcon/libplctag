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

#ifndef __PLCTAG_OMRON_TAG_H__
#define __PLCTAG_OMRON_TAG_H__ 1

/* do these first */
#define MAX_TAG_NAME (260)
#define MAX_TAG_TYPE_INFO (64)

/* they are used in some of these includes */
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/omron/conn.h>
#include <libplctag/protocols/omron/omron_common.h>

typedef enum {
    OMRON_TYPE_BOOL,
    OMRON_TYPE_BOOL_ARRAY,
    OMRON_TYPE_CONTROL,
    OMRON_TYPE_COUNTER,
    OMRON_TYPE_FLOAT32,
    OMRON_TYPE_FLOAT64,
    OMRON_TYPE_INT8,
    OMRON_TYPE_INT16,
    OMRON_TYPE_INT32,
    OMRON_TYPE_INT64,
    OMRON_TYPE_STRING,
    OMRON_TYPE_SHORT_STRING,
    OMRON_TYPE_TIMER,
    OMRON_TYPE_TAG_ENTRY, /* not a real AB type, but a pseudo type for AB's internal tag entry. */
    OMRON_TYPE_TAG_UDT,   /* as above, but for UDTs. */
    OMRON_TYPE_TAG_RAW    /* raw CIP tag */
} elem_type_t;


struct omron_tag_t {
    /*struct plc_tag_t p_tag;*/
    TAG_BASE_STRUCT;

    /* how do we talk to this device? */
    plc_type_t plc_type;

    /* pointers back to conn */
    omron_conn_p conn;
    int use_connected_msg;

    /* this contains the encoded name */
    uint8_t encoded_name[MAX_TAG_NAME];
    int encoded_name_size;

    //    const char *read_group;

    /* storage for the encoded type. */
    uint8_t encoded_type_info[MAX_TAG_TYPE_INFO];
    int encoded_type_info_size;

    elem_type_t elem_type;

    int elem_count;
    int elem_size;

    int special_tag;

    /* Used for standard tags. How much data can we send per packet? */
    int write_data_per_packet;

    /* used for listing tags. */
    uint32_t next_id;

    /* used for UDT tags. */
    uint8_t udt_get_fields;
    uint16_t udt_id;

    /* requests */
    int pre_write_read;
    int first_read;
    omron_request_p req;
    int offset;

    int allow_packing;
    int supports_fragmented_read;

    /* flags for operations */
    // int abort_requested;
    int read_in_progress;
    int write_in_progress;
    /*int connect_in_progress;*/
};


#endif

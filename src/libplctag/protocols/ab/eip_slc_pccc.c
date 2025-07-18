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

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/eip_slc_pccc.h>
#include <libplctag/protocols/ab/error_codes.h>
#include <libplctag/protocols/ab/pccc.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <utils/debug.h>


static int tag_read_start(ab_tag_p tag);
static int tag_status(ab_tag_p tag);
static int tag_tickler(ab_tag_p tag);
static int tag_write_start(ab_tag_p tag);

struct tag_vtable_t slc_vtable = {(tag_vtable_func)ab_tag_abort_request, /* shared */
                                  (tag_vtable_func)tag_read_start, (tag_vtable_func)tag_status, (tag_vtable_func)tag_tickler,
                                  (tag_vtable_func)tag_write_start, (tag_vtable_func)NULL, /* wake_plc */

                                  /* data accessors */
                                  ab_get_int_attrib, ab_set_int_attrib,

                                  ab_get_byte_array_attrib};


/* default string types used for PLC-5 PLCs. */
tag_byte_order_t slc_tag_byte_order = {.is_allocated = 0,

                                       .int16_order = {0, 1},
                                       .int32_order = {0, 1, 2, 3},
                                       .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                       .float32_order = {0, 1, 2, 3},
                                       .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                       .str_is_defined = 1,
                                       .str_is_counted = 1,
                                       .str_is_fixed_length = 1,
                                       .str_is_zero_terminated = 0,
                                       .str_is_byte_swapped = 1,

                                       .str_pad_to_multiple_bytes = 2,
                                       .str_count_word_bytes = 2,
                                       .str_max_capacity = 82,
                                       .str_total_length = 84,
                                       .str_pad_bytes = 0};


static int check_read_status(ab_tag_p tag);
static int check_write_status(ab_tag_p tag);


START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;        /* ALWAYS 0x006f Unconnected Send*/
    uint16_le encap_length;         /* packet size in bytes - 24 */
    uint32_le encap_session_handle; /* from session set up */
    uint32_le encap_status;         /* always _sent_ as 0 */
    uint64_le encap_sender_context; /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le encap_options;        /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle; /* ALWAYS 0 */
    uint16_le router_timeout;   /* in seconds, 5 or 10 seems to be good.*/

    /* Common Packet Format - CPF Unconnected */
    uint16_le cpf_item_count;      /* ALWAYS 2 */
    uint16_le cpf_nai_item_type;   /* ALWAYS 0 */
    uint16_le cpf_nai_item_length; /* ALWAYS 0 */
    uint16_le cpf_udi_item_type;   /* ALWAYS 0x00B2 - Unconnected Data Item */
    uint16_le cpf_udi_item_length; /* REQ: fill in with length of remaining data. */

    /* PCCC Command Req Routing */
    uint8_t service_code;           /* ALWAYS 0x4B, Execute PCCC */
    uint8_t req_path_size;          /* ALWAYS 0x02, in 16-bit words */
    uint8_t req_path[4];            /* ALWAYS 0x20,0x67,0x24,0x01 for PCCC */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_le vendor_id;            /* Our CIP Vendor ID */
    uint32_le vendor_serial_number; /* Our CIP Vendor Serial Number */

    /* PCCC Command */
    uint8_t pccc_command;       /* CMD read, write etc. */
    uint8_t pccc_status;        /* STS 0x00 in request */
    uint16_le pccc_seq_num;     /* TNS transaction/sequence id */
    uint8_t pccc_function;      /* FNC sub-function of command */
    uint8_t pccc_transfer_size; /* total number of bytes requested */
} END_PACK pccc_req;


/*
 * tag_status
 *
 * Get the tag status.
 */

int tag_status(ab_tag_p tag) {
    if(!tag->session) {
        /* this is not OK.  This is fatal! */
        return PLCTAG_ERR_CREATE;
    }

    if(tag->read_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(tag->write_in_progress) { return PLCTAG_STATUS_PENDING; }

    return tag->status;
}


int tag_tickler(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        pdebug(DEBUG_SPEW, "Read in progress.");
        rc = check_read_status(tag);
        tag->status = (int8_t)rc;

        /* check to see if the read finished. */
        if(!tag->read_in_progress) {
            /* read done */
            if(tag->first_read) {
                tag->first_read = 0;
                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, PLCTAG_STATUS_OK);
            }

            tag->read_complete = 1;
        }

        return rc;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_SPEW, "Write in progress.");
        rc = check_write_status(tag);
        tag->status = (int8_t)rc;

        /* check to see if the write finished. */
        if(!tag->write_in_progress) { tag->write_complete = 1; }

        return rc;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return tag->status;
}


/*
 * tag_read_start
 *
 * Start a PCCC tag read (SLC).
 */

int tag_read_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    ;
    int overhead;
    int data_per_packet;
    pccc_req *pccc;
    uint8_t *data;
    uint8_t *embed_start;

    pdebug(DEBUG_INFO, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    tag->read_in_progress = 1;

    /* how many packets will we need? How much overhead? */
    // overhead = sizeof(pccc_resp) + 4 + tag->encoded_name_size; /* MAGIC 4 = fudge */

    /* calculate based on the response. */
    overhead = 1    /* PCCC CMD */
               + 1  /* PCCC status */
               + 2; /* PCCC packet sequence number */

    data_per_packet = session_get_max_payload(tag->session) - overhead;

    if(data_per_packet <= 0) {
        pdebug(DEBUG_WARN, "Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead,
               session_get_max_payload(tag->session));
        tag->read_in_progress = 0;
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(data_per_packet < tag->size) {
        pdebug(DEBUG_DETAIL, "Unable to send request: Tag size is %d, write overhead is %d, and write data per packet is %d!",
               tag->size, overhead, data_per_packet);
        tag->read_in_progress = 0;
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to get new request.  rc=%d", rc);
        tag->read_in_progress = 0;
        return rc;
    }

    /* point the struct pointers to the buffer*/
    pccc = (pccc_req *)(tag->req->data);

    /* point to the end of the struct */
    data = ((uint8_t *)pccc) + sizeof(pccc_req);

    /* set up the embedded PCCC packet */
    embed_start = (uint8_t *)(&pccc->service_code);

    /* copy encoded tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* fill in the fixed fields. */

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND); /* set up for unconnected sending */

    /* router timeout */
    pccc->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    pccc->cpf_item_count = h2le16(2);                                   /* ALWAYS 2 */
    pccc->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI);                  /* ALWAYS 0 */
    pccc->cpf_nai_item_length = h2le16(0);                              /* ALWAYS 0 */
    pccc->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI);                  /* ALWAYS 0x00B2 - Unconnected Data Item */
    pccc->cpf_udi_item_length = h2le16((uint16_t)(data - embed_start)); /* REQ: fill in with length of remaining data. */

    /* Command Routing */
    pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE; /* ALWAYS 0x4B, Execute PCCC */
    pccc->req_path_size = 2;                      /* ALWAYS 2, size in words of path, next field */
    pccc->req_path[0] = 0x20;                     /* class */
    pccc->req_path[1] = 0x67;                     /* PCCC Execute */
    pccc->req_path[2] = 0x24;                     /* instance */
    pccc->req_path[3] = 0x01;                     /* instance 1 */

    /* PCCC ID */
    pccc->request_id_size = 7;                             /* ALWAYS 7 */
    pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);            /* Our CIP Vendor */
    pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN); /* our unique serial number */

    /* fill in the PCCC command */
    pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    pccc->pccc_status = 0; /* STS 0 in request */
    pccc->pccc_seq_num = h2le16(conn_seq_id);
    pccc->pccc_function = AB_EIP_SLC_RANGE_READ_FUNC;
    pccc->pccc_transfer_size = (uint8_t)(tag->elem_size * tag->elem_count); /* size to read/write in bytes. */

    /*
     * after the embedded packet, we need to tell the message router
     * how to get to the target device.
     */

    /* set the size of the request */
    tag->req->request_size = (int)(data - (tag->req->data));

    /* mark it as ready to send */
    // req->send_request = 1;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);

        ab_tag_abort_request(tag);

        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * check_read_status
 *
 * NOTE that we can have only one outstanding request because PCCC
 * does not support fragments.
 */


static int check_read_status(ab_tag_p tag) {
    pccc_resp *pccc;
    uint8_t *data;
    uint8_t *data_end;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting");

    /* the request reference is valid. */

    pccc = (pccc_resp *)(tag->req->data);

    /* point to the start of the data */
    data = (uint8_t *)pccc + sizeof(*pccc);

    data_end = (tag->req->data + le2h16(pccc->encap_length) + sizeof(eip_encap));

    /* fake exceptions */
    do {
        if(le2h16(pccc->encap_command) != AB_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", pccc->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(pccc->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(pccc->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: (%d) %s", pccc->general_status,
                   decode_cip_error_long((uint8_t *)&(pccc->general_status)));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", pccc->pccc_status,
                   pccc_decode_error(&pccc->pccc_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /* did we get the right amount of data? */
        if((data_end - data) != tag->size) {
            if((int)(data_end - data) > tag->size) {
                pdebug(DEBUG_WARN, "Too much data received!  Expected %d bytes but got %d bytes!", tag->size,
                       (int)(data_end - data));
                rc = PLCTAG_ERR_TOO_LARGE;
            } else {
                pdebug(DEBUG_WARN, "Too little data received!  Expected %d bytes but got %d bytes!", tag->size,
                       (int)(data_end - data));
                rc = PLCTAG_ERR_TOO_SMALL;
            }
            break;
        }

        /* copy data into the tag. */
        mem_copy(tag->data, data, (int)(data_end - data));

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request. */
    ab_tag_abort_request(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


/* FIXME  convert to unconnected messages. */

int tag_write_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    pccc_req *pccc;
    uint8_t *data;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    uint8_t *embed_start;
    int overhead, data_per_packet;

    pdebug(DEBUG_INFO, "Starting.");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    tag->write_in_progress = 1;

    /* overhead comes from the request*/
    overhead = 1   /* PCCC command */
               + 1 /* PCCC status */
               + 2 /* PCCC sequence number */
               + 1 /* PCCC function */
               + 1 /* request total transfer size in bytes. */
               + (tag->encoded_name_size);

    data_per_packet = session_get_max_payload(tag->session) - overhead;

    if(data_per_packet <= 0) {
        pdebug(DEBUG_WARN, "Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead,
               session_get_max_payload(tag->session));
        tag->write_in_progress = 0;
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(data_per_packet < tag->size) {
        pdebug(DEBUG_DETAIL, "Tag size is %d, write overhead is %d, and write data per packet is %d.",
               session_get_max_payload(tag->session), overhead, data_per_packet);
        tag->write_in_progress = 0;
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to get new request.  rc=%d", rc);
        tag->write_in_progress = 0;
        return rc;
    }

    pccc = (pccc_req *)(tag->req->data);

    /* set up the embedded PCCC packet */
    embed_start = (uint8_t *)(&pccc->service_code);

    /* point to the end of the struct */
    data = (tag->req->data) + sizeof(pccc_req);

    /* copy encoded tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* write the mask if this is a bit tag */
    if(tag->is_bit) {
        for(int i = 0; i < tag->elem_size; i++) {
            if((tag->bit / 8) == i) {
                *data = (uint8_t)(1 << (tag->bit % 8));

                pdebug(DEBUG_DETAIL, "adding mask byte %d: %x", i, *data);

                data++;
            } else {
                /* this is not the data we care about. */
                *data = (uint8_t)0x00;

                pdebug(DEBUG_DETAIL, "adding mask byte %d: %x", i, *data);

                data++;
            }
        }
    }

    /* now copy the data to write */
    mem_copy(data, tag->data, tag->size);
    data += tag->size;

    /* now fill in the rest of the structure. */

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);

    /* router timeout */
    pccc->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    pccc->cpf_item_count = h2le16(2);                                   /* ALWAYS 2 */
    pccc->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI);                  /* ALWAYS 0 */
    pccc->cpf_nai_item_length = h2le16(0);                              /* ALWAYS 0 */
    pccc->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI);                  /* ALWAYS 0x00B2 - Unconnected Data Item */
    pccc->cpf_udi_item_length = h2le16((uint16_t)(data - embed_start)); /* REQ: fill in with length of remaining data. */

    pdebug(DEBUG_DETAIL, "Total data length %d.", (int)(unsigned int)(data - (uint8_t *)(pccc)));
    pdebug(DEBUG_DETAIL, "Total payload length %d.", (int)(unsigned int)(data - embed_start));

    /* Command Routing */
    pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE; /* ALWAYS 0x4B, Execute PCCC */
    pccc->req_path_size = 2;                      /* ALWAYS 2, size in words of path, next field */
    pccc->req_path[0] = 0x20;                     /* class */
    pccc->req_path[1] = 0x67;                     /* PCCC Execute */
    pccc->req_path[2] = 0x24;                     /* instance */
    pccc->req_path[3] = 0x01;                     /* instance 1 */

    /* PCCC ID */
    pccc->request_id_size = 7;                             /* ALWAYS 7 */
    pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);            /* Our CIP Vendor */
    pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN); /* our unique serial number */

    /* PCCC Command */
    pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;                    /* STS 0 in request */
    pccc->pccc_seq_num = h2le16(conn_seq_id); /* FIXME - get sequence ID from session? */
    pccc->pccc_function = (tag->is_bit ? AB_EIP_SLC_RANGE_BIT_WRITE_FUNC : AB_EIP_SLC_RANGE_WRITE_FUNC);
    pccc->pccc_transfer_size = (uint8_t)(tag->size);

    /* get ready to add the request to the queue for this session */
    tag->req->request_size = (int)(data - (tag->req->data));

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);

        ab_tag_abort_request(tag);

        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * check_write_status
 *
 * Fragments are not supported.
 */
static int check_write_status(ab_tag_p tag) {
    pccc_resp *pccc;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    /* the request reference is valid. */

    pccc = (pccc_resp *)(tag->req->data);

    /* fake exception */
    do {
        /* check the response status */
        if(le2h16(pccc->encap_command) != AB_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_WARN, "EIP unexpected response packet type: %d!", pccc->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(pccc->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(pccc->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d", pccc->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", pccc->pccc_status,
                   pccc_decode_error(&pccc->pccc_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request. */
    ab_tag_abort_request(tag);

    pdebug(DEBUG_SPEW, "Done.");

    /* Success! */
    return rc;
}

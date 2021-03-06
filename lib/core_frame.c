/*
    core_frame.c

    Copyright (C) 2012  Red Hat, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "core/frame.h"
#include "utils.h"
#include "strbuf.h"
#include "json.h"
#include "generic_frame.h"
#include "thread.h"
#include "stacktrace.h"
#include "internal_utils.h"
#include <string.h>

/* Method table */

static void
core_append_bthash_text(struct sr_core_frame *frame, enum sr_bthash_flags flags,
                        struct sr_strbuf *strbuf);
static void
core_append_duphash_text(struct sr_core_frame *frame, enum sr_duphash_flags flags,
                         struct sr_strbuf *strbuf);

DEFINE_NEXT_FUNC(core_next, struct sr_frame, struct sr_core_frame)
DEFINE_SET_NEXT_FUNC(core_set_next, struct sr_frame, struct sr_core_frame)

struct frame_methods core_frame_methods =
{
    .append_to_str = (append_to_str_fn_t) sr_core_frame_append_to_str,
    .next = (next_frame_fn_t) core_next,
    .set_next = (set_next_frame_fn_t) core_set_next,
    .cmp = (frame_cmp_fn_t) sr_core_frame_cmp,
    .cmp_distance = (frame_cmp_fn_t) sr_core_frame_cmp_distance,
    .frame_append_bthash_text =
        (frame_append_bthash_text_fn_t) core_append_bthash_text,
    .frame_append_duphash_text =
        (frame_append_duphash_text_fn_t) core_append_duphash_text,
    .frame_free = (frame_free_fn_t) sr_core_frame_free,
};

/* Public functions */

struct sr_core_frame *
sr_core_frame_new()
{
    struct sr_core_frame *frame = sr_malloc(sizeof(struct sr_core_frame));
    sr_core_frame_init(frame);
    return frame;
}

void
sr_core_frame_init(struct sr_core_frame *frame)
{
    frame->address = -1;
    frame->build_id = NULL;
    frame->build_id_offset = -1;
    frame->function_name = NULL;
    frame->file_name = NULL;
    frame->fingerprint = NULL;
    frame->fingerprint_hashed = true;
    frame->next = NULL;
    frame->type = SR_REPORT_CORE;
}

void
sr_core_frame_free(struct sr_core_frame *frame)
{
    if (!frame)
        return;

    free(frame->build_id);
    free(frame->function_name);
    free(frame->file_name);
    free(frame->fingerprint);
    free(frame);
}

struct sr_core_frame *
sr_core_frame_dup(struct sr_core_frame *frame, bool siblings)
{
    struct sr_core_frame *result = sr_core_frame_new();
    memcpy(result, frame, sizeof(struct sr_core_frame));

    /* Handle siblings. */
    if (siblings)
    {
        if (result->next)
            result->next = sr_core_frame_dup(result->next, true);
    }
    else
        result->next = NULL; /* Do not copy that. */

    /* Duplicate all strings if the copy is not shallow. */
    if (result->build_id)
        result->build_id = sr_strdup(result->build_id);
    if (result->function_name)
        result->function_name = sr_strdup(result->function_name);
    if (result->file_name)
        result->file_name = sr_strdup(result->file_name);
    if (result->fingerprint)
        result->fingerprint = sr_strdup(result->fingerprint);

    return result;
}

bool
sr_core_frame_calls_func(struct sr_core_frame *frame,
                          const char *function_name,
                          ...)
{
    if (!frame->function_name ||
        0 != strcmp(frame->function_name, function_name))
    {
        return false;
    }

    va_list file_names;
    va_start(file_names, function_name);
    bool success = false;
    bool empty = true;

    while (true)
    {
        char *file_name = va_arg(file_names, char*);
        if (!file_name)
            break;

        empty = false;

        if (frame->file_name &&
            NULL != strstr(frame->file_name, file_name))
        {
            success = true;
            break;
        }
    }

   va_end(file_names);
   return success || empty;
}

int
sr_core_frame_cmp(struct sr_core_frame *frame1,
                  struct sr_core_frame *frame2)
{
    /* Build ID. */
    int build_id = sr_strcmp0(frame1->build_id,
                              frame2->build_id);
    if (build_id != 0)
        return build_id;

    /* Build ID offset */
    int build_id_offset = frame1->build_id_offset - frame2->build_id_offset;
    if (build_id_offset != 0)
        return build_id_offset;

    /* Function name. */
    int function_name = sr_strcmp0(frame1->function_name,
                                    frame2->function_name);
    if (function_name != 0)
        return function_name;

    /* File name */
    int file_name = sr_strcmp0(frame1->file_name,
                                frame2->file_name);
    if (file_name != 0)
        return file_name;

    /* Fingerprint. */
    int fingerprint = sr_strcmp0(frame1->fingerprint,
                                  frame2->fingerprint);
    if (fingerprint != 0)
        return fingerprint;

    return 0;
}

int
sr_core_frame_cmp_distance(struct sr_core_frame *frame1,
                           struct sr_core_frame *frame2)
{
    /* If both function names are present, compare those. */
    if (frame1->function_name && frame2->function_name)
        return strcmp(frame1->function_name, frame2->function_name);

    /* Try matching build ID and offset. */
    int build_id = sr_strcmp0(frame1->build_id,
                              frame2->build_id);

    int build_id_offset = frame1->build_id_offset - frame2->build_id_offset;

    if (build_id == 0 && build_id_offset == 0)
        return 0;

    /* Build ID mismatch - this might still mean that the frames are the same
     * but from a different build. Try falling back to fingerprints if those
     * are present.
     */
    if (frame1->fingerprint && frame2->fingerprint)
        return strcmp(frame1->fingerprint, frame2->fingerprint);

    /* Fingerprints are not present, return the result of build ID and offset
     * comparison.
     */
    if (build_id)
        return build_id;

    return build_id_offset;
}

struct sr_core_frame *
sr_core_frame_append(struct sr_core_frame *dest,
                     struct sr_core_frame *item)
{
    if (!dest)
        return item;

    struct sr_core_frame *dest_loop = dest;
    while (dest_loop->next)
        dest_loop = dest_loop->next;

    dest_loop->next = item;
    return dest;
}

struct sr_core_frame *
sr_core_frame_from_json(struct sr_json_value *root,
                        char **error_message)
{
    if (!JSON_CHECK_TYPE(root, SR_JSON_OBJECT, "frame"))
        return NULL;

    struct sr_core_frame *result = sr_core_frame_new();

    bool success =
        JSON_READ_UINT64(root, "address", &result->address) &&
        JSON_READ_STRING(root, "build_id", &result->build_id) &&
        JSON_READ_UINT64(root, "build_id_offset", &result->build_id_offset) &&
        JSON_READ_STRING(root, "function_name", &result->function_name) &&
        JSON_READ_STRING(root, "file_name", &result->file_name) &&
        JSON_READ_STRING(root, "fingerprint", &result->fingerprint) &&
        JSON_READ_BOOL(root, "fingerprint_hashed", &result->fingerprint_hashed);

    if (!success)
    {
        sr_core_frame_free(result);
        return NULL;
    }

    return result;
}

char *
sr_core_frame_to_json(struct sr_core_frame *frame)
{
    struct sr_strbuf *strbuf = sr_strbuf_new();

    if (frame->address != -1)
    {
        sr_strbuf_append_strf(strbuf,
                              ",   \"address\": %"PRIu64"\n",
                              frame->address);
    }

    if (frame->build_id)
    {
        sr_strbuf_append_str(strbuf, ",   \"build_id\": ");
        sr_json_append_escaped(strbuf, frame->build_id);
        sr_strbuf_append_str(strbuf, "\n");
    }

    if (frame->build_id_offset != -1)
    {
        sr_strbuf_append_strf(strbuf,
                              ",   \"build_id_offset\": %"PRIu64"\n",
                              frame->build_id_offset);
    }

    if (frame->function_name)
    {
        sr_strbuf_append_str(strbuf, ",   \"function_name\": ");
        sr_json_append_escaped(strbuf, frame->function_name);
        sr_strbuf_append_str(strbuf, "\n");
    }

    if (frame->file_name)
    {
        sr_strbuf_append_str(strbuf, ",   \"file_name\": ");
        sr_json_append_escaped(strbuf, frame->file_name);
        sr_strbuf_append_str(strbuf, "\n");
    }

    if (frame->fingerprint)
    {
        sr_strbuf_append_str(strbuf, ",   \"fingerprint\": ");
        sr_json_append_escaped(strbuf, frame->fingerprint);
        sr_strbuf_append_str(strbuf, "\n");

        if (frame->fingerprint_hashed == false)
            sr_strbuf_append_str(strbuf, ",   \"fingerprint_hashed\": false\n");
    }

    if (strbuf->len > 0)
        strbuf->buf[0] = '{';
    else
        sr_strbuf_append_char(strbuf, '{');

    sr_strbuf_append_char(strbuf, '}');
    return sr_strbuf_free_nobuf(strbuf);
}

void
sr_core_frame_append_to_str(struct sr_core_frame *frame,
                            struct sr_strbuf *dest)
{
    if (frame->function_name)
    {
        sr_strbuf_append_str(dest, frame->function_name);
    }
    else
    {
        sr_strbuf_append_strf(dest, "%s+%"PRIu64, frame->build_id,
                              frame->build_id_offset);
    }

    if (frame->file_name)
    {
        sr_strbuf_append_strf(dest, " in %s", frame->file_name);
    }
}

static void
core_append_bthash_text(struct sr_core_frame *frame, enum sr_bthash_flags flags,
                        struct sr_strbuf *strbuf)
{
    if (frame->address)
        sr_strbuf_append_strf(strbuf, "0x%"PRIx64", ", frame->address);
    else
        sr_strbuf_append_str(strbuf, "<unknown>, ");

    sr_strbuf_append_strf(strbuf, "%s+0x%"PRIx64", %s, %s\n",
                          OR_UNKNOWN(frame->build_id),
                          frame->build_id_offset,
                          OR_UNKNOWN(frame->file_name),
                          OR_UNKNOWN(frame->fingerprint));
}

static void
core_append_duphash_text(struct sr_core_frame *frame, enum sr_duphash_flags flags,
                         struct sr_strbuf *strbuf)
{
    /* Build id should be the preferred deduplication mechanism. */
    if (frame->build_id)
        sr_strbuf_append_strf(strbuf, "%s+0x%"PRIx64"\n",
                              frame->build_id,
                              frame->build_id_offset);

    /* If we don't have it, try the function name. */
    else if (frame->function_name)
        sr_strbuf_append_strf(strbuf, "  %s\n", frame->function_name);

    /* Function fingerprint? */
    else if (frame->fingerprint)
        sr_strbuf_append_strf(strbuf, "%s+0x%"PRIx64"\n",
                              frame->fingerprint,
                              frame->build_id_offset);

    /* Address should be better than nothing. */
    else
        sr_strbuf_append_strf(strbuf, "0x%"PRIx64"\n", frame->address);
}

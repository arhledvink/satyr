/*
    sharedlib.c

    Copyright (C) 2011  Red Hat, Inc.

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
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "sharedlib.h"
#include "strbuf.h"
#include "utils.h"

struct btp_sharedlib *
btp_sharedlib_new()
{
    struct btp_sharedlib *result = btp_malloc(sizeof(struct btp_sharedlib));
    btp_sharedlib_init(result);
    return result;
}

void
btp_sharedlib_init(struct btp_sharedlib *sharedlib)
{
    sharedlib->from = -1;
    sharedlib->to = -1;
    sharedlib->symbols = SYMS_NOT_FOUND;
    sharedlib->soname = NULL;
    sharedlib->next = NULL;
}

void
btp_sharedlib_free(struct btp_sharedlib *sharedlib)
{
    if (!sharedlib)
        return;

    free(sharedlib->soname);
    free(sharedlib);
}

void
btp_sharedlib_append(struct btp_sharedlib *a,
                     struct btp_sharedlib *b)
{
    struct btp_sharedlib *tmp = a;
    while (tmp->next)
        tmp = tmp->next;

    tmp->next = b;
}

int
btp_sharedlib_count(struct btp_sharedlib *a)
{
    struct btp_sharedlib *tmp = a;
    int count = 0;
    while (tmp)
    {
        ++count;
        tmp = tmp->next;
    }

    return count;
}

struct btp_sharedlib *
btp_sharedlib_find_address(struct btp_sharedlib *first,
                           unsigned long address)
{
    struct btp_sharedlib *tmp = first;
    while (tmp)
    {
        if (address >= tmp->from && address <= tmp->to)
            return tmp;

        tmp = tmp->next;
    }

    return NULL;
}

static char *
find_sharedlib_section_start(const char *input)
{
    /* searching for
       From      To      Syms Read      Shared Object Library
    */
    char *tmp, *result;

    /* ugly */
    for (result = strstr(input, "From"); result; result = strstr(result + 1, "From"))
    {
        /* must be at the beginning of the line
           or at the beginning of whole input */
        if (result != input && *(result - 1) != '\n')
            continue;

        tmp = result + strlen("From");
        while (isspace(*tmp))
            ++tmp;

        if (strncmp("To", tmp, strlen("To")) != 0)
            continue;

        tmp += strlen("To");
        while (isspace(*tmp))
            ++tmp;

        if (strncmp("Syms Read", tmp, strlen("Syms Read")) != 0)
            continue;

        tmp += strlen("Syms Read");
        while (isspace(*tmp))
            ++tmp;

        if (strncmp("Shared Object Library\n", tmp, strlen("Shared Object Library\n")) != 0)
            continue;

        /* jump to the next line - the first loaded library */
        return tmp + strlen("Shared Object Library\n");
    }

    return NULL;
}

struct btp_sharedlib *
btp_sharedlib_parse(const char *input)
{
    char *tmp = find_sharedlib_section_start(input);
    if (!tmp)
        return NULL;

    struct btp_sharedlib *first = NULL, *current = NULL;
    unsigned long from, to;
    int symbols;
    /* Parsing
       From                 To                  Syms Read        Shared Object Library
       0x0123456789abcdef   0xfedcba987654321   Yes (*)|Yes|No   /usr/lib64/libbtparser.so.2.2.2
    */
    while (1)
    {
        /* From To */
        if (sscanf(tmp, "0x%lx 0x%lx", &from, &to) != 2)
            break;

        while (isxdigit(*tmp) || isspace(*tmp) || *tmp == 'x')
            ++tmp;

        /* Syms Read */
        if (strncmp("Yes (*)", tmp, strlen("Yes (*)")) == 0)
        {
            tmp += strlen("Yes (*)");
            symbols = SYMS_NOT_FOUND;
        }
        else if (strncmp("Yes", tmp, strlen("Yes")) == 0)
        {
            tmp += strlen("Yes");
            symbols = SYMS_OK;
        }
        else if (strncmp("No", tmp, strlen("No")) == 0)
        {
            tmp += strlen("No");
            symbols = SYMS_WRONG;
        }
        else
            break;

        while (isspace(*tmp))
            ++tmp;

        /* Shared Object Library */
        struct btp_strbuf *buf = btp_strbuf_new();
        while (*tmp && *tmp != '\n')
        {
            btp_strbuf_append_char(buf, *tmp);
            ++tmp;
        }

        if (current)
        {
            /* round 2+ */
            current->next = btp_sharedlib_new();
            current = current->next;
        }
        else
        {
            /* round 1 */
            current = btp_sharedlib_new();
            first = current;
        }

        current->from = from;
        current->to = to;
        current->symbols = symbols;
        current->soname = btp_strbuf_free_nobuf(buf);

        /* we are on '\n' character, jump to next line */
        ++tmp;
    }

    return first;
}

/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/* cJSON */
/* JSON parser in C. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <float.h>

#include "cJSON.h"

/* define our own boolean type */
#ifdef true
#undef true
#endif
#define true ((cJSON_bool)1)

#ifdef false
#undef false
#endif
#define false ((cJSON_bool)0)

typedef int cJSON_bool;

/* define isnan and isinf for ANSI C, if in C99 or above, isnan and isinf has been defined in math.h */
#ifndef isinf
#define isinf(d) (isnan((d - d)) && !isnan(d))
#endif
#ifndef isnan
#define isnan(d) (d != d)
#endif

static const char *global_error_pointer = NULL;

const char *cJSON_GetErrorPtr(void)
{
    return global_error_pointer;
}

/* This is a safeguard to prevent copy-pasters from using incompatible C and header files */
#define cJSON_VERSION_CHECK(x, y) (((x) << 16) | (y))

/* Utility to jump whitespace and cr/lf */
static const char *skip_whitespace(const char *in)
{
    while (in && *in && ((unsigned char)*in <= 32))
    {
        in++;
    }
    return in;
}

/* Parse the input text to generate a number, and populate the result into item. */
static const char *parse_number(cJSON *item, const char *num)
{
    double n = 0;
    double sign = 1;
    double scale = 0;
    int subscale = 0;
    int signsubscale = 1;

    if (*num == '-')
    {
        sign = -1;
        num++;
    }
    if (*num == '0')
    {
        num++;
    }
    if (*num >= '1' && *num <= '9')
    {
        do
        {
            n = (n * 10.0) + (*num - '0');
            num++;
        } while (*num >= '0' && *num <= '9');
    }
    if (*num == '.' && num[1] >= '0' && num[1] <= '9')
    {
        num++;
        do
        {
            n = (n * 10.0) + (*num - '0');
            scale--;
            num++;
        } while (*num >= '0' && *num <= '9');
    }
    if (*num == 'e' || *num == 'E')
    {
        num++;
        if (*num == '+')
        {
            num++;
        }
        else if (*num == '-')
        {
            signsubscale = -1;
            num++;
        }
        while (*num >= '0' && *num <= '9')
        {
            subscale = (subscale * 10) + (*num - '0');
            num++;
        }
    }

    n = sign * n * pow(10.0, (scale + subscale * signsubscale));

    item->valuedouble = n;
    item->valueint = (int)n;
    item->type = cJSON_Number;

    return num;
}

/* Render the number nicely from the given item into a string. */
static char *print_number(const cJSON *item)
{
    char *str = NULL;
    double d = item->valuedouble;
    int len;

    if (d == 0)
    {
        str = (char*)malloc(2);
        if (str)
        {
            strcpy(str, "0");
        }
    }
    else if (fabs(((double)item->valueint) - d) <= DBL_EPSILON && d <= INT_MAX && d >= INT_MIN)
    {
        str = (char*)malloc(21);
        if (str)
        {
            sprintf(str, "%d", item->valueint);
        }
    }
    else
    {
        str = (char*)malloc(64);
        if (str)
        {
            if (fabs(floor(d) - d) <= DBL_EPSILON && fabs(d) < 1.0e60)
            {
                sprintf(str, "%.0f", d);
            }
            else if (fabs(d) < 1.0e-6 || fabs(d) > 1.0e9)
            {
                sprintf(str, "%e", d);
            }
            else
            {
                sprintf(str, "%f", d);
            }
        }
    }

    /* Trim trailing zeros and decimal point if needed */
    if (str)
    {
        len = strlen(str);
        if (strchr(str, '.'))
        {
            while (len > 1 && str[len - 1] == '0')
            {
                str[--len] = '\0';
            }
            if (len > 1 && str[len - 1] == '.')
            {
                str[--len] = '\0';
            }
        }
    }

    return str;
}

/* Parse the input text into an unescaped cstring, and populate item. */
static const unsigned char firstByteMark[7] = { 0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };
static const char *parse_string(cJSON *item, const char *str)
{
    const char *ptr = str + 1;
    char *ptr2;
    char *out;
    int len = 0;
    unsigned uc, uc2;

    if (*str != '\"')
    {
        global_error_pointer = str;
        return NULL;
    }

    while (*ptr != '\"' && *ptr && ++len)
    {
        if (*ptr++ == '\\')
        {
            ptr++;
        }
    }

    out = (char*)malloc(len + 1);
    if (!out)
    {
        return NULL;
    }

    ptr = str + 1;
    ptr2 = out;
    while (*ptr != '\"' && *ptr)
    {
        if (*ptr != '\\')
        {
            *ptr2++ = *ptr++;
        }
        else
        {
            ptr++;
            switch (*ptr)
            {
                case 'b':
                    *ptr2++ = '\b';
                    break;
                case 'f':
                    *ptr2++ = '\f';
                    break;
                case 'n':
                    *ptr2++ = '\n';
                    break;
                case 'r':
                    *ptr2++ = '\r';
                    break;
                case 't':
                    *ptr2++ = '\t';
                    break;
                case 'u':
                    uc = 0;
                    for (int i = 0; i < 4; i++)
                    {
                        ptr++;
                        uc <<= 4;
                        if (*ptr >= '0' && *ptr <= '9')
                            uc += *ptr - '0';
                        else if (*ptr >= 'a' && *ptr <= 'f')
                            uc += 10 + *ptr - 'a';
                        else if (*ptr >= 'A' && *ptr <= 'F')
                            uc += 10 + *ptr - 'A';
                        else
                        {
                            global_error_pointer = ptr;
                            free(out);
                            return NULL;
                        }
                    }
                    /* Handle UTF-16 surrogate pairs */
                    if (uc >= 0xD800 && uc <= 0xDBFF)
                    {
                        if (ptr[1] == '\\' && ptr[2] == 'u')
                        {
                            ptr += 2;
                            uc2 = 0;
                            for (int i = 0; i < 4; i++)
                            {
                                ptr++;
                                uc2 <<= 4;
                                if (*ptr >= '0' && *ptr <= '9')
                                    uc2 += *ptr - '0';
                                else if (*ptr >= 'a' && *ptr <= 'f')
                                    uc2 += 10 + *ptr - 'a';
                                else if (*ptr >= 'A' && *ptr <= 'F')
                                    uc2 += 10 + *ptr - 'A';
                            }
                            uc = 0x10000 + (((uc & 0x3FF) << 10) | (uc2 & 0x3FF));
                        }
                    }
                    /* Encode as UTF-8 */
                    if (uc < 0x80)
                    {
                        *ptr2++ = uc;
                    }
                    else if (uc < 0x800)
                    {
                        *ptr2++ = 0xC0 | (uc >> 6);
                        *ptr2++ = 0x80 | (uc & 0x3F);
                    }
                    else if (uc < 0x10000)
                    {
                        *ptr2++ = 0xE0 | (uc >> 12);
                        *ptr2++ = 0x80 | ((uc >> 6) & 0x3F);
                        *ptr2++ = 0x80 | (uc & 0x3F);
                    }
                    else
                    {
                        *ptr2++ = 0xF0 | (uc >> 18);
                        *ptr2++ = 0x80 | ((uc >> 12) & 0x3F);
                        *ptr2++ = 0x80 | ((uc >> 6) & 0x3F);
                        *ptr2++ = 0x80 | (uc & 0x3F);
                    }
                    break;
                default:
                    *ptr2++ = *ptr;
                    break;
            }
            ptr++;
        }
    }
    *ptr2 = '\0';
    if (*ptr == '\"')
    {
        ptr++;
    }
    item->valuestring = out;
    item->type = cJSON_String;
    return ptr;
}

/* Render the cstring provided to an escaped version that can be printed. */
static char *print_string_ptr(const char *str)
{
    const char *ptr;
    char *ptr2;
    char *out;
    int len = 0;
    unsigned char token;

    if (!str)
    {
        out = (char*)malloc(3);
        if (out)
        {
            strcpy(out, "\"\"");
        }
        return out;
    }

    for (ptr = str; *ptr; ptr++)
    {
        token = *ptr;
        if (strchr("\"\\\b\f\n\r\t", token))
        {
            len += 2;
        }
        else if (token < 32)
        {
            len += 6;
        }
        else
        {
            len++;
        }
    }

    out = (char*)malloc(len + 3);
    if (!out)
    {
        return NULL;
    }

    ptr2 = out;
    *ptr2++ = '\"';
    for (ptr = str; *ptr; ptr++)
    {
        token = *ptr;
        if (token > 31 && token != '\"' && token != '\\')
        {
            *ptr2++ = token;
        }
        else
        {
            *ptr2++ = '\\';
            switch (token)
            {
                case '\\':
                    *ptr2++ = '\\';
                    break;
                case '\"':
                    *ptr2++ = '\"';
                    break;
                case '\b':
                    *ptr2++ = 'b';
                    break;
                case '\f':
                    *ptr2++ = 'f';
                    break;
                case '\n':
                    *ptr2++ = 'n';
                    break;
                case '\r':
                    *ptr2++ = 'r';
                    break;
                case '\t':
                    *ptr2++ = 't';
                    break;
                default:
                    sprintf(ptr2, "u%04x", token);
                    ptr2 += 5;
                    break;
            }
        }
    }
    *ptr2++ = '\"';
    *ptr2 = '\0';
    return out;
}

static char *print_string(const cJSON *item)
{
    return print_string_ptr(item->valuestring);
}

/* Predeclare these prototypes. */
static const char *parse_value(cJSON *item, const char *value);
static char *print_value(const cJSON *item, int depth, int fmt);
static const char *parse_array(cJSON *item, const char *value);
static char *print_array(const cJSON *item, int depth, int fmt);
static const char *parse_object(cJSON *item, const char *value);
static char *print_object(const cJSON *item, int depth, int fmt);

/* Utility to jump whitespace and cr/lf */
static cJSON *cJSON_New_Item(void)
{
    cJSON *node = (cJSON*)malloc(sizeof(cJSON));
    if (node)
    {
        memset(node, 0, sizeof(cJSON));
    }
    return node;
}

/* Delete a cJSON structure. */
void cJSON_Delete(cJSON *item)
{
    cJSON *next;
    while (item)
    {
        next = item->next;
        if (!(item->type & cJSON_IsReference) && item->child)
        {
            cJSON_Delete(item->child);
        }
        if (!(item->type & cJSON_IsReference) && item->valuestring)
        {
            free(item->valuestring);
        }
        if (!(item->type & cJSON_StringIsConst) && item->string)
        {
            free(item->string);
        }
        free(item);
        item = next;
    }
}

/* Parse the input text to generate a number, and populate the result into item. */
static const char *parse_value(cJSON *item, const char *value)
{
    if (!value)
    {
        return NULL;
    }

    value = skip_whitespace(value);

    if (!strncmp(value, "null", 4))
    {
        item->type = cJSON_NULL;
        return value + 4;
    }
    if (!strncmp(value, "false", 5))
    {
        item->type = cJSON_False;
        return value + 5;
    }
    if (!strncmp(value, "true", 4))
    {
        item->type = cJSON_True;
        item->valueint = 1;
        return value + 4;
    }
    if (*value == '\"')
    {
        return parse_string(item, value);
    }
    if (*value == '-' || (*value >= '0' && *value <= '9'))
    {
        return parse_number(item, value);
    }
    if (*value == '[')
    {
        return parse_array(item, value);
    }
    if (*value == '{')
    {
        return parse_object(item, value);
    }

    global_error_pointer = value;
    return NULL;
}

/* Render a value to text. */
static char *print_value(const cJSON *item, int depth, int fmt)
{
    char *out = NULL;
    if (!item)
    {
        return NULL;
    }
    switch (item->type & 0xFF)
    {
        case cJSON_NULL:
            out = (char*)malloc(5);
            if (out)
            {
                strcpy(out, "null");
            }
            break;
        case cJSON_False:
            out = (char*)malloc(6);
            if (out)
            {
                strcpy(out, "false");
            }
            break;
        case cJSON_True:
            out = (char*)malloc(5);
            if (out)
            {
                strcpy(out, "true");
            }
            break;
        case cJSON_Number:
            out = print_number(item);
            break;
        case cJSON_String:
            out = print_string(item);
            break;
        case cJSON_Array:
            out = print_array(item, depth, fmt);
            break;
        case cJSON_Object:
            out = print_object(item, depth, fmt);
            break;
        case cJSON_Raw:
            if (item->valuestring)
            {
                out = (char*)malloc(strlen(item->valuestring) + 1);
                if (out)
                {
                    strcpy(out, item->valuestring);
                }
            }
            break;
    }
    return out;
}

/* Build an array from input text. */
static const char *parse_array(cJSON *item, const char *value)
{
    cJSON *child;
    if (*value != '[')
    {
        global_error_pointer = value;
        return NULL;
    }

    item->type = cJSON_Array;
    value = skip_whitespace(value + 1);
    if (*value == ']')
    {
        return value + 1;
    }

    item->child = child = cJSON_New_Item();
    if (!item->child)
    {
        return NULL;
    }
    value = skip_whitespace(parse_value(child, skip_whitespace(value)));
    if (!value)
    {
        return NULL;
    }

    while (*value == ',')
    {
        cJSON *new_item;
        if (!(new_item = cJSON_New_Item()))
        {
            return NULL;
        }
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip_whitespace(parse_value(child, skip_whitespace(value + 1)));
        if (!value)
        {
            return NULL;
        }
    }

    if (*value == ']')
    {
        return value + 1;
    }
    global_error_pointer = value;
    return NULL;
}

/* Render an array to text */
static char *print_array(const cJSON *item, int depth, int fmt)
{
    char **entries;
    char *out = NULL;
    char *ptr, *ret;
    size_t len = 5;
    cJSON *child = item->child;
    int numentries = 0;
    int i = 0;
    int fail = 0;

    /* Count entries */
    while (child)
    {
        numentries++;
        child = child->next;
    }

    if (!numentries)
    {
        out = (char*)malloc(3);
        if (out)
        {
            strcpy(out, "[]");
        }
        return out;
    }

    entries = (char**)malloc(numentries * sizeof(char*));
    if (!entries)
    {
        return NULL;
    }
    memset(entries, 0, numentries * sizeof(char*));

    child = item->child;
    while (child && !fail)
    {
        ret = print_value(child, depth + 1, fmt);
        entries[i++] = ret;
        if (ret)
        {
            len += strlen(ret) + 2 + (fmt ? 1 : 0);
        }
        else
        {
            fail = 1;
        }
        child = child->next;
    }

    if (!fail)
    {
        out = (char*)malloc(len);
    }
    if (!out)
    {
        fail = 1;
    }

    if (fail)
    {
        for (i = 0; i < numentries; i++)
        {
            if (entries[i])
            {
                free(entries[i]);
            }
        }
        free(entries);
        return NULL;
    }

    *out = '[';
    ptr = out + 1;
    *ptr = '\0';
    for (i = 0; i < numentries; i++)
    {
        size_t tmplen = strlen(entries[i]);
        memcpy(ptr, entries[i], tmplen);
        ptr += tmplen;
        if (i != numentries - 1)
        {
            *ptr++ = ',';
            if (fmt)
            {
                *ptr++ = ' ';
            }
        }
        free(entries[i]);
    }
    free(entries);
    *ptr++ = ']';
    *ptr = '\0';
    return out;
}

/* Build an object from the text. */
static const char *parse_object(cJSON *item, const char *value)
{
    cJSON *child;
    if (*value != '{')
    {
        global_error_pointer = value;
        return NULL;
    }

    item->type = cJSON_Object;
    value = skip_whitespace(value + 1);
    if (*value == '}')
    {
        return value + 1;
    }

    item->child = child = cJSON_New_Item();
    if (!item->child)
    {
        return NULL;
    }
    value = skip_whitespace(parse_string(child, skip_whitespace(value)));
    if (!value)
    {
        return NULL;
    }
    child->string = child->valuestring;
    child->valuestring = NULL;
    if (*value != ':')
    {
        global_error_pointer = value;
        return NULL;
    }
    value = skip_whitespace(parse_value(child, skip_whitespace(value + 1)));
    if (!value)
    {
        return NULL;
    }

    while (*value == ',')
    {
        cJSON *new_item;
        if (!(new_item = cJSON_New_Item()))
        {
            return NULL;
        }
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip_whitespace(parse_string(child, skip_whitespace(value + 1)));
        if (!value)
        {
            return NULL;
        }
        child->string = child->valuestring;
        child->valuestring = NULL;
        if (*value != ':')
        {
            global_error_pointer = value;
            return NULL;
        }
        value = skip_whitespace(parse_value(child, skip_whitespace(value + 1)));
        if (!value)
        {
            return NULL;
        }
    }

    if (*value == '}')
    {
        return value + 1;
    }
    global_error_pointer = value;
    return NULL;
}

/* Render an object to text. */
static char *print_object(const cJSON *item, int depth, int fmt)
{
    char **entries = NULL;
    char **names = NULL;
    char *out = NULL;
    char *ptr, *ret, *str;
    size_t len = 7;
    int i = 0;
    cJSON *child = item->child;
    int numentries = 0;
    int fail = 0;

    /* Count entries */
    while (child)
    {
        numentries++;
        child = child->next;
    }

    if (!numentries)
    {
        out = (char*)malloc(fmt ? depth + 4 : 3);
        if (!out)
        {
            return NULL;
        }
        ptr = out;
        *ptr++ = '{';
        if (fmt)
        {
            *ptr++ = '\n';
            for (i = 0; i < depth; i++)
            {
                *ptr++ = '\t';
            }
        }
        *ptr++ = '}';
        *ptr = '\0';
        return out;
    }

    entries = (char**)malloc(numentries * sizeof(char*));
    if (!entries)
    {
        return NULL;
    }
    names = (char**)malloc(numentries * sizeof(char*));
    if (!names)
    {
        free(entries);
        return NULL;
    }
    memset(entries, 0, numentries * sizeof(char*));
    memset(names, 0, numentries * sizeof(char*));

    child = item->child;
    depth++;
    while (child && !fail)
    {
        names[i] = str = print_string_ptr(child->string);
        entries[i++] = ret = print_value(child, depth, fmt);
        if (str && ret)
        {
            len += strlen(ret) + strlen(str) + 2 + (fmt ? 3 + depth : 0);
        }
        else
        {
            fail = 1;
        }
        child = child->next;
    }

    if (!fail)
    {
        out = (char*)malloc(len);
    }
    if (!out)
    {
        fail = 1;
    }

    if (fail)
    {
        for (i = 0; i < numentries; i++)
        {
            if (names[i])
            {
                free(names[i]);
            }
            if (entries[i])
            {
                free(entries[i]);
            }
        }
        free(names);
        free(entries);
        return NULL;
    }

    *out = '{';
    ptr = out + 1;
    if (fmt)
    {
        *ptr++ = '\n';
    }
    *ptr = '\0';
    for (i = 0; i < numentries; i++)
    {
        if (fmt)
        {
            for (int j = 0; j < depth; j++)
            {
                *ptr++ = '\t';
            }
        }
        size_t tmplen = strlen(names[i]);
        memcpy(ptr, names[i], tmplen);
        ptr += tmplen;
        *ptr++ = ':';
        if (fmt)
        {
            *ptr++ = ' ';
        }
        tmplen = strlen(entries[i]);
        memcpy(ptr, entries[i], tmplen);
        ptr += tmplen;
        if (i != numentries - 1)
        {
            *ptr++ = ',';
        }
        if (fmt)
        {
            *ptr++ = '\n';
        }
        free(names[i]);
        free(entries[i]);
    }
    free(names);
    free(entries);
    if (fmt)
    {
        for (i = 0; i < depth - 1; i++)
        {
            *ptr++ = '\t';
        }
    }
    *ptr++ = '}';
    *ptr = '\0';
    return out;
}

/* Get Array size/item / object item. */
int cJSON_GetArraySize(const cJSON *array)
{
    cJSON *child = array ? array->child : NULL;
    int size = 0;
    while (child)
    {
        size++;
        child = child->next;
    }
    return size;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    cJSON *child = array ? array->child : NULL;
    while (child && index > 0)
    {
        index--;
        child = child->next;
    }
    return child;
}

cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string)
{
    cJSON *child = object ? object->child : NULL;
    while (child && child->string && strcasecmp(child->string, string))
    {
        child = child->next;
    }
    return child;
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string)
{
    cJSON *child = object ? object->child : NULL;
    while (child && child->string && strcmp(child->string, string))
    {
        child = child->next;
    }
    return child;
}

int cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItem(object, string) ? 1 : 0;
}

/* Parse an object - create a new root, and populate. */
cJSON *cJSON_ParseWithOpts(const char *value, const char **return_parse_end, int require_null_terminated)
{
    const char *end = NULL;
    cJSON *c;

    global_error_pointer = NULL;

    c = cJSON_New_Item();
    if (!c)
    {
        return NULL;
    }

    end = parse_value(c, skip_whitespace(value));
    if (!end)
    {
        cJSON_Delete(c);
        return NULL;
    }

    if (require_null_terminated)
    {
        end = skip_whitespace(end);
        if (*end)
        {
            cJSON_Delete(c);
            global_error_pointer = end;
            return NULL;
        }
    }
    if (return_parse_end)
    {
        *return_parse_end = end;
    }
    return c;
}

cJSON *cJSON_Parse(const char *value)
{
    return cJSON_ParseWithOpts(value, NULL, 0);
}

cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    /* Make a copy of the string with null terminator */
    char *buffer = (char*)malloc(buffer_length + 1);
    cJSON *result;
    if (!buffer)
    {
        return NULL;
    }
    memcpy(buffer, value, buffer_length);
    buffer[buffer_length] = '\0';
    result = cJSON_Parse(buffer);
    free(buffer);
    return result;
}

cJSON *cJSON_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, int require_null_terminated)
{
    char *buffer = (char*)malloc(buffer_length + 1);
    cJSON *result;
    if (!buffer)
    {
        return NULL;
    }
    memcpy(buffer, value, buffer_length);
    buffer[buffer_length] = '\0';
    result = cJSON_ParseWithOpts(buffer, return_parse_end, require_null_terminated);
    free(buffer);
    return result;
}

/* Render a cJSON item/entity/structure to text. */
char *cJSON_Print(const cJSON *item)
{
    return print_value(item, 0, 1);
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    return print_value(item, 0, 0);
}

char *cJSON_PrintBuffered(const cJSON *item, int prebuffer, int fmt)
{
    (void)prebuffer;
    return print_value(item, 0, fmt);
}

int cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const int format)
{
    char *printed = print_value(item, 0, format);
    if (!printed)
    {
        return 0;
    }
    if ((int)strlen(printed) >= length)
    {
        free(printed);
        return 0;
    }
    strcpy(buffer, printed);
    free(printed);
    return 1;
}

/* Utility functions */
char *cJSON_GetStringValue(const cJSON *item)
{
    if (!cJSON_IsString(item))
    {
        return NULL;
    }
    return item->valuestring;
}

double cJSON_GetNumberValue(const cJSON *item)
{
    if (!cJSON_IsNumber(item))
    {
        return 0.0 / 0.0; /* NaN */
    }
    return item->valuedouble;
}

/* Type checking functions */
int cJSON_IsInvalid(const cJSON *item)
{
    return item == NULL || (item->type & 0xFF) == cJSON_Invalid;
}

int cJSON_IsFalse(const cJSON *item)
{
    return item != NULL && (item->type & 0xFF) == cJSON_False;
}

int cJSON_IsTrue(const cJSON *item)
{
    return item != NULL && (item->type & 0xFF) == cJSON_True;
}

int cJSON_IsBool(const cJSON *item)
{
    return item != NULL && ((item->type & 0xFF) == cJSON_True || (item->type & 0xFF) == cJSON_False);
}

int cJSON_IsNull(const cJSON *item)
{
    return item != NULL && (item->type & 0xFF) == cJSON_NULL;
}

int cJSON_IsNumber(const cJSON *item)
{
    return item != NULL && (item->type & 0xFF) == cJSON_Number;
}

int cJSON_IsString(const cJSON *item)
{
    return item != NULL && (item->type & 0xFF) == cJSON_String;
}

int cJSON_IsArray(const cJSON *item)
{
    return item != NULL && (item->type & 0xFF) == cJSON_Array;
}

int cJSON_IsObject(const cJSON *item)
{
    return item != NULL && (item->type & 0xFF) == cJSON_Object;
}

int cJSON_IsRaw(const cJSON *item)
{
    return item != NULL && (item->type & 0xFF) == cJSON_Raw;
}

/* Create functions */
cJSON *cJSON_CreateNull(void)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_NULL;
    }
    return item;
}

cJSON *cJSON_CreateTrue(void)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_True;
    }
    return item;
}

cJSON *cJSON_CreateFalse(void)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_False;
    }
    return item;
}

cJSON *cJSON_CreateBool(int boolean)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = boolean ? cJSON_True : cJSON_False;
    }
    return item;
}

cJSON *cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_Number;
        item->valuedouble = num;
        item->valueint = (int)num;
    }
    return item;
}

cJSON *cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_String;
        item->valuestring = string ? strdup(string) : NULL;
        if (!item->valuestring && string)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }
    return item;
}

cJSON *cJSON_CreateRaw(const char *raw)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_Raw;
        item->valuestring = raw ? strdup(raw) : NULL;
        if (!item->valuestring && raw)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }
    return item;
}

cJSON *cJSON_CreateArray(void)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_Array;
    }
    return item;
}

cJSON *cJSON_CreateObject(void)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_Object;
    }
    return item;
}

cJSON *cJSON_CreateStringReference(const char *string)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_String | cJSON_IsReference;
        item->valuestring = (char*)string;
    }
    return item;
}

cJSON *cJSON_CreateObjectReference(const cJSON *child)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_Object | cJSON_IsReference;
        item->child = (cJSON*)child;
    }
    return item;
}

cJSON *cJSON_CreateArrayReference(const cJSON *child)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_Array | cJSON_IsReference;
        item->child = (cJSON*)child;
    }
    return item;
}

/* Add item to array/object. */
static int add_item_to_array(cJSON *array, cJSON *item)
{
    cJSON *child;

    if (array == NULL || item == NULL)
    {
        return 0;
    }

    child = array->child;
    if (child == NULL)
    {
        array->child = item;
    }
    else
    {
        while (child->next)
        {
            child = child->next;
        }
        child->next = item;
        item->prev = child;
    }
    return 1;
}

int cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    return add_item_to_array(array, item);
}

static int add_item_to_object(cJSON *object, const char *string, cJSON *item, int constant_key)
{
    if (object == NULL || string == NULL || item == NULL)
    {
        return 0;
    }

    if (constant_key)
    {
        item->string = (char*)string;
        item->type |= cJSON_StringIsConst;
    }
    else
    {
        char *new_key = strdup(string);
        if (new_key == NULL)
        {
            return 0;
        }
        if (!(item->type & cJSON_StringIsConst) && item->string)
        {
            free(item->string);
        }
        item->string = new_key;
        item->type &= ~cJSON_StringIsConst;
    }

    return add_item_to_array(object, item);
}

int cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, 0);
}

int cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, 1);
}

int cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)
{
    if (array == NULL || item == NULL)
    {
        return 0;
    }
    return add_item_to_array(array, cJSON_CreateObjectReference(item));
}

int cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item)
{
    if (object == NULL || string == NULL || item == NULL)
    {
        return 0;
    }
    return add_item_to_object(object, string, cJSON_CreateObjectReference(item), 0);
}

/* Helper functions for adding to object */
cJSON *cJSON_AddNullToObject(cJSON *object, const char *name)
{
    cJSON *null_item = cJSON_CreateNull();
    if (add_item_to_object(object, name, null_item, 0))
    {
        return null_item;
    }
    cJSON_Delete(null_item);
    return NULL;
}

cJSON *cJSON_AddTrueToObject(cJSON *object, const char *name)
{
    cJSON *true_item = cJSON_CreateTrue();
    if (add_item_to_object(object, name, true_item, 0))
    {
        return true_item;
    }
    cJSON_Delete(true_item);
    return NULL;
}

cJSON *cJSON_AddFalseToObject(cJSON *object, const char *name)
{
    cJSON *false_item = cJSON_CreateFalse();
    if (add_item_to_object(object, name, false_item, 0))
    {
        return false_item;
    }
    cJSON_Delete(false_item);
    return NULL;
}

cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, int boolean)
{
    cJSON *bool_item = cJSON_CreateBool(boolean);
    if (add_item_to_object(object, name, bool_item, 0))
    {
        return bool_item;
    }
    cJSON_Delete(bool_item);
    return NULL;
}

cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double number)
{
    cJSON *number_item = cJSON_CreateNumber(number);
    if (add_item_to_object(object, name, number_item, 0))
    {
        return number_item;
    }
    cJSON_Delete(number_item);
    return NULL;
}

cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *string)
{
    cJSON *string_item = cJSON_CreateString(string);
    if (add_item_to_object(object, name, string_item, 0))
    {
        return string_item;
    }
    cJSON_Delete(string_item);
    return NULL;
}

cJSON *cJSON_AddRawToObject(cJSON *object, const char *name, const char *raw)
{
    cJSON *raw_item = cJSON_CreateRaw(raw);
    if (add_item_to_object(object, name, raw_item, 0))
    {
        return raw_item;
    }
    cJSON_Delete(raw_item);
    return NULL;
}

cJSON *cJSON_AddObjectToObject(cJSON *object, const char *name)
{
    cJSON *object_item = cJSON_CreateObject();
    if (add_item_to_object(object, name, object_item, 0))
    {
        return object_item;
    }
    cJSON_Delete(object_item);
    return NULL;
}

cJSON *cJSON_AddArrayToObject(cJSON *object, const char *name)
{
    cJSON *array_item = cJSON_CreateArray();
    if (add_item_to_object(object, name, array_item, 0))
    {
        return array_item;
    }
    cJSON_Delete(array_item);
    return NULL;
}

/* Detach/Delete items */
cJSON *cJSON_DetachItemViaPointer(cJSON *parent, cJSON *item)
{
    if (parent == NULL || item == NULL)
    {
        return NULL;
    }

    if (item->prev != NULL)
    {
        item->prev->next = item->next;
    }
    if (item->next != NULL)
    {
        item->next->prev = item->prev;
    }
    if (item == parent->child)
    {
        parent->child = item->next;
    }

    item->prev = NULL;
    item->next = NULL;

    return item;
}

cJSON *cJSON_DetachItemFromArray(cJSON *array, int which)
{
    cJSON *item = cJSON_GetArrayItem(array, which);
    return cJSON_DetachItemViaPointer(array, item);
}

void cJSON_DeleteItemFromArray(cJSON *array, int which)
{
    cJSON_Delete(cJSON_DetachItemFromArray(array, which));
}

cJSON *cJSON_DetachItemFromObject(cJSON *object, const char *string)
{
    cJSON *item = cJSON_GetObjectItem(object, string);
    return cJSON_DetachItemViaPointer(object, item);
}

cJSON *cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, string);
    return cJSON_DetachItemViaPointer(object, item);
}

void cJSON_DeleteItemFromObject(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObject(object, string));
}

void cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObjectCaseSensitive(object, string));
}

/* Array utilities */
cJSON *cJSON_CreateIntArray(const int *numbers, int count)
{
    int i;
    cJSON *n = NULL;
    cJSON *a = cJSON_CreateArray();

    for (i = 0; a && i < count; i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if (!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if (!cJSON_AddItemToArray(a, n))
        {
            cJSON_Delete(n);
            cJSON_Delete(a);
            return NULL;
        }
    }

    return a;
}

cJSON *cJSON_CreateFloatArray(const float *numbers, int count)
{
    int i;
    cJSON *n = NULL;
    cJSON *a = cJSON_CreateArray();

    for (i = 0; a && i < count; i++)
    {
        n = cJSON_CreateNumber((double)numbers[i]);
        if (!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if (!cJSON_AddItemToArray(a, n))
        {
            cJSON_Delete(n);
            cJSON_Delete(a);
            return NULL;
        }
    }

    return a;
}

cJSON *cJSON_CreateDoubleArray(const double *numbers, int count)
{
    int i;
    cJSON *n = NULL;
    cJSON *a = cJSON_CreateArray();

    for (i = 0; a && i < count; i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if (!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if (!cJSON_AddItemToArray(a, n))
        {
            cJSON_Delete(n);
            cJSON_Delete(a);
            return NULL;
        }
    }

    return a;
}

cJSON *cJSON_CreateStringArray(const char *const *strings, int count)
{
    int i;
    cJSON *n = NULL;
    cJSON *a = cJSON_CreateArray();

    for (i = 0; a && i < count; i++)
    {
        n = cJSON_CreateString(strings[i]);
        if (!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if (!cJSON_AddItemToArray(a, n))
        {
            cJSON_Delete(n);
            cJSON_Delete(a);
            return NULL;
        }
    }

    return a;
}

/* Insert/Replace items */
int cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem)
{
    cJSON *after_inserted;

    if (which < 0 || newitem == NULL)
    {
        return 0;
    }

    after_inserted = cJSON_GetArrayItem(array, which);
    if (after_inserted == NULL)
    {
        return cJSON_AddItemToArray(array, newitem);
    }

    newitem->next = after_inserted;
    newitem->prev = after_inserted->prev;
    after_inserted->prev = newitem;
    if (newitem->prev != NULL)
    {
        newitem->prev->next = newitem;
    }
    if (after_inserted == array->child)
    {
        array->child = newitem;
    }

    return 1;
}

int cJSON_ReplaceItemViaPointer(cJSON *parent, cJSON *item, cJSON *replacement)
{
    if (parent == NULL || replacement == NULL || item == NULL)
    {
        return 0;
    }

    if (replacement == item)
    {
        return 1;
    }

    replacement->next = item->next;
    replacement->prev = item->prev;

    if (replacement->next != NULL)
    {
        replacement->next->prev = replacement;
    }
    if (parent->child == item)
    {
        parent->child = replacement;
    }
    else
    {
        if (replacement->prev != NULL)
        {
            replacement->prev->next = replacement;
        }
    }

    item->next = NULL;
    item->prev = NULL;
    cJSON_Delete(item);

    return 1;
}

int cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem)
{
    if (which < 0)
    {
        return 0;
    }
    return cJSON_ReplaceItemViaPointer(array, cJSON_GetArrayItem(array, which), newitem);
}

int cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem)
{
    cJSON *item = cJSON_GetObjectItem(object, string);
    if (item == NULL)
    {
        return 0;
    }
    if (newitem->string)
    {
        free(newitem->string);
    }
    newitem->string = strdup(string);
    return cJSON_ReplaceItemViaPointer(object, item, newitem);
}

int cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string, cJSON *newitem)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, string);
    if (item == NULL)
    {
        return 0;
    }
    if (newitem->string)
    {
        free(newitem->string);
    }
    newitem->string = strdup(string);
    return cJSON_ReplaceItemViaPointer(object, item, newitem);
}

/* Duplicate */
cJSON *cJSON_Duplicate(const cJSON *item, int recurse)
{
    cJSON *newitem = NULL;
    cJSON *child = NULL;
    cJSON *next = NULL;
    cJSON *newchild = NULL;

    if (!item)
    {
        return NULL;
    }

    newitem = cJSON_New_Item();
    if (!newitem)
    {
        return NULL;
    }

    newitem->type = item->type & (~cJSON_IsReference);
    newitem->valueint = item->valueint;
    newitem->valuedouble = item->valuedouble;

    if (item->valuestring)
    {
        newitem->valuestring = strdup(item->valuestring);
        if (!newitem->valuestring)
        {
            cJSON_Delete(newitem);
            return NULL;
        }
    }

    if (item->string)
    {
        newitem->string = strdup(item->string);
        if (!newitem->string)
        {
            cJSON_Delete(newitem);
            return NULL;
        }
    }

    if (!recurse)
    {
        return newitem;
    }

    child = item->child;
    while (child)
    {
        newchild = cJSON_Duplicate(child, 1);
        if (!newchild)
        {
            cJSON_Delete(newitem);
            return NULL;
        }
        if (next)
        {
            next->next = newchild;
            newchild->prev = next;
            next = newchild;
        }
        else
        {
            newitem->child = newchild;
            next = newchild;
        }
        child = child->next;
    }

    return newitem;
}

/* Compare */
int cJSON_Compare(const cJSON *a, const cJSON *b, int case_sensitive)
{
    if ((a == NULL) || (b == NULL) || ((a->type & 0xFF) != (b->type & 0xFF)))
    {
        return 0;
    }

    switch (a->type & 0xFF)
    {
        case cJSON_False:
        case cJSON_True:
        case cJSON_NULL:
            return 1;

        case cJSON_Number:
            return (fabs(a->valuedouble - b->valuedouble) <= DBL_EPSILON);

        case cJSON_String:
        case cJSON_Raw:
            if ((a->valuestring == NULL) || (b->valuestring == NULL))
            {
                return 0;
            }
            return (strcmp(a->valuestring, b->valuestring) == 0);

        case cJSON_Array:
        {
            cJSON *a_element = a->child;
            cJSON *b_element = b->child;

            while (a_element && b_element)
            {
                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return 0;
                }
                a_element = a_element->next;
                b_element = b_element->next;
            }

            return (a_element == NULL && b_element == NULL);
        }

        case cJSON_Object:
        {
            cJSON *a_element;
            cJSON *b_element;
            cJSON_ArrayForEach(a_element, a)
            {
                b_element = case_sensitive
                    ? cJSON_GetObjectItemCaseSensitive(b, a_element->string)
                    : cJSON_GetObjectItem(b, a_element->string);
                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return 0;
                }
            }

            cJSON_ArrayForEach(b_element, b)
            {
                a_element = case_sensitive
                    ? cJSON_GetObjectItemCaseSensitive(a, b_element->string)
                    : cJSON_GetObjectItem(a, b_element->string);
                if (a_element == NULL)
                {
                    return 0;
                }
            }

            return 1;
        }

        default:
            return 0;
    }
}

void cJSON_Minify(char *json)
{
    char *into = json;

    if (json == NULL)
    {
        return;
    }

    while (*json)
    {
        if (*json == ' ' || *json == '\t' || *json == '\r' || *json == '\n')
        {
            json++;
        }
        else if (*json == '\"')
        {
            *into++ = *json++;
            while (*json && *json != '\"')
            {
                if (*json == '\\')
                {
                    *into++ = *json++;
                }
                *into++ = *json++;
            }
            *into++ = *json++;
        }
        else
        {
            *into++ = *json++;
        }
    }

    *into = '\0';
}

double cJSON_SetNumberHelper(cJSON *object, double number)
{
    if (object)
    {
        object->valueint = (int)number;
        object->valuedouble = number;
    }
    return number;
}

char *cJSON_SetValuestring(cJSON *object, const char *valuestring)
{
    char *copy;
    if (!cJSON_IsString(object) || valuestring == NULL)
    {
        return NULL;
    }
    copy = strdup(valuestring);
    if (copy == NULL)
    {
        return NULL;
    }
    if (object->valuestring)
    {
        free(object->valuestring);
    }
    object->valuestring = copy;
    return copy;
}

/* Memory functions */
static void *(*global_malloc)(size_t sz) = malloc;
static void (*global_free)(void *ptr) = free;

void cJSON_InitHooks(cJSON_Hooks *hooks)
{
    if (hooks == NULL)
    {
        global_malloc = malloc;
        global_free = free;
        return;
    }

    global_malloc = (hooks->malloc_fn == NULL) ? malloc : hooks->malloc_fn;
    global_free = (hooks->free_fn == NULL) ? free : hooks->free_fn;
}

void *cJSON_malloc(size_t size)
{
    return global_malloc(size);
}

void cJSON_free(void *object)
{
    global_free(object);
}

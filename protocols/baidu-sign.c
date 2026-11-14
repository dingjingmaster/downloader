#include "baidu-sign.h"

#include <clib/base64.h>
#include <clib/memory.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

int baidu_sign2 (const char* sign3, const char* sign1, unsigned char* output, size_t capacity,
                 size_t* outputSize)
{
    unsigned char key[256];
    unsigned char box[256];
    size_t        keyLength;
    size_t        dataLength;
    unsigned int  swapIndex = 0;

    if (sign3 == NULL || sign1 == NULL || outputSize == NULL || (output == NULL && strlen (sign1) > 0)) {
        errno = EINVAL;
        return -1;
    }
    keyLength = strlen (sign3);
    dataLength = strlen (sign1);
    if (keyLength == 0) {
        errno = EINVAL;
        return -1;
    }
    if (dataLength > capacity) {
        errno = ENOSPC;
        return -1;
    }
    for (unsigned int index = 0; index < 256U; index++) {
        key[index] = (unsigned char)sign3[index % keyLength];
        box[index] = (unsigned char)index;
    }
    for (unsigned int index = 0; index < 256U; index++) {
        unsigned char temporary;
        swapIndex = (swapIndex + box[index] + key[index]) & 0xffU;
        temporary = box[index];
        box[index] = box[swapIndex];
        box[swapIndex] = temporary;
    }
    {
        unsigned int left = 0;
        unsigned int right = 0;
        for (size_t index = 0; index < dataLength; index++) {
            unsigned char temporary;
            unsigned char mask;
            left = (left + 1U) & 0xffU;
            right = (right + box[left]) & 0xffU;
            temporary = box[left];
            box[left] = box[right];
            box[right] = temporary;
            mask = box[(box[left] + box[right]) & 0xffU];
            output[index] = (unsigned char)((unsigned char)sign1[index] ^ mask);
        }
    }
    *outputSize = dataLength;
    return 0;
}

int baidu_compute_sign (const char* sign3, const char* sign1, char* output, size_t capacity)
{
    unsigned char* bytes;
    size_t         size = 0;
    size_t         dataLength;
    size_t         required = 0;
    int            result;

    if (sign1 == NULL || output == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    dataLength = strlen (sign1);
    if (dataLength == 0) {
        output[0] = '\0';
        return 0;
    }
    bytes = (unsigned char*)c_malloc (dataLength);
    if (bytes == NULL)
        return -1;
    result = baidu_sign2 (sign3, sign1, bytes, dataLength, &size);
    if (result == 0 && c_base64_encode (bytes, size, output, capacity, &required) != 0) {
        errno = ENOSPC;
        result = -1;
    }
    c_free (bytes);
    return result;
}

static int baidu_extract_quoted (const char* html, const char* marker, char* destination, size_t capacity)
{
    const char* start = strstr (html, marker);
    const char* end;
    size_t      length;

    if (start == NULL)
        return -1;
    start += strlen (marker);
    end = strchr (start, '"');
    if (end == NULL)
        return -1;
    length = (size_t)(end - start);
    if (length == 0 || length >= capacity)
        return -1;
    memcpy (destination, start, length);
    destination[length] = '\0';
    return 0;
}

static int baidu_extract_digits (const char* html, const char* marker, char* destination, size_t capacity)
{
    const char* start = strstr (html, marker);
    size_t      length = 0;

    if (start == NULL)
        return -1;
    start += strlen (marker);
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    while (start[length] >= '0' && start[length] <= '9')
        length++;
    if (length == 0 || length >= capacity)
        return -1;
    memcpy (destination, start, length);
    destination[length] = '\0';
    return 0;
}

int baidu_page_extract_sign (const char* html, char* sign1, size_t sign1Capacity,
                             char* sign3, size_t sign3Capacity,
                             char* timestamp, size_t timestampCapacity)
{
    if (html == NULL || sign1 == NULL || sign3 == NULL || timestamp == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (baidu_extract_quoted (html, "\"sign1\":\"", sign1, sign1Capacity) != 0 ||
        baidu_extract_quoted (html, "\"sign3\":\"", sign3, sign3Capacity) != 0 ||
        baidu_extract_digits (html, "\"timestamp\":", timestamp, timestampCapacity) != 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

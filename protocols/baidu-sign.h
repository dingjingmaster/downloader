#ifndef DOWNLOADER_BAIDU_SIGN_H
#define DOWNLOADER_BAIDU_SIGN_H

#include <clib/macro.h>

#include <stddef.h>

C_BEGIN_EXTERN_C

/* Applies the 256-byte RC4 variant used by Baidu's web client.
 * sign3 is the key, sign1 is the transformed hex string; output is bytes. */
int baidu_sign2 (const char* sign3, const char* sign1, unsigned char* output, size_t capacity,
                 size_t* outputSize);

/* Computes the Base64 sign sent to /api/download. */
int baidu_compute_sign (const char* sign3, const char* sign1, char* output, size_t capacity);

/* Extracts sign1/sign3/timestamp from the /disk/home page. */
int baidu_page_extract_sign (const char* html, char* sign1, size_t sign1Capacity,
                             char* sign3, size_t sign3Capacity,
                             char* timestamp, size_t timestampCapacity);

C_END_EXTERN_C

#endif

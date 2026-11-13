#ifndef DOWNLOADER_ENGINES_H
#define DOWNLOADER_ENGINES_H

#include <clib/types.h>

C_BEGIN_EXTERN_C

/* Write the versions of the libraries linked into this executable to stdout. */
void download_engines_print_versions (void);

/* Offline check of library initialization and required streaming formats. */
int download_engines_check (void);

C_END_EXTERN_C

#endif

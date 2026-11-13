#include "engines.h"

#include <stdio.h>

int main (void)
{
    download_engines_print_versions ();
    if (download_engines_check () != 0) {
        perror ("bundled download engines");
        return 1;
    }
    return 0;
}

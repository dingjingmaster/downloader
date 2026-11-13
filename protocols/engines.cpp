#include "engines.h"

#include <aria2/aria2.h>
#include <cstdio>
#include <cerrno>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
}

void download_engines_print_versions (void)
{
    std::printf ("downloader 0.1.0\nlibaria2 1.37.0\nFFmpeg %s\nlibsmb2 6.0.0\n", av_version_info ());
}

int download_engines_check (void)
{
    if (aria2::libraryInit () != 0) {
        errno = EIO;
        return -1;
    }
    aria2::SessionConfig config;
    config.useSignalHandler = false;
    aria2::KeyVals options = {
        { "no-conf", "true" },
        { "no-netrc", "true" },
        { "enable-rpc", "false" },
        { "enable-dht", "false" },
        { "enable-dht6", "false" }
    };
    aria2::Session* session = aria2::sessionNew (options, config);
    int result = session == NULL ? -1 : aria2::sessionFinal (session);
    if (aria2::libraryDeinit () != 0)
        result = -1;

    struct smb2_context* smb = smb2_init_context ();
    if (smb == NULL)
        result = -1;
    else
        smb2_destroy_context (smb);

    if (av_find_input_format ("hls") == NULL || av_find_input_format ("dash") == NULL ||
        av_guess_format ("mp4", NULL, NULL) == NULL)
        result = -1;
    if (result != 0)
        errno = EIO;
    return result;
}

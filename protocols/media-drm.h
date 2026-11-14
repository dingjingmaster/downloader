#ifndef DOWNLOADER_MEDIA_DRM_H
#define DOWNLOADER_MEDIA_DRM_H

#include <clib/macro.h>

C_BEGIN_EXTERN_C

/* Recognized protection systems. This round identifies DRM and reports it
 * clearly; it never claims commercial DRM content is downloadable. */
typedef enum
{
    DOWNLOAD_MEDIA_DRM_NONE,
    DOWNLOAD_MEDIA_DRM_WIDEVINE,
    DOWNLOAD_MEDIA_DRM_PLAYREADY,
    DOWNLOAD_MEDIA_DRM_FAIRPLAY,
    DOWNLOAD_MEDIA_DRM_SAMPLE_AES,
    DOWNLOAD_MEDIA_DRM_UNKNOWN
} DownloadMediaDrm;

DownloadMediaDrm media_drm_from_scheme_id (const char* schemeIdUri);
DownloadMediaDrm media_drm_from_keyformat (const char* keyFormat);
const char*       media_drm_name (DownloadMediaDrm system);

C_END_EXTERN_C

#endif

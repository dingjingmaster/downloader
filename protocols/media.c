#define _POSIX_C_SOURCE 200809L
#include "media.h"
#include "media-drm.h"

#include <clib/directory.h>
#include <clib/json.h>
#include <clib/memory.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/version.h>
#include <libavutil/time.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <openssl/evp.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define DOWNLOADER_MEDIA_BUFFER_SIZE 32768
#define DOWNLOADER_MEDIA_MANIFEST_LIMIT (8 * 1024 * 1024)
#define DOWNLOADER_MEDIA_WHITELIST "http,https,tcp,tls,crypto"

typedef enum
{
    DOWNLOAD_MEDIA_RUNNING,
    DOWNLOAD_MEDIA_PAUSED,
    DOWNLOAD_MEDIA_CANCELLED,
    DOWNLOAD_MEDIA_STOP
} DownloadMediaControl;

struct ProtocolTask
{
    char                 url[DOWNLOAD_TASK_URL_SIZE];
    char                 tempPath[DOWNLOAD_TASK_PATH_SIZE];
    ProtocolCallbacks    callbacks;
    void*                userData;
    DownloadMediaControl control;
    bool                 dash;
    bool                 active;
    bool                 notifying;
    int                  networkError;
    char                 failure[DOWNLOAD_TASK_ERROR_SIZE];
    FILE*                file;
    uint64_t             writtenLength;
    uint64_t             networkBytes;
    int64_t              startedAt;
    int64_t              notifiedAt;
    int64_t              lastRecordingNotify;
    int64_t              durationMillis;
    uint32_t             gapCount;
    uint32_t             recordDurationSeconds;
    uint64_t             recordSizeBytes;
    bool                 live;
    char                 cacheDirectory[DOWNLOAD_TASK_PATH_SIZE];
};

typedef struct
{
    ProtocolTask* owner;
    AVIOContext*  remote;
    unsigned char* prefix;
    size_t        prefixLength;
    size_t        prefixPosition;
    bool          manifest;
    FILE*         cacheRead;
    FILE*         cacheWrite;
    char          cacheFinal[DOWNLOAD_TASK_PATH_SIZE];
    char          cachePart[DOWNLOAD_TASK_PATH_SIZE];
    uint64_t      cacheBytes;
    uint64_t      expectedSize;
} MediaInput;

/* Preserve FFmpeg's AVOption traversal through our custom AVIO layer. HLS
 * and DASH query "location" here to resolve relative URLs after redirects. */
static void* media_input_child_next (void* opaque, void* previous)
{
    AVIOContext* stream = opaque;
    MediaInput* input = stream->opaque;
    return previous == NULL && input != NULL ? input->remote : NULL;
}

static const AVClass gcMediaInputClass = {
    .class_name = "Downloader media input",
    .item_name = av_default_item_name,
    .version = LIBAVUTIL_VERSION_INT,
    .child_next = media_input_child_next
};

static bool media_http_url (const char* url)
{
    return url != NULL && (strncasecmp (url, "http://", 7) == 0 || strncasecmp (url, "https://", 8) == 0);
}

static const char* media_source_url (const char* url)
{
    if (url != NULL && (strncasecmp (url, "hls+", 4) == 0 || strncasecmp (url, "dash+", 5) == 0))
        return strchr (url, '+') + 1;
    return url;
}

static bool media_manifest_url (const char* url)
{
    size_t length = strcspn (url, "?#");

    return (length >= 5 && strncasecmp (url + length - 5, ".m3u8", 5) == 0) ||
           (length >= 4 && strncasecmp (url + length - 4, ".m3u", 4) == 0) ||
           (length >= 4 && strncasecmp (url + length - 4, ".mpd", 4) == 0);
}

static bool media_segment_url (const char* url)
{
    size_t length = strcspn (url, "?#");

    return (length >= 3 && strncasecmp (url + length - 3, ".ts", 3) == 0) ||
           (length >= 4 && strncasecmp (url + length - 4, ".m4s", 4) == 0) ||
           (length >= 4 && strncasecmp (url + length - 4, ".m4a", 4) == 0) ||
           (length >= 5 && strncasecmp (url + length - 5, ".m4v", 5) == 0);
}

static bool media_can_handle (const char* url, const char* protocol)
{
    return protocol != NULL && (strcmp (protocol, "hls") == 0 || strcmp (protocol, "dash") == 0) &&
           media_http_url (media_source_url (url));
}

static void media_progress (ProtocolTask* task, bool force, bool complete)
{
    int64_t now = av_gettime_relative ();
    int64_t elapsed = now - task->startedAt;
    uint64_t speed = elapsed > 0 ? (uint64_t)((double)task->networkBytes * 1000000.0 / elapsed) : 0;

    if (!task->notifying && (force || now - task->notifiedAt >= 100000)) {
        task->notifiedAt = now;
        task->notifying = true;
        if (task->callbacks.onProgress != NULL)
            task->callbacks.onProgress (task->userData, complete ? task->writtenLength : 0,
                                        task->writtenLength, speed);
        task->notifying = false;
    }
    if (task->live && task->callbacks.onRecording != NULL &&
        (force || now - task->lastRecordingNotify >= 1000000)) {
        task->lastRecordingNotify = now;
        task->callbacks.onRecording (task->userData, task->durationMillis, task->gapCount);
    }
}

static int media_interrupt (void* opaque)
{
    ProtocolTask* task = opaque;
    media_progress (task, false, false);
    return task->control != DOWNLOAD_MEDIA_RUNNING || task->networkError != 0;
}

static int media_network_failure (ProtocolTask* task, int result, const char* detail)
{
    if (task->networkError == 0 && task->control == DOWNLOAD_MEDIA_RUNNING) {
        task->networkError = result;
        if (detail != NULL)
            snprintf (task->failure, sizeof (task->failure), "%s", detail);
    }
    return result;
}

static bool media_contains_ci (const char* value, const char* needle)
{
    size_t needleLength = strlen (needle);

    for (const char* cursor = value; *cursor != '\0'; cursor++) {
        if (strncasecmp (cursor, needle, needleLength) == 0)
            return true;
    }
    return false;
}

DownloadMediaDrm media_drm_from_scheme_id (const char* schemeIdUri)
{
    if (schemeIdUri == NULL || schemeIdUri[0] == '\0')
        return DOWNLOAD_MEDIA_DRM_NONE;
    if (media_contains_ci (schemeIdUri, "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed") ||
        media_contains_ci (schemeIdUri, "widevine"))
        return DOWNLOAD_MEDIA_DRM_WIDEVINE;
    if (media_contains_ci (schemeIdUri, "9a04f079-9840-4286-ab92-e65be0885f95") ||
        media_contains_ci (schemeIdUri, "playready"))
        return DOWNLOAD_MEDIA_DRM_PLAYREADY;
    if (media_contains_ci (schemeIdUri, "94ce86fb-07ff-4f43-adb8-93d2fa968ca2") ||
        media_contains_ci (schemeIdUri, "fairplay"))
        return DOWNLOAD_MEDIA_DRM_FAIRPLAY;
    if (media_contains_ci (schemeIdUri, "sample-aes"))
        return DOWNLOAD_MEDIA_DRM_SAMPLE_AES;
    return DOWNLOAD_MEDIA_DRM_UNKNOWN;
}

DownloadMediaDrm media_drm_from_keyformat (const char* keyFormat)
{
    if (keyFormat == NULL || keyFormat[0] == '\0')
        return DOWNLOAD_MEDIA_DRM_UNKNOWN;
    DownloadMediaDrm system = media_drm_from_scheme_id (keyFormat);
    return system == DOWNLOAD_MEDIA_DRM_NONE ? DOWNLOAD_MEDIA_DRM_UNKNOWN : system;
}

const char* media_drm_name (DownloadMediaDrm system)
{
    switch (system) {
    case DOWNLOAD_MEDIA_DRM_WIDEVINE:
        return "Widevine";
    case DOWNLOAD_MEDIA_DRM_PLAYREADY:
        return "PlayReady";
    case DOWNLOAD_MEDIA_DRM_FAIRPLAY:
        return "FairPlay";
    case DOWNLOAD_MEDIA_DRM_SAMPLE_AES:
        return "SAMPLE-AES";
    case DOWNLOAD_MEDIA_DRM_UNKNOWN:
        return "unknown DRM";
    case DOWNLOAD_MEDIA_DRM_NONE:
        break;
    }
    return "none";
}

static DownloadMediaDrm media_drm_from_xml (xmlNode* node)
{
    for (; node != NULL; node = node->next) {
        if (node->type == XML_ELEMENT_NODE && xmlStrEqual (node->name, BAD_CAST "ContentProtection")) {
            xmlChar* scheme = xmlGetProp (node, BAD_CAST "schemeIdUri");
            /* Any ContentProtection is unsupported, including a missing or
             * unrecognized scheme; never treat it as clear content. */
            DownloadMediaDrm system = scheme == NULL || scheme[0] == '\0'
                                          ? DOWNLOAD_MEDIA_DRM_UNKNOWN
                                          : media_drm_from_scheme_id ((const char*)scheme);
            xmlFree (scheme);
            if (system != DOWNLOAD_MEDIA_DRM_NONE)
                return system;
        }
        DownloadMediaDrm child = media_drm_from_xml (node->children);
        if (child != DOWNLOAD_MEDIA_DRM_NONE)
            return child;
    }
    return DOWNLOAD_MEDIA_DRM_NONE;
}

/* AES-128 identity keys are ordinary HLS encryption, not SAMPLE-AES/DRM.
 * Conservatively reject unknown key formats instead of silently ignoring them. */
static int media_validate_hls_keys (ProtocolTask* task, const char* manifest)
{
    for (const char* line = manifest; *line != 0;) {
        size_t length = strcspn (line, "\r\n");
        if (strncmp (line, "#EXT-X-KEY:", 11) == 0 ||
            strncmp (line, "#EXT-X-SESSION-KEY:", 19) == 0) {
            const char* cursor = strchr (line, ':') + 1;
            const char* end = line + length;
            bool methodSeen = false;
            bool formatSeen = false;
            bool supported = true;
            bool sampleAes = false;
            const char* keyFormatValue = NULL;
            size_t keyFormatLength = 0;
            while (cursor < end) {
                const char* name = cursor;
                while (cursor < end && *cursor != '=')
                    cursor++;
                if (cursor == end) {
                    supported = false;
                    break;
                }
                size_t nameLength = (size_t)(cursor - name);
                cursor++;
                bool quoted = cursor < end && *cursor == '"';
                if (quoted)
                    cursor++;
                const char* value = cursor;
                while (cursor < end && *cursor != (quoted ? '"' : ','))
                    cursor++;
                size_t valueLength = (size_t)(cursor - value);
                if (nameLength == 6 && memcmp (name, "METHOD", 6) == 0) {
                    sampleAes = valueLength >= 10 && strncasecmp (value, "SAMPLE-AES", 10) == 0;
                    supported = supported && !methodSeen && !quoted &&
                                ((valueLength == 7 && memcmp (value, "AES-128", 7) == 0) ||
                                 (valueLength == 4 && memcmp (value, "NONE", 4) == 0));
                    methodSeen = true;
                } else if (nameLength == 9 && memcmp (name, "KEYFORMAT", 9) == 0) {
                    if (quoted) {
                        keyFormatValue = value;
                        keyFormatLength = valueLength;
                    }
                    supported = supported && !formatSeen && quoted && valueLength == 8 &&
                                memcmp (value, "identity", 8) == 0;
                    formatSeen = true;
                }
                if (quoted) {
                    if (cursor == end) {
                        supported = false;
                        break;
                    }
                    cursor++;
                }
                if (cursor < end && *cursor++ != ',') {
                    supported = false;
                    break;
                }
            }
            supported = supported && methodSeen;
            if (!supported) {
                DownloadMediaDrm drm = DOWNLOAD_MEDIA_DRM_UNKNOWN;
                char format[DOWNLOAD_TASK_ERROR_SIZE];
                if (sampleAes) {
                    drm = DOWNLOAD_MEDIA_DRM_SAMPLE_AES;
                } else if (keyFormatValue != NULL && keyFormatLength < sizeof (format)) {
                    memcpy (format, keyFormatValue, keyFormatLength);
                    format[keyFormatLength] = '\0';
                    drm = media_drm_from_keyformat (format);
                }
                char detail[DOWNLOAD_TASK_ERROR_SIZE];
                snprintf (detail, sizeof (detail), "DRM %s is not supported", media_drm_name (drm));
                return media_network_failure (task, AVERROR (ENOTSUP), detail);
            }
        }
        line += length;
        while (*line == '\r' || *line == '\n')
            line++;
    }
    return 0;
}

static int media_validate_manifest (ProtocolTask* task, const unsigned char* manifest, size_t length)
{
    if (length >= 7 && memcmp (manifest, "#EXTM3U", 7) == 0) {
        int result = media_validate_hls_keys (task, (const char*)manifest);
        if (result < 0)
            return result;
        /* Only a media playlist decides liveness, and once live the task stays
         * a recording even if the playlist later gains ENDLIST. */
        if (strstr ((const char*)manifest, "#EXTINF:") != NULL &&
            strstr ((const char*)manifest, "#EXT-X-ENDLIST") == NULL)
            task->live = true;
        return 0;
    }
    xmlDoc* document = xmlReadMemory ((const char*)manifest, (int)length, NULL, NULL,
                                     XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    xmlNode* root = document != NULL ? xmlDocGetRootElement (document) : NULL;
    int result = 0;
    if (root == NULL || !xmlStrEqual (root->name, BAD_CAST "MPD") ||
        document->intSubset != NULL || document->extSubset != NULL) {
        result = media_network_failure (task, AVERROR (ENOTSUP), "Unsupported DASH manifest (DTD is forbidden)");
    } else {
        xmlChar* type = xmlGetProp (root, BAD_CAST "type");
        DownloadMediaDrm drm = media_drm_from_xml (root);
        if (drm != DOWNLOAD_MEDIA_DRM_NONE) {
            char detail[DOWNLOAD_TASK_ERROR_SIZE];
            snprintf (detail, sizeof (detail), "DRM %s is not supported", media_drm_name (drm));
            result = media_network_failure (task, AVERROR (ENOTSUP), detail);
        } else {
            task->live = type != NULL && !xmlStrEqual (type, BAD_CAST "static");
        }
        xmlFree (type);
    }
    xmlFreeDoc (document);
    return result;
}

/* Segments and manifests are cached by URL plus byte-range options, so a
 * resumed run replays completed resources from disk instead of the network.
 * Only a resource read to EOF is promoted to the final cache name; partially
 * read resources are re-fetched. Encryption keys (crypto+) are never cached. */
static bool media_cache_key (const char* url, const AVDictionary* options, char* key, size_t capacity)
{
    static const char* const rangeKeys[] = { "offset", "end_offset", "range" };
    EVP_MD_CTX*   context = EVP_MD_CTX_new ();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digestLength = 0;
    bool          result = false;

    if (context == NULL)
        return false;
    if (EVP_DigestInit_ex (context, EVP_sha256 (), NULL) != 1 ||
        EVP_DigestUpdate (context, url, strlen (url)) != 1)
        goto done;
    for (size_t index = 0; index < sizeof (rangeKeys) / sizeof (rangeKeys[0]); index++) {
        AVDictionaryEntry* entry = options == NULL ? NULL : av_dict_get (options, rangeKeys[index], NULL, 0);
        if (entry == NULL)
            continue;
        if (EVP_DigestUpdate (context, rangeKeys[index], strlen (rangeKeys[index])) != 1 ||
            EVP_DigestUpdate (context, "=", 1) != 1 ||
            EVP_DigestUpdate (context, entry->value, strlen (entry->value)) != 1 ||
            EVP_DigestUpdate (context, ";", 1) != 1)
            goto done;
    }
    if (EVP_DigestFinal_ex (context, digest, &digestLength) != 1 || capacity < digestLength * 2U + 1U)
        goto done;
    for (unsigned int index = 0; index < digestLength; index++)
        snprintf (key + index * 2U, 3U, "%02x", digest[index]);
    key[digestLength * 2U] = '\0';
    result = true;
done:
    EVP_MD_CTX_free (context);
    return result;
}

static void media_cache_finish (MediaInput* input)
{
    bool ok = true;

    if (input->cacheWrite == NULL)
        return;
    if (fflush (input->cacheWrite) != 0 || fsync (fileno (input->cacheWrite)) != 0)
        ok = false;
    if (fclose (input->cacheWrite) != 0)
        ok = false;
    input->cacheWrite = NULL;
    if (!ok) {
        if (input->owner->networkError == 0)
            input->owner->networkError = errno != 0 ? errno : EIO;
        (void)unlink (input->cachePart);
        return;
    }
    if (rename (input->cachePart, input->cacheFinal) != 0 && input->owner->networkError == 0)
        input->owner->networkError = errno != 0 ? errno : EIO;
}

static bool media_cache_write (MediaInput* input, const unsigned char* data, size_t size)
{
    if (input->cacheWrite == NULL)
        return true;
    if (fwrite (data, 1, size, input->cacheWrite) != size) {
        if (input->owner->networkError == 0)
            input->owner->networkError = errno != 0 ? errno : ENOSPC;
        return false;
    }
    return true;
}

static uint64_t media_option_u64 (const AVDictionary* options, const char* key, bool* present)
{
    AVDictionaryEntry* entry = options == NULL ? NULL : av_dict_get (options, key, NULL, 0);
    char*              end = NULL;
    unsigned long long value;

    *present = false;
    if (entry == NULL)
        return 0;
    errno = 0;
    value = strtoull (entry->value, &end, 10);
    if (errno != 0 || end == entry->value)
        return 0;
    *present = true;
    return (uint64_t)value;
}

static void media_cache_open (ProtocolTask* task, MediaInput* input, const char* url,
                              const AVDictionary* options)
{
    char key[EVP_MAX_MD_SIZE * 2U + 1U];
    bool hasOffset = false;
    bool hasEnd = false;
    uint64_t offset = media_option_u64 (options, "offset", &hasOffset);
    uint64_t end = media_option_u64 (options, "end_offset", &hasEnd);
    char final[DOWNLOAD_TASK_PATH_SIZE];
    char part[DOWNLOAD_TASK_PATH_SIZE];
    int  descriptor;

    if (task->cacheDirectory[0] == '\0' || strncmp (url, "crypto+", 7) == 0 ||
        media_manifest_url (url) || !media_cache_key (url, options, key, sizeof (key)))
        return;
    if (hasOffset && hasEnd && end > offset)
        input->expectedSize = end - offset;
    if (snprintf (final, sizeof (final), "%s/%s", task->cacheDirectory, key) >= (int)sizeof (final) ||
        snprintf (part, sizeof (part), "%s/%s.part", task->cacheDirectory, key) >= (int)sizeof (part))
        return;
    descriptor = open (final, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor >= 0) {
        struct stat status;
        if (fstat (descriptor, &status) == 0 && S_ISREG (status.st_mode) && status.st_nlink == 1) {
            input->cacheRead = fdopen (descriptor, "rb");
            if (input->cacheRead != NULL) {
                snprintf (input->cacheFinal, sizeof (input->cacheFinal), "%s", final);
                return;
            }
        }
        (void)close (descriptor);
    }
    descriptor = open (part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0) {
        struct stat status;
        if (fstat (descriptor, &status) == 0 && S_ISREG (status.st_mode) && status.st_nlink == 1) {
            input->cacheWrite = fdopen (descriptor, "wb");
            if (input->cacheWrite != NULL) {
                snprintf (input->cacheFinal, sizeof (input->cacheFinal), "%s", final);
                snprintf (input->cachePart, sizeof (input->cachePart), "%s", part);
                return;
            }
        }
        (void)close (descriptor);
    }
}

static int media_stream_read (MediaInput* input, unsigned char* buffer, int size)
{
    int result;

    if (input->cacheRead != NULL) {
        size_t count = fread (buffer, 1, (size_t)size, input->cacheRead);
        if (count > 0)
            return (int)count;
        return ferror (input->cacheRead) ? AVERROR (errno != 0 ? errno : EIO) : AVERROR_EOF;
    }
    result = avio_read (input->remote, buffer, size);
    if (result > 0) {
        input->owner->networkBytes += (uint64_t)result;
        input->cacheBytes += (uint64_t)result;
        if (!media_cache_write (input, buffer, (size_t)result))
            return AVERROR (input->owner->networkError);
        if (input->expectedSize > 0 && input->cacheBytes >= input->expectedSize)
            media_cache_finish (input);
    } else if (result == AVERROR_EOF) {
        media_cache_finish (input);
    } else {
        /* A ranged resource is complete once its declared size arrived; the
         * HTTP layer may still report a range-boundary read error. */
        if (input->expectedSize > 0 && input->cacheBytes >= input->expectedSize) {
            media_cache_finish (input);
            return AVERROR_EOF;
        }
        media_network_failure (input->owner, result, "Media segment read failed or was truncated");
    }
    if (input->remote->error < 0 && input->remote->error != AVERROR_EOF &&
        !(input->expectedSize > 0 && input->cacheBytes >= input->expectedSize))
        media_network_failure (input->owner, input->remote->error, "Media HTTP transfer ended prematurely");
    return result;
}

static int media_input_read (void* opaque, uint8_t* buffer, int size)
{
    MediaInput* input = opaque;
    if (media_interrupt (input->owner))
        return AVERROR_EXIT;
    if (input->prefixPosition < input->prefixLength) {
        size_t count = input->prefixLength - input->prefixPosition;
        if (count > (size_t)size)
            count = (size_t)size;
        memcpy (buffer, input->prefix + input->prefixPosition, count);
        input->prefixPosition += count;
        return (int)count;
    }
    if (input->manifest)
        return AVERROR_EOF;
    return media_stream_read (input, buffer, size);
}

static int64_t media_input_seek (void* opaque, int64_t offset, int whence)
{
    MediaInput* input = opaque;
    if (input->cacheRead != NULL) {
        if (whence == AVSEEK_SIZE) {
            struct stat status;
            return fstat (fileno (input->cacheRead), &status) == 0 ? (int64_t)status.st_size : AVERROR (errno);
        }
        if (fseeko (input->cacheRead, (off_t)offset, whence & ~AVSEEK_FORCE) != 0)
            return AVERROR (errno);
        off_t position = ftello (input->cacheRead);
        return position < 0 ? AVERROR (errno) : position;
    }
    if (whence == AVSEEK_SIZE)
        return input->manifest ? (int64_t)input->prefixLength : avio_size (input->remote);
    /* Sequential segment consumption; retaining the probe prefix must not cause
     * remote seeks to silently skip data. FFmpeg can seek within its IO buffer. */
    (void)offset;
    return AVERROR (ENOSYS);
}

static int media_input_close (AVFormatContext* format, AVIOContext* stream)
{
    MediaInput* input = stream->opaque;
    int result = 0;
    if (input->cacheRead != NULL) {
        if (fclose (input->cacheRead) != 0)
            result = AVERROR (errno != 0 ? errno : EIO);
        input->cacheRead = NULL;
    }
    if (input->cacheWrite != NULL) {
        /* Closed before EOF: the .part file is not a completed resource. */
        if (fclose (input->cacheWrite) != 0 && result == 0)
            result = AVERROR (errno != 0 ? errno : EIO);
        input->cacheWrite = NULL;
    }
    if (input->remote != NULL) {
        int closed = avio_closep (&input->remote);
        if (closed < 0 && result == 0)
            result = closed;
    }
    (void)format;
    c_free (input->prefix);
    c_free (input);
    av_freep (&stream->buffer);
    avio_context_free (&stream);
    return result;
}

static int media_input_open (AVFormatContext* format, AVIOContext** stream, const char* url,
                             int flags, AVDictionary** options)
{
    ProtocolTask* task = format->opaque;
    MediaInput* input = NULL;
    AVDictionary* settings = NULL;
    AVIOInterruptCB interrupt = {media_interrupt, task};
    int result;
    unsigned char* buffer = NULL;

    *stream = NULL;
    const char* transport = strncmp (url, "crypto+", 7) == 0 ? url + 7 : url;
    if (!media_http_url (transport) || flags != AVIO_FLAG_READ)
        return media_network_failure (task, AVERROR (EACCES), "Media playlists may reference HTTP(S) resources only");
    input = c_malloc0 (sizeof (*input));
    if (input == NULL)
        return media_network_failure (task, AVERROR (ENOMEM), "Cannot allocate media input");
    input->owner = task;
    media_cache_open (task, input, url, options != NULL ? *options : NULL);
    result = av_dict_copy (&settings, options != NULL ? *options : NULL, 0);
    if (result < 0)
        goto failed;
#define DOWNLOADER_MEDIA_OPTION(key, value) \
    do { result = av_dict_set (&settings, key, value, 0); if (result < 0) goto failed; } while (0)
    DOWNLOADER_MEDIA_OPTION ("protocol_whitelist", DOWNLOADER_MEDIA_WHITELIST);
    DOWNLOADER_MEDIA_OPTION ("tls_verify", "1");
    DOWNLOADER_MEDIA_OPTION ("rw_timeout", "15000000");
    DOWNLOADER_MEDIA_OPTION ("timeout", "15000000");
    DOWNLOADER_MEDIA_OPTION ("http_persistent", "0");
#undef DOWNLOADER_MEDIA_OPTION
    if (input->cacheRead == NULL) {
        result = avio_open2 (&input->remote, url, AVIO_FLAG_READ, &interrupt, &settings);
        av_dict_free (&settings);
        if (result < 0)
            goto failed;
    } else {
        av_dict_free (&settings);
    }
    input->prefix = c_malloc (513);
    if (input->prefix == NULL) {
        result = AVERROR (ENOMEM);
        goto failed;
    }
    result = media_stream_read (input, input->prefix, 512);
    if (result < 0)
        goto failed;
    input->prefixLength = (size_t)result;
    input->prefix[input->prefixLength] = 0;
    const unsigned char* start = input->prefix;
    if (input->prefixLength >= 3 && memcmp (start, "\xef\xbb\xbf", 3) == 0)
        start += 3;
    while (*start == ' ' || *start == '\n' || *start == '\r' || *start == '\t')
        start++;
    input->manifest = strncmp ((const char*)start, "<?xml", 5) == 0 ||
                      strncmp ((const char*)start, "<MPD", 4) == 0 ||
                      strncmp ((const char*)start, "<!--", 4) == 0 ||
                      strncmp ((const char*)start, "#EXTM3U", 7) == 0;
    if (input->manifest) {
        size_t leading = (size_t)(start - input->prefix);
        unsigned char* expanded = c_realloc (input->prefix, DOWNLOADER_MEDIA_MANIFEST_LIMIT + 1);
        if (expanded == NULL) {
            result = AVERROR (ENOMEM);
            goto failed;
        }
        input->prefix = expanded;
        start = input->prefix + leading;
        while (input->prefixLength < DOWNLOADER_MEDIA_MANIFEST_LIMIT) {
            result = media_stream_read (input, input->prefix + input->prefixLength,
                                        DOWNLOADER_MEDIA_MANIFEST_LIMIT - (int)input->prefixLength);
            if (result == AVERROR_EOF)
                break;
            if (result < 0)
                goto failed;
            if (result == 0)
                break;
            input->prefixLength += (size_t)result;
        }
        if (input->prefixLength == DOWNLOADER_MEDIA_MANIFEST_LIMIT) {
            result = AVERROR (EFBIG);
            goto failed;
        }
        input->prefix[input->prefixLength] = 0;
        result = media_validate_manifest (task, start, input->prefixLength - (size_t)(start - input->prefix));
        if (result < 0)
            goto failed;
    }
    if (input->remote != NULL && input->remote->error < 0 && input->remote->error != AVERROR_EOF) {
        result = input->remote->error;
        goto failed;
    }
    buffer = av_malloc (DOWNLOADER_MEDIA_BUFFER_SIZE);
    if (buffer == NULL) {
        result = AVERROR (ENOMEM);
        goto failed;
    }
    *stream = avio_alloc_context (buffer, DOWNLOADER_MEDIA_BUFFER_SIZE, 0, input,
                                  media_input_read, NULL, media_input_seek);
    if (*stream == NULL) {
        result = AVERROR (ENOMEM);
        goto failed;
    }
    (*stream)->av_class = &gcMediaInputClass;
    (*stream)->seekable = 0;
    return 0;
failed:
    if (media_segment_url (url) && task->control == DOWNLOAD_MEDIA_RUNNING) {
        /* A missing segment cannot be silently skipped by the demuxer; count
         * the gap and let the task fail with a clear reason instead of
         * publishing a recording that pretends to be complete. Pause/cancel
         * aborts are not gaps. */
        task->gapCount++;
        media_progress (task, true, false);
    }
    av_dict_free (&settings);
    av_free (buffer);
    if (input->cacheRead != NULL)
        (void)fclose (input->cacheRead);
    if (input->cacheWrite != NULL)
        (void)fclose (input->cacheWrite);
    if (input->remote != NULL)
        avio_closep (&input->remote);
    c_free (input->prefix);
    c_free (input);
    return media_network_failure (task, result, task->failure[0] ? NULL : "Cannot open/read media playlist or segment");
}

/* Validate the opened inode before truncating: tempPath can already exist
 * after pause/restart. O_NONBLOCK also prevents a substituted FIFO from hanging
 * the worker before fstat rejects it. This function owns the fd on every path. */
static int media_output_open (ProtocolTask* task)
{
    int descriptor = open (task->tempPath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    struct stat status;
    int error;

    if (descriptor < 0)
        return AVERROR (errno);
    if (fstat (descriptor, &status) != 0) {
        error = errno;
        goto failed;
    }
    if (!S_ISREG (status.st_mode) || status.st_nlink != 1) {
        error = !S_ISREG (status.st_mode) ? EINVAL : EMLINK;
        goto failed;
    }
    if (ftruncate (descriptor, 0) != 0) {
        error = errno;
        goto failed;
    }
    task->file = fdopen (descriptor, "r+b");
    if (task->file == NULL) {
        error = errno;
        goto failed;
    }
    return 0;
failed:
    (void)close (descriptor);
    errno = error;
    return AVERROR (error);
}

static int media_output_write (void* opaque, const uint8_t* buffer, int size)
{
    ProtocolTask* task = opaque;
    if (fwrite (buffer, 1, (size_t)size, task->file) != (size_t)size)
        return AVERROR (errno != 0 ? errno : EIO);
    off_t position = ftello (task->file);
    if (position < 0)
        return AVERROR (errno);
    if ((uint64_t)position > task->writtenLength)
        task->writtenLength = (uint64_t)position;
    return size;
}

static int64_t media_output_seek (void* opaque, int64_t offset, int whence)
{
    ProtocolTask* task = opaque;
    if (whence == AVSEEK_SIZE)
        return (int64_t)task->writtenLength;
    if (fseeko (task->file, (off_t)offset, whence & ~AVSEEK_FORCE) != 0)
        return AVERROR (errno);
    off_t position = ftello (task->file);
    return position < 0 ? AVERROR (errno) : position;
}

static void media_filename (ProtocolTask* task)
{
    char filename[DOWNLOAD_TASK_FILE_NAME_SIZE];
    const char* path = strstr (task->url, "://") + 3;
    const char* end = path + strcspn (path, "?#");
    const char* base = path;
    for (const char* cursor = path; cursor < end; cursor++) {
        if (*cursor == '/')
            base = cursor + 1;
    }
    const char* extension = NULL;
    for (const char* cursor = base; cursor < end; cursor++) {
        if (*cursor == '.')
            extension = cursor;
    }
    size_t length = (size_t)((extension != NULL ? extension : end) - base);
    if (length == 0 || length > sizeof (filename) - 5) {
        base = "media";
        length = 5;
    }
    memcpy (filename, base, length);
    memcpy (filename + length, ".mkv", 5);
    if (task->callbacks.onFileName != NULL)
        task->callbacks.onFileName (task->userData, filename);
}

static int media_record_options (ProtocolTask* task, const char* extra)
{
    CJsonDocument* document;
    CJsonValue*    record;
    int            result = 0;

    if (extra == NULL || extra[0] == '\0')
        return 0;
    document = c_json_document_new_from_data (c_string_view_from_cstr (extra), C_JSON_PARSE_NONE, NULL);
    if (document == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (c_json_value_type (c_json_document_root (document)) != C_JSON_TYPE_OBJECT) {
        errno = EINVAL;
        result = -1;
        goto done;
    }
    record = c_json_object_get (c_json_document_root (document), c_string_view_from_cstr ("record"));
    if (record != NULL) {
        CJsonValue* duration;
        CJsonValue* size;
        int64_t     durationValue = 0;
        int64_t     sizeValue = 0;
        if (c_json_value_type (record) != C_JSON_TYPE_OBJECT) {
            errno = EINVAL;
            result = -1;
            goto done;
        }
        duration = c_json_object_get (record, c_string_view_from_cstr ("duration"));
        if (duration != NULL &&
            (c_json_value_get_int64 (duration, &durationValue) != 0 || durationValue <= 0 ||
             durationValue > 7 * 24 * 3600)) {
            errno = EINVAL;
            result = -1;
            goto done;
        }
        size = c_json_object_get (record, c_string_view_from_cstr ("size"));
        if (size != NULL && (c_json_value_get_int64 (size, &sizeValue) != 0 || sizeValue <= 0)) {
            errno = EINVAL;
            result = -1;
            goto done;
        }
        task->recordDurationSeconds = (uint32_t)durationValue;
        task->recordSizeBytes = (uint64_t)sizeValue;
    }
done:
    c_json_document_free (document);
    return result;
}

static int media_open (ProtocolTask** result, const DownloadTask* task, const ProtocolCallbacks* callbacks,
                       void* userData)
{
    if (result == NULL || task == NULL) {
        errno = EINVAL;
        return -1;
    }
    *result = NULL;
    if (memchr (task->resourceUrl, 0, sizeof (task->resourceUrl)) == NULL ||
        memchr (task->protocol, 0, sizeof (task->protocol)) == NULL ||
        memchr (task->tempPath, 0, sizeof (task->tempPath)) == NULL ||
        !media_can_handle (task->resourceUrl, task->protocol) || task->tempPath[0] == 0) {
        errno = EINVAL;
        return -1;
    }
    ProtocolTask* context = c_malloc0 (sizeof (*context));
    if (context == NULL) {
        errno = ENOMEM;
        return -1;
    }
    snprintf (context->url, sizeof (context->url), "%s", media_source_url (task->resourceUrl));
    snprintf (context->tempPath, sizeof (context->tempPath), "%s", task->tempPath);
    context->dash = strcmp (task->protocol, "dash") == 0;
    if (media_record_options (context, task->extraJson) != 0) {
        c_free (context);
        return -1;
    }
    if (callbacks != NULL)
        context->callbacks = *callbacks;
    context->userData = userData;
    *result = context;
    return 0;
}

static void media_cache_remove (ProtocolTask* task)
{
    if (task->cacheDirectory[0] != '\0') {
        (void)c_directory_remove_recursive (task->cacheDirectory);
        task->cacheDirectory[0] = '\0';
    }
}

static int media_run (ProtocolTask* task)
{
    AVFormatContext* input = NULL;
    AVFormatContext* output = NULL;
    AVIOContext* manifest = NULL;
    AVIOContext* destination = NULL;
    AVPacket* packet = NULL;
    AVDictionary* settings = NULL;
    uint8_t* effectiveUrl = NULL;
    int* streamMap = NULL;
    int result = 0;
    int selected[2] = {-1, -1};
    int64_t first[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    int64_t last[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    bool packetsWritten = false;
    const char* stage = "initializing media input";

    if (task == NULL || task->active) {
        errno = task == NULL ? EINVAL : EBUSY;
        return -1;
    }
    task->active = true;
    task->networkError = 0;
    task->failure[0] = 0;
    task->writtenLength = 0;
    task->networkBytes = 0;
    task->startedAt = av_gettime_relative ();
    media_progress (task, true, false);
    if (task->control != DOWNLOAD_MEDIA_RUNNING)
        goto finished;
    if (task->callbacks.onStarted != NULL)
        task->callbacks.onStarted (task->userData);
    media_filename (task);
    if (media_interrupt (task))
        goto finished;
    if (snprintf (task->cacheDirectory, sizeof (task->cacheDirectory), "%s.segments", task->tempPath) >=
        (int)sizeof (task->cacheDirectory)) {
        result = AVERROR (ENAMETOOLONG);
        stage = "segment cache path is too long";
        goto finished;
    }
    if (c_directory_create_with_mode (task->cacheDirectory, 0700) != 0 && errno != EEXIST) {
        result = AVERROR (errno);
        stage = "cannot create segment cache";
        goto finished;
    }
    {
        struct stat cacheStatus;
        if (lstat (task->cacheDirectory, &cacheStatus) != 0 || !S_ISDIR (cacheStatus.st_mode) ||
            S_ISLNK (cacheStatus.st_mode)) {
            result = AVERROR (EINVAL);
            stage = "segment cache is not a private directory";
            goto finished;
        }
    }
    input = avformat_alloc_context ();
    if (input == NULL) {
        result = AVERROR (ENOMEM);
        goto finished;
    }
    input->opaque = task;
    input->interrupt_callback.callback = media_interrupt;
    input->interrupt_callback.opaque = task;
    input->io_open = media_input_open;
    input->io_close2 = media_input_close;
    result = media_input_open (input, &manifest, task->url, AVIO_FLAG_READ, NULL);
    if (result < 0)
        goto finished;
    if (!((MediaInput*)manifest->opaque)->manifest) {
        result = AVERROR (EINVAL);
        stage = "input is not an HLS/DASH manifest";
        goto finished;
    }
    input->pb = manifest;
    input->flags |= AVFMT_FLAG_CUSTOM_IO;
    input->error_recognition = AV_EF_EXPLODE | AV_EF_CRCCHECK;
    result = av_dict_set (&settings, "protocol_whitelist", DOWNLOADER_MEDIA_WHITELIST, 0);
    if (result >= 0)
        result = av_dict_set (&settings, "http_persistent", "0", 0);
    /* Record a live window from its oldest segment, so the recording includes
     * codec setup instead of starting at the live edge. */
    if (result >= 0)
        result = av_dict_set (&settings, "live_start_index", "0", 0);
    if (result >= 0 && !task->dash)
        result = av_dict_set (&settings, "http_multiple", "0", 0);
    if (result < 0)
        goto finished;
    const AVInputFormat* demuxer = av_find_input_format (task->dash ? "dash" : "hls");
    if (demuxer == NULL) {
        result = AVERROR (ENOTSUP);
        stage = "bundled FFmpeg lacks the required demuxer";
        goto finished;
    }
    /* DASH also builds SegmentTemplate paths from AVFormatContext.url, rather
     * than exclusively using the IO location option. Supply the final URL to
     * both paths; leave task->url intact so resume repeats the original request. */
    result = av_opt_get (manifest, "location", AV_OPT_SEARCH_CHILDREN, &effectiveUrl);
    if (result < 0 && result != AVERROR_OPTION_NOT_FOUND)
        goto finished;
    if (effectiveUrl != NULL && !media_http_url ((const char*)effectiveUrl)) {
        result = AVERROR (EACCES);
        stage = "redirected manifest must use HTTP(S)";
        goto finished;
    }
    result = avformat_open_input (&input, effectiveUrl != NULL ? (const char*)effectiveUrl : task->url,
                                  demuxer, &settings);
    av_dict_free (&settings);
    if (result < 0)
        goto finished;
    stage = "probing media streams";
    result = avformat_find_stream_info (input, NULL);
    if (result < 0 || task->networkError != 0)
        goto finished;
    /* Live windows can report a finite duration; the manifest type is
     * authoritative and already set by media_validate_manifest. */
    selected[0] = av_find_best_stream (input, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    selected[1] = av_find_best_stream (input, AVMEDIA_TYPE_AUDIO, -1, selected[0], NULL, 0);
    if (selected[0] < 0 && selected[1] < 0) {
        result = AVERROR (ENOTSUP);
        stage = "no supported audio/video streams";
        goto finished;
    }
    result = avformat_alloc_output_context2 (&output, NULL, "matroska", NULL);
    if (result < 0)
        goto finished;
    streamMap = c_malloc (input->nb_streams * sizeof (*streamMap));
    if (streamMap == NULL) {
        result = AVERROR (ENOMEM);
        goto finished;
    }
    for (unsigned int index = 0; index < input->nb_streams; index++) {
        streamMap[index] = -1;
        AVStream* source = input->streams[index];
        if ((int)index != selected[0] && (int)index != selected[1]) {
            source->discard = AVDISCARD_ALL;
            continue;
        }
        AVStream* target = avformat_new_stream (output, NULL);
        if (target == NULL) {
            result = AVERROR (ENOMEM);
            goto finished;
        }
        streamMap[index] = target->index;
        result = avcodec_parameters_copy (target->codecpar, source->codecpar);
        if (result < 0)
            goto finished;
        target->codecpar->codec_tag = 0;
        target->time_base = source->time_base;
    }
    stage = "opening temporary Matroska output";
    result = media_output_open (task);
    if (result < 0)
        goto finished;
    unsigned char* buffer = av_malloc (DOWNLOADER_MEDIA_BUFFER_SIZE);
    if (buffer == NULL) {
        result = AVERROR (ENOMEM);
        goto finished;
    }
    destination = avio_alloc_context (buffer, DOWNLOADER_MEDIA_BUFFER_SIZE, 1, task,
                                      NULL, media_output_write, media_output_seek);
    if (destination == NULL) {
        av_free (buffer);
        result = AVERROR (ENOMEM);
        goto finished;
    }
    output->pb = destination;
    output->flags |= AVFMT_FLAG_CUSTOM_IO;
    stage = "writing Matroska header (codec may not be supported)";
    result = avformat_write_header (output, NULL);
    if (result < 0)
        goto finished;
    packet = av_packet_alloc ();
    if (packet == NULL) {
        result = AVERROR (ENOMEM);
        goto finished;
    }
    stage = "reading/remuxing media packets";
    while (!media_interrupt (task)) {
        result = av_read_frame (input, packet);
        if (result < 0)
            break;
        if ((packet->flags & AV_PKT_FLAG_CORRUPT) ||
            av_packet_get_side_data (packet, AV_PKT_DATA_ENCRYPTION_INFO, NULL) != NULL) {
            result = AVERROR_INVALIDDATA;
            stage = "corrupt or encrypted media packet";
            goto finished;
        }
        int index = packet->stream_index;
        if (index < 0 || (unsigned int)index >= input->nb_streams) {
            result = AVERROR_INVALIDDATA;
            goto finished;
        }
        if (streamMap[index] < 0) {
            av_packet_unref (packet);
            continue;
        }
        AVStream* source = input->streams[index];
        int slot = index == selected[0] ? 0 : 1;
        int64_t timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        if (timestamp != AV_NOPTS_VALUE) {
            int64_t start = av_rescale_q (timestamp, source->time_base, AV_TIME_BASE_Q);
            int64_t end = start + av_rescale_q (packet->duration, source->time_base, AV_TIME_BASE_Q);
            if (first[slot] == AV_NOPTS_VALUE || start < first[slot])
                first[slot] = start;
            if (last[slot] == AV_NOPTS_VALUE || end > last[slot])
                last[slot] = end;
        }
        packet->stream_index = streamMap[index];
        av_packet_rescale_ts (packet, source->time_base, output->streams[packet->stream_index]->time_base);
        packet->pos = -1;
        result = av_interleaved_write_frame (output, packet);
        av_packet_unref (packet);
        if (result < 0)
            goto finished;
        packetsWritten = true;
        {
            int64_t durationUs = 0;
            for (int slot = 0; slot < 2; slot++) {
                if (first[slot] != AV_NOPTS_VALUE && last[slot] != AV_NOPTS_VALUE &&
                    last[slot] - first[slot] > durationUs)
                    durationUs = last[slot] - first[slot];
            }
            task->durationMillis = durationUs / 1000;
        }
        if ((task->recordDurationSeconds > 0 &&
             task->durationMillis >= (int64_t)task->recordDurationSeconds * 1000) ||
            (task->recordSizeBytes > 0 && task->writtenLength >= task->recordSizeBytes)) {
            task->control = DOWNLOAD_MEDIA_STOP;
            break;
        }
    }
    if (task->control == DOWNLOAD_MEDIA_STOP && (!task->live || task->networkError != 0)) {
        /* A VOD finish (or a finish after a failed segment) must not publish a
         * truncated file as complete. */
        result = task->networkError != 0 ? task->networkError : AVERROR (ENOTSUP);
        stage = "recording finish is only valid for a complete live segment stream";
        goto finished;
    }
    {
        bool finishedByStop = task->control == DOWNLOAD_MEDIA_STOP;
        if (!finishedByStop && task->control != DOWNLOAD_MEDIA_RUNNING)
            goto finished;
        if (!finishedByStop && (result != AVERROR_EOF || task->networkError != 0))
            goto finished;
        result = AVERROR_INVALIDDATA;
        stage = "incomplete media timeline or empty recording";
        if (!packetsWritten)
            goto finished;
        if (finishedByStop) {
            result = 0;
        } else if (!task->live) {
            /* Detect demuxers turning a truncated final segment into EOF. Allow
             * normal mux/manifest rounding, but never silently accept a missing
             * whole segment for a finite VOD. */
            for (int slot = 0; slot < 2; slot++) {
                if (selected[slot] >= 0 && (first[slot] == AV_NOPTS_VALUE ||
                    last[slot] - first[slot] + 500000 < input->duration))
                    goto finished;
            }
        }
    }
    stage = "writing Matroska trailer";
    result = av_write_trailer (output);
    avio_flush (destination);
    if (result >= 0 && destination->error < 0)
        result = destination->error;
    if (result >= 0 && (task->control == DOWNLOAD_MEDIA_RUNNING || task->control == DOWNLOAD_MEDIA_STOP))
        media_cache_remove (task);
finished:
    av_free (effectiveUrl);
    av_dict_free (&settings);
    av_packet_free (&packet);
    c_free (streamMap);
    avformat_close_input (&input);
    if (manifest != NULL)
        media_input_close (NULL, manifest);
    avformat_free_context (output);
    if (destination != NULL) {
        av_freep (&destination->buffer);
        avio_context_free (&destination);
    }
    if (task->file != NULL) {
        if (fclose (task->file) != 0 && result >= 0)
            result = AVERROR (errno != 0 ? errno : EIO);
        task->file = NULL;
    }
    task->active = false;
    if (task->control == DOWNLOAD_MEDIA_CANCELLED) {
        media_cache_remove (task);
        errno = ECANCELED;
        return -1;
    }
    if (task->control == DOWNLOAD_MEDIA_PAUSED) {
        if (task->callbacks.onPaused != NULL)
            task->callbacks.onPaused (task->userData);
        return 1;
    }
    if (task->networkError != 0)
        result = task->networkError;
    if (result < 0) {
        char reason[AV_ERROR_MAX_STRING_SIZE];
        char message[DOWNLOAD_TASK_ERROR_SIZE];
        av_strerror (result, reason, sizeof (reason));
        snprintf (message, sizeof (message), "%.440s: %.63s", task->failure[0] ? task->failure : stage, reason);
        int error = result < 0 && result > -4096 ? -result : EIO;
        if (task->callbacks.onFailed != NULL)
            task->callbacks.onFailed (task->userData, error, message);
        errno = error;
        return -1;
    }
    media_progress (task, true, true);
    if (task->control == DOWNLOAD_MEDIA_PAUSED) {
        if (task->callbacks.onPaused != NULL)
            task->callbacks.onPaused (task->userData);
        return 1;
    }
    if (task->control == DOWNLOAD_MEDIA_CANCELLED) {
        errno = ECANCELED;
        return -1;
    }
    if (task->callbacks.onCompleted != NULL)
        task->callbacks.onCompleted (task->userData);
    return 0;
}

static int media_pause (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (task->control != DOWNLOAD_MEDIA_CANCELLED)
        task->control = DOWNLOAD_MEDIA_PAUSED;
    return 0;
}

static int media_resume (ProtocolTask* task)
{
    if (task == NULL || task->control == DOWNLOAD_MEDIA_CANCELLED) {
        errno = task == NULL ? EINVAL : ECANCELED;
        return -1;
    }
    task->control = DOWNLOAD_MEDIA_RUNNING;
    return 0;
}

static int media_cancel (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task->control = DOWNLOAD_MEDIA_CANCELLED;
    return 0;
}

static int media_stop (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!task->live) {
        errno = ENOTSUP;
        return -1;
    }
    if (task->control != DOWNLOAD_MEDIA_CANCELLED)
        task->control = DOWNLOAD_MEDIA_STOP;
    return 0;
}

static void media_close (ProtocolTask* task)
{
    c_free (task);
}

static bool media_retryable (ProtocolTask* task, int errorCode, const char* message)
{
    (void)errorCode;
    (void)message;
    if (task == NULL || task->networkError == 0)
        return false;
    /* A missing/unopenable segment is a content error, not a transient network
     * interruption; keep it terminal instead of retrying a broken playlist. */
    if (task->gapCount > 0)
        return false;
    return task->networkError != AVERROR_EXIT && task->networkError != AVERROR (ENOTSUP) &&
           task->networkError != AVERROR (EACCES) && task->networkError != AVERROR (EINVAL) &&
           task->networkError != AVERROR (ENOSPC) && task->networkError != AVERROR (EROFS);
}

const ProtocolOps* media_protocol_ops (void)
{
    static const ProtocolOps ops = {
        .name = "media", .canHandle = media_can_handle, .open = media_open,
        .run = media_run, .pause = media_pause, .resume = media_resume,
        .cancel = media_cancel, .stop = media_stop, .close = media_close, .retryable = media_retryable
    };
    return &ops;
}

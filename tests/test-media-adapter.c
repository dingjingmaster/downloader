#define _POSIX_C_SOURCE 200809L
/* Standalone adapter test: include implementation to exercise manifest guards.
 * No root build integration needed. Optional arguments: URL OUTPUT [pause|cancel].
 * A stall test URL should accept HTTP and delay its response; callbacks must
 * interrupt it in < 3 seconds. Normal URL runs can use generated local VOD.
 */
#include "../protocols/media.c"
#include <assert.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stddef.h>
#include <libavutil/opt.h>

typedef struct
{
    ProtocolTask* task;
    uint64_t      total;
    uint64_t      downloaded;
    int64_t       started;
    int           stop;
    int           completed;
    int           failed;
    int           paused;
    char          filename[DOWNLOAD_TASK_FILE_NAME_SIZE];
} MediaTest;

static void test_progress (void* opaque, uint64_t total, uint64_t downloaded, uint64_t speed)
{
    MediaTest* test = opaque;
    (void)speed;
    test->total = total;
    test->downloaded = downloaded;
    if (test->stop && av_gettime_relative () - test->started > 200000) {
        if (test->stop == 1)
            assert (media_protocol_ops ()->pause (test->task) == 0);
        else
            assert (media_protocol_ops ()->cancel (test->task) == 0);
    }
}

static void test_completed (void* opaque)
{
    ((MediaTest*)opaque)->completed++;
}

static void test_paused (void* opaque)
{
    ((MediaTest*)opaque)->paused++;
}

static void test_failed (void* opaque, int code, const char* message)
{
    ((MediaTest*)opaque)->failed++;
    fprintf (stderr, "media error %d: %s\n", code, message);
}

static void test_filename (void* opaque, const char* name)
{
    MediaTest* test = opaque;
    snprintf (test->filename, sizeof (test->filename), "%s", name);
}

static void test_manifest (const char* manifest, bool accepted)
{
    ProtocolTask task = {0};
    int result = media_validate_manifest (&task, (const unsigned char*)manifest, strlen (manifest));
    assert ((result == 0) == accepted);
    assert ((task.networkError == 0) == accepted);
}

static void test_output_inode (void)
{
    char directory[] = "/tmp/downloader-media-inode-XXXXXX";
    char victim[DOWNLOAD_TASK_PATH_SIZE];
    char contents[32] = {0};
    ProtocolTask task = {0};
    struct stat status;
    assert (mkdtemp (directory) != NULL);
    snprintf (victim, sizeof (victim), "%s/victim", directory);
    snprintf (task.tempPath, sizeof (task.tempPath), "%s/output.part", directory);
    FILE* file = fopen (victim, "wb");
    assert (file != NULL);
    assert (fwrite ("keep these bytes", 1, 16, file) == 16);
    assert (fclose (file) == 0);

    assert (symlink (victim, task.tempPath) == 0);
    assert (media_output_open (&task) == AVERROR (ELOOP));
    assert (task.file == NULL);
    assert (unlink (task.tempPath) == 0);
    assert (link (victim, task.tempPath) == 0);
    assert (media_output_open (&task) == AVERROR (EMLINK));
    assert (task.file == NULL);
    assert (unlink (task.tempPath) == 0);
    file = fopen (victim, "rb");
    assert (file != NULL);
    assert (fread (contents, 1, sizeof (contents), file) == 16);
    assert (memcmp (contents, "keep these bytes", 16) == 0);
    assert (fclose (file) == 0);

    assert (mkfifo (task.tempPath, 0600) == 0);
    assert (media_output_open (&task) == AVERROR (EINVAL));
    assert (task.file == NULL);
    assert (unlink (task.tempPath) == 0);
    assert (mkdir (task.tempPath, 0700) == 0);
    assert (media_output_open (&task) < 0);
    assert (rmdir (task.tempPath) == 0);

    assert (media_output_open (&task) == 0);
    assert (fwrite ("old partial output", 1, 18, task.file) == 18);
    assert (fclose (task.file) == 0);
    task.file = NULL;
    assert (media_output_open (&task) == 0);
    assert (fstat (fileno (task.file), &status) == 0);
    assert (status.st_size == 0 && status.st_nlink == 1 && S_ISREG (status.st_mode));
    assert (fwrite ("new", 1, 3, task.file) == 3);
    assert (fclose (task.file) == 0);
    task.file = NULL;
    assert (unlink (task.tempPath) == 0);
    assert (unlink (victim) == 0);
    assert (rmdir (directory) == 0);
}

static void test_location_option (void)
{
    /* Model the remote AVIO option without any network dependency. */
    static const AVOption options[] = {
        {.name = "location", .offset = offsetof (AVIOContext, opaque),
         .type = AV_OPT_TYPE_STRING, .flags = AV_OPT_FLAG_READONLY},
        {.name = NULL}
    };
    static const AVClass remoteClass = {
        .class_name = "Test remote AVIO", .item_name = av_default_item_name,
        .version = LIBAVUTIL_VERSION_INT, .option = options
    };
    const char* effectiveUrl = "https://example.invalid/redirected/stream.m3u8";
    AVIOContext remote = {.av_class = &remoteClass, .opaque = (void*)effectiveUrl};
    MediaInput input = {.remote = &remote};
    AVIOContext wrapper = {.av_class = &gcMediaInputClass, .opaque = &input};
    uint8_t* location = NULL;
    assert (av_opt_get (&wrapper, "location", AV_OPT_SEARCH_CHILDREN, &location) == 0);
    assert (strcmp ((const char*)location, effectiveUrl) == 0);
    av_free (location);
    assert (media_input_child_next (&wrapper, &remote) == NULL);
}

int main (int argc, char** argv)
{
    test_output_inode ();
    test_location_option ();
    const ProtocolOps* ops = media_protocol_ops ();
    DownloadTask task = {0};
    MediaTest test = {0};
    ProtocolCallbacks callbacks = {
        .onFileName = test_filename, .onProgress = test_progress,
        .onCompleted = test_completed, .onPaused = test_paused, .onFailed = test_failed
    };
    assert (ops->canHandle ("hls+https://example.invalid/a.m3u8", "hls"));
    assert (ops->canHandle ("dash+http://example.invalid/a.mpd", "dash"));
    assert (!ops->canHandle ("file:///tmp/a.m3u8", "hls"));
    assert (!ops->canHandle ("http://example.invalid/a", "http"));
    test_manifest ("#EXTM3U\n#EXTINF:1,\na.ts\n#EXT-X-ENDLIST\n", true);
    test_manifest ("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1000\na.m3u8\n", true);
    test_manifest ("#EXTM3U\n#EXTINF:1,\na.ts\n", false);
    test_manifest ("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n#EXT-X-ENDLIST\n", true);
    test_manifest ("#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"key\"\n#EXT-X-ENDLIST\n", false);
    test_manifest ("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key\",KEYFORMAT=\"identity\"\n#EXT-X-ENDLIST\n", true);
    test_manifest ("#EXTM3U\n#EXT-X-KEY:URI=\"METHOD=NONE,spoof\",METHOD=SAMPLE-AES\n#EXT-X-ENDLIST\n", false);
    test_manifest ("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,KEYFORMAT=\"com.drm\"\n#EXT-X-ENDLIST\n", false);
    test_manifest ("<MPD type='static'/>", true);
    test_manifest ("<MPD/>", true);
    test_manifest ("<MPD type = 'dynamic'/>", false);
    test_manifest ("<MPD><Period><ContentProtection/></Period></MPD>", false);
    test_manifest ("<!DOCTYPE MPD [<!ENTITY x 'static'>]><MPD type='&x;'/>", false);
    assert (ops->open (NULL, &task, NULL, NULL) == -1 && errno == EINVAL);
    snprintf (task.resourceUrl, sizeof (task.resourceUrl), "hls+http://example.invalid/a.m3u8?token=1");
    snprintf (task.protocol, sizeof (task.protocol), "hls");
    snprintf (task.tempPath, sizeof (task.tempPath), "/directory-that-does-not-exist/media.part");
    assert (ops->open (&test.task, &task, &callbacks, &test) == 0);
    assert (test.task->file == NULL);
    media_filename (test.task);
    assert (strcmp (test.filename, "a.mkv") == 0);
    assert (ops->pause (test.task) == 0);
    assert (ops->run (test.task) == 1 && test.paused == 1 && test.completed == 0);
    assert (ops->resume (test.task) == 0);
    assert (ops->cancel (test.task) == 0);
    assert (ops->run (test.task) == -1 && errno == ECANCELED);
    ops->close (test.task);
    if (argc >= 3) {
        memset (&test, 0, sizeof (test));
        snprintf (task.resourceUrl, sizeof (task.resourceUrl), "%s", argv[1]);
        snprintf (task.protocol, sizeof (task.protocol), "%s",
                  strstr (argv[1], ".mpd") || strncmp (argv[1], "dash+", 5) == 0 ? "dash" : "hls");
        snprintf (task.tempPath, sizeof (task.tempPath), "%s", argv[2]);
        if (argc >= 4)
            test.stop = strcmp (argv[3], "pause") == 0 ? 1 : 2;
        assert (ops->open (&test.task, &task, &callbacks, &test) == 0);
        test.started = av_gettime_relative ();
        int result = ops->run (test.task);
        if (test.stop) {
            assert (av_gettime_relative () - test.started < 3000000);
            assert (test.completed == 0 && test.failed == 0);
            assert (result == (test.stop == 1 ? 1 : -1));
            assert (test.task->file == NULL);
        } else if (result == 0) {
            struct stat status;
            assert (stat (argv[2], &status) == 0);
            assert (test.completed == 1 && test.failed == 0);
            assert (test.total == (uint64_t)status.st_size && test.downloaded == test.total);
            assert (test.total > 0);
        }
        ops->close (test.task);
        if (!test.stop && result != 0)
            return 1;
    }
    puts ("media adapter tests passed");
    return 0;
}

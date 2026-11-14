#define _GNU_SOURCE

#include "credential-store.h"
#include "database.h"
#include "protocol.h"
#include "task-manager.h"

#include <clib/directory.h>
#include <clib/json.h>
#include <clib/memory.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t gExpandCount = 60;
static bool   gExpandError = false;

static bool expand_can_handle (const char* url, const char* protocol)
{
    return strcmp (protocol, "https") == 0 && strstr (url, "expand.test") != NULL;
}

static bool plain_can_handle (const char* url, const char* protocol)
{
    return strcmp (protocol, "https") == 0 && strstr (url, "plain.test") != NULL;
}

static int fixture_expand (const DownloadTask* request, ProtocolItem** items, size_t* count,
                           char* groupName, size_t groupNameCapacity, ProtocolError* error)
{
    assert (request->connectTimeout == 7);
    assert (request->transferTimeout == 11);
    if (gExpandError) {
        snprintf (error->code, sizeof (error->code), "baidu_password_invalid");
        snprintf (error->message, sizeof (error->message), "提取码错误");
        return -1;
    }
    *items = (ProtocolItem*)c_malloc0 (gExpandCount * sizeof (**items));
    assert (*items != NULL);
    *count = gExpandCount;
    snprintf (groupName, groupNameCapacity, "资料");
    for (size_t index = 0; index < gExpandCount; index++) {
        snprintf ((*items)[index].resourceUrl, sizeof ((*items)[index].resourceUrl),
                  "https://expand.test/file-%zu", index);
        snprintf ((*items)[index].fileName, sizeof ((*items)[index].fileName), "file-%zu.bin", index);
        snprintf ((*items)[index].targetSubdirectory, sizeof ((*items)[index].targetSubdirectory),
                  "资料/2024");
        snprintf ((*items)[index].extraJson, sizeof ((*items)[index].extraJson),
                  "{\"expandtest\":{\"index\":%zu}}", index);
        (*items)[index].totalBytes = 100 + index;
        snprintf ((*items)[index].checksumType, sizeof ((*items)[index].checksumType), "md5");
        snprintf ((*items)[index].checksumValue, sizeof ((*items)[index].checksumValue), "abc");
    }
    return 0;
}

static const ProtocolOps gcExpandOps = {
    .name = "expandtest",
    .canHandle = expand_can_handle,
    .expand = fixture_expand
};

static const ProtocolOps gcPlainOps = {
    .name = "plain",
    .canHandle = plain_can_handle
};

typedef struct
{
    char             root[512];
    char             downloads[512];
    Database*        database;
    CredentialStore* store;
    TaskManager*     manager;
} Fixture;

static void fixture_open (Fixture* fixture)
{
    char path[600];

    memset (fixture, 0, sizeof (*fixture));
    snprintf (fixture->root, sizeof (fixture->root), "/tmp/downloader-task-create-XXXXXX");
    assert (mkdtemp (fixture->root) != NULL);
    snprintf (fixture->downloads, sizeof (fixture->downloads), "%s/downloads", fixture->root);
    snprintf (path, sizeof (path), "%s/meta.db", fixture->root);
    assert (database_open (path, &fixture->database) == 0);
    fixture->store = credential_store_open (path, NULL);
    assert (fixture->store != NULL);
    {
        CredentialProfile profile;
        memset (&profile, 0, sizeof (profile));
        snprintf (profile.name, sizeof (profile.name), "acct");
        profile.kind = DOWNLOADER_CREDENTIAL_BASIC;
        snprintf (profile.username, sizeof (profile.username), "u");
        snprintf (profile.secret, sizeof (profile.secret), "s");
        assert (credential_store_save_credential (fixture->store, &profile) == 0);
    }
    {
        ProxyProfile profile;
        memset (&profile, 0, sizeof (profile));
        snprintf (profile.name, sizeof (profile.name), "px");
        snprintf (profile.url, sizeof (profile.url), "http://127.0.0.1:9");
        assert (credential_store_save_proxy (fixture->store, &profile) == 0);
    }
    fixture->manager = task_manager_new (fixture->database, NULL);
    assert (fixture->manager != NULL);
    assert (task_manager_set_download_directory (fixture->manager, fixture->downloads) == 0);
    assert (task_manager_set_transfer_options (fixture->manager, 2, 7, 11) == 0);
    assert (task_manager_set_request_policy (fixture->manager, fixture->store, "", "", "", "", "") == 0);
}

static void fixture_close (Fixture* fixture)
{
    task_manager_free (fixture->manager);
    credential_store_close (fixture->store);
    database_close (fixture->database);
    (void)c_directory_remove_recursive (fixture->root);
}

static size_t fixture_task_count (Fixture* fixture)
{
    DownloadTask* tasks = NULL;
    size_t        count = 0;
    size_t        total = 0;

    assert (database_list (fixture->database, 0, 100, &tasks, &count, &total) == 0);
    c_free (tasks);
    return total;
}

static void test_plain_single (void)
{
    Fixture          fixture;
    TaskCreateResult result;

    gExpandCount = 60;
    gExpandError = false;
    fixture_open (&fixture);
    memset (&result, 0, sizeof (result));
    assert (task_manager_create_expanded (fixture.manager, "https://plain.test/a.bin", "a.bin",
                                          fixture.downloads, NULL, &result) == 0);
    assert (result.taskCount == 1);
    assert (result.returnedCount == 1);
    assert (result.skippedCount == 0);
    assert (result.groupUuid[0] == '\0');
    assert (strcmp (result.tasks[0].fileName, "a.bin") == 0);
    c_free (result.tasks);
    fixture_close (&fixture);
}

static void test_expand_group_truncation (void)
{
    Fixture          fixture;
    TaskCreateResult result;
    DownloadTask*    groupTasks = NULL;
    size_t           groupCount = 0;

    gExpandCount = 60;
    gExpandError = false;
    fixture_open (&fixture);
    memset (&result, 0, sizeof (result));
    assert (task_manager_create_expanded (fixture.manager, "https://expand.test/root", NULL,
                                          fixture.downloads, NULL, &result) == 0);
    assert (result.taskCount == 60);
    assert (result.returnedCount == 50);
    assert (result.truncated);
    assert (result.groupUuid[0] != '\0');
    assert (strcmp (result.groupName, "资料") == 0);
    assert (database_list_group (fixture.database, result.groupUuid, &groupTasks, &groupCount) == 0);
    assert (groupCount == 60);
    assert (strstr (groupTasks[0].targetDirectory, "/资料/2024") != NULL);
    assert (groupTasks[0].totalBytes == 100);
    assert (strcmp (groupTasks[0].checksumValue, "abc") == 0);
    c_free (groupTasks);
    c_free (result.tasks);
    fixture_close (&fixture);
}

static void test_auth_profile_copied (void)
{
    Fixture          fixture;
    TaskCreateResult result;
    DownloadTask     task;
    CJsonDocument*   document;
    CJsonValue*      root;
    CStringView      auth;
    size_t           errorOffset;

    gExpandCount = 1;
    gExpandError = false;
    fixture_open (&fixture);
    memset (&result, 0, sizeof (result));
    assert (task_manager_create_expanded (fixture.manager, "https://expand.test/root", NULL,
                                          fixture.downloads, "{\"auth_profile\":\"acct\"}", &result) == 0);
    assert (result.taskCount == 1);
    assert (database_load (fixture.database, result.tasks[0].uuid, &task) == 0);
    document = c_json_document_new_from_data (c_string_view_from_cstr (task.extraJson), C_JSON_PARSE_NONE,
                                              &errorOffset);
    assert (document != NULL);
    root = c_json_document_root (document);
    assert (c_json_object_get (root, c_string_view_from_cstr ("expandtest")) != NULL);
    auth = c_json_value_get_string (c_json_object_get (root, c_string_view_from_cstr ("auth_profile")));
    assert (auth.data != NULL && auth.size == 4);
    assert (memcmp (auth.data, "acct", 4) == 0);
    c_json_document_free (document);
    c_free (result.tasks);
    fixture_close (&fixture);
}

static void test_proxy_profile_copied (void)
{
    Fixture          fixture;
    TaskCreateResult result;
    DownloadTask     task;
    CJsonDocument*   document;
    CJsonValue*      root;
    CStringView      value;
    size_t           errorOffset;

    gExpandCount = 1;
    gExpandError = false;
    fixture_open (&fixture);
    memset (&result, 0, sizeof (result));
    assert (task_manager_create_expanded (
                fixture.manager, "https://expand.test/root", NULL, fixture.downloads,
                "{\"auth_profile\":\"acct\",\"proxy_profile\":\"px\"}", &result) == 0);
    assert (result.taskCount == 1);
    assert (database_load (fixture.database, result.tasks[0].uuid, &task) == 0);
    document = c_json_document_new_from_data (c_string_view_from_cstr (task.extraJson), C_JSON_PARSE_NONE,
                                              &errorOffset);
    assert (document != NULL);
    root = c_json_document_root (document);
    value = c_json_value_get_string (c_json_object_get (root, c_string_view_from_cstr ("auth_profile")));
    assert (value.data != NULL && value.size == 4 && memcmp (value.data, "acct", 4) == 0);
    value = c_json_value_get_string (c_json_object_get (root, c_string_view_from_cstr ("proxy_profile")));
    assert (value.data != NULL && value.size == 2 && memcmp (value.data, "px", 2) == 0);
    c_json_document_free (document);
    c_free (result.tasks);
    fixture_close (&fixture);
}

static void test_skip_existing (void)
{
    Fixture          fixture;
    TaskCreateResult result;
    DownloadTask*    groupTasks = NULL;
    size_t           groupCount = 0;
    char             existing[600];

    gExpandCount = 60;
    gExpandError = false;
    fixture_open (&fixture);
    snprintf (existing, sizeof (existing), "%s/资料/2024", fixture.downloads);
    assert (c_directory_create_parents (existing) == 0);
    snprintf (existing, sizeof (existing), "%s/资料/2024/file-0.bin", fixture.downloads);
    {
        FILE* file = fopen (existing, "wb");
        assert (file != NULL && fputs ("existing", file) >= 0 && fclose (file) == 0);
    }
    memset (&result, 0, sizeof (result));
    assert (task_manager_create_expanded (fixture.manager, "https://expand.test/root", NULL,
                                          fixture.downloads, NULL, &result) == 0);
    assert (result.skippedCount == 1);
    assert (result.taskCount == 59);
    assert (database_list_group (fixture.database, result.groupUuid, &groupTasks, &groupCount) == 0);
    assert (groupCount == 59);
    for (size_t index = 0; index < groupCount; index++)
        assert (strcmp (groupTasks[index].fileName, "file-0.bin") != 0);
    c_free (groupTasks);
    c_free (result.tasks);
    fixture_close (&fixture);
}

static void test_all_skipped (void)
{
    Fixture          fixture;
    TaskCreateResult result;
    char             existing[600];

    gExpandCount = 1;
    gExpandError = false;
    fixture_open (&fixture);
    snprintf (existing, sizeof (existing), "%s/资料/2024", fixture.downloads);
    assert (c_directory_create_parents (existing) == 0);
    snprintf (existing, sizeof (existing), "%s/资料/2024/file-0.bin", fixture.downloads);
    {
        FILE* file = fopen (existing, "wb");
        assert (file != NULL && fputs ("existing", file) >= 0 && fclose (file) == 0);
    }
    memset (&result, 0, sizeof (result));
    assert (task_manager_create_expanded (fixture.manager, "https://expand.test/root", NULL,
                                          fixture.downloads, NULL, &result) == -1);
    assert (strcmp (result.errorCode, "target_files_exist") == 0);
    assert (fixture_task_count (&fixture) == 0);
    c_free (result.tasks);
    fixture_close (&fixture);
}

static void test_expand_error (void)
{
    Fixture          fixture;
    TaskCreateResult result;

    gExpandCount = 1;
    gExpandError = true;
    fixture_open (&fixture);
    memset (&result, 0, sizeof (result));
    assert (task_manager_create_expanded (fixture.manager, "https://expand.test/root", NULL,
                                          fixture.downloads, NULL, &result) == -1);
    assert (strcmp (result.errorCode, "baidu_password_invalid") == 0);
    assert (strcmp (result.errorMessage, "提取码错误") == 0);
    assert (fixture_task_count (&fixture) == 0);
    c_free (result.tasks);
    fixture_close (&fixture);
}

int main (void)
{
    assert (protocol_register (&gcExpandOps) == 0);
    assert (protocol_register (&gcPlainOps) == 0);
    test_plain_single ();
    test_expand_group_truncation ();
    test_auth_profile_copied ();
    test_proxy_profile_copied ();
    test_skip_existing ();
    test_all_skipped ();
    test_expand_error ();
    return 0;
}

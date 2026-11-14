#define _GNU_SOURCE

#include "database.h"
#include "protocol.h"
#include "task-manager.h"
#include "web-api.h"

#include <clib/memory.h>
#include <clib/json.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static WebRequest make_request (const char* method, const char* path, const char* query, const char* body)
{
    WebRequest request;

    memset (&request, 0, sizeof (request));
    request.method = method;
    request.path = path;
    request.query = query;
    request.body = body;
    request.bodySize = body == NULL ? 0 : strlen (body);
    return request;
}

static void release_response (WebResponse* response)
{
    if (response->bodyOwned)
        c_free ((void*)response->body);
}

static const char* gExpandErrorCode = NULL;

static bool expand_fixture_can_handle (const char* url, const char* protocol)
{
    return strcmp (protocol, "https") == 0 && strstr (url, "expand.test") != NULL;
}

static int expand_fixture (const DownloadTask* request, ProtocolItem** items, size_t* count,
                           char* groupName, size_t groupNameCapacity, ProtocolError* error)
{
    (void)request;
    if (gExpandErrorCode != NULL) {
        (void)snprintf (error->code, sizeof (error->code), "%s", gExpandErrorCode);
        (void)snprintf (error->message, sizeof (error->message), "fixture error");
        return -1;
    }
    *items = (ProtocolItem*)c_malloc0 (3U * sizeof (**items));
    assert (*items != NULL);
    *count = 3;
    (void)snprintf (groupName, groupNameCapacity, "资料");
    for (size_t index = 0; index < 3; index++) {
        (void)snprintf ((*items)[index].resourceUrl, sizeof ((*items)[index].resourceUrl),
                        "https://expand.test/file-%zu", index);
        (void)snprintf ((*items)[index].fileName, sizeof ((*items)[index].fileName), "file-%zu.bin", index);
        (void)snprintf ((*items)[index].targetSubdirectory, sizeof ((*items)[index].targetSubdirectory), "资料");
        (void)snprintf ((*items)[index].extraJson, sizeof ((*items)[index].extraJson), "{}");
        (*items)[index].totalBytes = 10;
    }
    return 0;
}

static const ProtocolOps gcExpandFixtureOps = {
    .name = "expandtest",
    .canHandle = expand_fixture_can_handle,
    .expand = expand_fixture
};

static void test_task_api (TaskManager* manager)
{
    WebRequest  request;
    WebResponse response;
    const char* body;

    request = make_request ("GET", "/api/tasks", "page=1&page_size=20", NULL);
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (response.status == 200);
    assert (strstr (response.body, "\"tasks\"") != NULL);
    release_response (&response);

    body = "{\"url\":\"https://example.test/new.bin\",\"file_name\":\"new.bin\",\"target_directory\":\"/tmp\",\"protocol\":\"https\"}";
    request = make_request ("POST", "/api/tasks", NULL, body);
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (response.status == 201);
    assert (strstr (response.body, "new.bin") != NULL);
    release_response (&response);

    body = "{\"url\":\"smb://example.test/share/%E9%9B%AA.bin\",\"target_directory\":\"/tmp\"}";
    request = make_request ("POST", "/api/tasks", NULL, body);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 201);
    size_t errorOffset;
    CJsonDocument* document = c_json_document_new_from_data (
        c_string_view_from_data (response.body, response.bodySize), C_JSON_PARSE_NONE, &errorOffset);
    assert (document != NULL);
    CJsonValue* task = c_json_object_get (c_json_document_root (document), c_string_view_from_cstr ("task"));
    CStringView name = c_json_value_get_string (c_json_object_get (task, c_string_view_from_cstr ("file_name")));
    assert (name.size == strlen ("雪.bin") && memcmp (name.data, "雪.bin", name.size) == 0);
    c_json_document_free (document);
    release_response (&response);
}

static void test_url_redaction (TaskManager* manager)
{
    WebRequest  request;
    WebResponse response;
    const char* body =
        "{\"url\":\"smb://alice:topsecret@example.test/share/file.bin\",\"target_directory\":\"/tmp\"}";

    request = make_request ("POST", "/api/tasks", NULL, body);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 201);
    assert (strstr (response.body, "topsecret") == NULL);
    assert (strstr (response.body, "alice") != NULL);
    release_response (&response);
    request = make_request ("GET", "/api/tasks", "page=1&page_size=20", NULL);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    assert (strstr (response.body, "topsecret") == NULL);
    assert (strstr (response.body, "alice") != NULL);
    release_response (&response);
}

static void test_invalid_requests (TaskManager* manager)
{
    WebRequest  request;
    WebResponse response;

    request = make_request ("POST", "/api/tasks", NULL, "not-json");
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (response.status == 400);
    release_response (&response);
    request = make_request ("GET", "/api/tasks", "page=0&page_size=20", NULL);
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (response.status == 400);
    release_response (&response);
    request = make_request ("GET", "/api/tasks", "page=1x&page_size=20", NULL);
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (response.status == 400);
    release_response (&response);
    request = make_request ("GET", "/api/tasks/missing", NULL, NULL);
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (response.status == 404);
    release_response (&response);
    request = make_request ("POST", "/api/tasks/missing/unknown", NULL, NULL);
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (response.status == 404);
    release_response (&response);
}

static void test_directory_picker (TaskManager* manager)
{
    WebRequest request = make_request ("POST", "/api/directories", NULL, "{\"path\":\"/\"}");
    WebResponse response;
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (response.status == 200);
    assert (strstr (response.body, "\"directories\":[") != NULL);
    assert (strstr (response.body, "\"path\":\"/\"") != NULL);
    release_response (&response);
    request.body = "{\"path\":42}";
    request.bodySize = strlen (request.body);
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (response.status == 400);
    release_response (&response);
    request.body = "{\"path\":\"/definitely-missing-downloader-directory\"}";
    request.bodySize = strlen (request.body);
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (response.status == 400);
    release_response (&response);
}

static void test_profile_api (TaskManager* manager)
{
    WebRequest  request;
    WebResponse response;

    request = make_request ("POST", "/api/auth-profiles", NULL,
                            "{\"name\":\"api-basic\",\"kind\":\"basic\",\"username\":\"alice\","
                            "\"secret\":\"top-secret\"}");
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (response.status == 201);
    release_response (&response);

    request = make_request ("GET", "/api/auth-profiles", NULL, NULL);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    assert (strstr (response.body, "api-basic") != NULL);
    assert (strstr (response.body, "top-secret") == NULL);
    release_response (&response);

    request = make_request ("POST", "/api/proxy-profiles", NULL,
                            "{\"name\":\"api-proxy\",\"url\":\"http://proxy.test:3128\","
                            "\"username\":\"pu\",\"secret\":\"proxy-secret\"}");
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 201);
    release_response (&response);
    request = make_request ("GET", "/api/proxy-profiles", NULL, NULL);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    assert (strstr (response.body, "api-proxy") != NULL);
    assert (strstr (response.body, "proxy-secret") == NULL);
    release_response (&response);

    request = make_request ("POST", "/api/sites", NULL,
                            "{\"domain\":\"example.test\",\"auth_profile\":\"api-basic\","
                            "\"proxy_profile\":\"api-proxy\"}");
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 201);
    release_response (&response);
    request = make_request ("GET", "/api/sites", NULL, NULL);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    assert (strstr (response.body, "example.test") != NULL);
    release_response (&response);

    request = make_request ("POST", "/api/auth-profiles", NULL, "{\"kind\":\"basic\"}");
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 400);
    release_response (&response);
    request = make_request ("POST", "/api/auth-profiles", NULL,
                            "{\"name\":\"bad\",\"kind\":\"unknown\",\"secret\":\"x\"}");
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 400);
    release_response (&response);

    request = make_request ("DELETE", "/api/auth-profiles/api-basic", NULL, NULL);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    release_response (&response);
    request = make_request ("GET", "/api/auth-profiles", NULL, NULL);
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (strstr (response.body, "api-basic") == NULL);
    release_response (&response);
    request = make_request ("DELETE", "/api/proxy-profiles/api-proxy", NULL, NULL);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    release_response (&response);
}

static void test_group_api (TaskManager* manager)
{
    WebRequest     request;
    WebResponse    response;
    char           groupId[DOWNLOAD_TASK_GROUP_UUID_SIZE];
    char           path[160];
    CJsonDocument* document;
    CJsonValue*    group;
    CStringView    identifier;
    size_t         errorOffset;

    request = make_request ("POST", "/api/tasks", NULL,
                            "{\"url\":\"https://expand.test/root\",\"target_directory\":\"/tmp\"}");
    assert (web_api_handle (manager, &request, &response) == 0);
    assert (response.status == 201);
    assert (strstr (response.body, "\"group\"") != NULL);
    assert (strstr (response.body, "\"task_count\":3") != NULL);
    assert (strstr (response.body, "\"tasks\"") != NULL);
    assert (strstr (response.body, "\"task\"") != NULL);
    document = c_json_document_new_from_data (c_string_view_from_data (response.body, response.bodySize),
                                              C_JSON_PARSE_NONE, &errorOffset);
    assert (document != NULL);
    group = c_json_object_get (c_json_document_root (document), c_string_view_from_cstr ("group"));
    identifier = c_json_value_get_string (c_json_object_get (group, c_string_view_from_cstr ("id")));
    assert (identifier.data != NULL && identifier.size < sizeof (groupId));
    memcpy (groupId, identifier.data, identifier.size);
    groupId[identifier.size] = '\0';
    c_json_document_free (document);
    release_response (&response);

    request = make_request ("GET", "/api/tasks", "page=1&page_size=20", NULL);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    assert (strstr (response.body, "\"group_id\"") != NULL);
    assert (strstr (response.body, "\"group_name\"") != NULL);
    assert (strstr (response.body, "\"groups\"") != NULL);
    assert (strstr (response.body, "\"completed\"") != NULL);
    release_response (&response);

    (void)snprintf (path, sizeof (path), "/api/groups/%s/pause", groupId);
    request = make_request ("POST", path, NULL, "{}");
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    assert (strstr (response.body, "\"affected\":3") != NULL);
    release_response (&response);
    request = make_request ("GET", "/api/tasks", "page=1&page_size=20", NULL);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    assert (strstr (response.body, "\"state\":\"paused\"") != NULL);
    release_response (&response);

    (void)snprintf (path, sizeof (path), "/api/groups/%s/resume", groupId);
    request = make_request ("POST", path, NULL, "{}");
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    release_response (&response);
    request = make_request ("GET", "/api/tasks", "page=1&page_size=20", NULL);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    assert (strstr (response.body, "\"state\":\"queued\"") != NULL);
    release_response (&response);

    (void)snprintf (path, sizeof (path), "/api/groups/%s", groupId);
    request = make_request ("DELETE", path, NULL, NULL);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    release_response (&response);
    request = make_request ("GET", "/api/tasks", "page=1&page_size=20", NULL);
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 200);
    assert (strstr (response.body, groupId) == NULL);
    release_response (&response);

    gExpandErrorCode = "baidu_password_invalid";
    request = make_request ("POST", "/api/tasks", NULL,
                            "{\"url\":\"https://expand.test/error\",\"target_directory\":\"/tmp\"}");
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 400);
    assert (strstr (response.body, "baidu_password_invalid") != NULL);
    release_response (&response);
    gExpandErrorCode = "baidu_share_too_large";
    request = make_request ("POST", "/api/tasks", NULL,
                            "{\"url\":\"https://expand.test/error\",\"target_directory\":\"/tmp\"}");
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 413);
    release_response (&response);
    gExpandErrorCode = "baidu_transfer_conflict";
    request = make_request ("POST", "/api/tasks", NULL,
                            "{\"url\":\"https://expand.test/error\",\"target_directory\":\"/tmp\"}");
    assert (web_api_handle (manager, &request, &response) == 0 && response.status == 409);
    release_response (&response);
    gExpandErrorCode = NULL;
}

int main (void)
{
    char             databasePath[128];
    Database*        database;
    CredentialStore* store;
    TaskManager*     manager;
    DownloadTask     task;

    (void)snprintf (databasePath, sizeof (databasePath), "/tmp/downloader-web-api-%ld.db", (long)getpid ());
    (void)unlink (databasePath);
    assert (database_open (databasePath, &database) == 0);
    store = credential_store_open (databasePath, NULL);
    assert (store != NULL);
    manager = task_manager_new (database, NULL);
    assert (manager != NULL);
    assert (task_manager_set_request_policy (manager, store, "", "", "", "", "") == 0);
    assert (protocol_register (&gcExpandFixtureOps) == 0);
    assert (task_manager_create (manager, "https://example.test/file.bin", "file.bin", "/tmp", "https", "{}", &task) == 0);
    test_task_api (manager);
    test_url_redaction (manager);
    test_invalid_requests (manager);
    test_directory_picker (manager);
    test_profile_api (manager);
    test_group_api (manager);
    task_manager_free (manager);
    credential_store_close (store);
    database_close (database);
    assert (unlink (databasePath) == 0);
    return 0;
}

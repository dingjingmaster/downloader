#define _GNU_SOURCE

#include "database.h"
#include "task-manager.h"
#include "web-api.h"

#include <clib/memory.h>

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

int main (void)
{
    char         databasePath[128];
    Database*    database;
    TaskManager* manager;
    DownloadTask task;

    (void)snprintf (databasePath, sizeof (databasePath), "/tmp/downloader-web-api-%ld.db", (long)getpid ());
    (void)unlink (databasePath);
    assert (database_open (databasePath, &database) == 0);
    manager = task_manager_new (database, NULL);
    assert (manager != NULL);
    assert (task_manager_create (manager, "https://example.test/file.bin", "file.bin", "/tmp", "https", "{}", &task) == 0);
    test_task_api (manager);
    test_invalid_requests (manager);
    test_directory_picker (manager);
    task_manager_free (manager);
    database_close (database);
    assert (unlink (databasePath) == 0);
    return 0;
}

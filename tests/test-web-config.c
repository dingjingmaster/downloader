#define _GNU_SOURCE

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_config (const char* path, const char* webValues)
{
    FILE* file = fopen (path, "w");

    assert (file != NULL);
    fprintf (file,
             "[daemon]\n"
             "ipc_socket = /tmp/downloader-web-test.sock\n"
             "\n"
             "[download]\n"
             "download_dir = /tmp/downloads\n"
             "%s\n",
             webValues == NULL ? "" : webValues);
    assert (fclose (file) == 0);
}

static void test_web_defaults (const char* root)
{
    char     path[512];
    char*    argv[4];
    Config*  config;

    snprintf (path, sizeof (path), "%s/default.ini", root);
    write_config (path, "");
    argv[0] = (char*)"downloader";
    argv[1] = (char*)"--config";
    argv[2] = path;
    argv[3] = NULL;

    config = config_load (3, argv);
    assert (config != NULL);
    assert (config_web_enabled (config));
    assert (strcmp (config_web_listen (config), "0.0.0.0") == 0);
    assert (config_web_port (config) == 8000);
    config_free (config);
}

static void test_web_can_be_disabled (const char* root)
{
    char     path[512];
    char*    argv[4];
    Config*  config;

    snprintf (path, sizeof (path), "%s/disabled.ini", root);
    write_config (path, "[web]\nenabled = false\nlisten = 127.0.0.1\nport = 18080\n");
    argv[0] = (char*)"downloader";
    argv[1] = (char*)"--config";
    argv[2] = path;
    argv[3] = NULL;

    config = config_load (3, argv);
    assert (config != NULL);
    assert (!config_web_enabled (config));
    assert (strcmp (config_web_listen (config), "127.0.0.1") == 0);
    assert (config_web_port (config) == 18080);
    config_free (config);
}

static void test_web_rejects_invalid_port (const char* root)
{
    char     path[512];
    char*    argv[4];
    Config*  config;

    snprintf (path, sizeof (path), "%s/invalid.ini", root);
    write_config (path, "[web]\nport = 65536\n");
    argv[0] = (char*)"downloader";
    argv[1] = (char*)"--config";
    argv[2] = path;
    argv[3] = NULL;

    errno = 0;
    config = config_load (3, argv);
    assert (config == NULL);
    assert (errno == EINVAL);
}

static void test_web_auth_requires_both_credentials (const char* root)
{
    char    path[512];
    char*   argv[4];
    Config* config;

    snprintf (path, sizeof (path), "%s/username-only.ini", root);
    write_config (path, "[web]\nusername = admin\n");
    argv[0] = (char*)"downloader";
    argv[1] = (char*)"--config";
    argv[2] = path;
    argv[3] = NULL;
    errno = 0;
    config = config_load (3, argv);
    assert (config == NULL);
    assert (errno == EINVAL);

    snprintf (path, sizeof (path), "%s/password-only.ini", root);
    write_config (path, "[web]\npassword = secret\n");
    errno = 0;
    config = config_load (3, argv);
    assert (config == NULL);
    assert (errno == EINVAL);
}

int main (void)
{
    char  templatePath[] = "/tmp/downloader-web-config-XXXXXX";
    char* root = mkdtemp (templatePath);

    assert (root != NULL);
    test_web_defaults (root);
    test_web_can_be_disabled (root);
    test_web_rejects_invalid_port (root);
    test_web_auth_requires_both_credentials (root);
    return 0;
}

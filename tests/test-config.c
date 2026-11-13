#define _GNU_SOURCE

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_config (const char* path, const char* ipcSocket)
{
    FILE* file = fopen (path, "w");

    assert (file != NULL);
    fprintf (file,
             "[daemon]\n"
             "ipc_socket = %s\n"
             "log_level = debug\n"
             "\n"
             "[download]\n"
             "download_dir = /tmp/downloads\n"
             "database = test.db\n"
             "max_concurrent_tasks = 7\n"
             "max_retries = 4\n",
             ipcSocket);
    assert (fclose (file) == 0);
}

static void make_directory (const char* path)
{
    assert (mkdir (path, 0700) == 0);
}

static void test_explicit_config_has_priority (const char* root)
{
    char         binPath[512];
    char         configDirectory[512];
    char         relativeConfig[512];
    char         explicitConfig[512];
    char*        argv[] = { (char*)"downloader", (char*)"--config", explicitConfig, NULL };
    Config*      config;

    snprintf (binPath, sizeof (binPath), "%s/bin", root);
    snprintf (configDirectory, sizeof (configDirectory), "%s/config", binPath);
    snprintf (relativeConfig, sizeof (relativeConfig), "%s/download.ini", configDirectory);
    snprintf (explicitConfig, sizeof (explicitConfig), "%s/explicit.ini", root);
    make_directory (binPath);
    make_directory (configDirectory);
    write_config (relativeConfig, "/tmp/relative.sock");
    write_config (explicitConfig, "/tmp/explicit.sock");
    argv[0] = binPath;

    config = config_load (3, argv);
    assert (config != NULL);
    assert (strcmp (config_path (config), explicitConfig) == 0);
    assert (strcmp (config_ipc_socket (config), "/tmp/explicit.sock") == 0);
    assert (config_max_concurrent_tasks (config) == 7);
    config_free (config);
}

static void test_program_config_is_used (const char* root)
{
    char    programDirectory[512];
    char    binPath[512];
    char    configDirectory[512];
    char    configPath[512];
    char*   argv[] = { binPath, NULL };
    Config* config;

    snprintf (programDirectory, sizeof (programDirectory), "%s/program", root);
    snprintf (binPath, sizeof (binPath), "%s/downloader", programDirectory);
    snprintf (configDirectory, sizeof (configDirectory), "%s/config", programDirectory);
    snprintf (configPath, sizeof (configPath), "%s/download.ini", configDirectory);
    make_directory (programDirectory);
    make_directory (configDirectory);
    write_config (configPath, "/tmp/program.sock");

    config = config_load (1, argv);
    assert (config != NULL);
    assert (strcmp (config_path (config), configPath) == 0);
    assert (strcmp (config_ipc_socket (config), "/tmp/program.sock") == 0);
    config_free (config);
}

static void test_missing_config_fails (const char* root)
{
    char*   argv[] = { (char*)"/path/that/does/not/exist/downloader", NULL };
    Config* config;

    (void)root;
    errno = 0;
    config = config_load (1, argv);
    assert (config == NULL);
    assert (errno == ENOENT);
}

int main (void)
{
    char  templatePath[] = "/tmp/downloader-config-XXXXXX";
    char* root = mkdtemp (templatePath);

    assert (root != NULL);
    test_explicit_config_has_priority (root);
    test_program_config_is_used (root);
    test_missing_config_fails (root);
    return 0;
}

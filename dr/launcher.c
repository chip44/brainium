#include <libgen.h>
#include <link.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dr_api.h"
#include "dr_config.h"
#include "dr_inject.h"

#include "ring.h"
#include "shm.h"

#define STATUS_MSG(x) ((x) ? "success" : "failure")

typedef struct __attribute__((aligned(16))) {
    char *path;
    size_t path_size;
} find_dr_lib_data;

static int
find_dr_lib_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;

    find_dr_lib_data *d = data;

    if (info->dlpi_name != NULL && strstr(info->dlpi_name, "libdynamorio.so") != NULL) {
        if (realpath(info->dlpi_name, d->path) == NULL) {
            snprintf(d->path, d->path_size, "%s", info->dlpi_name);
        }
        return 1;
    }

    return 0;
}

static bool
dr_override_root(const char *app_name, const int pid)
{
    char dr_lib_path[PATH_MAX] = {0};

    find_dr_lib_data data = {
        .path = dr_lib_path,
        .path_size = sizeof(dr_lib_path),
    };

    dl_iterate_phdr(find_dr_lib_cb, &data);
    fprintf(stderr, "dl_iterate_phdr: %s\n", dr_lib_path);
    if (dr_lib_path[0] == '\0') {
        return false;
    }

    dr_config_status_t status = dr_register_inject_paths(app_name, pid, false, DR_PLATFORM_DEFAULT,
                                                         dr_lib_path, dr_lib_path);
    fprintf(stderr, "dr_register_inject_paths: %s\n", dr_config_status_code_to_string(status));
    if (status != DR_SUCCESS) {
        return false;
    }

    return true;
}

int
main(int argc, char *argv[])
{
    if (argc < 4) {
        exit(EXIT_FAILURE);
    }

    int exitcode = EXIT_FAILURE;
    dr_config_status_t status;
    bool success;
    char *client_path = argv[1];
    char *vis_path = argv[2];
    char app_path[PATH_MAX];
    snprintf(app_path, sizeof(app_path), "%s", argv[3]);
    const char *app_name = basename(app_path);
    const char **app_argv = (const char **)&argv[3];
    process_id_t pid = -1;
    pid_t vis_pid = -1;

    // with $HOME set, DR always creates $HOME/.dynamorio...
    unsetenv("HOME");

    char config_dir[MAXIMUM_PATH];
    status = dr_get_config_dir(false, true, config_dir, sizeof(config_dir));
    fprintf(stderr, "dr_get_config_dir: %s\n", dr_config_status_code_to_string(status));

    void *inject_data = NULL;
    int errcode = dr_inject_process_create(app_path, app_argv, &inject_data);
    fprintf(stderr, "dr_inject_prepare_to_exec: %s\n", strerror(errcode));
    if (errcode != 0) {
        goto cleanup;
    }

    pid = dr_inject_get_process_id(inject_data);

    status = dr_register_process(app_name, pid, false, "", DR_MODE_CODE_MANIPULATION, false,
                                 DR_PLATFORM_DEFAULT, NULL);
    fprintf(stderr, "dr_register_process: %s\n", dr_config_status_code_to_string(status));
    if (status != DR_SUCCESS) {
        goto cleanup;
    }

    success = dr_override_root(app_name, pid);
    fprintf(stderr, "dr_override_root: %s\n", STATUS_MSG(success));
    if (!success) {
        goto cleanup;
    }

    dr_config_client_t client_info = {
        .struct_size = sizeof(client_info),
        .path = client_path,
        .options = "",
    };
    status = dr_register_client_ex(app_name, pid, false, DR_PLATFORM_DEFAULT, &client_info);
    fprintf(stderr, "dr_register_client_ex: %s\n", dr_config_status_code_to_string(status));
    if (status != DR_SUCCESS) {
        goto cleanup;
    }

    success = dr_inject_process_inject(inject_data, false, NULL);
    fprintf(stderr, "dr_inject_process_inject: %s\n", STATUS_MSG(success));
    if (!success) {
        goto cleanup;
    }

    ring_buf_t *ring_shm = shm_init(true);
    success = ring_shm != NULL;
    fprintf(stderr, "shm_init: %s\n", STATUS_MSG(success));
    if (!success) {
        goto cleanup;
    }

    vis_pid = fork();
    if (vis_pid == -1) {
        perror("fork");
        goto cleanup;
    }
    if (vis_pid == 0) {
        // close all fds except stdin/stdout/stderr to avoid holding DR's internal pipes open
        close_range(3, ~0U, 0);
        fprintf(stderr, "execlp: %s\n", vis_path);
        execlp(vis_path, vis_path, NULL);
        // only reached if execlp failed
        perror("execlp");
        exit(EXIT_FAILURE);
    }

    success = dr_inject_process_run(inject_data);
    fprintf(stderr, "dr_inject_process_run: %s\n", STATUS_MSG(success));
    if (!success) {
        goto cleanup;
    }

    dr_inject_wait_for_child(inject_data, 0);

cleanup:
    exitcode = dr_inject_process_exit(inject_data, true);
    fprintf(stderr, "dr_inject_process_exit: %d\n", exitcode);

    if (pid != -1) {
        status = dr_unregister_process(app_name, pid, false, DR_PLATFORM_DEFAULT);
        fprintf(stderr, "dr_unregister_process: %s\n", dr_config_status_code_to_string(status));
    }

    if (vis_pid != -1) {
        int vis_status;
        if (waitpid(vis_pid, &vis_status, 0) == -1) {
            perror("waitpid");
        }
        fprintf(stderr, "waitpid: %d\n", vis_status);
    }

    shm_exit(&ring_shm);

    return exitcode;
}

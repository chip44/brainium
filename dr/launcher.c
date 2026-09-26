#include <libgen.h>
#include <link.h>
#include <linux/limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dr_api.h"
#include "dr_config.h"
#include "dr_defines.h"
#include "dr_inject.h"

#include "ring.h"
#include "shm.h"

#define STATUS_MSG(x) ((x) ? "success" : "failure")

typedef struct __attribute__((aligned(16))) {
    char *path;
    size_t path_size;
} find_dr_lib_data;

static volatile sig_atomic_t g_shutting_down = 0;
static process_id_t g_app_pid = -1;
static pid_t g_vis_pid = -1;

/*
 * send SIGTERM to children (app and vis) when the launcher gets SIGTERM/SIGINT
 */
static void
handle_shutdown_signal(int sig)
{
    fprintf(stderr, "got signal %d, shutting down...\n", sig);

    g_shutting_down = 1;

    if (g_app_pid != -1) {
        kill(g_app_pid, SIGTERM);
    }
    if (g_vis_pid != -1) {
        kill(g_vis_pid, SIGTERM);
    }
}

/*
 * signal handlers for graceful shutdown
 */
static void
install_signal_handlers(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = handle_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/*
 * wait for `pid` to exit, or gracefully terminate if shutting down
 */
static void
waitpid_graceful(const pid_t pid)
{
    int status;

    if (g_shutting_down) {
        // give it some time to exit after SIGTERM, then SIGKILL if it's still running
        int waited_ms = 0;
        pid_t r;
        while ((r = waitpid(pid, &status, WNOHANG)) == 0 && waited_ms < 2000) {
            usleep(100000);
            waited_ms += 100;
        }
        if (r == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        } else if (r == -1) {
            perror("waitpid");
        }
    } else {
        // still running, just wait until user closes it
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
        }
    }

    fprintf(stderr, "waitpid: %d\n", status);
}

/*
 * callback for `dl_iterate_phdr` to find libdynamorio.so
 */
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

/*
 * inject the linked libdynamorio.so path to avoid specifying DR root
 */
static bool
dr_override_root(const char *app_name, const process_id_t app_pid)
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

    dr_config_status_t status = dr_register_inject_paths(
        app_name, app_pid, false, DR_PLATFORM_DEFAULT, dr_lib_path, dr_lib_path);
    fprintf(stderr, "dr_register_inject_paths: %s\n", dr_config_status_code_to_string(status));
    if (status != DR_SUCCESS) {
        return false;
    }

    return true;
}

/*
 * register the process with DR and inject the client into it (without running it)
 */
static process_id_t
dr_setup(char app_path[], const char *app_argv[], char client_path[], void **inject_data)
{
    const char *app_name = basename(app_path);

    dr_config_status_t status;
    bool success;

    char config_dir[MAXIMUM_PATH];
    status = dr_get_config_dir(false, true, config_dir, sizeof(config_dir));
    fprintf(stderr, "dr_get_config_dir: %s\n", dr_config_status_code_to_string(status));

    int errcode = dr_inject_process_create(app_path, app_argv, inject_data);
    fprintf(stderr, "dr_inject_prepare_to_exec: %s\n", strerror(errcode));
    if (errcode != 0) {
        return -1;
    }

    process_id_t app_pid = dr_inject_get_process_id(*inject_data);

    status = dr_register_process(app_name, app_pid, false, "", DR_MODE_CODE_MANIPULATION, false,
                                 DR_PLATFORM_DEFAULT, NULL);
    fprintf(stderr, "dr_register_process: %s\n", dr_config_status_code_to_string(status));
    if (status != DR_SUCCESS) {
        return -1;
    }

    success = dr_override_root(app_name, app_pid);
    fprintf(stderr, "dr_override_root: %s\n", STATUS_MSG(success));
    if (!success) {
        return -1;
    }

    dr_config_client_t client_info = {
        .struct_size = sizeof(client_info),
        .path = client_path,
        .options = "",
    };
    status = dr_register_client_ex(app_name, app_pid, false, DR_PLATFORM_DEFAULT, &client_info);
    fprintf(stderr, "dr_register_client_ex: %s\n", dr_config_status_code_to_string(status));
    if (status != DR_SUCCESS) {
        return -1;
    }

    success = dr_inject_process_inject(*inject_data, false, NULL);
    fprintf(stderr, "dr_inject_process_inject: %s\n", STATUS_MSG(success));
    if (!success) {
        return -1;
    }

    return app_pid;
}

/*
 * run the injected process and wait for it to exit
 */
static bool
dr_run(void **inject_data)
{
    bool success = dr_inject_process_run(*inject_data);
    fprintf(stderr, "dr_inject_process_run: %s\n", STATUS_MSG(success));
    if (!success) {
        return false;
    }

    return dr_inject_wait_for_child(*inject_data, 0);
}

/*
 * stop and unregister the injected process
 */
static int
dr_cleanup(char app_path[], const process_id_t app_pid, void **inject_data)
{
    const char *app_name = basename(app_path);

    int exitcode = dr_inject_process_exit(*inject_data, true);
    fprintf(stderr, "dr_inject_process_exit: %d\n", exitcode);

    if (app_pid != -1) {
        dr_config_status_t status =
            dr_unregister_process(app_name, app_pid, false, DR_PLATFORM_DEFAULT);
        fprintf(stderr, "dr_unregister_process: %s\n", dr_config_status_code_to_string(status));
    }

    return exitcode;
}

int
main(int argc, char *argv[])
{
    if (argc < 4) {
        exit(EXIT_FAILURE);
    }

    int exitcode = EXIT_FAILURE;
    bool success;

    char *client_path = argv[1];
    char *vis_path = argv[2];
    char app_path[PATH_MAX];
    snprintf(app_path, sizeof(app_path), "%s", argv[3]);
    const char **app_argv = (const char **)&argv[3];

    process_id_t app_pid = -1;
    pid_t vis_pid = -1;
    void *inject_data = NULL;

    install_signal_handlers();

    // with $HOME set, DR always creates $HOME/.dynamorio...
    unsetenv("HOME");

    app_pid = dr_setup(app_path, app_argv, client_path, &inject_data);
    if (app_pid == -1) {
        goto cleanup;
    }
    g_app_pid = app_pid;

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
    g_vis_pid = vis_pid;

    success = dr_run(&inject_data);
    if (!success) {
        goto cleanup;
    }

cleanup:
    exitcode = dr_cleanup(app_path, app_pid, &inject_data);

    if (vis_pid != -1) {
        waitpid_graceful(vis_pid);
    }

    shm_exit(&ring_shm);

    return exitcode;
}

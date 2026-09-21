#include <libgen.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dr_api.h"
#include "dr_config.h"
#include "dr_inject.h"

// TODO: remove
#define DR_ROOT "/opt/dynamorio-git"

static void
usage(const char *prog)
{
    fprintf(stderr, "usage: %s <client.so> <app> [app args...]\n", prog);
    exit(EXIT_SUCCESS);
}

int
main(int argc, char *argv[])
{
    if (argc < 3) {
        usage(argv[0]);
    }

    char *client_path = argv[1];
    char app_path[PATH_MAX];
    snprintf(app_path, sizeof(app_path), "%s", argv[2]);
    const char *app_name = basename(app_path);
    const char **app_argv = (const char **)&argv[3];

    char config_dir[MAXIMUM_PATH];
    dr_get_config_dir(false, true, config_dir, sizeof(config_dir));
    fprintf(stderr, "INFO: using DR config dir '%s'\n", config_dir);

    void *inject_data;
    int errcode = dr_inject_prepare_to_exec(app_path, app_argv, &inject_data);
    if (errcode != 0) {
        fprintf(stderr, "ERROR: couldn't prepare '%s' for injection (errcode=%d)\n", app_name,
                errcode);
        return EXIT_FAILURE;
    }

    process_id_t pid = dr_inject_get_process_id(inject_data);

    dr_config_status_t status = dr_register_process(
        app_name, pid, false, DR_ROOT, DR_MODE_CODE_MANIPULATION, false, DR_PLATFORM_DEFAULT, NULL);
    if (status != DR_SUCCESS) {
        fprintf(stderr, "ERROR: failed to register process '%s' (status=%d)\n", app_name, status);
        goto error;
    }

    dr_config_client_t client_info = {0};
    client_info.struct_size = sizeof(client_info);
    client_info.id = 0;
    client_info.priority = 0;
    client_info.path = client_path;
    client_info.options = "";
    client_info.is_alt_bitwidth = false;
    if (dr_register_client_ex(app_name, pid, false, DR_PLATFORM_DEFAULT, &client_info) !=
        DR_SUCCESS) {
        fprintf(stderr, "ERROR: failed to register client '%s'\n", client_path);
        goto error;
    }

    if (!dr_inject_process_inject(inject_data, false, NULL)) {
        fprintf(stderr, "ERROR: injection failed for '%s'\n", app_name);
        goto error;
    }

    if (!dr_inject_process_run(inject_data)) {
        fprintf(stderr, "ERROR: couldn't start '%s'\n", app_name);
        goto error;
    }

    dr_inject_wait_for_child(inject_data, 0);
    int exitcode = dr_inject_process_exit(inject_data, false);

    dr_unregister_process(app_name, pid, false, DR_PLATFORM_DEFAULT);
    fprintf(stderr, "INFO: exit status: '%d'\n", exitcode);
    return exitcode;

error:
    dr_inject_process_exit(inject_data, true);
    dr_unregister_process(app_name, pid, false, DR_PLATFORM_DEFAULT);
    return EXIT_FAILURE;
}

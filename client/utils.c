#define _POSIX_C_SOURCE 200809L
#include <stdio.h>

#define LINUX
#define X86_64

// #include "drx.h"

#include "/opt/dynamorio/include/dr_api.h"

#include "/opt/dynamorio/ext/include/drx.h"

#include "utils.h"

#ifdef WINDOWS
#include <io.h>
#endif

file_t
log_file_open(client_id_t id, void *drcontext, const char *path, const char *name, uint flags)
{
    file_t log = 0;
    char log_dir[MAXIMUM_PATH];
    char buf[MAXIMUM_PATH];
    size_t len = 0;
    char *dirsep = NULL;

    DR_ASSERT(name != NULL);
    len = dr_snprintf(log_dir, BUFFER_SIZE_ELEMENTS(log_dir), "%s",
                      path == NULL ? dr_get_client_path(id) : path);
    DR_ASSERT(len > 0);
    NULL_TERMINATE_BUFFER(log_dir);
    dirsep = log_dir + len - 1;
    if (path == NULL /* removing client lib */ ||
        /* path does not have a trailing / and is too large to add it */
        (*dirsep != '/' IF_WINDOWS(&&*dirsep != '\\') &&
         len == BUFFER_SIZE_ELEMENTS(log_dir) - 1)) {
        for (dirsep = log_dir + len; *dirsep != '/' IF_WINDOWS(&&*dirsep != '\\'); dirsep--) {
            DR_ASSERT(dirsep > log_dir);
        }
    }
    /* remove trailing / if necessary */
    if (*dirsep == '/' IF_WINDOWS(|| *dirsep == '\\')) {
        *dirsep = 0;
    } else if (sizeof(log_dir) > (dirsep + 1 - log_dir) / sizeof(log_dir[0])) {
        *(dirsep + 1) = 0;
    }
    NULL_TERMINATE_BUFFER(log_dir);
    /* we do not need call drx_init before using drx_open_unique_appid_file */
    log = drx_open_unique_appid_file(log_dir, dr_get_process_id(), name, "log", flags, buf,
                                     BUFFER_SIZE_ELEMENTS(buf));
    if (log != INVALID_FILE) {
        char msg[MAXIMUM_PATH];
        len = dr_snprintf(msg, BUFFER_SIZE_ELEMENTS(msg), "Data file %s created", buf);
        DR_ASSERT(len > 0);
        NULL_TERMINATE_BUFFER(msg);
        dr_log(drcontext, DR_LOG_ALL, 1, "%s", msg);
    }
    return log;
}

void
log_file_close(file_t log)
{
    dr_close_file(log);
}

FILE *
log_stream_from_file(file_t f)
{
#ifdef WINDOWS
    int fd = _open_osfhandle((intptr_t)f, 0);
    if (fd == -1)
        return NULL;
    return _fdopen(fd, "w");
#else
    return fdopen(f, "w");
#endif
}

void
log_stream_close(FILE *f)
{
    fclose(f); /* closes underlying fd too for all platforms */
}

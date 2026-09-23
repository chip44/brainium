#include <fcntl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dr_api.h"

#include "ring.h"
#include "shm.h"

ring_buf_t *
shm_init(bool init)
{
    // create and open shm descriptor
    int flags = init ? (O_RDWR | O_CREAT | O_EXCL) : O_RDWR;
    int fd = shm_open(SHM_NAME, flags, 0666);
    if (fd == -1) {
        DR_ASSERT(false);
        return NULL;
    }

    if (init) {
        // set size of shm
        if (ftruncate(fd, sizeof(ring_buf_t)) == -1) {
            DR_ASSERT(false);
            close(fd);
            return NULL;
        }
    }

    // map shm descriptor to memory
    ring_buf_t *ring = mmap(NULL, sizeof(ring_buf_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    close(fd);

    if (ring == MAP_FAILED) {
        DR_ASSERT(false);
        return NULL;
    }

    if (init) {
        ring_init(ring);
    }

    return ring;
}

void
shm_exit(ring_buf_t **ring)
{
    if (*ring) {
        munmap(*ring, sizeof(ring_buf_t));
        *ring = NULL;
    }

    shm_unlink(SHM_NAME);
}

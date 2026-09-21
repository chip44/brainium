#include <fcntl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dr_api.h"

#include "shm.h"

shm_ring_t *
shm_init(void)
{
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    DR_ASSERT(fd >= 0);

    if (ftruncate(fd, sizeof(shm_ring_t)) != 0) {
        DR_ASSERT(false);
        close(fd);
        return NULL;
    }

    shm_ring_t *ring = mmap(NULL, sizeof(shm_ring_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    close(fd);

    DR_ASSERT(ring != MAP_FAILED);

    memset(ring, 0, sizeof(*ring));

    return ring;
}

void
shm_exit(shm_ring_t **ring)
{
    if (*ring) {
        uint64_t w = atomic_load_explicit(&(*ring)->write_idx, memory_order_relaxed);
        uint64_t r = atomic_load_explicit(&(*ring)->read_idx, memory_order_acquire);
        printf("writes=%lu reads=%lu\n", w, r);
        munmap(*ring, sizeof(shm_ring_t));
        *ring = NULL;
    }
    shm_unlink(SHM_NAME);
}

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dr_api.h"

/* each mem_ref_t includes the type of reference (read or write),
 * the address referenced, and the size of the reference.
 */
typedef struct __attribute__((aligned(32))) {
    bool write;
    void *addr;
    size_t size;
    app_pc pc;
} mem_ref_t;

_Static_assert(sizeof(mem_ref_t) == 32, "mem_ref_t must be 32 bytes");
_Static_assert(_Alignof(mem_ref_t) == 32, "mem_ref_t must be 32-byte aligned");

#define SHM_NAME "/brainium_shm"
#define SHM_RING_SIZE (1 << 20)

/* shared memory ring buffer */
typedef struct __attribute__((aligned(128))) {
    _Atomic uint64_t write_idx;
    _Atomic uint64_t read_idx;
    _Atomic uint64_t dropped;
    uint64_t _pad;
    mem_ref_t entries[SHM_RING_SIZE];
} shm_ring_t;

_Static_assert(sizeof(shm_ring_t) % 128 == 0, "shm_ring_t size should be cache-line aligned");
_Static_assert(offsetof(shm_ring_t, entries) % 32 == 0,
               "shm_ring_t.entries must be 32-byte aligned");

shm_ring_t *shm_init(void);
void shm_exit(shm_ring_t **ring);

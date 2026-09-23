#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * number of slots in the ring, must be a power of two
 */
#define RING_SIZE (1 << 20)

/*
 * used instead of modulo to turn position into slot index
 */
#define RING_MASK (RING_SIZE - 1)

/*
 * a memory reference.
 */
typedef struct __attribute__((aligned(32))) {
    bool write;  /* type of reference (read or write) */
    void *addr;  /* address referenced */
    size_t size; /* size of reference */
} mem_ref_t;

_Static_assert(sizeof(mem_ref_t) == 32, "mem_ref_t must be 32 bytes");
_Static_assert(_Alignof(mem_ref_t) == 32, "mem_ref_t must be 32-byte aligned");

/*
 * one slot in the ring.
 */
typedef struct __attribute__((aligned(64))) {
    _Atomic uint64_t seq; /* handshake between producer and consumer for this slot:
                            == pos -> free and can be claimed
                            == pos+1 -> written and ready to read
                            == pos+capacity -> read and free for next lap */
    mem_ref_t data;       /* published entry, valid to read only once seq says so */
} ring_cell_t;

_Static_assert(sizeof(ring_cell_t) == 64, "ring_cell_t must be 64 bytes");
_Static_assert(_Alignof(ring_cell_t) == 64, "ring_cell_t must be 64-byte aligned");

typedef struct __attribute__((aligned(128))) {
    ring_cell_t buffer[RING_SIZE]; /* array of slots */
    _Atomic uint64_t enqueue_pos;  /* write cursor (shared between producers) */
    char _pad[64];                 /* keep dequeue_pos off enqueue_pos's cache line */
    uint64_t dequeue_pos;          /* read cursor, consumer only */
} ring_buf_t;

_Static_assert(sizeof(ring_buf_t) % 128 == 0, "shm_ring_t size should be cache-line aligned");
_Static_assert(offsetof(ring_buf_t, buffer) % 32 == 0, "shm_ring_t.buffer must be 32-byte aligned");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "atomic uint64 implementation should be lock-free");

bool ring_init(ring_buf_t *ring);

bool ring_try_push(ring_buf_t *ring, const mem_ref_t *entry);
void ring_push(ring_buf_t *ring, const mem_ref_t *entry);
void ring_push_bulk(ring_buf_t *ring, const mem_ref_t *entries, size_t count);

bool ring_try_pop(ring_buf_t *ring, mem_ref_t *out);

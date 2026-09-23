#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "ring.h"

bool
ring_init(ring_buf_t *ring)
{
    // seed each slot's sequence with own index to make every slot initially free to claim
    for (size_t i = 0; i < RING_SIZE; i++) {
        atomic_init(&ring->buffer[i].seq, (uint64_t)i);
    }

    atomic_init(&ring->enqueue_pos, (uint64_t)0);
    ring->dequeue_pos = 0;

    return true;
}

bool
ring_try_push(ring_buf_t *ring, const mem_ref_t *entry)
{
    ring_cell_t *cell = NULL;
    uint64_t pos = atomic_load_explicit(&ring->enqueue_pos, memory_order_relaxed);

    while (true) {
        cell = &ring->buffer[pos & RING_MASK];
        uint64_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        int64_t diff = (int64_t)seq - (int64_t)pos;

        if (diff == 0) {
            /* slot is free so try to claim it.
             * compare_exchange failing just means another producer beat us to it. pos gets
             * refreshed to the current value automatically and we loop again. */
            if (atomic_compare_exchange_weak_explicit(&ring->enqueue_pos, &pos, pos + 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                break;
            }
        } else if (diff < 0) {
            // ring is full (consumer hasn't freed this slot yet from the previous iteration)
            return false;
        } else {
            // enqueue_pos was moved past what we loaded, catch up and retry
            pos = atomic_load_explicit(&ring->enqueue_pos, memory_order_relaxed);
        }
    }

    cell->data = *entry;

    atomic_store_explicit(&cell->seq, pos + 1, memory_order_release);

    return true;
}

void
ring_push(ring_buf_t *ring, const mem_ref_t *entry)
{
    while (!ring_try_push(ring, entry)) {
        // spin until there's room to push
    }
}

void
ring_push_bulk(ring_buf_t *ring, const mem_ref_t *entries, size_t count)
{
    if (count == 0) {
        return;
    }

    uint64_t start = atomic_fetch_add_explicit(&ring->enqueue_pos, count, memory_order_relaxed);

    for (size_t i = 0; i < count; i++) {
        uint64_t pos = start + i;
        ring_cell_t *cell = &ring->buffer[pos & RING_MASK];

        // if the consumer is keeping up, this slot is already free and the loop doesn't spin at all
        uint64_t seq;
        do {
            seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        } while (seq != pos);

        cell->data = entries[i];
        atomic_store_explicit(&cell->seq, pos + 1, memory_order_release);
    }
}

bool
ring_try_pop(ring_buf_t *ring, mem_ref_t *out)
{
    uint64_t pos = ring->dequeue_pos;
    ring_cell_t *cell = &ring->buffer[pos & RING_MASK];

    uint64_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
    int64_t diff = (int64_t)seq - (int64_t)(pos + 1);

    if (diff < 0) {
        // no producer has published this slot yet
        return false;
    }

    *out = cell->data;

    // free the slot for the next producer */
    atomic_store_explicit(&cell->seq, pos + RING_MASK + 1, memory_order_release);
    ring->dequeue_pos = pos + 1;

    return true;
}

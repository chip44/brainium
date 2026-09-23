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

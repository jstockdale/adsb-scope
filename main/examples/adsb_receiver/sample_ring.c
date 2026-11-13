// sample_ring.c – see sample_ring.h.

#include "sample_ring.h"

#include <string.h>

// 'head' and 'tail' are each written by one side only. The writer
// publishes with release order after the slot contents are in place, and
// the other side reads with acquire order, so a slot is never seen before
// its contents on a weakly ordered CPU.

size_t sample_ring_mem_size(uint32_t slots, uint32_t slot_bytes)
{
    size_t recs = (size_t)slots * sizeof(sample_ring_rec_t);
    return recs + (size_t)slots * slot_bytes;
}

bool sample_ring_init(sample_ring_t *r, void *mem, uint32_t slots, uint32_t slot_bytes)
{
    if (!r || !mem || slots < 2 || slot_bytes == 0 || (slot_bytes & 1))
        return false;
    memset(r, 0, sizeof(*r));
    r->rec = (sample_ring_rec_t *)mem; // first: it has the stricter alignment
    r->data = (uint8_t *)mem + (size_t)slots * sizeof(sample_ring_rec_t);
    r->slots = slots;
    r->slot_bytes = slot_bytes;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return true;
}

void sample_ring_reset(sample_ring_t *r)
{
    atomic_store_explicit(&r->tail, 0, memory_order_release);
    atomic_store_explicit(&r->head, 0, memory_order_release);
    r->pending_lost = 0;
    r->pending_gap = false;
}

uint32_t sample_ring_fill(const sample_ring_t *r)
{
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    return (h + r->slots - t) % r->slots;
}

void sample_ring_note_loss(sample_ring_t *r, uint32_t samples)
{
    r->pending_lost += samples;
    r->pending_gap = true;
}

bool sample_ring_put(sample_ring_t *r, const uint8_t *src, uint32_t len, int64_t t_us)
{
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    uint32_t next = (h + 1) % r->slots;
    uint32_t used;
    bool odd = false;
    sample_ring_rec_t *rec;

    if (len > r->slot_bytes)
        len = r->slot_bytes;
    if (len & 1)
    {
        // Half an IQ pair: the next transfer would be read with I and Q
        // swapped. Drop the stray byte and break the stream after it.
        len--;
        odd = true;
    }
    if (next == t)
    {
        // Full. The decoder is behind: lose this transfer whole, and say so.
        r->dropped++;
        r->dropped_bytes += len;
        r->pending_lost += len / 2;
        r->pending_gap = true;
        if (odd)
            r->pending_lost++;
        return false;
    }
    if (len == 0)
    {
        if (odd)
            sample_ring_note_loss(r, 1);
        return true; // nothing to store
    }

    rec = &r->rec[h];
    memcpy(r->data + (size_t)h * r->slot_bytes, src, len);
    rec->len = len;
    rec->t_us = t_us;
    rec->gap = r->pending_gap ? 1 : 0;
    rec->lost = r->pending_lost > UINT32_MAX ? UINT32_MAX : (uint32_t)r->pending_lost;
    if (r->pending_gap)
        r->gaps++;
    r->pending_gap = false;
    r->pending_lost = 0;
    if (odd)
        sample_ring_note_loss(r, 1);

    atomic_store_explicit(&r->head, next, memory_order_release);
    r->stored++;
    used = (next + r->slots - t) % r->slots;
    if (used > r->max_fill)
        r->max_fill = used;
    return true;
}

uint32_t sample_ring_peek(sample_ring_t *r, uint32_t max_bytes,
                          const uint8_t **data, uint32_t *bytes,
                          bool *gap, uint32_t *lost, int64_t *t_us)
{
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    uint32_t i = t, n = 0, total = 0;

    if (t == h)
        return 0;

    *data = r->data + (size_t)t * r->slot_bytes;
    *gap = r->rec[t].gap != 0;
    *lost = r->rec[t].lost;
    *t_us = r->rec[t].t_us;

    for (;;)
    {
        total += r->rec[i].len;
        n++;
        // The run can go on only if this slot is full (the next one then
        // starts where it ends in memory) and is not the last in the buffer.
        if (r->rec[i].len != r->slot_bytes || i + 1 == r->slots)
            break;
        i++;
        if (i == h || r->rec[i].gap || total + r->rec[i].len > max_bytes)
            break;
    }
    *bytes = total;
    return n;
}

void sample_ring_release(sample_ring_t *r, uint32_t n)
{
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    atomic_store_explicit(&r->tail, (t + n) % r->slots, memory_order_release);
}

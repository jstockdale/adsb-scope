// harness/ring/test_ring.c – host tests for the sample ring
// (main/examples/adsb_receiver/sample_ring.c).
//
// 1. Directed tests of every rule in sample_ring.h.
// 2. A long random single-threaded run checked against a simple model.
// 3. A two-thread run (one producer, one consumer, as on the device) in
//    which the producer outruns the consumer on purpose: every byte that
//    comes out must be the right byte, and every byte that does not come
//    out must be accounted for by a reported gap of exactly that size.
//
// Exit status is the number of failed checks.
#include "sample_ring.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            if (fails < 20)                                         \
                fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);\
            fails++;                                                \
        }                                                           \
    } while (0)

static uint64_t rng_state = 0x243F6A8885A308D3ull;
static uint64_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1Dull;
}

// The byte at a given position of the endless test stream.
static inline uint8_t stream_byte(uint64_t pos)
{
    uint64_t x = pos * 0x9E3779B97F4A7C15ull;
    return (uint8_t)(x >> 56) ^ (uint8_t)pos;
}

static void fill(uint8_t *buf, uint64_t pos, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++)
        buf[i] = stream_byte(pos + i);
}

static sample_ring_t *make(uint32_t slots, uint32_t slot_bytes, void **mem)
{
    sample_ring_t *r = malloc(sizeof(*r));
    *mem = malloc(sample_ring_mem_size(slots, slot_bytes));
    CHECK(sample_ring_init(r, *mem, slots, slot_bytes));
    return r;
}

static void test_directed(void)
{
    void *mem;
    sample_ring_t *r = make(4, 8, &mem);
    sample_ring_t bad;
    uint8_t buf[16];
    const uint8_t *data;
    uint32_t bytes, lost, n;
    bool gap;
    int64_t t;

    CHECK(!sample_ring_init(&bad, mem, 1, 8));   // needs two slots
    CHECK(!sample_ring_init(&bad, mem, 4, 0));
    CHECK(!sample_ring_init(&bad, mem, 4, 7));   // whole IQ pairs only
    CHECK(!sample_ring_init(&bad, NULL, 4, 8));

    CHECK(sample_ring_fill(r) == 0);
    CHECK(sample_ring_peek(r, 100, &data, &bytes, &gap, &lost, &t) == 0);

    // Three full slots fit a 4-slot ring (one is always left empty); the
    // fourth is dropped and its 4 samples are reported with the next one.
    fill(buf, 0, 8);   CHECK(sample_ring_put(r, buf, 8, 100));
    fill(buf, 8, 8);   CHECK(sample_ring_put(r, buf, 8, 101));
    fill(buf, 16, 8);  CHECK(sample_ring_put(r, buf, 8, 102));
    CHECK(sample_ring_fill(r) == 3 && r->max_fill == 3);
    fill(buf, 24, 8);  CHECK(!sample_ring_put(r, buf, 8, 103));
    CHECK(r->dropped == 1 && r->dropped_bytes == 8 && r->stored == 3);

    // One contiguous run of all three, no gap before it
    n = sample_ring_peek(r, 1000, &data, &bytes, &gap, &lost, &t);
    CHECK(n == 3 && bytes == 24 && !gap && lost == 0 && t == 100);
    CHECK(data[0] == stream_byte(0) && data[23] == stream_byte(23));
    // max_bytes limits the run, but never below one slot
    CHECK(sample_ring_peek(r, 16, &data, &bytes, &gap, &lost, &t) == 2 && bytes == 16);
    CHECK(sample_ring_peek(r, 15, &data, &bytes, &gap, &lost, &t) == 1 && bytes == 8);
    CHECK(sample_ring_peek(r, 0, &data, &bytes, &gap, &lost, &t) == 1 && bytes == 8);
    sample_ring_release(r, 2);
    CHECK(sample_ring_fill(r) == 1);

    // The slot stored after the drop carries the gap; a run stops before it.
    // It also lands in the last slot of the buffer, where a run must stop.
    fill(buf, 32, 8);  CHECK(sample_ring_put(r, buf, 8, 104));
    n = sample_ring_peek(r, 1000, &data, &bytes, &gap, &lost, &t);
    CHECK(n == 1 && bytes == 8 && !gap && data[0] == stream_byte(16));
    sample_ring_release(r, 1);
    n = sample_ring_peek(r, 1000, &data, &bytes, &gap, &lost, &t);
    CHECK(n == 1 && bytes == 8 && gap && lost == 4 && t == 104 && data[0] == stream_byte(32));
    CHECK(r->gaps == 1);
    sample_ring_release(r, 1);
    CHECK(sample_ring_fill(r) == 0);

    // A run does not continue across the end of the buffer
    sample_ring_reset(r);
    fill(buf, 0, 8);   sample_ring_put(r, buf, 8, 1);
    fill(buf, 8, 8);   sample_ring_put(r, buf, 8, 2);
    fill(buf, 16, 8);  sample_ring_put(r, buf, 8, 3);
    sample_ring_release(r, 3);                       // tail = head = slot 3
    fill(buf, 24, 8);  sample_ring_put(r, buf, 8, 4); // slot 3
    fill(buf, 32, 8);  sample_ring_put(r, buf, 8, 5); // slot 0
    n = sample_ring_peek(r, 1000, &data, &bytes, &gap, &lost, &t);
    CHECK(n == 1 && bytes == 8 && data[0] == stream_byte(24));
    sample_ring_release(r, 1);
    n = sample_ring_peek(r, 1000, &data, &bytes, &gap, &lost, &t);
    CHECK(n == 1 && bytes == 8 && data[0] == stream_byte(32) && !gap);
    sample_ring_release(r, 1);

    // A short slot ends a run: what follows it is not adjacent in memory.
    // It does not mark a gap: on air the samples are still consecutive.
    sample_ring_reset(r);
    fill(buf, 0, 4);   sample_ring_put(r, buf, 4, 1);
    fill(buf, 4, 8);   sample_ring_put(r, buf, 8, 2);
    n = sample_ring_peek(r, 1000, &data, &bytes, &gap, &lost, &t);
    CHECK(n == 1 && bytes == 4 && !gap);
    sample_ring_release(r, 1);
    n = sample_ring_peek(r, 1000, &data, &bytes, &gap, &lost, &t);
    CHECK(n == 1 && bytes == 8 && !gap && data[0] == stream_byte(4));
    sample_ring_release(r, 1);

    // Declared losses add up and are delivered once; 0 means "unknown size"
    sample_ring_reset(r);
    sample_ring_note_loss(r, 100);
    sample_ring_note_loss(r, 23);
    fill(buf, 0, 8);   sample_ring_put(r, buf, 8, 1);
    fill(buf, 8, 8);   sample_ring_put(r, buf, 8, 2);
    n = sample_ring_peek(r, 1000, &data, &bytes, &gap, &lost, &t);
    CHECK(n == 2 && gap && lost == 123);
    sample_ring_release(r, 2);
    sample_ring_note_loss(r, 0);
    sample_ring_put(r, buf, 8, 3);
    n = sample_ring_peek(r, 1000, &data, &bytes, &gap, &lost, &t);
    CHECK(n == 1 && gap && lost == 0);
    sample_ring_release(r, 1);

    // An odd length loses its last byte and breaks the stream after it;
    // an over-long one is cut to the slot; an empty one stores nothing.
    sample_ring_reset(r);
    fill(buf, 0, 7);   CHECK(sample_ring_put(r, buf, 7, 1));
    fill(buf, 8, 16);  CHECK(sample_ring_put(r, buf, 16, 2));
    CHECK(sample_ring_put(r, buf, 0, 3) && sample_ring_fill(r) == 2);
    n = sample_ring_peek(r, 1000, &data, &bytes, &gap, &lost, &t);
    CHECK(n == 1 && bytes == 6 && !gap);
    sample_ring_release(r, 1);
    n = sample_ring_peek(r, 1000, &data, &bytes, &gap, &lost, &t);
    CHECK(n == 1 && bytes == 8 && gap && lost == 1);
    sample_ring_release(r, 1);

    free(mem);
    free(r);
}

// Shared by the random and threaded tests: check one run against the
// endless stream, given where the consumer believes it is.
static void consume_run(const uint8_t *data, uint32_t bytes, bool gap, uint32_t lost, uint64_t *pos,
                        uint64_t *gap_count, uint64_t *lost_total)
{
    uint32_t i;
    if (gap)
    {
        *pos += (uint64_t)lost * 2;
        (*gap_count)++;
        *lost_total += lost;
    }
    for (i = 0; i < bytes; i++)
    {
        if (data[i] != stream_byte(*pos + i))
        {
            CHECK(!"wrong byte: samples joined across a loss, or corrupted");
            break;
        }
    }
    *pos += bytes;
}

static void test_random(void)
{
    static const uint32_t geoms[][2] = {{2, 2}, {3, 64}, {8, 512}, {64, 5632}, {192, 5632}};
    size_t g;
    for (g = 0; g < sizeof(geoms) / sizeof(geoms[0]); g++)
    {
        uint32_t slots = geoms[g][0], sb = geoms[g][1];
        void *mem;
        sample_ring_t *r = make(slots, sb, &mem);
        uint8_t *buf = malloc(sb);
        uint64_t wpos = 0, rpos = 0, gaps = 0, lost_total = 0, produced_lost = 0;
        int step;
        for (step = 0; step < 200000; step++)
        {
            unsigned op = (unsigned)(rnd() % 100);
            if (op < 55)
            {
                // mostly full transfers, sometimes short ones (even length)
                uint32_t len = (rnd() % 10) ? sb : (uint32_t)(2 * (1 + rnd() % (sb / 2)));
                fill(buf, wpos, len);
                if (!sample_ring_put(r, buf, len, (int64_t)step))
                    produced_lost += len / 2;
                wpos += len;
            }
            else if (op < 58)
            {
                // the reader declares samples lost upstream
                uint32_t lost = (uint32_t)(1 + rnd() % 5000);
                sample_ring_note_loss(r, lost);
                produced_lost += lost;
                wpos += (uint64_t)lost * 2;
            }
            else
            {
                const uint8_t *data;
                uint32_t bytes, lost, n, max = (uint32_t)(rnd() % (8 * sb));
                bool gap;
                int64_t t;
                n = sample_ring_peek(r, max, &data, &bytes, &gap, &lost, &t);
                if (n)
                {
                    CHECK(bytes <= max || n == 1);
                    consume_run(data, bytes, gap, lost, &rpos, &gaps, &lost_total);
                    sample_ring_release(r, n);
                }
            }
        }
        // drain; a loss after the last stored transfer is still pending
        for (;;)
        {
            const uint8_t *data;
            uint32_t bytes, lost, n;
            bool gap;
            int64_t t;
            n = sample_ring_peek(r, 1u << 30, &data, &bytes, &gap, &lost, &t);
            if (!n)
                break;
            consume_run(data, bytes, gap, lost, &rpos, &gaps, &lost_total);
            sample_ring_release(r, n);
        }
        CHECK(lost_total + r->pending_lost == produced_lost);
        CHECK(rpos + r->pending_lost * 2 == wpos);
        CHECK(gaps == r->gaps);
        CHECK(r->max_fill <= slots - 1);
        fprintf(stderr, "  random  %3u x %4u: %u stored, %u dropped, %llu gaps\n", slots, sb,
                r->stored, r->dropped, (unsigned long long)gaps);
        free(buf);
        free(mem);
        free(r);
    }
}

// ---- two threads ----------------------------------------------------------

#define T_SLOTS 64
#define T_BYTES 5632
#define T_TRANSFERS 400000

static sample_ring_t *t_ring;
static atomic_int t_done;

static void *producer(void *arg)
{
    uint8_t *buf = malloc(T_BYTES);
    uint64_t pos = 0;
    int i;
    (void)arg;
    for (i = 0; i < T_TRANSFERS; i++)
    {
        fill(buf, pos, T_BYTES);
        sample_ring_put(t_ring, buf, T_BYTES, i);
        pos += T_BYTES;
        if ((i & 0x3ff) == 0)
        {
            struct timespec ts = {0, 200000}; // let the consumer catch up now and then
            nanosleep(&ts, NULL);
        }
    }
    free(buf);
    atomic_store(&t_done, 1);
    return NULL;
}

static void test_threads(void)
{
    void *mem;
    pthread_t th;
    uint64_t rpos = 0, gaps = 0, lost_total = 0, runs = 0;
    uint32_t seed = 12345;
    t_ring = make(T_SLOTS, T_BYTES, &mem);
    atomic_store(&t_done, 0);
    CHECK(pthread_create(&th, NULL, producer, NULL) == 0);
    for (;;)
    {
        const uint8_t *data;
        uint32_t bytes, lost, n;
        bool gap;
        int64_t t;
        int finished = atomic_load(&t_done);
        n = sample_ring_peek(t_ring, 131072, &data, &bytes, &gap, &lost, &t);
        if (!n)
        {
            if (finished)
                break;
            continue;
        }
        consume_run(data, bytes, gap, lost, &rpos, &gaps, &lost_total);
        runs++;
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) % 50 == 0)
        {
            struct timespec ts = {0, 300000}; // a slow decode: the ring fills and overflows
            nanosleep(&ts, NULL);
        }
        sample_ring_release(t_ring, n);
    }
    pthread_join(th, NULL);
    CHECK(t_ring->stored + t_ring->dropped == T_TRANSFERS);
    CHECK(lost_total + t_ring->pending_lost == (uint64_t)t_ring->dropped * (T_BYTES / 2));
    CHECK(rpos + t_ring->pending_lost * 2 == (uint64_t)T_TRANSFERS * T_BYTES);
    CHECK(t_ring->dropped > 0);   // the test is only meaningful if it overflowed
    CHECK(t_ring->stored > 0);
    fprintf(stderr, "  threads %3d x %4d: %u stored, %u dropped, %llu gaps, %llu runs, max fill %u\n",
            T_SLOTS, T_BYTES, t_ring->stored, t_ring->dropped, (unsigned long long)gaps,
            (unsigned long long)runs, t_ring->max_fill);
    free(mem);
    free(t_ring);
}

int main(void)
{
    test_directed();
    test_random();
    test_threads();
    fprintf(stderr, "test_ring: %d failures\n", fails);
    return fails > 255 ? 255 : fails;
}

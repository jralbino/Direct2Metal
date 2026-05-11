#ifndef MULTICORE_H
#define MULTICORE_H

#include <stdint.h>

#define TASK_CONV2D  2
#define TASK_CONV1X1 1
#define TASK_DEBAYER 3

/* S2 (ISO 26262): all shared task fields are volatile to prevent the compiler
 * from caching them across the epoch memory barrier. */
struct ParallelTask {
    volatile int         type;
    volatile const void* in;
    volatile const void* w_rep;
    volatile const void* w1x1;
    volatile const void* bias;
    volatile void*       out;
    volatile int         H, W, C_in, C_out, K, stride, pad, n_grp;
    volatile bool        do_silu;
    /* TASK_DEBAYER fields */
    volatile const void* raw_in;
    volatile void*       fb_out;
    volatile uint32_t    fb_pitch_b;
};

extern void parallel_conv2d(const float* in, int H_in, int W_in, int C_in,
                            const float* w_rep, const float* bias, int C_out, int K,
                            int stride, int pad, bool do_silu, float* out);

extern void parallel_conv1x1(const float* in, int H, int W, int C_in,
                             const float* w, const float* b, int C_out, bool do_silu, float* out);

/* Async camera debayer dispatch (cores 1-3 process bands in parallel).
 * Workflow: parallel_debayer_start(raw, fb, pitch) — kicks workers and
 * returns immediately. parallel_debayer_wait() blocks until all 3 secondary
 * cores finish their bands. Core 0 is free to do unrelated work (camera FSI
 * wait) between the two calls. Falls back to single-core if secondaries are
 * not ready. */
extern void parallel_debayer_start(const uint8_t* raw, uint8_t* fb, uint32_t pitch);
extern void parallel_debayer_wait();

#endif

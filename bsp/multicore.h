#ifndef MULTICORE_H
#define MULTICORE_H

#include <stdint.h>

#define TASK_CONV2D         2
#define TASK_CONV1X1        1
/* B4-async: cores 1-3 take all the work (1/3 each); core 0 stays free to
 * service the camera + display loop while the conv runs. */
#define TASK_CONV2D_ASYNC   4
#define TASK_CONV1X1_ASYNC  5

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
};

extern void parallel_conv2d(const float* in, int H_in, int W_in, int C_in,
                            const float* w_rep, const float* bias, int C_out, int K,
                            int stride, int pad, bool do_silu, float* out);

extern void parallel_conv1x1(const float* in, int H, int W, int C_in,
                             const float* w, const float* b, int C_out, bool do_silu, float* out);

/* Async variants — dispatch to cores 1-3 only (1/3 chunk each), return
 * immediately, leave core 0 free. Caller must poll `parallel_async_done()`
 * or call `parallel_async_wait()` before reading `out`. Between kick and
 * wait, core 0 can do anything (typically camera + display work). */
extern void parallel_conv2d_async_start(const float* in, int H_in, int W_in, int C_in,
                                        const float* w_rep, const float* bias, int C_out, int K,
                                        int stride, int pad, bool do_silu, float* out);
extern void parallel_conv1x1_async_start(const float* in, int H, int W, int C_in,
                                         const float* w, const float* b, int C_out,
                                         bool do_silu, float* out);
extern bool parallel_async_done();
extern void parallel_async_wait();

#endif

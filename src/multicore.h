#ifndef MULTICORE_H
#define MULTICORE_H

#define TASK_CONV2D  2
#define TASK_CONV1X1 1

struct ParallelTask {
    int type;
    const void* in;
    const void* w_rep;
    const void* w1x1;
    const void* bias;
    void* out;
    int H; int W; int C_in; int C_out; int K; int stride; int pad; int n_grp;
    bool do_silu;
};

extern void parallel_conv2d(const float* in, int H_in, int W_in, int C_in,
                            const float* w_rep, const float* bias, int C_out, int K,
                            int stride, int pad, bool do_silu, float* out);

extern void parallel_conv1x1(const float* in, int H, int W, int C_in,
                             const float* w, const float* b, int C_out, bool do_silu, float* out);

#endif
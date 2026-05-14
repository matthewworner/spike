// analyze.c
// Reads a .wptr trace file and prints full analysis
//
// Usage: ./analyze <trace.wptr> [n_blocks]

#include "tracer.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <trace.wptr> [n_blocks]\n",
                argv[0]);
        return 1;
    }

    const char* path     = argv[1];
    uint32_t    n_blocks = (argc > 2) ? (uint32_t)atoi(argv[2]) : 32;

    printf("\nSpike trace analysis\n");
    printf("════════════════════════\n");
    printf("file:   %s\n", path);
    printf("blocks: %u\n\n", n_blocks);

    TraceSummary s;
    if (tracer_summarise(path, &s) != 0) return 1;
    tracer_print_summary(&s);

    float* matrix = calloc(n_blocks * n_blocks, sizeof(float));
    if (!matrix) {
        fprintf(stderr, "analyze: out of memory\n");
        return 1;
    }

    printf("  transition matrix (top predictions)\n");
    printf("  ────────────────────────────────────\n");
    tracer_build_transitions(path, n_blocks, matrix);

    // predictor accuracy ceiling:
    // for each block i, what is the probability the predictor's
    // top-1 pick is correct? averaged across all blocks that
    // have any observed successors.
    //
    // this is the predictor's expected top-1 accuracy —
    // NOT the expected hot rate, which also depends on slot count
    // and eviction policy. kept separate to avoid confusion.
    float top1_avg  = 0.0f;
    float top2_avg  = 0.0f;
    uint32_t n_rows = 0;

    for (uint32_t i = 0; i < n_blocks; i++) {
        float best  = 0.0f;
        float second = 0.0f;

        for (uint32_t j = 0; j < n_blocks; j++) {
            float p = matrix[i * n_blocks + j];
            if (p > best)        { second = best; best = p; }
            else if (p > second) { second = p; }
        }

        if (best > 0.0f) {
            top1_avg += best;
            top2_avg += best + second;
            n_rows++;
        }
    }

    if (n_rows > 0) {
        top1_avg /= n_rows;
        top2_avg /= n_rows;
        if (top2_avg > 1.0f) top2_avg = 1.0f;  // cap at 100%
    }

    printf("  predictor accuracy (from this trace)\n");
    printf("  ─────────────────────────────────────\n");
    printf("  top-1 hit rate:   %.1f%%\n", top1_avg * 100.0f);
    printf("  top-2 hit rate:   %.1f%%\n", top2_avg * 100.0f);
    printf("  current hot rate: %.1f%%  (no prediction, LRU only)\n\n",
           s.hot_rate * 100.0f);

    if (top1_avg > s.hot_rate)
        printf("  prediction will help — top-1 accuracy > current hot rate\n");
    else
        printf("  note: need more training data for prediction to help\n");
    printf("\n");

    free(matrix);
    return 0;
}

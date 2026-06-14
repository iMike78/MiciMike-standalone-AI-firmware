/* FFT utility - allocates kissfft resources */
#include "fft_util.h"
#include "kiss_fft.h"
#include <stdlib.h>
#include <string.h>

int FftPopulateState(struct FftState* state, size_t input_size) {
    state->input_size = input_size;
    state->fft_size = 1;
    while (state->fft_size < input_size) state->fft_size <<= 1;

    state->input = (int16_t*)calloc(state->fft_size, sizeof(int16_t));
    state->output = (struct complex_int16_t*)calloc(
        state->fft_size / 2 + 1, sizeof(struct complex_int16_t));

    /* scratch: fft_in[fft_size] + fft_out[fft_size] + kissfft cfg */
    size_t cfg_size = 0;
    kiss_fft_cfg cfg = kiss_fft_alloc(state->fft_size, 0, NULL, &cfg_size);
    (void)cfg;

    state->scratch_size = 2 * state->fft_size * sizeof(kiss_fft_cpx) + cfg_size;
    state->scratch = malloc(state->scratch_size);
    if (!state->scratch) return 0;
    memset(state->scratch, 0, state->scratch_size);

    /* Place kissfft cfg after the two buffers */
    void* cfg_ptr = (char*)state->scratch + 2 * state->fft_size * sizeof(kiss_fft_cpx);
    kiss_fft_alloc(state->fft_size, 0, cfg_ptr, &cfg_size);

    return 1;
}

void FftFreeStateContents(struct FftState* state) {
    free(state->input);
    free(state->output);
    free(state->scratch);
}

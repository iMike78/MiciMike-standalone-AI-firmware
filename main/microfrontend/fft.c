/* FFT implementation using kissfft (bundled with esp-tflite-micro) */
#include "fft.h"
#include "kiss_fft.h"
#include <string.h>

void FftCompute(struct FftState* state, const int16_t* input,
                int input_scale_shift) {
    const size_t fft_size = state->fft_size;
    kiss_fft_cpx* fft_in = (kiss_fft_cpx*)state->scratch;
    kiss_fft_cpx* fft_out = fft_in + fft_size;
    kiss_fft_cfg cfg = (kiss_fft_cfg)((char*)(fft_out + fft_size));

    for (size_t i = 0; i < state->input_size; i++) {
        fft_in[i].r = (float)(input[i] >> input_scale_shift);
        fft_in[i].i = 0.0f;
    }
    for (size_t i = state->input_size; i < fft_size; i++) {
        fft_in[i].r = 0.0f;
        fft_in[i].i = 0.0f;
    }

    kiss_fft(cfg, fft_in, fft_out);

    for (size_t i = 0; i <= fft_size / 2; i++) {
        state->output[i].real = (int16_t)fft_out[i].r;
        state->output[i].imag = (int16_t)fft_out[i].i;
    }
}

void FftInit(struct FftState* state) { }
void FftReset(struct FftState* state) { }

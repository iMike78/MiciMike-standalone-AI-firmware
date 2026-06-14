/**
 * Audio Resampling Utilities - Implementation
 *
 * 16kHz→24kHz: ratio 2:3, linear interpolation
 * 24kHz→48kHz: ratio 1:2, sample duplication with linear interpolation
 */

#include "audio_resample.h"

size_t audio_resample_to_api(const int32_t *in, size_t in_frames,
                             int16_t *out, size_t out_size)
{
    // Step 1: Extract left channel (or average L+R) and convert 32→16 bit
    // Step 2: Resample 16kHz → 24kHz (ratio 2:3)

    size_t max_out = out_size / sizeof(int16_t);
    size_t out_idx = 0;

    // Resampling 16→24kHz using linear interpolation
    // For every 2 input samples, produce 3 output samples
    // Output positions: 0, 0.667, 1.333 (in input sample coordinates)

    for (size_t i = 0; i + 1 < in_frames && out_idx + 3 <= max_out; i += 2) {
        // Extract left channel, shift 32bit→16bit (top 16 bits)
        int16_t s0 = (int16_t)(in[i * 2] >> 16);
        int16_t s1 = (int16_t)(in[(i + 1) * 2] >> 16);

        // Look ahead for interpolation
        int16_t s2;
        if (i + 2 < in_frames) {
            s2 = (int16_t)(in[(i + 2) * 2] >> 16);
        } else {
            s2 = s1;  // repeat last sample at boundary
        }

        // Output 3 samples for every 2 input samples
        // pos 0.0 → s0
        out[out_idx++] = s0;
        // pos 0.667 → lerp(s0, s1, 0.667)
        out[out_idx++] = (int16_t)(s0 + ((int32_t)(s1 - s0) * 2 / 3));
        // pos 1.333 → lerp(s1, s2, 0.333)
        out[out_idx++] = (int16_t)(s1 + ((int32_t)(s2 - s1) * 1 / 3));
    }

    return out_idx;
}

size_t audio_resample_to_speaker(const int16_t *in, size_t in_samples,
                                 int32_t *out, size_t out_size)
{
    // 24kHz mono 16bit → 48kHz stereo 32bit
    // Clean 2x upsample: linear interpolation between samples
    // Then duplicate mono to both L and R channels

    size_t max_frames = out_size / (2 * sizeof(int32_t));  // stereo frames
    size_t out_frames = 0;

    for (size_t i = 0; i < in_samples && out_frames + 2 <= max_frames; i++) {
        int32_t s0 = (int32_t)in[i] << 16;  // 16bit → 32bit (shift up)

        int32_t s1;
        if (i + 1 < in_samples) {
            s1 = (int32_t)in[i + 1] << 16;
        } else {
            s1 = s0;
        }

        // First output frame: original sample (L=R)
        out[out_frames * 2]     = s0;  // left
        out[out_frames * 2 + 1] = s0;  // right
        out_frames++;

        // Second output frame: interpolated midpoint (L=R)
        int32_t mid = s0 / 2 + s1 / 2;
        out[out_frames * 2]     = mid;
        out[out_frames * 2 + 1] = mid;
        out_frames++;
    }

    return out_frames;
}

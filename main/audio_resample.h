/**
 * Audio Resampling Utilities
 *
 * Converts between hardware formats and OpenAI Realtime API format:
 *   Input:  16kHz/32bit/stereo → 24kHz/16bit/mono (for API upload)
 *   Output: 24kHz/16bit/mono   → 48kHz/32bit/stereo (for speaker)
 */

#pragma once
#include <stdint.h>
#include <stddef.h>

/**
 * Convert I2S input (16kHz/32bit/stereo) to OpenAI format (24kHz/16bit/mono).
 * Uses linear interpolation for 16→24kHz upsampling.
 *
 * @param in        input buffer (interleaved stereo 32-bit samples)
 * @param in_frames number of stereo frames in input
 * @param out       output buffer (mono 16-bit samples at 24kHz)
 * @param out_size  output buffer capacity in bytes
 * @return          number of output samples written
 */
size_t audio_resample_to_api(const int32_t *in, size_t in_frames,
                             int16_t *out, size_t out_size);

/**
 * Convert OpenAI response (24kHz/16bit/mono) to I2S output (48kHz/32bit/stereo).
 * 24→48kHz is a clean 2x upsample (sample duplication or interpolation).
 *
 * @param in        input buffer (mono 16-bit samples at 24kHz)
 * @param in_samples number of input samples
 * @param out       output buffer (interleaved stereo 32-bit samples at 48kHz)
 * @param out_size  output buffer capacity in bytes
 * @return          number of stereo frames written
 */
size_t audio_resample_to_speaker(const int16_t *in, size_t in_samples,
                                 int32_t *out, size_t out_size);

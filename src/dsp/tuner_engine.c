/*
 * tuner_engine.c - YIN pitch detection engine
 *
 * Copyright (C) 2026 Jeremiah Ticket
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * Implements the YIN algorithm for monophonic fundamental frequency detection.
 * Reference: de Cheveigne, A. & Kawahara, H. (2002). "YIN, a fundamental
 * frequency estimator for speech and music."
 */

#include "tuner_engine.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------- */
/* Engine state                                                                */
/* -------------------------------------------------------------------------- */

struct tuner_engine {
    /* Mono sample buffer (linear with 50% overlap shift) */
    float buffer[TUNER_YIN_WINDOW];
    int   buf_pos;

    /* YIN working arrays (allocated once) */
    float diff[TUNER_YIN_HALF];
    float cmnd[TUNER_YIN_HALF];   /* cumulative mean normalized difference */

    /* Latest result */
    tuner_detection_t result;
    int               has_result;

    /* Configuration */
    float a4_hz;
    float noise_gate;   /* RMS threshold 0.0-1.0 (mapped to int16 range) */

    /* RMS tracking */
    float rms_level;
};

/* -------------------------------------------------------------------------- */
/* Utility functions                                                           */
/* -------------------------------------------------------------------------- */

int tuner_freq_to_midi(float freq, float a4_hz) {
    if (freq <= 0.0f) return -1;
    return (int)roundf(12.0f * log2f(freq / a4_hz) + 69.0f);
}

float tuner_midi_to_freq(int midi_note, float a4_hz) {
    return a4_hz * powf(2.0f, (midi_note - 69.0f) / 12.0f);
}

float tuner_cents_offset(float freq, int midi_note, float a4_hz) {
    if (freq <= 0.0f) return 0.0f;
    float target = tuner_midi_to_freq(midi_note, a4_hz);
    return 1200.0f * log2f(freq / target);
}

/* -------------------------------------------------------------------------- */
/* YIN core algorithm                                                          */
/* -------------------------------------------------------------------------- */

/*
 * Step 2: Difference function
 * d(tau) = sum_{j=0}^{W-1} (x[j] - x[j+tau])^2
 */
static void yin_difference(const float *buf, float *diff, int half_window) {
    diff[0] = 0.0f;
    for (int tau = 1; tau < half_window; tau++) {
        float sum = 0.0f;
        for (int j = 0; j < half_window; j++) {
            float delta = buf[j] - buf[j + tau];
            sum += delta * delta;
        }
        diff[tau] = sum;
    }
}

/*
 * Step 3: Cumulative mean normalized difference function
 * d'(tau) = d(tau) / ((1/tau) * sum_{j=1}^{tau} d(j))
 * d'(0) = 1
 */
static void yin_cumulative_mean(const float *diff, float *cmnd, int half_window) {
    cmnd[0] = 1.0f;
    float running_sum = 0.0f;
    for (int tau = 1; tau < half_window; tau++) {
        running_sum += diff[tau];
        if (running_sum < 1e-10f) {
            cmnd[tau] = 1.0f;
        } else {
            cmnd[tau] = diff[tau] * tau / running_sum;
        }
    }
}

/*
 * Step 4: Absolute threshold
 * Find the first tau where cmnd[tau] < threshold, then find the
 * minimum from there until cmnd rises again.
 */
static int yin_absolute_threshold(const float *cmnd, int half_window, float threshold) {
    int tau = 2;  /* Start at lag 2 (avoid trivial lag 0/1) */

    /* Find first dip below threshold */
    while (tau < half_window) {
        if (cmnd[tau] < threshold) {
            /* Walk to local minimum */
            while (tau + 1 < half_window && cmnd[tau + 1] < cmnd[tau]) {
                tau++;
            }
            return tau;
        }
        tau++;
    }

    return -1;  /* No periodic signal found */
}

/*
 * Step 5: Parabolic interpolation around the best tau
 * Returns refined tau as a float.
 */
static float yin_parabolic_interp(const float *cmnd, int tau, int half_window) {
    if (tau <= 0 || tau >= half_window - 1) {
        return (float)tau;
    }

    float s0 = cmnd[tau - 1];
    float s1 = cmnd[tau];
    float s2 = cmnd[tau + 1];

    float denom = 2.0f * (2.0f * s1 - s2 - s0);
    if (fabsf(denom) < 1e-10f) {
        return (float)tau;
    }

    return tau + (s0 - s2) / denom;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

tuner_engine_t *tuner_engine_create(void) {
    tuner_engine_t *e = (tuner_engine_t *)calloc(1, sizeof(tuner_engine_t));
    if (!e) return NULL;

    e->a4_hz      = 440.0f;
    e->noise_gate = 0.01f;  /* Default: very low gate */
    e->has_result = 0;
    e->buf_pos    = 0;
    e->rms_level  = 0.0f;

    return e;
}

void tuner_engine_destroy(tuner_engine_t *engine) {
    free(engine);
}

void tuner_engine_set_a4(tuner_engine_t *engine, float a4_hz) {
    if (engine && a4_hz >= 400.0f && a4_hz <= 500.0f) {
        engine->a4_hz = a4_hz;
    }
}

void tuner_engine_set_noise_gate(tuner_engine_t *engine, float threshold) {
    if (engine) {
        engine->noise_gate = threshold;
    }
}

static void tuner_engine_analyze(tuner_engine_t *e) {
    /* Compute RMS of the window */
    float sum_sq = 0.0f;
    for (int i = 0; i < TUNER_YIN_WINDOW; i++) {
        sum_sq += e->buffer[i] * e->buffer[i];
    }
    e->rms_level = sqrtf(sum_sq / TUNER_YIN_WINDOW);

    /* Noise gate: signal too quiet */
    if (e->rms_level < e->noise_gate) {
        e->has_result = 0;
        e->result.frequency  = 0.0f;
        e->result.confidence = 0.0f;
        return;
    }

    /* YIN Steps 2-5 */
    yin_difference(e->buffer, e->diff, TUNER_YIN_HALF);
    yin_cumulative_mean(e->diff, e->cmnd, TUNER_YIN_HALF);

    int tau = yin_absolute_threshold(e->cmnd, TUNER_YIN_HALF, TUNER_YIN_THRESHOLD);
    if (tau < 0) {
        e->has_result = 0;
        e->result.frequency  = 0.0f;
        e->result.confidence = 0.0f;
        return;
    }

    float refined_tau = yin_parabolic_interp(e->cmnd, tau, TUNER_YIN_HALF);
    if (refined_tau <= 0.0f) {
        e->has_result = 0;
        return;
    }

    float freq = (float)TUNER_SAMPLE_RATE / refined_tau;

    /* Range check */
    if (freq < TUNER_MIN_FREQ || freq > TUNER_MAX_FREQ) {
        e->has_result = 0;
        e->result.frequency  = 0.0f;
        e->result.confidence = 0.0f;
        return;
    }

    /* Confidence = 1 - cmnd[tau] (lower cmnd = higher confidence) */
    float conf = 1.0f - e->cmnd[tau];
    if (conf < 0.0f) conf = 0.0f;
    if (conf > 1.0f) conf = 1.0f;

    /* Map to musical note */
    int midi = tuner_freq_to_midi(freq, e->a4_hz);
    int note_idx = midi % 12;
    if (note_idx < 0) note_idx += 12;
    int octave = (midi / 12) - 1;
    float cents = tuner_cents_offset(freq, midi, e->a4_hz);

    /* Clamp cents to +/- 50 */
    if (cents > 50.0f) cents = 50.0f;
    if (cents < -50.0f) cents = -50.0f;

    e->result.frequency    = freq;
    e->result.confidence   = conf;
    e->result.midi_note    = midi;
    e->result.note_index   = note_idx;
    e->result.octave       = octave;
    e->result.cents_offset = cents;
    e->has_result = 1;
}

void tuner_engine_feed(tuner_engine_t *engine, const int16_t *stereo_in, int frames) {
    if (!engine || !stereo_in) return;

    for (int i = 0; i < frames; i++) {
        /* Downmix stereo to mono, normalize to -1..+1 */
        float left  = stereo_in[i * 2]     / 32768.0f;
        float right = stereo_in[i * 2 + 1] / 32768.0f;
        float mono  = (left + right) * 0.5f;

        engine->buffer[engine->buf_pos++] = mono;

        if (engine->buf_pos >= TUNER_YIN_WINDOW) {
            tuner_engine_analyze(engine);

            /* Overlap: shift second half to beginning for continuity */
            memmove(engine->buffer,
                    engine->buffer + TUNER_YIN_HALF,
                    TUNER_YIN_HALF * sizeof(float));
            engine->buf_pos = TUNER_YIN_HALF;
        }
    }
}

int tuner_engine_get_result(const tuner_engine_t *engine, tuner_detection_t *result) {
    if (!engine || !result) return 0;
    *result = engine->result;
    return engine->has_result;
}

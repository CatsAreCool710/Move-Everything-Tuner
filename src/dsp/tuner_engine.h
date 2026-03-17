/*
 * tuner_engine.h - YIN pitch detection engine
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
 * Implements the YIN algorithm (de Cheveigne & Kawahara, 2002) for
 * monophonic pitch detection. Optimized for real-time use on ARM64.
 *
 * Audio is fed in blocks of 128 stereo-interleaved int16 samples.
 * Internally downmixes to mono float and buffers until a full analysis
 * window is available (2048 samples = ~46ms at 44100 Hz).
 */

#ifndef TUNER_ENGINE_H
#define TUNER_ENGINE_H

#include <stdint.h>

/* Analysis window size in mono samples */
#define TUNER_YIN_WINDOW    2048

/* Half-window used for autocorrelation lag search */
#define TUNER_YIN_HALF      (TUNER_YIN_WINDOW / 2)

/* Sample rate */
#define TUNER_SAMPLE_RATE   44100

/* Minimum detectable frequency (~30 Hz, covers bass low B0) */
#define TUNER_MIN_FREQ      30.0f

/* Maximum detectable frequency (~4200 Hz, covers high instruments) */
#define TUNER_MAX_FREQ      4200.0f

/* YIN threshold for periodic signal detection (lower = stricter) */
#define TUNER_YIN_THRESHOLD 0.15f

/* Note name table (extern to avoid duplication across translation units) */
extern const char *TUNER_NOTE_NAMES[];

/* Detection result */
typedef struct {
    float frequency;       /* Detected frequency in Hz, 0 if none */
    float confidence;      /* Detection confidence 0.0-1.0 */
    int   midi_note;       /* MIDI note number (69 = A4) */
    int   note_index;      /* Note within octave 0-11 (0=C) */
    int   octave;          /* Octave number (4 = middle) */
    float cents_offset;    /* Cents deviation from nearest note (-50 to +50) */
} tuner_detection_t;

/* Engine state (opaque) */
typedef struct tuner_engine tuner_engine_t;

/*
 * Create a new pitch detection engine.
 * Returns NULL on allocation failure.
 */
tuner_engine_t *tuner_engine_create(void);

/*
 * Destroy the engine and free resources.
 */
void tuner_engine_destroy(tuner_engine_t *engine);

/*
 * Feed a block of stereo interleaved int16 audio.
 * frames = number of stereo frames (typically 128).
 * Internally buffers and runs detection when enough samples accumulate.
 */
void tuner_engine_feed(tuner_engine_t *engine, const int16_t *stereo_in, int frames);

/*
 * Get the latest detection result.
 * Returns 1 if a valid detection is available, 0 if no pitch detected.
 */
int tuner_engine_get_result(const tuner_engine_t *engine, tuner_detection_t *result);

/*
 * Set the A4 reference frequency (default 440.0).
 */
void tuner_engine_set_a4(tuner_engine_t *engine, float a4_hz);

/*
 * Set the noise gate threshold (0.0 - 1.0, maps to RMS level).
 * Signals below this level are treated as silence.
 */
void tuner_engine_set_noise_gate(tuner_engine_t *engine, float threshold);

/*
 * Convert a frequency to the nearest MIDI note number given A4 reference.
 */
int tuner_freq_to_midi(float freq, float a4_hz);

/*
 * Get the exact frequency of a MIDI note given A4 reference.
 */
float tuner_midi_to_freq(int midi_note, float a4_hz);

/*
 * Get cents offset from a MIDI note given detected frequency and A4 reference.
 */
float tuner_cents_offset(float freq, int midi_note, float a4_hz);

#endif /* TUNER_ENGINE_H */

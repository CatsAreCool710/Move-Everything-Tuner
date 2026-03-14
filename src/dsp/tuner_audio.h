/*
 * tuner_audio.h - Audio feedback generator for the tuner
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
 * Provides three feedback modes:
 *   1. Step Guide     - Musical interval figures that shrink toward unison
 *   2. Reference Tone - Sine wave or plucked string at the target note
 *   3. Off            - Silent (detection still runs)
 *
 * The step guide plays a two-note melodic figure after the user strums
 * and the note dies down. The interval direction and size encode tuning
 * information: ascending = flat (go higher), descending = sharp (go lower),
 * whole step = far off, half step = close, unison = in tune.
 *
 * The reference tone has two styles:
 *   - Sine: continuous pure tone at the target note frequency
 *   - Pluck: Karplus-Strong plucked string, re-triggers on note change
 *            and repeats periodically so the pitch stays audible
 *
 * All generators produce mono float samples that are then mixed with
 * optional input passthrough and converted to stereo int16 output.
 */

#ifndef TUNER_AUDIO_H
#define TUNER_AUDIO_H

#include <stdint.h>

/* Feedback modes */
typedef enum {
    TUNER_FB_STEP_GUIDE = 0,
    TUNER_FB_REFERENCE  = 1,
    TUNER_FB_OFF        = 2
} tuner_feedback_mode_t;

/* Reference tone style */
typedef enum {
    TUNER_REF_SINE       = 0,
    TUNER_REF_PLUCK      = 1,
    TUNER_REF_SOFT_PLUCK = 2   /* Gentler pizzicato — longer sustain, warmer tone */
} tuner_ref_style_t;

/* Guide octave setting */
typedef enum {
    TUNER_GUIDE_OCT_AUTO  = 0,   /* Transpose to comfortable octave 3-4 */
    TUNER_GUIDE_OCT_MATCH = 1    /* Match detected note octave */
} tuner_guide_octave_t;

/* Audio generator state (opaque) */
typedef struct tuner_audio tuner_audio_t;

/*
 * Create the audio feedback generator.
 * sample_rate: expected 44100
 */
tuner_audio_t *tuner_audio_create(int sample_rate);

/*
 * Destroy the generator and free resources.
 */
void tuner_audio_destroy(tuner_audio_t *audio);

/*
 * Set the feedback mode.
 */
void tuner_audio_set_mode(tuner_audio_t *audio, tuner_feedback_mode_t mode);

/*
 * Set feedback tone volume (0.0 - 1.0).
 */
void tuner_audio_set_volume(tuner_audio_t *audio, float volume);

/*
 * Set guide octave mode (auto / match).
 */
void tuner_audio_set_guide_octave(tuner_audio_t *audio, tuner_guide_octave_t mode);

/*
 * Set reference tone style (sine / pluck).
 */
void tuner_audio_set_ref_style(tuner_audio_t *audio, tuner_ref_style_t style);

/*
 * Update the tuning state. Called when detection results change.
 *   target_freq:  the frequency of the target note (in Hz)
 *   target_midi:  MIDI note number of the target
 *   cents_offset: deviation from target (-50 to +50)
 *   has_signal:   1 if a pitch is detected, 0 if silence
 *   in_tune:      1 if within the tune threshold
 *   a4_ref:       A4 reference frequency (e.g. 440.0)
 */
void tuner_audio_update(tuner_audio_t *audio,
                        float target_freq,
                        int   target_midi,
                        float cents_offset,
                        int   has_signal,
                        int   in_tune,
                        float a4_ref);

/*
 * Render a block of feedback audio.
 *   out:    mono float buffer (frames samples)
 *   frames: number of samples to render
 *
 * The caller is responsible for mixing this with passthrough and
 * converting to stereo int16.
 */
void tuner_audio_render(tuner_audio_t *audio, float *out, int frames);

/*
 * Set the current input peak level (0.0 - 1.0).
 * Called each block before render so the generator knows when the
 * player is actively playing vs when the note has died down.
 */
void tuner_audio_set_input_level(tuner_audio_t *audio, float peak);

/*
 * Returns 1 if the audio generator is actively producing tones
 * (i.e. not in a waiting/silent state). The caller should avoid
 * feeding audio to the pitch detector during playback to prevent
 * the speaker output from being picked up by the mic and causing
 * feedback loops.
 */
int tuner_audio_is_playing(const tuner_audio_t *audio);

/*
 * Returns 1 (and clears the flag) if the guide just started playing
 * and the caller should clear any held pitch detection. This ensures
 * that after the figure plays and cooldown ends, the system requires
 * a genuinely fresh detection from a new strum rather than replaying
 * based on a stale held detection.
 */
int tuner_audio_wants_clear(tuner_audio_t *audio);

/*
 * Set the reference octave shift (in semitones, must be multiple of 12).
 * Applied to both reference tone and step guide (in auto octave mode)
 * to transpose notes into an audible range while preserving relative
 * intervals between strings.
 */
void tuner_audio_set_ref_shift(tuner_audio_t *audio, int semitones);

/*
 * Set step guide note duration (ms). Default 200ms.
 * Range: 50-500ms.
 */
void tuner_audio_set_guide_tone_ms(tuner_audio_t *audio, int ms);

/*
 * Set step guide gap between notes (ms). Default 40ms.
 * Range: 10-200ms.
 */
void tuner_audio_set_guide_gap_ms(tuner_audio_t *audio, int ms);

#endif /* TUNER_AUDIO_H */

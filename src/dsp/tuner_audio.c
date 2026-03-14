/*
 * tuner_audio.c - Audio feedback generator for the tuner
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
 * Implements three feedback modes:
 *   1. Step Guide: two-note melodic figure whose interval shrinks toward
 *      unison. Descending = sharp, ascending = flat.
 *   2. Reference Tone: continuous sine or Karplus-Strong pluck at the
 *      target note frequency. Re-triggers on note change.
 *   3. Off: silent.
 *
 * Step Guide state machine: WAITING -> PLAYING -> COOLDOWN -> WAITING
 */

#include "tuner_audio.h"
#include "tuner_engine.h"   /* for tuner_midi_to_freq */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWO_PI (2.0 * M_PI)

/* -------------------------------------------------------------------------- */
/* Step Guide timing constants                                                 */
/* -------------------------------------------------------------------------- */

#define GUIDE_NOTE_DUR_MS    200    /* Each note in the figure (ms) */
#define GUIDE_GAP_MS         40     /* Gap between notes (ms) */

/* How long input must be quiet before guide tones play (ms) */
#define GUIDE_QUIET_MS       200

/* Input peak threshold below which we consider "quiet".
 * Move mic ambient floor is ~0.03-0.05, feedback from speaker at 0.7 vol
 * can reach ~0.65 peak. Must be above ambient to distinguish silence. */
#define GUIDE_QUIET_THRESH   0.12f

/* Cooldown after figure plays — let speaker echo die before listening */
#define GUIDE_COOLDOWN_MS    800

/* Envelope attack/release to avoid clicks (ms) */
#define ENV_ATTACK_MS        5
#define ENV_RELEASE_MS       10

#define MS_TO_SAMPLES(ms, sr) ((int)((ms) * (sr) / 1000))

/* -------------------------------------------------------------------------- */
/* Karplus-Strong pluck constants                                              */
/* -------------------------------------------------------------------------- */

/* Maximum delay line length: supports down to ~27 Hz (MIDI 16, A0=27.5 Hz)
 * at 44100 Hz sample rate: 44100 / 27 ≈ 1633 samples. Round up. */
#define KS_MAX_DELAY  2048

/* How long a pluck rings before re-triggering (ms) */
#define KS_RETRIGGER_MS  2000

/* Lowpass feedback coefficient (0.0 = max damping, 1.0 = no damping).
 * Higher = longer sustain and brighter. 0.996 gives a nice guitar-like decay. */
#define KS_DAMPING  0.996f

/* -------------------------------------------------------------------------- */
/* Audio generator state                                                       */
/* -------------------------------------------------------------------------- */

struct tuner_audio {
    int sample_rate;

    /* Mode and volume */
    tuner_feedback_mode_t mode;
    tuner_ref_style_t     ref_style;
    float volume;
    tuner_guide_octave_t  guide_octave;

    /* Current tuning state (updated each block by caller) */
    float target_freq;
    int   target_midi;
    float cents_offset;
    int   has_signal;
    int   in_tune;
    float a4_ref;           /* A4 reference frequency from user setting */

    /* Sine oscillator phases */
    double phase;           /* First note oscillator */
    double guide_phase;     /* Second note oscillator */

    /* Reference tone state */
    int   last_ref_midi;    /* Track target changes for re-trigger */
    int   ref_attack_pos;   /* Attack envelope position (samples) */
    int   ref_attack_len;   /* Attack envelope length (samples) */

    /* Karplus-Strong pluck state */
    float ks_delay[KS_MAX_DELAY]; /* Delay line buffer */
    int   ks_delay_len;     /* Current delay length (= sample_rate / freq) */
    int   ks_pos;           /* Current read position in delay line */
    float ks_prev;          /* Previous sample for lowpass filter */
    int   ks_samples_since; /* Samples since last trigger (for re-trigger) */
    int   ks_retrigger;     /* Samples between re-triggers */
    int   ks_active;        /* 1 = pluck is currently ringing */

    /* Step guide timing (in samples) */
    int   guide_note1_len;
    int   guide_gap_len;
    int   guide_note2_len;

    /* Step guide frequencies */
    float guide_freq1;      /* ACTIVE guide note frequency (latched at play start) */
    float guide_freq2;      /* ACTIVE target note frequency (latched at play start) */
    float pending_freq1;    /* Pending guide frequency (updated each block) */
    float pending_freq2;    /* Pending target frequency (updated each block) */

    /* Step guide state machine */
    int   guide_pos;        /* Sample position within the figure */
    int   guide_waiting;    /* 1 = WAITING state */
    int   guide_played;     /* 1 = COOLDOWN state */
    int   quiet_samples;    /* Consecutive quiet samples counted */
    int   quiet_needed;     /* Samples of quiet needed before playing */
    int   cooldown_samples; /* Samples into cooldown */
    int   cooldown_needed;  /* Total cooldown duration in samples */
    int   wants_clear;      /* 1 = guide just started, plugin should clear detection */

    /* Octave shift for reference/guide tones (semitones, multiple of 12) */
    int   ref_shift;

    /* Input level (set each block by caller) */
    float input_peak;
    int   saw_loud_input;   /* 1 = saw a real strum since last reset */
};

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

/* Apply preset-based octave shift to a MIDI note for reference/guide playback.
 * Preserves relative intervals between strings while shifting the whole set
 * into an audible range for the Move's small speaker. The shift is computed
 * per-preset so that the lowest string lands at MIDI >= 35 (B1). */
static int apply_ref_shift(int midi_note, int shift) {
    int result = midi_note + shift;
    if (result < 0) result = 0;
    if (result > 127) result = 127;
    return result;
}

static inline float sine_sample(double ph) {
    return (float)sin(ph);
}

/* Compute envelope value for attack/release at given position in a note. */
static inline float envelope(int pos, int length, int attack, int release) {
    if (pos < attack) {
        return (float)pos / (float)attack;
    }
    int release_start = length - release;
    if (release > 0 && pos >= release_start) {
        return (float)(length - pos) / (float)release;
    }
    return 1.0f;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

tuner_audio_t *tuner_audio_create(int sample_rate) {
    tuner_audio_t *a = (tuner_audio_t *)calloc(1, sizeof(tuner_audio_t));
    if (!a) return NULL;

    a->sample_rate    = sample_rate;
    a->mode           = TUNER_FB_STEP_GUIDE;
    a->ref_style      = TUNER_REF_SINE;
    a->volume         = 0.4f;
    a->guide_octave   = TUNER_GUIDE_OCT_AUTO;
    a->a4_ref         = 440.0f;

    a->guide_note1_len = MS_TO_SAMPLES(GUIDE_NOTE_DUR_MS, sample_rate);
    a->guide_gap_len   = MS_TO_SAMPLES(GUIDE_GAP_MS, sample_rate);
    a->guide_note2_len = MS_TO_SAMPLES(GUIDE_NOTE_DUR_MS, sample_rate);
    a->quiet_needed    = MS_TO_SAMPLES(GUIDE_QUIET_MS, sample_rate);
    a->cooldown_needed = MS_TO_SAMPLES(GUIDE_COOLDOWN_MS, sample_rate);

    a->guide_waiting   = 1;

    /* Reference tone */
    a->last_ref_midi   = -1;
    a->ref_attack_len  = MS_TO_SAMPLES(ENV_ATTACK_MS, sample_rate);

    /* Pluck */
    a->ks_retrigger    = MS_TO_SAMPLES(KS_RETRIGGER_MS, sample_rate);

    return a;
}

void tuner_audio_destroy(tuner_audio_t *audio) {
    free(audio);
}

void tuner_audio_set_mode(tuner_audio_t *audio, tuner_feedback_mode_t mode) {
    if (!audio) return;
    audio->mode = mode;
    /* Reset oscillator state on mode change */
    audio->phase = 0.0;
    audio->guide_phase = 0.0;
    audio->guide_pos = 0;
    audio->guide_waiting = 1;
    audio->guide_played = 0;
    audio->quiet_samples = 0;
    audio->cooldown_samples = 0;
    audio->saw_loud_input = 0;
    /* Reset reference tone state */
    audio->last_ref_midi = -1;
    audio->ref_attack_pos = 0;
    audio->ks_active = 0;
}

void tuner_audio_set_volume(tuner_audio_t *audio, float volume) {
    if (!audio) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    audio->volume = volume;
}

void tuner_audio_set_guide_octave(tuner_audio_t *audio, tuner_guide_octave_t mode) {
    if (audio) audio->guide_octave = mode;
}

void tuner_audio_set_ref_style(tuner_audio_t *audio, tuner_ref_style_t style) {
    if (!audio) return;
    audio->ref_style = style;
    /* Force re-trigger so user hears the new style immediately */
    audio->last_ref_midi = -1;
    audio->ks_active = 0;
    audio->phase = 0.0;
    audio->ref_attack_pos = 0;
}

void tuner_audio_set_input_level(tuner_audio_t *audio, float peak) {
    if (audio) audio->input_peak = peak;
}

int tuner_audio_is_playing(const tuner_audio_t *audio) {
    if (!audio) return 0;

    if (audio->mode == TUNER_FB_REFERENCE) {
        /* Reference tone plays continuously — always "playing" */
        return (audio->target_freq > 0.0f);
    }

    if (audio->mode == TUNER_FB_STEP_GUIDE) {
        /*
         * "Playing" means actively producing tones OR in cooldown
         * (speaker is still ringing during cooldown, so mic input is garbage).
         * Only return 0 in the WAITING state.
         */
        if (audio->guide_waiting) return 0;
        return 1;
    }

    return 0;
}

int tuner_audio_wants_clear(tuner_audio_t *audio) {
    if (!audio) return 0;
    if (audio->wants_clear) {
        audio->wants_clear = 0;
        return 1;
    }
    return 0;
}

void tuner_audio_set_ref_shift(tuner_audio_t *audio, int semitones) {
    if (audio) audio->ref_shift = semitones;
}

void tuner_audio_set_guide_tone_ms(tuner_audio_t *audio, int ms) {
    if (!audio) return;
    if (ms < 50) ms = 50;
    if (ms > 500) ms = 500;
    audio->guide_note1_len = MS_TO_SAMPLES(ms, audio->sample_rate);
    audio->guide_note2_len = MS_TO_SAMPLES(ms, audio->sample_rate);
}

void tuner_audio_set_guide_gap_ms(tuner_audio_t *audio, int ms) {
    if (!audio) return;
    if (ms < 10) ms = 10;
    if (ms > 200) ms = 200;
    audio->guide_gap_len = MS_TO_SAMPLES(ms, audio->sample_rate);
}

void tuner_audio_update(tuner_audio_t *audio,
                        float target_freq,
                        int   target_midi,
                        float cents_offset,
                        int   has_signal,
                        int   in_tune,
                        float a4_ref) {
    if (!audio) return;

    audio->target_freq  = target_freq;
    audio->target_midi  = target_midi;
    audio->cents_offset = cents_offset;
    audio->has_signal   = has_signal;
    audio->in_tune      = in_tune;
    audio->a4_ref       = a4_ref;

    if (audio->mode != TUNER_FB_STEP_GUIDE) return;

    /* Compute pending guide frequencies using the user's A4 reference */
    int guide_midi = target_midi;
    if (audio->guide_octave == TUNER_GUIDE_OCT_AUTO) {
        guide_midi = apply_ref_shift(target_midi, audio->ref_shift);
    }

    float abs_cents = fabsf(cents_offset);

    /*
     * Write to PENDING fields — latched into active guide_freq1/guide_freq2
     * only at the WAITING -> PLAYING transition. Prevents stale detections
     * from changing pitch mid-figure or after cooldown.
     */
    if (in_tune || abs_cents < 3.0f) {
        /* Unison — just play the target note */
        audio->pending_freq1 = tuner_midi_to_freq(guide_midi, a4_ref);
        audio->pending_freq2 = audio->pending_freq1;
    } else if (abs_cents < 25.0f) {
        /* Half step interval */
        audio->pending_freq2 = tuner_midi_to_freq(guide_midi, a4_ref);
        if (cents_offset > 0) {
            audio->pending_freq1 = tuner_midi_to_freq(guide_midi + 1, a4_ref);
        } else {
            audio->pending_freq1 = tuner_midi_to_freq(guide_midi - 1, a4_ref);
        }
    } else {
        /* Whole step interval */
        audio->pending_freq2 = tuner_midi_to_freq(guide_midi, a4_ref);
        if (cents_offset > 0) {
            audio->pending_freq1 = tuner_midi_to_freq(guide_midi + 2, a4_ref);
        } else {
            audio->pending_freq1 = tuner_midi_to_freq(guide_midi - 2, a4_ref);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Step guide renderer                                                         */
/* -------------------------------------------------------------------------- */

static void render_step_guide(tuner_audio_t *a, float *out, int frames) {
    /*
     * No signal and in WAITING state: reset state machine.
     * (Don't reset during PLAYING/COOLDOWN — detection is deliberately cleared.)
     */
    if (!a->has_signal && a->guide_waiting) {
        memset(out, 0, frames * sizeof(float));
        a->guide_pos = 0;
        a->guide_played = 0;
        a->quiet_samples = 0;
        a->cooldown_samples = 0;
        a->saw_loud_input = 0;
        return;
    }

    int attack_samples  = MS_TO_SAMPLES(ENV_ATTACK_MS, a->sample_rate);
    int release_samples = MS_TO_SAMPLES(ENV_RELEASE_MS, a->sample_rate);
    int note1_end       = a->guide_note1_len;
    int gap_end         = note1_end + a->guide_gap_len;
    int note2_end       = gap_end + a->guide_note2_len;

    /* --- COOLDOWN: wait for speaker echo to die --- */
    if (a->guide_played) {
        memset(out, 0, frames * sizeof(float));
        a->cooldown_samples += frames;
        if (a->cooldown_samples >= a->cooldown_needed) {
            a->guide_played = 0;
            a->guide_waiting = 1;
            a->quiet_samples = 0;
            a->saw_loud_input = 0;
        }
        return;
    }

    /* --- WAITING: wait for strum then quiet --- */
    if (a->guide_waiting) {
        memset(out, 0, frames * sizeof(float));

        if (a->input_peak > GUIDE_QUIET_THRESH) {
            /* Loud input — real strum */
            a->saw_loud_input = 1;
            a->quiet_samples = 0;
            return;
        }

        if (!a->saw_loud_input) {
            /* Haven't heard a strum yet */
            return;
        }

        /* Input quiet after strum — count up */
        a->quiet_samples += frames;
        if (a->quiet_samples < a->quiet_needed) {
            return;
        }

        /* Quiet long enough — latch frequencies and start playing */
        a->guide_freq1 = a->pending_freq1;
        a->guide_freq2 = a->pending_freq2;
        a->guide_waiting = 0;
        a->guide_played = 0;
        a->guide_pos = 0;
        a->phase = 0.0;
        a->guide_phase = 0.0;
        a->wants_clear = 1;
    }

    /* --- PLAYING: render the two-note figure --- */

    /* In-tune: play a short confirmation tone then enter cooldown */
    if (a->in_tune && fabsf(a->guide_freq1 - a->guide_freq2) < 0.1f) {
        if (a->guide_pos < a->guide_note1_len) {
            double phase_inc = TWO_PI * a->guide_freq2 / a->sample_rate;
            for (int i = 0; i < frames; i++) {
                float env = envelope(a->guide_pos + i, a->guide_note1_len,
                                     attack_samples, release_samples);
                out[i] = sine_sample(a->phase) * a->volume * 0.5f * env;
                a->phase += phase_inc;
                if (a->phase >= TWO_PI) a->phase -= TWO_PI;
            }
            a->guide_pos += frames;
        } else {
            memset(out, 0, frames * sizeof(float));
            a->guide_played = 1;
            a->cooldown_samples = 0;
        }
        return;
    }

    /* Normal figure: note1 -> gap -> note2 */
    for (int i = 0; i < frames; i++) {
        float sample = 0.0f;

        if (a->guide_pos < note1_end) {
            float env = envelope(a->guide_pos, a->guide_note1_len,
                                 attack_samples, release_samples);
            double phase_inc = TWO_PI * a->guide_freq1 / a->sample_rate;
            sample = sine_sample(a->phase) * env * a->volume;
            a->phase += phase_inc;
            if (a->phase >= TWO_PI) a->phase -= TWO_PI;
        } else if (a->guide_pos < gap_end) {
            sample = 0.0f;
        } else if (a->guide_pos < note2_end) {
            int pos_in_note = a->guide_pos - gap_end;
            float env = envelope(pos_in_note, a->guide_note2_len,
                                 attack_samples, release_samples);
            double phase_inc = TWO_PI * a->guide_freq2 / a->sample_rate;
            sample = sine_sample(a->guide_phase) * env * a->volume;
            a->guide_phase += phase_inc;
            if (a->guide_phase >= TWO_PI) a->guide_phase -= TWO_PI;
        } else {
            /* Figure complete — enter cooldown */
            a->guide_played = 1;
            a->cooldown_samples = 0;
            memset(out + i, 0, (frames - i) * sizeof(float));
            return;
        }

        out[i] = sample;
        a->guide_pos++;
    }
}

/* -------------------------------------------------------------------------- */
/* Karplus-Strong pluck helpers                                                */
/* -------------------------------------------------------------------------- */

/* Simple LCG random for noise excitation (deterministic, no stdlib needed) */
static uint32_t ks_rng_state = 12345;

static float ks_noise(void) {
    ks_rng_state = ks_rng_state * 1103515245 + 12345;
    return ((float)(ks_rng_state >> 16) / 32768.0f) - 1.0f;
}

/* Fill the delay line with filtered noise to excite the string */
static void ks_trigger(tuner_audio_t *a) {
    if (a->target_freq <= 0.0f) return;

    int delay_len = (int)(a->sample_rate / a->target_freq);
    if (delay_len < 2) delay_len = 2;
    if (delay_len > KS_MAX_DELAY) delay_len = KS_MAX_DELAY;

    a->ks_delay_len = delay_len;
    a->ks_pos = 0;
    a->ks_prev = 0.0f;
    a->ks_samples_since = 0;
    a->ks_active = 1;

    /* Fill delay line with band-limited noise (slight lowpass for warmth) */
    float prev = 0.0f;
    for (int i = 0; i < delay_len; i++) {
        float n = ks_noise();
        /* Simple one-pole lowpass: mix with previous sample */
        float filtered = 0.5f * n + 0.5f * prev;
        a->ks_delay[i] = filtered;
        prev = filtered;
    }
}

/* -------------------------------------------------------------------------- */
/* Reference tone renderer (sine or pluck)                                     */
/* -------------------------------------------------------------------------- */

static void render_reference(tuner_audio_t *a, float *out, int frames) {
    if (a->target_freq <= 0.0f) {
        memset(out, 0, frames * sizeof(float));
        return;
    }

    /* Transpose target to audible range for the Move's small speaker.
     * Notes below ~C3 (130 Hz) are inaudible on the tiny speaker.
     * Use the same comfortable octave logic as step guide. */
    int ref_midi = apply_ref_shift(a->target_midi, a->ref_shift);
    float ref_freq = tuner_midi_to_freq(ref_midi, a->a4_ref);

    /* Detect target note change — re-trigger */
    if (ref_midi != a->last_ref_midi) {
        a->last_ref_midi = ref_midi;
        a->phase = 0.0;
        a->ref_attack_pos = 0;
        if (a->ref_style == TUNER_REF_PLUCK || a->ref_style == TUNER_REF_SOFT_PLUCK) {
            /* Update target_freq temporarily for ks_trigger delay calculation */
            float saved = a->target_freq;
            a->target_freq = ref_freq;
            ks_trigger(a);
            a->target_freq = saved;
        }
    }

    if (a->ref_style == TUNER_REF_PLUCK || a->ref_style == TUNER_REF_SOFT_PLUCK) {
        /* --- Karplus-Strong pluck --- */
        /* Soft pluck uses higher damping for warmer, longer-sustaining tone */
        float damp = (a->ref_style == TUNER_REF_SOFT_PLUCK) ? 0.999f : KS_DAMPING;

        /* Auto re-trigger if the pluck has decayed */
        a->ks_samples_since += frames;
        if (a->ks_samples_since >= a->ks_retrigger) {
            float saved = a->target_freq;
            a->target_freq = ref_freq;
            ks_trigger(a);
            a->target_freq = saved;
        }

        if (!a->ks_active || a->ks_delay_len < 2) {
            memset(out, 0, frames * sizeof(float));
            return;
        }

        for (int i = 0; i < frames; i++) {
            /* Read current sample from delay line */
            float cur = a->ks_delay[a->ks_pos];

            /* Lowpass: average current and previous, with damping */
            int next_pos = (a->ks_pos + 1) % a->ks_delay_len;
            float next = a->ks_delay[next_pos];
            float filtered = damp * 0.5f * (cur + next);

            /* Write back into delay line */
            a->ks_delay[a->ks_pos] = filtered;
            a->ks_pos = next_pos;

            out[i] = cur * a->volume;
        }
    } else {
        /* --- Continuous sine --- */
        int attack = a->ref_attack_len;
        double phase_inc = TWO_PI * ref_freq / a->sample_rate;

        for (int i = 0; i < frames; i++) {
            float env = 1.0f;
            if (a->ref_attack_pos < attack) {
                env = (float)a->ref_attack_pos / (float)attack;
                a->ref_attack_pos++;
            }
            out[i] = sine_sample(a->phase) * a->volume * 0.5f * env;
            a->phase += phase_inc;
            if (a->phase >= TWO_PI) a->phase -= TWO_PI;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Public render entry point                                                   */
/* -------------------------------------------------------------------------- */

void tuner_audio_render(tuner_audio_t *audio, float *out, int frames) {
    if (!audio) {
        memset(out, 0, frames * sizeof(float));
        return;
    }

    switch (audio->mode) {
        case TUNER_FB_STEP_GUIDE:
            render_step_guide(audio, out, frames);
            break;
        case TUNER_FB_REFERENCE:
            render_reference(audio, out, frames);
            break;
        case TUNER_FB_OFF:
        default:
            memset(out, 0, frames * sizeof(float));
            break;
    }
}

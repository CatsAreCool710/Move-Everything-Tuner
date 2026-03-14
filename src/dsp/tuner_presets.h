/*
 * tuner_presets.h - Instrument tuning definitions
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
 * Each preset defines a set of target notes (as MIDI note numbers)
 * for a specific instrument and tuning. Presets are grouped by
 * category (guitar, bass, bowed, etc.) for navigation.
 *
 * MIDI note reference:
 *   C2=36  C#2=37  D2=38  D#2=39  E2=40  F2=41  F#2=42
 *   G2=43  G#2=44  A2=45  A#2=46  B2=47
 *   C3=48  C#3=49  D3=50  D#3=51  E3=52  F3=53  F#3=54
 *   G3=55  G#3=56  A3=57  A#3=58  B3=59
 *   C4=60  C#4=61  D4=62  D#4=63  E4=64  F4=65  F#4=66
 *   G4=67  G#4=68  A4=69  A#4=70  B4=71
 *   C5=72  D5=74  E5=76
 *
 * Some tunings sourced from bashtuner by Storm Dragon and Jeremiah Ticket (WTFPL license):
 *   https://git.stormux.org/storm/bashtuner
 */

#ifndef TUNER_PRESETS_H
#define TUNER_PRESETS_H

#include <stdlib.h>  /* abs */

#define TUNER_MAX_STRINGS 12

/* Default reference tone styles per instrument family */
#define TUNER_REF_DEFAULT_SINE       0
#define TUNER_REF_DEFAULT_PLUCK      1
#define TUNER_REF_DEFAULT_SOFT_PLUCK 2

typedef struct {
    const char *id;              /* Internal ID (matches set_param string) */
    const char *name;            /* Display name */
    int         num_strings;     /* Number of strings (0 for chromatic) */
    int         notes[TUNER_MAX_STRINGS]; /* MIDI note numbers per string */
    const char *labels[TUNER_MAX_STRINGS]; /* String labels (e.g. "E2") */
    int         default_ref_style; /* 0=sine, 1=pluck, 2=soft_pluck */
} tuner_preset_t;

/* Simple inline string comparison (avoids strcmp dependency) */
static inline int tuner_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == 0 && *b == 0);
}

/*
 * Preset definitions — grouped by category.
 *
 * Categories:
 *   0: Chromatic  (index 0)
 *   1: Guitar     (indices 1-9)
 *   2: 12-String  (indices 10-11)
 *   3: Bass       (indices 12-13)
 *   4: Ukulele    (indices 14-15)
 *   5: Lap Steel  (index 16)
 *   6: Bowed      (indices 17-19)
 *   7: Other      (indices 20-21)
 *
 * Strings are ordered by physical position (thickest to thinnest).
 * Re-entrant tunings (ukulele, banjo, 12-string) may have a high
 * string first, so MIDI notes are NOT necessarily ascending.
 */
static const tuner_preset_t TUNER_PRESETS[] = {

    /* ---- Category 0: Chromatic ---- */

    { "chromatic", "Chromatic", 0, {0}, {0},
      TUNER_REF_DEFAULT_SINE },

    /* ---- Category 1: Guitar (indices 1-9) ---- */

    { "guitar", "Guitar", 6,
      {40, 45, 50, 55, 59, 64},
      {"E2", "A2", "D3", "G3", "B3", "E4"},
      TUNER_REF_DEFAULT_PLUCK },

    { "guitar_halfdown", "Gtr Half Step Dn", 6,
      {39, 44, 49, 54, 58, 63},
      {"D#2", "G#2", "C#3", "F#3", "A#3", "D#4"},
      TUNER_REF_DEFAULT_PLUCK },

    { "guitar_dstd", "Gtr D Standard", 6,
      {38, 43, 48, 53, 57, 62},
      {"D2", "G2", "C3", "F3", "A3", "D4"},
      TUNER_REF_DEFAULT_PLUCK },

    { "guitar_dropd", "Gtr Drop D", 6,
      {38, 45, 50, 55, 59, 64},
      {"D2", "A2", "D3", "G3", "B3", "E4"},
      TUNER_REF_DEFAULT_PLUCK },

    { "guitar_dropdg", "Gtr Drop DG", 6,
      {38, 43, 50, 55, 59, 64},
      {"D2", "G2", "D3", "G3", "B3", "E4"},
      TUNER_REF_DEFAULT_PLUCK },

    { "guitar_opend", "Gtr Open D", 6,
      {38, 45, 50, 54, 57, 62},
      {"D2", "A2", "D3", "F#3", "A3", "D4"},
      TUNER_REF_DEFAULT_PLUCK },

    { "guitar_openg", "Gtr Open G", 6,
      {38, 43, 50, 55, 59, 62},
      {"D2", "G2", "D3", "G3", "B3", "D4"},
      TUNER_REF_DEFAULT_PLUCK },

    { "guitar_dadgad", "Gtr DADGAD", 6,
      {38, 45, 50, 55, 57, 62},
      {"D2", "A2", "D3", "G3", "A3", "D4"},
      TUNER_REF_DEFAULT_PLUCK },

    { "guitar_nickdrake", "Gtr Nick Drake", 6,
      {36, 43, 48, 53, 60, 64},
      {"C2", "G2", "C3", "F3", "C4", "E4"},
      TUNER_REF_DEFAULT_PLUCK },

    /* ---- Category 2: 12-String (indices 10-11) ---- */

    /* 12-string standard: paired courses (octave + unison for top 2) */
    { "12string", "12-String", 10,
      {52, 40, 57, 45, 62, 50, 67, 55, 59, 64},
      {"E3", "E2", "A3", "A2", "D4", "D3", "G4", "G3", "B3", "E4"},
      TUNER_REF_DEFAULT_PLUCK },

    /* 12-string dropped 1 step */
    { "12string_d", "12-Str D Std", 10,
      {50, 38, 55, 43, 60, 48, 65, 53, 57, 62},
      {"D3", "D2", "G3", "G2", "C4", "C3", "F4", "F3", "A3", "D4"},
      TUNER_REF_DEFAULT_PLUCK },

    /* ---- Category 3: Bass (indices 12-13) ---- */

    { "bass", "Bass", 4,
      {28, 33, 38, 43},
      {"E1", "A1", "D2", "G2"},
      TUNER_REF_DEFAULT_PLUCK },

    { "bass5", "Bass 5-String", 5,
      {23, 28, 33, 38, 43},
      {"B0", "E1", "A1", "D2", "G2"},
      TUNER_REF_DEFAULT_PLUCK },

    /* ---- Category 4: Ukulele (indices 14-15) ---- */

    /* Standard re-entrant (high G) */
    { "ukulele", "Ukulele", 4,
      {67, 60, 64, 69},
      {"G4", "C4", "E4", "A4"},
      TUNER_REF_DEFAULT_PLUCK },

    { "ukulele_halfdown", "Uke Half Step Dn", 4,
      {66, 59, 63, 68},
      {"F#4", "B3", "D#4", "G#4"},
      TUNER_REF_DEFAULT_PLUCK },

    /* ---- Category 5: Lap Steel (index 16) ---- */

    { "steel_c6", "Lap Steel C6", 6,
      {48, 52, 55, 57, 60, 64},
      {"C3", "E3", "G3", "A3", "C4", "E4"},
      TUNER_REF_DEFAULT_PLUCK },

    /* ---- Category 6: Bowed (indices 17-19) ---- */

    { "violin", "Violin", 4,
      {55, 62, 69, 76},
      {"G3", "D4", "A4", "E5"},
      TUNER_REF_DEFAULT_SOFT_PLUCK },

    { "viola", "Viola", 4,
      {48, 55, 62, 69},
      {"C3", "G3", "D4", "A4"},
      TUNER_REF_DEFAULT_SOFT_PLUCK },

    { "cello", "Cello", 4,
      {36, 43, 50, 57},
      {"C2", "G2", "D3", "A3"},
      TUNER_REF_DEFAULT_SOFT_PLUCK },

    /* ---- Category 7: Other (indices 20-21) ---- */

    { "mandolin", "Mandolin", 4,
      {55, 62, 69, 76},
      {"G3", "D4", "A4", "E5"},
      TUNER_REF_DEFAULT_PLUCK },

    /* Banjo - 5 string re-entrant (open G) */
    { "banjo", "Banjo", 5,
      {67, 50, 55, 59, 62},
      {"G4", "D3", "G3", "B3", "D4"},
      TUNER_REF_DEFAULT_PLUCK },
};

#define TUNER_NUM_PRESETS (int)(sizeof(TUNER_PRESETS) / sizeof(TUNER_PRESETS[0]))

/* Find the preset index by ID string. Returns -1 if not found. */
static inline int tuner_find_preset_index(const char *id) {
    for (int i = 0; i < TUNER_NUM_PRESETS; i++) {
        if (tuner_streq(TUNER_PRESETS[i].id, id)) return i;
    }
    return -1;
}

/* Given a detected MIDI note, find the closest string in a preset.
 * Returns the string index (0-based), or -1 if chromatic. */
static inline int tuner_find_closest_string(const tuner_preset_t *preset, int midi_note) {
    if (!preset || preset->num_strings == 0) return -1;

    int best = 0;
    int best_dist = 1000;
    for (int i = 0; i < preset->num_strings; i++) {
        int dist = abs(midi_note - preset->notes[i]);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

#endif /* TUNER_PRESETS_H */

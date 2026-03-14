/*
 * ui.js - Move Everything Tuner UI
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
 * Interactive tool module UI that:
 *   - Polls DSP for pitch detection results
 *   - Renders tuner display (128x64, 1-bit monochrome)
 *   - Handles knob/button/jog/arrow input
 *   - Drives screen reader announcements (autospeak)
 *   - Uses ME shared utilities for menu system and input filtering
 */

/* -------------------------------------------------------------------------- */
/* Imports from Move Everything shared utilities (absolute paths)              */
/* -------------------------------------------------------------------------- */

import { announce, announceParameter, announceView }
    from '/data/UserData/move-anything/shared/screen_reader.mjs';
import { shouldFilterMessage, decodeDelta }
    from '/data/UserData/move-anything/shared/input_filter.mjs';
import {
    MidiCC, MidiNoteOn,
    MoveKnob1, MoveKnob5, MoveKnob8,
    MoveMainKnob, MoveMainButton, MoveShift, MoveBack, MoveMenu,
    MoveUp, MoveDown, MoveLeft, MoveRight
} from '/data/UserData/move-anything/shared/constants.mjs';
import { createValue, createToggle, createEnum, formatItemValue }
    from '/data/UserData/move-anything/shared/menu_items.mjs';
import { createMenuState, handleMenuInput }
    from '/data/UserData/move-anything/shared/menu_nav.mjs';
import { createMenuStack }
    from '/data/UserData/move-anything/shared/menu_stack.mjs';
import { drawHierarchicalMenu }
    from '/data/UserData/move-anything/shared/menu_render.mjs';

/* -------------------------------------------------------------------------- */
/* Constants                                                                   */
/* -------------------------------------------------------------------------- */

const DISPLAY_W = 128;
const DISPLAY_H = 64;
const CHAR_W = 6;   /* 5x7 font, 6px per char including spacing */
const CHAR_H = 8;

/* Instrument preset IDs (must match tuner_presets.h order) */
const MODE_IDS = [
    'chromatic',                                                       /* 0  */
    'guitar', 'guitar_halfdown', 'guitar_dstd', 'guitar_dropd',       /* 1-4 */
    'guitar_dropdg', 'guitar_opend', 'guitar_openg', 'guitar_dadgad', /* 5-8 */
    'guitar_nickdrake',                                                /* 9  */
    '12string', '12string_d',                                          /* 10-11 */
    'bass', 'bass5',                                                   /* 12-13 */
    'ukulele', 'ukulele_halfdown',                                     /* 14-15 */
    'steel_c6',                                                        /* 16 */
    'violin', 'viola', 'cello',                                        /* 17-19 */
    'mandolin', 'banjo'                                                /* 20-21 */
];

const MODE_NAMES = [
    'Chromatic',
    'Guitar', 'Gtr Half Step Dn', 'Gtr D Standard', 'Gtr Drop D',
    'Gtr Drop DG', 'Gtr Open D', 'Gtr Open G', 'Gtr DADGAD',
    'Gtr Nick Drake',
    '12-String', '12-Str D Std',
    'Bass', 'Bass 5-String',
    'Ukulele', 'Uke Half Step Dn',
    'Lap Steel C6',
    'Violin', 'Viola', 'Cello',
    'Mandolin', 'Banjo'
];

/* String counts per preset (must match tuner_presets.h order) */
const MODE_STRING_COUNTS = [
    0,                         /* chromatic */
    6, 6, 6, 6, 6, 6, 6, 6, 6, /* guitar (9 tunings) */
    10, 10,                    /* 12-string */
    4, 5,                      /* bass */
    4, 4,                      /* ukulele */
    6,                         /* lap steel */
    4, 4, 4,                   /* bowed */
    4, 5                       /* mandolin, banjo */
];

/* Default ref style per preset (must match tuner_presets.h order) */
const PRESET_DEFAULT_REF_STYLES = [
    'sine',                                                   /* chromatic */
    'pluck', 'pluck', 'pluck', 'pluck', 'pluck',             /* guitar */
    'pluck', 'pluck', 'pluck', 'pluck',                       /* guitar cont. */
    'pluck', 'pluck',                                         /* 12-string */
    'pluck', 'pluck',                                         /* bass */
    'pluck', 'pluck',                                         /* ukulele */
    'pluck',                                                  /* lap steel */
    'soft_pluck', 'soft_pluck', 'soft_pluck',                 /* bowed */
    'pluck', 'pluck'                                          /* mandolin, banjo */
];

/* Preset categories for Shift+Jog navigation */
const CATEGORIES = [
    { name: 'Chromatic',  start: 0,  end: 0 },
    { name: 'Guitar',     start: 1,  end: 9 },
    { name: '12-String',  start: 10, end: 11 },
    { name: 'Bass',       start: 12, end: 13 },
    { name: 'Ukulele',    start: 14, end: 15 },
    { name: 'Lap Steel',  start: 16, end: 16 },
    { name: 'Bowed',      start: 17, end: 19 },
    { name: 'Other',      start: 20, end: 21 },
];

/* Feedback modes: step_guide, reference, off */
const FEEDBACK_IDS = ['step_guide', 'reference', 'off'];
const FEEDBACK_NAMES = ['Step Guide', 'Reference Tone', 'Off'];

const GUIDE_OCTAVE_IDS = ['auto', 'match'];
const GUIDE_OCTAVE_NAMES = ['Auto', 'Match'];

const REF_STYLE_IDS = ['sine', 'pluck', 'soft_pluck'];
const REF_STYLE_NAMES = ['Sine', 'Pluck', 'Soft Pluck'];

/* Chromatic note names for display and announcement */
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

/* Autospeak timing */
const AUTOSPEAK_DEBOUNCE_MS = 2000;
const AUTOSPEAK_CLOSE_DEBOUNCE_MS = 3000;
const NOTE_STABLE_MS = 800;

/* -------------------------------------------------------------------------- */
/* Drawing helpers                                                             */
/* -------------------------------------------------------------------------- */

function hLine(x, y, w, c) {
    fill_rect(x, y, w, 1, c);
}

function vLine(x, y, h, c) {
    fill_rect(x, y, 1, h, c);
}

function drawRectOutline(x, y, w, h, c) {
    fill_rect(x, y, w, 1, c);         /* top */
    fill_rect(x, y + h - 1, w, 1, c); /* bottom */
    fill_rect(x, y, 1, h, c);         /* left */
    fill_rect(x + w - 1, y, 1, h, c); /* right */
}

function printCentered(y, text, color) {
    const w = text.length * CHAR_W;
    const x = Math.max(0, Math.floor((DISPLAY_W - w) / 2));
    print(x, y, text, color === undefined ? 1 : color);
}

function printRight(y, text, color) {
    const w = text.length * CHAR_W;
    const x = Math.max(0, DISPLAY_W - w - 1);
    print(x, y, text, color === undefined ? 1 : color);
}

/* -------------------------------------------------------------------------- */
/* Utility: MIDI note to name                                                  */
/* -------------------------------------------------------------------------- */

function midiToNoteName(midi) {
    if (midi < 0 || midi > 127) return '---';
    const noteIdx = midi % 12;
    const octave = Math.floor(midi / 12) - 1;
    return NOTE_NAMES[noteIdx] + octave;
}

/* Screen-reader friendly: "C sharp 4" instead of "C#4" */
function midiToSpokenName(midi) {
    if (midi < 0 || midi > 127) return 'unknown';
    const noteIdx = midi % 12;
    const octave = Math.floor(midi / 12) - 1;
    const name = NOTE_NAMES[noteIdx].replace('#', ' sharp');
    return name + ' ' + octave;
}

/* Convert string label "E2" or "G#3" to spoken form "E 2" or "G sharp 3" */
function labelToSpoken(label) {
    if (!label || label === '---') return 'unknown';
    return label.replace('#', ' sharp').replace(/([A-G](?:\ssharp)?)(\d)/, '$1 $2');
}

/* -------------------------------------------------------------------------- */
/* State                                                                       */
/* -------------------------------------------------------------------------- */

let shiftHeld = false;
let inMenu = false;

/* Values polled from DSP */
let detectedNote = '---';
let detectedFreq = 0;
let centsOffset = 0;
let inTune = false;
let hasSignal = false;
let targetNote = '---';

/* Settings (mirrors DSP state) */
let modeIndex = 0;
let feedbackIndex = 0;     /* 0=step_guide */
let autospeakOn = true;
let a4Ref = 440;
let guideOctave = 'auto';
let refStyle = 'sine';
let passthrough = false;
let feedbackVolume = 40;
let passthroughVolume = 0;
let tuneThreshold = 3;
let noiseGate = 20;        /* matches module.json default */
let stringIndex = 0;
let stringCount = 0;
let stringLabel = '---';
let autoDetect = false;    /* manual note selection by default */
let manualMidi = 60;       /* C4 */
let refStyleAuto = true;   /* auto-select ref style per instrument */
let guideToneMs = 200;     /* step guide note duration */
let guideGapMs = 40;       /* step guide gap between notes */
let refMuteInput = true;   /* mute input knobs in reference mode */
let categoryIndex = 0;     /* current category (derived from modeIndex) */

/* Autospeak tracking */
let lastSpokenNote = '';
let lastSpokenCents = 0;
let lastSpeakTime = 0;
let noteStableStart = 0;
let noteStableNote = '';
let hasSpokenInitial = false;
let hasSpokenInTune = false;

/* Tick counter for polling rate */
let tickCount = 0;

/* -------------------------------------------------------------------------- */
/* Menu system                                                                 */
/* -------------------------------------------------------------------------- */

let menuState = null;
let menuStack = null;

function buildMenuItems() {
    return [
        createEnum('Instrument', {
            get: function() { return MODE_IDS[modeIndex]; },
            set: function(val) {
                modeIndex = MODE_IDS.indexOf(val);
                if (modeIndex < 0) modeIndex = 0;
                categoryIndex = getCategoryForPreset(modeIndex);
                stringIndex = 0;
                /* DSP resets string_index internally on preset change */
                queueParam('tn_inst', String(modeIndex));
                stringCount = MODE_STRING_COUNTS[modeIndex];
            },
            options: MODE_IDS,
            format: function(val) { return MODE_NAMES[MODE_IDS.indexOf(val)] || val; }
        }),
        createEnum('Feedback', {
            get: function() { return FEEDBACK_IDS[feedbackIndex]; },
            set: function(val) {
                feedbackIndex = FEEDBACK_IDS.indexOf(val);
                if (feedbackIndex < 0) feedbackIndex = 1; /* off */
                queueParam('feedback_mode', val);
            },
            options: FEEDBACK_IDS,
            format: function(val) { return FEEDBACK_NAMES[FEEDBACK_IDS.indexOf(val)] || val; }
        }),
        createToggle('Auto Detect', {
            get: function() { return autoDetect; },
            set: function(val) {
                autoDetect = val;
                queueParam('auto_detect', val ? 'on' : 'off');
            }
        }),
        createToggle('Autospeak', {
            get: function() { return autospeakOn; },
            set: function(val) {
                autospeakOn = val;
                queueParam('autospeak', val ? 'on' : 'off');
            }
        }),
        createValue('A4 Reference', {
            get: function() { return a4Ref; },
            set: function(val) {
                a4Ref = val;
                sendParamNow('a4_ref', String(val));
            },
            min: 410, max: 480, step: 1,
            format: function(v) { return v + ' Hz'; }
        }),
        createEnum('Guide Octave', {
            get: function() { return guideOctave; },
            set: function(val) {
                guideOctave = val;
                queueParam('guide_octave', val);
            },
            options: GUIDE_OCTAVE_IDS,
            format: function(val) { return GUIDE_OCTAVE_NAMES[GUIDE_OCTAVE_IDS.indexOf(val)] || val; }
        }),
        createEnum('Ref Style', {
            get: function() { return refStyle; },
            set: function(val) {
                refStyle = val;
                queueParam('ref_style', val);
            },
            options: REF_STYLE_IDS,
            format: function(val) { return REF_STYLE_NAMES[REF_STYLE_IDS.indexOf(val)] || val; }
        }),
        createToggle('Auto Ref Style', {
            get: function() { return refStyleAuto; },
            set: function(val) {
                refStyleAuto = val;
                queueParam('ref_style_auto', val ? 'on' : 'off');
            }
        }),
        createValue('Tone Length', {
            get: function() { return guideToneMs; },
            set: function(val) {
                guideToneMs = val;
                queueParam('guide_tone_ms', String(val));
            },
            min: 50, max: 500, step: 25,
            format: function(v) { return v + ' ms'; }
        }),
        createValue('Tone Gap', {
            get: function() { return guideGapMs; },
            set: function(val) {
                guideGapMs = val;
                queueParam('guide_gap_ms', String(val));
            },
            min: 10, max: 200, step: 10,
            format: function(v) { return v + ' ms'; }
        }),
        createToggle('Passthrough', {
            get: function() { return passthrough; },
            set: function(val) {
                passthrough = val;
                queueParam('passthrough', val ? 'on' : 'off');
            }
        }),
        createToggle('Ref Mutes Input', {
            get: function() { return refMuteInput; },
            set: function(val) {
                refMuteInput = val;
                queueParam('ref_mute_input', val ? 'on' : 'off');
            }
        }),
        createValue('Feedback Vol', {
            get: function() { return feedbackVolume; },
            set: function(val) {
                feedbackVolume = val;
                sendParamNow('feedback_volume', String(val));
            },
            min: 0, max: 100, step: 5,
            format: function(v) { return v + '%'; }
        }),
        createValue('Passthru Vol', {
            get: function() { return passthroughVolume; },
            set: function(val) {
                passthroughVolume = val;
                sendParamNow('passthrough_volume', String(val));
            },
            min: 0, max: 100, step: 5,
            format: function(v) { return v + '%'; }
        }),
        createValue('Threshold', {
            get: function() { return tuneThreshold; },
            set: function(val) {
                tuneThreshold = val;
                sendParamNow('tune_threshold', String(val));
            },
            min: 1, max: 10, step: 1,
            format: function(v) { return v + ' cents'; }
        }),
        createValue('Noise Gate', {
            get: function() { return noiseGate; },
            set: function(val) {
                noiseGate = val;
                sendParamNow('noise_gate', String(val));
            },
            min: 0, max: 100, step: 5,
            format: function(v) { return v + '%'; }
        }),
    ];
}

let menuItems = [];

/* -------------------------------------------------------------------------- */
/* DSP communication                                                           */
/* -------------------------------------------------------------------------- */

/*
 * Parameter queue — the ME framework coalesces back-to-back set_param calls,
 * so only one param per tick is delivered to the DSP plugin. Use queueParam()
 * for commands that must not be dropped. Use sendParamNow() for real-time
 * controls where only the latest value matters (knobs, volume, etc).
 * Pattern borrowed from the DJ Deck module.
 */
let paramQueue = [];

function getParam(key) {
    try { return host_module_get_param(key); }
    catch (e) { return ''; }
}

/* Send immediately — use for real-time knob values */
function sendParamNow(key, val) {
    try { host_module_set_param(key, String(val)); }
    catch (e) { /* ignore */ }
}

/* Queue for next tick — use for commands that must not be dropped */
function queueParam(key, val) {
    paramQueue.push([key, String(val)]);
}

/* Drain one queued param per tick (called from tick()) */
function drainParamQueue() {
    if (paramQueue.length > 0) {
        let p = paramQueue.shift();
        try { host_module_set_param(p[0], p[1]); }
        catch (e) { /* ignore */ }
    }
}

function pollDSP() {
    detectedNote = getParam('detected_note') || '---';
    detectedFreq = parseFloat(getParam('detected_freq')) || 0;
    centsOffset = parseInt(getParam('cents_offset')) || 0;
    inTune = getParam('in_tune') === '1';
    hasSignal = getParam('has_signal') === '1';
    targetNote = getParam('target_note') || '---';

    /* String count from local table (always in sync with modeIndex) */
    stringCount = MODE_STRING_COUNTS[modeIndex];
    if (stringCount > 0) {
        stringLabel = getParam('string_label') || '---';
    }

    /* Sync string_index from DSP (auto-detect may change it) */
    if (autoDetect && stringCount > 0) {
        const dspIdx = parseInt(getParam('string_index'));
        if (!isNaN(dspIdx)) {
            stringIndex = dspIdx;
            stringLabel = getParam('string_label') || '---';
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Autospeak logic                                                             */
/* -------------------------------------------------------------------------- */

function autospeakTick() {
    if (!autospeakOn || !hasSignal) {
        /* Reset tracking when no signal */
        noteStableNote = '';
        noteStableStart = 0;
        hasSpokenInitial = false;
        hasSpokenInTune = false;
        return;
    }

    const now = Date.now();

    /* Track note stability */
    if (detectedNote !== noteStableNote) {
        noteStableNote = detectedNote;
        noteStableStart = now;
        hasSpokenInitial = false;
        hasSpokenInTune = false;
    }

    const stableTime = now - noteStableStart;
    const timeSinceSpeak = now - lastSpeakTime;

    /* Speak initial detection after note is stable.
     * In reference mode (no detection), offset speech is gated on passthrough
     * since without hearing your instrument the spoken offset isn't actionable.
     * In step guide and off modes, always speak the full offset — the user
     * is actively playing and needs to know how far off they are. */
    const isRefMode = feedbackIndex === 1;
    const speakOffset = !isRefMode || passthrough;

    if (!hasSpokenInitial && stableTime >= NOTE_STABLE_MS) {
        if (speakOffset) {
            const dir = centsOffset > 0 ? 'sharp' : centsOffset < 0 ? 'flat' : '';
            if (inTune) {
                announce(detectedNote + ', in tune');
                hasSpokenInTune = true;
            } else {
                announce(detectedNote + ', ' + Math.abs(centsOffset) + ' cents ' + dir);
            }
        } else {
            /* Reference mode without passthrough — just announce the note name */
            announce(detectedNote);
        }
        hasSpokenInitial = true;
        lastSpokenNote = detectedNote;
        lastSpokenCents = centsOffset;
        lastSpeakTime = now;
        return;
    }

    /* Periodic cents updates: in reference mode, only with passthrough */
    if (!speakOffset) return;

    /* In-tune confirmation (speak once) */
    if (hasSpokenInitial && !hasSpokenInTune && inTune) {
        if (timeSinceSpeak >= 500) {
            announce(detectedNote + ', in tune');
            hasSpokenInTune = true;
            lastSpeakTime = now;
        }
        return;
    }

    /* Periodic updates while held — back off when close */
    if (hasSpokenInitial && !inTune) {
        const debounce = Math.abs(centsOffset) < 10
            ? AUTOSPEAK_CLOSE_DEBOUNCE_MS
            : AUTOSPEAK_DEBOUNCE_MS;

        if (timeSinceSpeak >= debounce) {
            const dir = centsOffset > 0 ? 'sharp' : 'flat';
            announce(Math.abs(centsOffset) + ' cents ' + dir);
            lastSpokenCents = centsOffset;
            lastSpeakTime = now;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Display rendering                                                           */
/* -------------------------------------------------------------------------- */

function drawTuner() {
    clear_screen();

    /* Header: instrument name + A4 ref */
    const modeName = MODE_NAMES[modeIndex] || 'Chromatic';
    print(1, 0, modeName, 1);
    printRight(0, 'A=' + a4Ref, 1);
    hLine(0, 9, DISPLAY_W, 1);

    if (!hasSignal) {
        /* No signal */
        if (autoDetect) {
            printCentered(22, 'Listening...', 1);
            printCentered(34, 'Play a note', 1);
        } else {
            /* Show the target note even when no signal */
            const tgt = (stringCount > 0) ? stringLabel : midiToNoteName(manualMidi);
            printCentered(16, 'Target: ' + tgt, 1);
            printCentered(30, 'Listening...', 1);
            printCentered(40, 'Play a note', 1);
        }
    } else {
        /* Detected note (large, centered) */
        printCentered(13, detectedNote, 1);

        /* Cents meter bar */
        const meterY = 28;
        const meterW = 100;
        const meterH = 5;
        const meterX = Math.floor((DISPLAY_W - meterW) / 2);
        const meterMid = meterX + Math.floor(meterW / 2);

        drawRectOutline(meterX, meterY, meterW, meterH, 1);
        vLine(meterMid, meterY - 2, meterH + 4, 1);

        const needlePos = meterMid + Math.round(centsOffset * (meterW / 2) / 50);
        const clampedNeedle = Math.max(meterX + 1, Math.min(meterX + meterW - 2, needlePos));

        if (inTune) {
            fill_rect(meterMid - 3, meterY + 1, 7, meterH - 2, 1);
        } else {
            vLine(clampedNeedle, meterY - 1, meterH + 2, 1);
            vLine(clampedNeedle - 1, meterY, meterH, 1);
        }

        /* Cents text */
        const centsStr = (centsOffset > 0 ? '+' : '') + centsOffset + 'c';
        printCentered(36, inTune ? 'IN TUNE' : centsStr, 1);

        /* Target note (instrument mode or manual chromatic) */
        if (stringCount > 0 && targetNote !== '---') {
            printCentered(46, 'Str: ' + stringLabel, 1);
        } else if (!autoDetect) {
            printCentered(46, 'Tgt: ' + midiToNoteName(manualMidi), 1);
        }
    }

    /* Footer */
    hLine(0, 53, DISPLAY_W, 1);

    let footerLeft;
    if (stringCount > 0) {
        footerLeft = 'Str:' + stringLabel;
    } else if (!autoDetect) {
        footerLeft = midiToNoteName(manualMidi);
    } else {
        footerLeft = 'Auto';
    }

    const fbShort = feedbackIndex === 0 ? 'Stp' : feedbackIndex === 1 ? 'Ref' : 'Off';
    const footerRight = fbShort + (autospeakOn ? ' Spk' : '');
    print(1, 55, footerLeft, 1);
    printRight(55, footerRight, 1);
}

function drawMenu() {
    clear_screen();
    drawHierarchicalMenu({
        title: 'Tuner Settings',
        items: menuItems,
        state: menuState,
        footer: null
    });
}

/* -------------------------------------------------------------------------- */
/* Input handling                                                              */
/* -------------------------------------------------------------------------- */

function handleInput(cc, value) {
    /* Shift tracking */
    if (cc === MoveShift) {
        shiftHeld = (value > 0);
        return;
    }

    /* Menu toggle */
    if (cc === MoveMenu && value > 0) {
        inMenu = !inMenu;
        if (inMenu) {
            menuItems = buildMenuItems();
            menuState = createMenuState();
            announceView('Settings');
        } else {
            announceView('Tuner');
        }
        return;
    }

    /* Menu mode: delegate to shared menu system with screen reader announcements */
    if (inMenu) {
        const prevIndex = menuState.selectedIndex;
        const prevEditing = menuState.editing;
        const prevEditValue = menuState.editValue;

        const result = handleMenuInput({
            cc: cc,
            value: value,
            items: menuItems,
            state: menuState,
            stack: menuStack,
            onBack: function() {
                inMenu = false;
                announceView('Tuner');
            },
            shiftHeld: shiftHeld
        });

        if (result.needsRedraw) {
            const item = menuItems[menuState.selectedIndex];
            if (!item) { return; }
            const label = item.label || '';

            if (menuState.selectedIndex !== prevIndex && !menuState.editing) {
                /* Navigated to a different item — announce label + value */
                const val = formatItemValue(item, false, null);
                announceParameter(label, val);
            } else if (menuState.editing && !prevEditing) {
                /* Entered edit mode */
                const val = formatItemValue(item, true, menuState.editValue);
                announce('Editing ' + label + ', ' + val.replace(/[\[\]]/g, ''));
            } else if (menuState.editing && menuState.editValue !== prevEditValue) {
                /* Value changed while editing */
                const val = formatItemValue(item, true, menuState.editValue);
                announce(val.replace(/[\[\]]/g, ''));
            } else if (!menuState.editing && prevEditing) {
                /* Exited edit mode (confirmed or cancelled) */
                const val = formatItemValue(item, false, null);
                announceParameter(label, val);
            } else if (!menuState.editing && !prevEditing &&
                       menuState.selectedIndex === prevIndex) {
                /* Same index, not editing — toggle click or quick adjust */
                if (item.type === 'toggle' || item.type === 'value' ||
                    item.type === 'enum') {
                    const val = formatItemValue(item, false, null);
                    announceParameter(label, val);
                }
            }
        }
        return;
    }

    /* === Tuner view input === */

    /* Back button */
    if (cc === MoveBack && value > 0) {
        if (shiftHeld) {
            autospeakOn = !autospeakOn;
            sendParamNow('autospeak', autospeakOn ? 'on' : 'off');
            announce('Autospeak ' + (autospeakOn ? 'on' : 'off'));
        } else {
            try { host_exit_module(); } catch (e) { /* ignore */ }
        }
        return;
    }

    /* Jog click: announce tuning state, or Shift+click: cycle feedback mode */
    if (cc === MoveMainButton && value > 0) {
        if (shiftHeld) {
            /* Shift+Jog click: cycle feedback mode */
            feedbackIndex = (feedbackIndex + 1) % FEEDBACK_IDS.length;
            sendParamNow('feedback_mode', FEEDBACK_IDS[feedbackIndex]);
            announce('Feedback: ' + FEEDBACK_NAMES[feedbackIndex]);
        } else {
            /* Jog click: announce current tuning state */
            if (hasSignal) {
                if (inTune) {
                    announce(detectedNote + ', in tune');
                } else {
                    const dir = centsOffset > 0 ? 'sharp' : centsOffset < 0 ? 'flat' : 'in tune';
                    announce(detectedNote + ', ' + Math.abs(centsOffset) + ' cents ' + dir);
                }
            } else {
                const tgt = (stringCount > 0) ? stringLabel : midiToNoteName(manualMidi);
                announce('No signal. Target: ' + tgt + '. Play a note to tune.');
            }
        }
        return;
    }

    /* Jog wheel: cycle presets within category, or Shift+Jog: jump categories */
    if (cc === MoveMainKnob) {
        const delta = decodeDelta(value);

        if (shiftHeld) {
            /* Shift+Jog: jump to next/previous category */
            categoryIndex = (categoryIndex + delta + CATEGORIES.length) % CATEGORIES.length;
            modeIndex = CATEGORIES[categoryIndex].start;
            stringIndex = 0;
            stringCount = MODE_STRING_COUNTS[modeIndex];
            queueParam('tn_inst', String(modeIndex));
            if (refStyleAuto) {
                refStyle = PRESET_DEFAULT_REF_STYLES[modeIndex];
            }
            if (stringCount > 0) {
                stringLabel = getParam('string_label') || '---';
            }
            announce(CATEGORIES[categoryIndex].name + '. ' + MODE_NAMES[modeIndex]);
        } else {
            /* Jog: cycle presets within current category */
            const cat = CATEGORIES[categoryIndex];
            const range = cat.end - cat.start + 1;
            const offset = modeIndex - cat.start;
            modeIndex = cat.start + ((offset + delta + range) % range);
            stringIndex = 0;
            stringCount = MODE_STRING_COUNTS[modeIndex];
            queueParam('tn_inst', String(modeIndex));
            if (refStyleAuto) {
                refStyle = PRESET_DEFAULT_REF_STYLES[modeIndex];
            }
            if (stringCount > 0) {
                stringLabel = getParam('string_label') || '---';
            }
            announce(MODE_NAMES[modeIndex]);
        }
        return;
    }

    /* Arrow buttons: note/string selection (only when auto-detect is OFF) */
    if ((cc === MoveUp || cc === MoveDown || cc === MoveLeft || cc === MoveRight)
        && value > 0 && !autoDetect) {

        /* Use local string count — always in sync with modeIndex */
        stringCount = MODE_STRING_COUNTS[modeIndex];

        if (stringCount > 0) {
            /* Instrument mode: up/down cycle strings */
            if (cc === MoveUp) {
                stringIndex = (stringIndex + 1) % stringCount;
            } else if (cc === MoveDown) {
                stringIndex = (stringIndex - 1 + stringCount) % stringCount;
            }
            /* Left/Right: unused in instrument mode */
            if (cc === MoveUp || cc === MoveDown) {
                sendParamNow('string_index', String(stringIndex));
                stringLabel = getParam('string_label') || '---';
                announce('String ' + (stringIndex + 1) + '. ' + labelToSpoken(stringLabel));
            }
        } else {
            /* Chromatic mode: up/down = semitone, left/right = octave */
            if (cc === MoveUp) {
                manualMidi = clamp(manualMidi + 1, 0, 127);
            } else if (cc === MoveDown) {
                manualMidi = clamp(manualMidi - 1, 0, 127);
            } else if (cc === MoveRight) {
                manualMidi = clamp(manualMidi + 12, 0, 127);
            } else if (cc === MoveLeft) {
                manualMidi = clamp(manualMidi - 12, 0, 127);
            }
            sendParamNow('manual_midi', String(manualMidi));
            announce(midiToSpokenName(manualMidi));
        }
        return;
    }

    /* Knobs (relative encoders) */
    if (cc >= MoveKnob1 && cc <= MoveKnob8) {
        const knobIndex = cc - MoveKnob1;
        const delta = decodeDelta(value);
        /* In reference mode with muting on, input knobs (1,3,4) are inactive */
        const isRef = feedbackIndex === 1;
        const muted = isRef && refMuteInput;

        switch (knobIndex) {
            case 0: {
                feedbackVolume = clamp(feedbackVolume + delta * 5, 0, 100);
                sendParamNow('feedback_volume', String(feedbackVolume));
                announceParameter('Volume', feedbackVolume + '%');
                break;
            }
            case 1: {
                if (muted) { announce('Passthru inactive in reference mode'); break; }
                passthroughVolume = clamp(passthroughVolume + delta * 5, 0, 100);
                sendParamNow('passthrough_volume', String(passthroughVolume));
                announceParameter('Passthru', passthroughVolume + '%');
                break;
            }
            case 2: {
                a4Ref = clamp(a4Ref + delta, 410, 480);
                sendParamNow('a4_ref', String(a4Ref));
                announceParameter('A4', a4Ref + ' Hz');
                break;
            }
            case 3: {
                if (muted) { announce('Gate inactive in reference mode'); break; }
                noiseGate = clamp(noiseGate + delta * 5, 0, 100);
                sendParamNow('noise_gate', String(noiseGate));
                announceParameter('Gate', noiseGate + '%');
                break;
            }
            case 4: {
                if (muted) { announce('Threshold inactive in reference mode'); break; }
                tuneThreshold = clamp(tuneThreshold + delta, 1, 10);
                sendParamNow('tune_threshold', String(tuneThreshold));
                announceParameter('Threshold', tuneThreshold + ' cents');
                break;
            }
        }
        return;
    }
}

function clamp(val, min, max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

/* Find category index for a given preset index */
function getCategoryForPreset(presetIdx) {
    for (let i = 0; i < CATEGORIES.length; i++) {
        if (presetIdx >= CATEGORIES[i].start && presetIdx <= CATEGORIES[i].end) {
            return i;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

globalThis.init = function() {
    menuItems = buildMenuItems();
    menuState = createMenuState();
    menuStack = createMenuStack();

    categoryIndex = getCategoryForPreset(modeIndex);

    /* Queue initial params to DSP (drained one per tick) */
    queueParam('tn_inst', String(modeIndex));
    queueParam('feedback_mode', FEEDBACK_IDS[feedbackIndex]);
    queueParam('autospeak', 'on');
    queueParam('a4_ref', String(a4Ref));
    queueParam('guide_octave', guideOctave);
    queueParam('ref_style', refStyle);
    queueParam('ref_style_auto', refStyleAuto ? 'on' : 'off');
    queueParam('passthrough', passthrough ? 'on' : 'off');
    queueParam('feedback_volume', String(feedbackVolume));
    queueParam('passthrough_volume', String(passthroughVolume));
    queueParam('tune_threshold', String(tuneThreshold));
    queueParam('noise_gate', String(noiseGate));
    queueParam('auto_detect', autoDetect ? 'on' : 'off');
    queueParam('manual_midi', String(manualMidi));
    queueParam('guide_tone_ms', String(guideToneMs));
    queueParam('guide_gap_ms', String(guideGapMs));
    queueParam('ref_mute_input', refMuteInput ? 'on' : 'off');

    announceView('Tuner');
    announce('Chromatic mode, target ' + midiToSpokenName(manualMidi) +
             '. Use arrows to select note. Turn jog to change instrument. Press menu for settings.');
};

globalThis.tick = function() {
    tickCount++;

    /* Drain one queued param per tick (ME framework coalesces back-to-back calls) */
    drainParamQueue();

    /* Poll DSP every other tick (~30 Hz at 60fps) */
    if (tickCount % 2 === 0) {
        pollDSP();
    }

    autospeakTick();

    if (inMenu) {
        drawMenu();
    } else {
        drawTuner();
    }
};

/* Knob parameter names for touch announcements (knobs 1-8) */
const KNOB_NAMES = ['Volume', 'Passthru', 'A4 Ref', 'Gate', 'Threshold', '', '', ''];

globalThis.onMidiMessageInternal = function(data) {
    if (!data || data.length < 3) return;

    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    /* Handle knob touch BEFORE filtering (touches are filtered by default).
     * Knob touches arrive as Note On with note 0-7, velocity > 0. */
    if (status === MidiNoteOn && d2 > 0 && d1 >= 0 && d1 <= 7) {
        const name = KNOB_NAMES[d1];
        if (name) {
            const isRef = feedbackIndex === 1;
            const muted = isRef && refMuteInput;
            /* Input-related knobs (1=passthru, 3=gate, 4=threshold) */
            if (muted && (d1 === 1 || d1 === 3 || d1 === 4)) {
                announce(name + ' inactive in reference mode');
                return;
            }
            let val = '';
            switch (d1) {
                case 0: val = feedbackVolume + '%'; break;
                case 1: val = passthroughVolume + '%'; break;
                case 2: val = a4Ref + ' Hz'; break;
                case 3: val = noiseGate + '%'; break;
                case 4: val = tuneThreshold + ' cents'; break;
            }
            announceParameter(name, val);
        }
        return;
    }

    if (shouldFilterMessage(data)) return;

    if (status === MidiCC) {
        handleInput(d1, d2);
    }
};

globalThis.onMidiMessageExternal = function(data) {
    /* External MIDI not used by tuner */
};

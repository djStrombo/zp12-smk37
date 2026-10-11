/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

/* SMK-37 Physical Button Indices (hardware scan matrix):
 * 0: KNOB BANK
 * 1: FADER BANK
 * 2: < (LEFT)
 * 3: > (RIGHT)
 * 4: OCTAVE-
 * 5: OCTAVE+
 * 6: ARP
 * 7: NOTE REPEAT
 * 8: SCALE
 * 9: CHORD
 * 10: GLOBE
 * 11: BT
 * 12: PATCH
 * 13: PARA
 * 14: FX
 * 15: SEQ
 * 16: SEQ PLAY
 * 17: SEQ REC
 */

/* ZP-12 Logical Actions */
#define ACT_NONE    0xFF
#define ACT_OCTDN   0   /* B_OCTDN */
#define ACT_OCTUP   1   /* B_OCTUP */
#define ACT_FX      2   /* B_FX */
#define ACT_SEL     3   /* B_SEL */
#define ACT_ENV     4   /* B_ENV */
#define ACT_LFO     5   /* B_LFO */
#define ACT_EDIT    6   /* B_EDIT */
#define ACT_GLO     7   /* B_GLO */
#define ACT_HOME    8   /* B_HOME */
#define ACT_SAVE    9   /* B_SAVE */
#define ACT_ARP     10  /* B_ARP */
#define ACT_SEQ     11  /* B_SEQ */
#define ACT_PLAY    12  /* B_PLAY */
#define ACT_REC     13  /* B_REC */

/* Physical button (0..17) to logical action mapping */
static const uint8_t SMK_BTN_MAP[18] = {
    ACT_SEL,     /* 0: KNOB BANK -> SELECT / SHIFT */
    ACT_SAVE,    /* 1: FADER BANK -> SAVE */
    ACT_OCTDN,   /* 2: < -> OCTAVE- */
    ACT_OCTUP,   /* 3: > -> OCTAVE+ */
    ACT_OCTDN,   /* 4: OCTAVE- -> OCTAVE- */
    ACT_OCTUP,   /* 5: OCTAVE+ -> OCTAVE+ */
    ACT_ARP,     /* 6: ARP -> ARP / ROLL */
    ACT_LFO,     /* 7: NOTE REPEAT -> LFO / STEP ERASE */
    ACT_HOME,    /* 8: SCALE -> MIX / HOME */
    ACT_ENV,     /* 9: CHORD -> ENVELOPE */
    ACT_GLO,     /* 10: GLOBE -> GLOBAL / MUTES */
    ACT_SAVE,    /* 11: BT -> SAVE */
    ACT_EDIT,    /* 12: PATCH -> EDIT SOUND */
    ACT_SEL,     /* 13: PARA -> SELECT / SHIFT */
    ACT_FX,      /* 14: FX -> FX / PUNCH-IN */
    ACT_SEQ,     /* 15: SEQ -> SEQUENCER / GRID */
    ACT_PLAY,    /* 16: SEQ PLAY -> PLAY / PAUSE */
    ACT_REC,     /* 17: SEQ REC -> RECORD / CLEAR */
};

/* ZP-12 Logical Encoder Actions */
#define ENC_ACT_NONE   0xFF
#define ENC_ACT_KNOB1  0   /* Screen Knob 1 */
#define ENC_ACT_KNOB2  1   /* Screen Knob 2 */
#define ENC_ACT_KNOB3  2   /* Screen Knob 3 */
#define ENC_ACT_KNOB4  3   /* Screen Knob 4 */
#define ENC_ACT_TEMPO  4   /* Tempo / BPM */
#define ENC_ACT_PADSEL 5   /* Pad / Algorithm Select */
#define ENC_ACT_SAMPLE 6   /* Preset / Sample Select */
#define ENC_ACT_MASTER 7   /* Master Volume */

/* Physical encoders K1..K8 (indices 0..7) default mapping */
static const uint8_t SMK_ENC_MAP[8] = {
    ENC_ACT_KNOB1,  /* K1 -> Knob 1 */
    ENC_ACT_KNOB2,  /* K2 -> Knob 2 */
    ENC_ACT_KNOB3,  /* K3 -> Knob 3 */
    ENC_ACT_KNOB4,  /* K4 -> Knob 4 */
    ENC_ACT_TEMPO,  /* K5 -> Tempo / BPM */
    ENC_ACT_PADSEL, /* K6 -> Pad Select */
    ENC_ACT_SAMPLE, /* K7 -> Preset / Sample */
    ENC_ACT_MASTER, /* K8 -> Master Volume */
};

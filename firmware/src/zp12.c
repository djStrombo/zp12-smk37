/* SPDX-License-Identifier: GPL-3.0-only */
/* zp12: a 12-bit sampling drum machine for the M-VAVE FM-1. One compilation unit: the HAL (header-only),
 * the platform of SLOOP / Felucca as sloopDX carries it (boot guard, USB-MIDI, the M-UPGRADE updater and the
 * USB rescue, so the web installer always reaches the FM-1), the sound core (sp_core.c), the factory kit
 * (build/gen/zp12_kit.h, tools/gen_kit.py) and the panel (sp_ui.c). */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#if BOARD_SMK37
#include "smk_input.h"
#else
#include "fm1_input.h"
#include "fm1_adc.h"
#endif
#include "fm1_timer.h"
#include "fm1_lcd_hw.h"
#include "fm1_audio.h"
#include "bootguard.h"

bootguard_t bootguard __attribute__((section(".noinit")));
static volatile uint32_t fm1_ms;

#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* (as sloopDX core.h) */
#include "libc.c"
#include "lcd.c"
#define CV_MAX (232u * 80u)                    /* the biggest region: the faders */
#include "gfx.c"

#define FELUCCA_OTA 1
#define FELUCCA_CDC 0
#define FELUCCA_UAC 0
#define FELUCCA_UART 0
#ifndef FELUCCA_ID
#if BOARD_SMK37
#define FELUCCA_ID "SMK-37 Pro_970"
#else
#define FELUCCA_ID "FM-1_970"                   /* package identity: 97N = zp12 build N */
#endif
#endif
#include "usb.c"

/* ---- flash: read anywhere, erase / program only the update's staging area (saving comes later) */
#if BOARD_SMK37
#define FL_RANGE_OK(off, n) (FL_STORE_OK(off, n) || FL_IN(off, n, FL_OTA_LO, FL_OTA_HI))
#else
/* (the app writes Felucca's store, the update staging and, for the web tools, the free rooms above it:
 * 0xE5000..0xE8FFF and 0xEA000..0xFBFFF; sp_link.c narrows that to zp12's own and sloopDX's banks) */
#define FL_RANGE_OK(off, n) (FL_STORE_OK(off, n) || FL_IN(off, n, FL_OTA_LO, 0xE9000u) || FL_IN(off, n, 0xEA000u, 0xFC000u))
#endif
#include "fm1_flash.h"
static uint8_t flash_ok;
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}
static int fl_erase4k_quiet(uint32_t off, uint32_t *took)
{
    uint32_t f = irq_save();
    int rc = FL_FAR(fl_erase4k_ram)(off, took);
    irq_restore(f);
    return rc;
}

static uint8_t recovery_active;
#define OTA_IDENTITY (recovery_active ? "FM-1_000" : FELUCCA_ID)
#include "ota.c"
static void recovery_poll(void);
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void)
{
    fm1_wdt_feed();
    if (recovery_active) recovery_poll();
}
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    return fl_erase4k_quiet(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    if (recovery_active) return;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 92, 240, &FONT_S, "UPDATE", C_WHITE, 1);
    if (step < 9u)
        draw_text_box(0, 124, 240, &FONT_S, STEP[step < 6u ? step : 0], C_WHITE, 1);
    else
        draw_text_box(0, 124, 240, &FONT_S, code == 1 ? "DRY RUN OK" : "FAILED", C_WHITE, 1);
    (void)code;
}
static void ota_commit(const uint8_t *parm)
{
    bootguard_clear(&bootguard);
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}
#include "recovery.c"

#define SP_WITH_FX 1
#include "sp_core.c"
#include "sp_fx.c"
#include "zp12_kit.h"
#include "sp_seq.c"
#include "sp_punch.c"
/* pad hits go to the audio ISR as requests (sp_seq.c sq_post): sounded, and recorded when REC */
#define SP_HIT(k, vel, semis) sq_post(RQ_HIT, k, (vel) | (uint32_t)((semis) + 64) << 8)
#if BOARD_SMK37
static uint8_t smk_scale_mode = 0;
static uint8_t smk_root_note = 0;
static uint8_t smk_chord_mode = 0;
static uint8_t smk_fader_bank = 0;
static int8_t smk_kbd_oct = 0;
static uint8_t smk_midi_mode = 0;
static uint8_t smk_midi_enc_bank = 0;
static uint8_t smk_midi_fad_bank = 0;
static uint8_t smk_midi_pad_bank = 0;
static const char *const SCALE_NAMES[5] = {
    "SCALE: OFF",
    "SCALE: MINOR",
    "SCALE: MAJOR",
    "SCALE: PENTATONIC",
    "SCALE: BLUES"
};
static const char *const ROOT_NOTE_NAMES[12] = {
    "ROOT KEY: C",  "ROOT KEY: C#", "ROOT KEY: D",  "ROOT KEY: D#",
    "ROOT KEY: E",  "ROOT KEY: F",  "ROOT KEY: F#", "ROOT KEY: G",
    "ROOT KEY: G#", "ROOT KEY: A",  "ROOT KEY: A#", "ROOT KEY: B"
};
static const char *const CHORD_NAMES[7] = {
    "CHORD: OFF",
    "CHORD: TRIAD",
    "CHORD: 5TH",
    "CHORD: 7TH",
    "CHORD: 9TH",
    "CHORD: SUS4",
    "CHORD: OCTAVE"
};
static const int8_t SCALE_SNAP_MIN[12] = { 0, 0, 2, 3, 3, 5, 5, 7, 8, 8, 10, 10 };
static const int8_t SCALE_SNAP_MAJ[12] = { 0, 0, 2, 2, 4, 5, 5, 7, 7, 9, 9, 11 };
static const int8_t SCALE_SNAP_PNT[12] = { 0, 0, 0, 3, 3, 5, 5, 7, 7, 7, 10, 10 };
static const int8_t SCALE_SNAP_BLU[12] = { 0, 0, 0, 3, 3, 5, 6, 7, 7, 7, 10, 10 };

static const uint8_t SCALE_DEG_MIN[7] = { 0, 2, 3, 5, 7, 8, 10 };
static const uint8_t SCALE_DEG_MAJ[7] = { 0, 2, 4, 5, 7, 9, 11 };
static const uint8_t SCALE_DEG_PNT[5] = { 0, 2, 4, 7, 9 };
static const uint8_t SCALE_DEG_BLU[6] = { 0, 3, 5, 6, 7, 10 };

int32_t smk_quantize_note(int32_t note_offset) {
    if (!smk_scale_mode) return note_offset;
    int32_t rel = note_offset - (int32_t)smk_root_note;
    int32_t oct = rel / 12;
    int32_t semi = rel % 12;
    if (semi < 0) { semi += 12; oct--; }
    switch (smk_scale_mode) {
    case 1: semi = SCALE_SNAP_MIN[semi]; break;
    case 2: semi = SCALE_SNAP_MAJ[semi]; break;
    case 3: semi = SCALE_SNAP_PNT[semi]; break;
    case 4: semi = SCALE_SNAP_BLU[semi]; break;
    default: break;
    }
    return oct * 12 + semi + (int32_t)smk_root_note;
}

static int32_t smk_scale_degree_note(int32_t root, int32_t degree_offset)
{
    if (!smk_scale_mode) return root + degree_offset;
    const uint8_t *deg;
    uint32_t num_deg;
    switch (smk_scale_mode) {
    case 1: deg = SCALE_DEG_MIN; num_deg = 7; break;
    case 2: deg = SCALE_DEG_MAJ; num_deg = 7; break;
    case 3: deg = SCALE_DEG_PNT; num_deg = 5; break;
    case 4: deg = SCALE_DEG_BLU; num_deg = 6; break;
    default: return root + degree_offset;
    }
    int32_t rel = root - (int32_t)smk_root_note;
    int32_t oct = rel / 12;
    int32_t semi = rel % 12;
    if (semi < 0) { semi += 12; oct--; }

    int32_t idx = 0;
    for (uint32_t i = 0; i < num_deg; i++) {
        if (deg[i] <= semi) idx = (int32_t)i;
    }
    int32_t target_idx = idx + degree_offset;
    int32_t add_oct = 0;
    while (target_idx >= (int32_t)num_deg) {
        target_idx -= (int32_t)num_deg;
        add_oct++;
    }
    while (target_idx < 0) {
        target_idx += (int32_t)num_deg;
        add_oct--;
    }
    return (oct + add_oct) * 12 + deg[target_idx] + (int32_t)smk_root_note;
}

static uint32_t smk_get_chord_notes(int32_t root, int32_t *notes)
{
    notes[0] = root;
    if (!smk_chord_mode) return 1;

    if (smk_scale_mode != 0) {
        switch (smk_chord_mode) {
        case 1: /* TRIAD (1, 3, 5) */
            notes[1] = smk_scale_degree_note(root, 2);
            notes[2] = smk_scale_degree_note(root, 4);
            return 3;
        case 2: /* 5TH (1, 5) */
            notes[1] = smk_scale_degree_note(root, 4);
            return 2;
        case 3: /* 7TH (1, 3, 5, 7) */
            notes[1] = smk_scale_degree_note(root, 2);
            notes[2] = smk_scale_degree_note(root, 4);
            notes[3] = smk_scale_degree_note(root, 6);
            return 4;
        case 4: /* 9TH (1, 3, 5, 7, 9) */
            notes[1] = smk_scale_degree_note(root, 2);
            notes[2] = smk_scale_degree_note(root, 4);
            notes[3] = smk_scale_degree_note(root, 6);
            notes[4] = smk_scale_degree_note(root, 8);
            return 5;
        case 5: /* SUS4 (1, 4, 5) */
            notes[1] = smk_scale_degree_note(root, 3);
            notes[2] = smk_scale_degree_note(root, 4);
            return 3;
        case 6: /* OCTAVE (1, 8) */
            notes[1] = root + 12;
            return 2;
        default: return 1;
        }
    } else {
        /* Chromatic chord shapes */
        switch (smk_chord_mode) {
        case 1: /* TRIAD (Major) */
            notes[1] = root + 4;
            notes[2] = root + 7;
            return 3;
        case 2: /* 5TH */
            notes[1] = root + 7;
            return 2;
        case 3: /* 7TH (Dominant 7) */
            notes[1] = root + 4;
            notes[2] = root + 7;
            notes[3] = root + 10;
            return 4;
        case 4: /* 9TH */
            notes[1] = root + 4;
            notes[2] = root + 7;
            notes[3] = root + 10;
            notes[4] = root + 14;
            return 5;
        case 5: /* SUS4 */
            notes[1] = root + 5;
            notes[2] = root + 7;
            return 3;
        case 6: /* OCTAVE */
            notes[1] = root + 12;
            return 2;
        default: return 1;
        }
    }
}
#endif
#include "sp_ui.c"
#include "sp_store.c"
#include "sp_samples.c"
#include "sp_link.c"

/* ---- the timer: 10 kHz key scan, milliseconds, USB at 2 kHz */
void fm1_timer5_irq(void)
{
    static uint32_t sub, last, acc;
    uint32_t t0 = fm1_ticks();
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t0 - last;
    last = t0;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        fm1_ms++;
    }
    if (sub % 5u == 0u)
        usb_poll();
    if (++sub == 10u)
        sub = 0;
}
extern void isr_timer5(void);

/* ---- the sound: ALNK0 I2S, two halves of 128 frames; sp_render in blocks of 32, the MASTER knob, Q15 -> 24 bit */
#define HALF_FRAMES 128u
#define HALF_WORDS (HALF_FRAMES * 2u)
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static volatile int32_t master_q12 = 2048;

void fm1_alnk0_irq(void)                       /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        int32_t *o = &abuf[fm1_audio_free_half() * HALF_WORDS];
        uint32_t i, b;
        for (i = 0; i < HALF_WORDS; i++)
            o[i] = 0;
        for (b = 0; b < HALF_FRAMES; b += SP_BLK) {
            sq_block();                                     /* the sequencer's hits of this block */
            sp_render(o + 2u * b);
        }
        sp_punch(o, HALF_FRAMES);                           /* FX held + white key 1-4: roll, reverse, tape stop */
        for (i = 0; i < HALF_WORDS; i++) {
            o[i] = sp_out((o[i] * master_q12) >> 12) << 8;   /* (-6 dB, a soft knee: sp_core.c) */
        }
        fm1_audio_ack_half();
    }
}
extern void isr_alnk0(void);

/* the USB side, polled from the main loop: the installer's update request, the UBOOT request */
static void zl_service(void);
static void fm1_service(void)
{
    fm1_wdt_feed();
    usb_retry(fm1_ms);
    zl_service();                               /* (the web tools' frames first: ota_service drops the rest) */
    ota_service();
    if (usb.ota_req) {
        usb.ota_req = 0;
        if (flash_ok)
            ota_session();                      /* returns only if nothing was committed */
        ui.force = 1;
    }
    if (usb.uboot_req) {
        fm1_audio_stop();
        lcd_fill(0, 0, 240, 240, C_BLACK);
        draw_text_box(0, 110, 240, &FONT_S, "UBOOT (USB)", C_WHITE, 1);
        usb_detach();
        fm1_delay_ms(30);
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    if ((uint32_t)fm1_ms >= 30000u && bootguard.pending)
        bootguard_clear(&bootguard);            /* 30 s up: this firmware boots */
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t i, t0, v[3] = {c->vec, c->pc, c->rets};
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, RGB(160, 0, 0));
    draw_text_box(0, 20, 240, &FONT_S, "ZP12 CRASH", C_WHITE, 1);
    for (i = 0; i < 3u; i++) {
        uint32_t k;
        for (k = 0; k < 8u; k++) b[k] = "0123456789ABCDEF"[(v[i] >> (28u - 4u * k)) & 15u];
        b[8] = 0;
        draw_text_box(10, 60 + i * 20, 220, &FONT_S, b, C_WHITE, 0);
    }
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 8000u * 1000u * FM1_TICKS_PER_US)
        fm1_wdt_feed();
    fm1_reboot();
}

static void zp12_main(void)
{
    uint32_t i, prev_notes = 0, t_frame = 0, seq_down = 0, seq_used = 0, t_sig = 0, t_change = 0, last_sig = 0, save_later = 0;
    int32_t knob_avg = 512 * 16;
    (void)seq_down;
    for (i = 0; i < KIT_NWAVE; i++) {
        sp_wave[i].d = KIT_DATA + KIT_WAVE[i].off;
        sp_wave[i].n = KIT_WAVE[i].n;
        sp_wave[i].rate = 26040;
    }
    for (i = 0; i < SP_NSOUND; i++)
        sp_sound[i] = KIT_PADS[i];
    sq_init();
    zu_load();                                          /* the own samples (before the pads that play them) */
    ui.back = 1;                                        /* pages back after 12 s, unless a save says otherwise */
    if (zs_load())                                      /* what was left: sounds, mix, effects, segments, song */
        zs_saved_sig = zs_sig();
    last_sig = zs_saved_sig;
    ui_init();
    {   /* the start (sp_ui.c ui_splash): ~3 s, its drums played; a key or a button cuts it short */
        uint32_t t0 = fm1_ms, t = 0, prev = 0, drawn = 0, k;
        lcd_fill(0, 0, 240, 240, P_NAVY);
        ui_splash(0);
        while (t < SPLASH_MS && !fm1_in.notes && !fm1_in.buttons) {
            fm1_service();
            t = fm1_ms - t0;
            for (k = 0; k < 4u; k++)                    /* the hits whose time came since the last pass */
                if (SPLASH_HIT[k].t > prev && SPLASH_HIT[k].t <= t)
                    SP_HIT(SPLASH_HIT[k].pad, SPLASH_HIT[k].vel, 0);
            prev = t;
            if (t - drawn >= 40u) { ui_splash(t); drawn = t; }
        }
        while (fm1_in.notes || fm1_in.buttons)          /* (the key that cut it short is not played) */
            fm1_service();
    }
    ui.force = 1;
    for (;;) {
        uint32_t n = fm1_in.notes, down = n & ~prev_notes, rel, b;
        fm1_service();
        prev_notes = n;
        {   /* SEL held: the faders 5-8; LFO held: ERASE (the pads held lose their hits as the playhead passes) */
            uint32_t bt = fm1_in.buttons, er = 0, k;
            ui.held = bt;                               /* (SAVE held + a black key: key_down) */
            if ((bt >> B_LFO) & 1u)
                for (k = 0; k < 27u; k++)
                    if ((n >> k) & 1u) {
                        uint32_t pd = ui.multi ? ui.sel : key_pad(k);
                        if (pd < 32u) er |= 1u << pd;
                    }
            sq.erase = er;
            {   /* FX held: white keys 1-4 punch in, the last one held wins (sp_punch.c) */
                int32_t px = -1;
                if ((bt >> B_FX) & 1u)
                    for (k = 0; k < 27u; k++)
                        if ((n >> k) & 1u && white_of(k) < PN_NFX) px = (int32_t)white_of(k);
                pn.req = (int8_t)px;
            }
#if BOARD_SMK37
            if (!smk_midi_mode) {
                for (i = 0; down; i++, down >>= 1)          /* the keys: pads (ui decides which) */
                    if (down & 1u)
                        key_down(i, (bt >> B_LFO) & 1u);
            }
#else
            for (i = 0; down; i++, down >>= 1)          /* the keys: pads (ui decides which) */
                if (down & 1u)
                    key_down(i, (bt >> B_LFO) & 1u);
#endif
        }
#if BOARD_SMK37
        /* USB MIDI keybed transmission */
        {
            static uint64_t smk_prev_keys = 0;
            static uint8_t smk_key_chord_notes[FM1_NKEY][5];
            static uint8_t smk_key_chord_cnt[FM1_NKEY];
            uint64_t cur_k = fm1_in.keys;
            uint64_t kd = cur_k & ~smk_prev_keys;
            uint64_t ku = ~cur_k & smk_prev_keys;
            smk_prev_keys = cur_k;

            uint32_t ki;
            for (ki = 0; ki < FM1_NKEY; ki++) {
                uint64_t bit = (uint64_t)1u << ki;
                if (kd & bit) {
                    /* Base key note transposed by keyboard octave */
                    int32_t m_note = 48 + (int32_t)ki + ((int32_t)smk_kbd_oct * 12);
                    m_note = sp_clamp(m_note, 0, 127);
                    /* Scale quantization */
                    int32_t q_note = smk_quantize_note(m_note);
                    /* Chord harmonization */
                    int32_t cnotes[5];
                    uint32_t cnt = smk_get_chord_notes(q_note, cnotes);
                    uint32_t vel = fm1_in.key_vel[ki];
                    if (!vel) vel = 96;

                    smk_key_chord_cnt[ki] = (uint8_t)cnt;
                    for (uint32_t c = 0; c < cnt; c++) {
                        int32_t n = sp_clamp(cnotes[c], 0, 127);
                        smk_key_chord_notes[ki][c] = (uint8_t)n;
                        midi_out_event(0x09u | (0x90u << 8) | ((uint32_t)n << 16) | (vel << 24));
                    }
                }
                if (ku & bit) {
                    /* Release all chord notes for this key */
                    uint32_t cnt = smk_key_chord_cnt[ki];
                    for (uint32_t c = 0; c < cnt; c++) {
                        uint32_t n = smk_key_chord_notes[ki][c];
                        midi_out_event(0x08u | (0x80u << 8) | (n << 16) | (0x40u << 24));
                    }
                    smk_key_chord_cnt[ki] = 0;
                }
            }
        }

        /* USB MIDI drum pads transmission */
        {
            static uint16_t smk_prev_pad_st = 0;
            uint16_t cur_pad_st = 0;
            uint32_t pi;
            for (pi = 0; pi < 16u; pi++) {
                if (fm1_in.pad_st[pi] & 0x80)
                    cur_pad_st |= (1u << pi);
            }
            uint16_t pd = cur_pad_st & ~smk_prev_pad_st;
            uint16_t pu = ~cur_pad_st & smk_prev_pad_st;
            smk_prev_pad_st = cur_pad_st;

            uint32_t pad_base = smk_midi_pad_bank ? 52u : 36u;
            for (pi = 0; pi < 16u; pi++) {
                uint16_t pbit = 1u << pi;
                if (pd & pbit) {
                    uint32_t vel = fm1_in.pad_press[pi];
                    if (!vel) vel = 100;
                    midi_out_event(0x09u | (0x99u << 8) | ((pad_base + pi) << 16) | ((vel & 0x7Fu) << 24));
                }
                if (pu & pbit) {
                    midi_out_event(0x08u | (0x89u << 8) | ((pad_base + pi) << 16) | (0x40u << 24));
                }
            }
        }

        /* USB MIDI Wheels transmission */
        {
            static uint16_t smk_prev_wheel[2] = {0xFFFFu, 0xFFFFu};
            uint16_t w0 = fm1_in.wheel[0];
            if (smk_prev_wheel[0] == 0xFFFFu || (w0 > smk_prev_wheel[0] + 3u) || (w0 + 3u < smk_prev_wheel[0])) {
                smk_prev_wheel[0] = w0;
                uint32_t pb = (uint32_t)w0 * 16383u / 1023u;
                uint32_t lsb = pb & 0x7Fu;
                uint32_t msb = (pb >> 7) & 0x7Fu;
                midi_out_event(0x0Eu | (0xE0u << 8) | (lsb << 16) | (msb << 24));
            }
            uint16_t w1 = fm1_in.wheel[1];
            if (smk_prev_wheel[1] == 0xFFFFu || (w1 > smk_prev_wheel[1] + 7u) || (w1 + 7u < smk_prev_wheel[1])) {
                smk_prev_wheel[1] = w1;
                uint32_t mod = (uint32_t)w1 * 127u / 1023u;
                midi_out_event(0x0Bu | (0xB0u << 8) | (0x01u << 16) | ((mod & 0x7Fu) << 24));
            }
        }
#endif
        b = fm1_input_edges(&rel);
        b = ui_holds(fm1_in.buttons, b, rel);           /* REC held: CLEAR; SAVE tapped / held + a black key */
#if BOARD_SMK37
        /* PATCH (B_HOME) Tap vs Hold:
         * Hold >= 800 ms toggles MIDI Controller Mode (Local Control Off).
         * Tap (< 800 ms) goes to Home (clearing step seq). */
        {
            static uint32_t patch_down_ms = 0;
            static uint8_t patch_held_fired = 0;

            if (b & (1u << B_HOME)) {
                b &= ~(1u << B_HOME);
                patch_down_ms = fm1_ms ? fm1_ms : 1;
                patch_held_fired = 0;
            }
            if (patch_down_ms && ((fm1_in.buttons >> B_HOME) & 1u)) {
                if (!patch_held_fired && (uint32_t)(fm1_ms - patch_down_ms) >= 800u) {
                    patch_held_fired = 1;
                    smk_midi_mode ^= 1u;
                    ui_say(smk_midi_mode ? "MIDI MODE: ON" : "STANDALONE ON");
                    ui.force = 1;
                }
            }
            if (rel & (1u << B_HOME)) {
                if (patch_down_ms && !patch_held_fired) {
                    ui.steps = 0;
                    page(PG_HOME);
                    ui.force = 1;
                }
                patch_down_ms = 0;
                patch_held_fired = 0;
            }
        }

        /* Keyboard Octave buttons */
        if (b & (1u << ACT_KBD_OCTDN)) {
            b &= ~(1u << ACT_KBD_OCTDN);
            if (smk_kbd_oct > -3) smk_kbd_oct--;
            char msg[16];
            msg[0] = 'O'; msg[1] = 'C'; msg[2] = 'T'; msg[3] = 'A'; msg[4] = 'V'; msg[5] = 'E'; msg[6] = ' ';
            if (smk_kbd_oct > 0) {
                msg[7] = '+';
                msg[8] = (char)('0' + smk_kbd_oct);
                msg[9] = 0;
            } else if (smk_kbd_oct < 0) {
                msg[7] = '-';
                msg[8] = (char)('0' + (-smk_kbd_oct));
                msg[9] = 0;
            } else {
                msg[7] = '0';
                msg[8] = 0;
            }
            ui_say(msg);
            ui.force = 1;
        }
        if (b & (1u << ACT_KBD_OCTUP)) {
            b &= ~(1u << ACT_KBD_OCTUP);
            if (smk_kbd_oct < 3) smk_kbd_oct++;
            char msg[16];
            msg[0] = 'O'; msg[1] = 'C'; msg[2] = 'T'; msg[3] = 'A'; msg[4] = 'V'; msg[5] = 'E'; msg[6] = ' ';
            if (smk_kbd_oct > 0) {
                msg[7] = '+';
                msg[8] = (char)('0' + smk_kbd_oct);
                msg[9] = 0;
            } else if (smk_kbd_oct < 0) {
                msg[7] = '-';
                msg[8] = (char)('0' + (-smk_kbd_oct));
                msg[9] = 0;
            } else {
                msg[7] = '0';
                msg[8] = 0;
            }
            ui_say(msg);
            ui.force = 1;
        }
        /* SCALE button: Tap cycles scale, Hold (>= 600ms) turns OFF */
        {
            static uint32_t scale_down_ms = 0;
            static uint8_t scale_held_fired = 0;

            if (b & (1u << ACT_SCALE)) {
                b &= ~(1u << ACT_SCALE);
                scale_down_ms = fm1_ms ? fm1_ms : 1;
                scale_held_fired = 0;
            }
            if (scale_down_ms && ((fm1_in.buttons >> ACT_SCALE) & 1u)) {
                if (!scale_held_fired && (uint32_t)(fm1_ms - scale_down_ms) >= 600u) {
                    scale_held_fired = 1;
                    smk_scale_mode = 0;
                    ui_say("SCALE: OFF");
                    ui.force = 1;
                }
            }
            if (rel & (1u << ACT_SCALE)) {
                if (scale_down_ms && !scale_held_fired) {
                    smk_scale_mode = (smk_scale_mode + 1) % 5;
                    ui_say(SCALE_NAMES[smk_scale_mode]);
                    ui.force = 1;
                }
                scale_down_ms = 0;
                scale_held_fired = 0;
            }
        }

        /* CHORD button: Tap cycles chord, Hold (>= 600ms) turns OFF */
        {
            static uint32_t chord_down_ms = 0;
            static uint8_t chord_held_fired = 0;

            if (b & (1u << ACT_CHORD)) {
                b &= ~(1u << ACT_CHORD);
                chord_down_ms = fm1_ms ? fm1_ms : 1;
                chord_held_fired = 0;
            }
            if (chord_down_ms && ((fm1_in.buttons >> ACT_CHORD) & 1u)) {
                if (!chord_held_fired && (uint32_t)(fm1_ms - chord_down_ms) >= 600u) {
                    chord_held_fired = 1;
                    smk_chord_mode = 0;
                    ui_say("CHORD: OFF");
                    ui.force = 1;
                }
            }
            if (rel & (1u << ACT_CHORD)) {
                if (chord_down_ms && !chord_held_fired) {
                    smk_chord_mode = (smk_chord_mode + 1) % 7;
                    if (smk_chord_mode != 0 && !smk_midi_mode) {
                        ui.multi = 1;
                    }
                    ui_say(CHORD_NAMES[smk_chord_mode]);
                    ui.force = 1;
                }
                chord_down_ms = 0;
                chord_held_fired = 0;
            }
        }

        /* NOTE REPEAT button: Tap cycles Root Key, Hold (>= 600ms) resets to C */
        {
            static uint32_t repeat_down_ms = 0;
            static uint8_t repeat_held_fired = 0;

            if (b & (1u << ACT_REPEAT)) {
                b &= ~(1u << ACT_REPEAT);
                repeat_down_ms = fm1_ms ? fm1_ms : 1;
                repeat_held_fired = 0;
            }
            if (repeat_down_ms && ((fm1_in.buttons >> ACT_REPEAT) & 1u)) {
                if (!repeat_held_fired && (uint32_t)(fm1_ms - repeat_down_ms) >= 600u) {
                    repeat_held_fired = 1;
                    smk_root_note = 0;
                    ui_say("ROOT KEY: C (OFF)");
                    ui.force = 1;
                }
            }
            if (rel & (1u << ACT_REPEAT)) {
                if (repeat_down_ms && !repeat_held_fired) {
                    smk_root_note = (smk_root_note + 1) % 12;
                    ui_say(ROOT_NOTE_NAMES[smk_root_note]);
                    ui.force = 1;
                }
                repeat_down_ms = 0;
                repeat_held_fired = 0;
            }
        }

        /* Banking buttons:
         * Button 0 (ACT_LFO): Knob Bank (in MIDI) / Step Erase (in ZP-12)
         * Button 1 (ACT_FBANK): Fader Bank
         * Button 0 + 1 combo: Pad Bank */
        {
            static uint8_t bank_combo_fired = 0;
            static uint8_t knob_btn_down = 0;
            static uint8_t fad_btn_down = 0;

            if (b & (1u << ACT_LFO)) {
                if (smk_midi_mode) {
                    b &= ~(1u << ACT_LFO);
                    knob_btn_down = 1;
                    if (fad_btn_down || ((fm1_in.buttons >> ACT_FBANK) & 1u)) {
                        bank_combo_fired = 1;
                        smk_midi_pad_bank ^= 1u;
                        ui_say(smk_midi_pad_bank ? "PAD BANK 2 (52-67)" : "PAD BANK 1 (36-51)");
                        ui.force = 1;
                    }
                }
                /* In ZP-12 mode, ACT_LFO passes through as B_LFO (native ERASE) */
            }

            if (b & (1u << ACT_FBANK)) {
                b &= ~(1u << ACT_FBANK);
                if (smk_midi_mode) {
                    fad_btn_down = 1;
                    if (knob_btn_down || ((fm1_in.buttons >> ACT_LFO) & 1u)) {
                        bank_combo_fired = 1;
                        smk_midi_pad_bank ^= 1u;
                        ui_say(smk_midi_pad_bank ? "PAD BANK 2 (52-67)" : "PAD BANK 1 (36-51)");
                        ui.force = 1;
                    }
                } else {
                    smk_fader_bank ^= 1u;
                    ui_say(smk_fader_bank ? "FADERS 5-8" : "FADERS 1-4");
                    ui.force = 1;
                }
            }

            if (rel & (1u << ACT_LFO)) {
                if (smk_midi_mode) {
                    if (knob_btn_down && !bank_combo_fired) {
                        smk_midi_enc_bank ^= 1u;
                        ui_say(smk_midi_enc_bank ? "KNOB BANK 2 (CC 28-35)" : "KNOB BANK 1 (CC 20-27)");
                        ui.force = 1;
                    }
                    knob_btn_down = 0;
                    if (!fad_btn_down) bank_combo_fired = 0;
                }
            }

            if (rel & (1u << ACT_FBANK)) {
                if (smk_midi_mode) {
                    if (fad_btn_down && !bank_combo_fired) {
                        smk_midi_fad_bank ^= 1u;
                        ui_say(smk_midi_fad_bank ? "FADER BANK 2 (CC 44-47)" : "FADER BANK 1 (CC 40-43)");
                        ui.force = 1;
                    }
                    fad_btn_down = 0;
                    if (!knob_btn_down) bank_combo_fired = 0;
                }
            }
        }

        /* < and > buttons (Buttons 2 & 3):
         * In MIDI mode: toggles Pad Bank 1 (36..51) vs Pad Bank 2 (52..67).
         * In ZP-12 mode: normal bar / bank navigation. */
        if (b & (1u << ACT_OCTDN)) {
            if (smk_midi_mode) {
                b &= ~(1u << ACT_OCTDN);
                smk_midi_pad_bank = 0;
                ui_say("PAD BANK 1 (36-51)");
                ui.force = 1;
            }
        }
        if (b & (1u << ACT_OCTUP)) {
            if (smk_midi_mode) {
                b &= ~(1u << ACT_OCTUP);
                smk_midi_pad_bank = 1;
                ui_say("PAD BANK 2 (52-67)");
                ui.force = 1;
            }
        }

        /* GLOBE button (Button 10): in MIDI mode toggles Pad Bank 1 / 2 */
        if (b & (1u << B_GLO)) {
            if (smk_midi_mode) {
                b &= ~(1u << B_GLO);
                smk_midi_pad_bank ^= 1u;
                ui_say(smk_midi_pad_bank ? "PAD BANK 2 (52-67)" : "PAD BANK 1 (36-51)");
                ui.force = 1;
            }
        }

        /* Read physical faders 0..3 */
        {
            static uint8_t fader_prev[4] = {0xFF, 0xFF, 0xFF, 0xFF};
            uint32_t fi;
            for (fi = 0; fi < 4u; fi++) {
                uint8_t fval = fm1_in.fader[fi];
                if (fader_prev[fi] == 0xFF) {
                    fader_prev[fi] = fval;
                } else if (fval != fader_prev[fi]) {
                    int32_t delta = (int32_t)fval - (int32_t)fader_prev[fi];
                    fader_prev[fi] = fval;
                    if (smk_midi_mode) {
                        uint8_t cc = (smk_midi_fad_bank ? 44 : 40) + fi;
                        midi_out_event(0x0Bu | (0xB0u << 8) | ((uint32_t)cc << 16) | ((uint32_t)fval << 24));
                        char m[24], *p = cat(m, "FADER ");
                        p = num(p, fi + 1, 1, 0);
                        p = cat(p, " CC");
                        p = num(p, cc, 2, 0);
                        p = cat(p, ":");
                        num(p, fval, 3, 0);
                        ui_say(m);
                        ui.force = 1;
                    } else {
                        uint32_t base = smk_fader_bank ? 4u : 0u;
                        uint8_t fact = SMK_FAD_MAP[fi];
                        switch (fact) {
                        case FAD_ACT_DEFAULT: {
                            uint32_t ch = base + fi;
                            if (ch < SP_NCH) {
                                ui.mix[ch] = fval;
                                sp_mix[ch] = fval;
                                ui.force = 1;
                            }
                            break;
                        }
                        case FAD_ACT_CH1:
                        case FAD_ACT_CH2:
                        case FAD_ACT_CH3:
                        case FAD_ACT_CH4:
                        case FAD_ACT_CH5:
                        case FAD_ACT_CH6:
                        case FAD_ACT_CH7:
                        case FAD_ACT_CH8: {
                            uint32_t ch = fact - FAD_ACT_CH1;
                            ui.mix[ch] = fval;
                            sp_mix[ch] = fval;
                            ui.force = 1;
                            break;
                        }
                        case FAD_ACT_MASTER: {
                            smk_master = (int32_t)fval * 8;
                            master_q12 = (int32_t)((smk_master * smk_master) >> 8);
                            break;
                        }
                        case FAD_ACT_KNOB1: knob(0, delta); ui.force = 1; break;
                        case FAD_ACT_KNOB2: knob(1, delta); ui.force = 1; break;
                        case FAD_ACT_KNOB3: knob(2, delta); ui.force = 1; break;
                        case FAD_ACT_KNOB4: knob(3, delta); ui.force = 1; break;
                        case FAD_ACT_CUTOFF: {
                            sp_sound[ui.sel].cut = fval;
                            ui.turn_pad = ui.sel;
                            ui.turn_ms = fm1_ms;
                            ui.force = 1;
                            break;
                        }
                        case FAD_ACT_RESO: {
                            sp_sound[ui.sel].reso = fval;
                            ui.force = 1;
                            break;
                        }
                        case FAD_ACT_TEMPO: {
                            sq.bpm10 = (uint16_t)sp_clamp(400 + (int32_t)fval * 2000 / 127, 400, 2400);
                            ui.force = 1;
                            break;
                        }
                        default: break;
                        }
                    }
                }
            }
        }
        smk_pad_piano = ui.multi;
#endif
        if (b & (1u << B_FX)) {                         /* FX down: a punch-in layer while held; tapped, the FX pages */
            ui.fx_used = 0;
            b &= ~(1u << B_FX);
        }
        if (rel & (1u << B_FX) && !ui.fx_used)
            button(B_FX);
        if (b & (1u << B_GLO)) {                        /* GLO down: mute / solo on the white keys while held; tapped, the GLO pages */
            ui.glo_used = 0;
            b &= ~(1u << B_GLO);
        }
        if (rel & (1u << B_GLO) && !ui.glo_used)
            button(B_GLO);
#if BOARD_SMK37
        if (b & (1u << B_SEQ)) {                        /* SEQ on SMK-37: Step grid -> SEQ 1 -> SEQ 2 -> SEQ 3 (SONG) */
            b &= ~(1u << B_SEQ);
            if (ui.steps) {
                /* Step seq -> 1st seq page */
                ui.steps = 0;
                page(PG_SEG);
            } else if (ui.page == PG_SEG) {
                /* 1st seq page -> 2nd seq page */
                page(PG_SEG2);
            } else if (ui.page == PG_SEG2) {
                /* 2nd seq page -> 3rd seq page (SONG) */
                page(PG_SONG);
            } else {
                /* 3rd seq page (or any other page) -> Step seq */
                ui.steps = 1;
            }
            ui.force = 1;
        }
#else
        if (b & (1u << B_SEQ)) {                        /* SEQ down: the step grid while held */
            seq_down = fm1_ms;
            seq_used = 0;
            ui.steps = 1;
            ui.force = 1;
            b &= ~(1u << B_SEQ);
        }
#endif
        if (ui.steps && (b & ((1u << B_OCTDN) | (1u << B_OCTUP)) || down))
            seq_used = 1;
        for (i = 0; b; i++, b >>= 1)
            if (b & 1u)
                button(i);
#if !BOARD_SMK37
        if (rel & (1u << B_SEQ) && ui.steps) {          /* SEQ up: a short tap opens the SEQ pages */
            ui.steps = 0;
            ui.force = 1;
            if ((uint32_t)(fm1_ms - seq_down) < 350u && !seq_used)
                button(B_SEQ);
        }
#endif
        {   /* the LEDs: built aside, then swapped in (the scan never shows half of them) */
            uint32_t lb, lk, lg, id;
            ui_leds(&lb, &lk, &lg);
#if !BOARD_SMK37
            uint8_t nl[FM1_NCOL], nd[FM1_NCOL];
            uint32_t c, r;
            memset(nl, 0, sizeof nl);
            memset(nd, 0, sizeof nd);
            for (c = 0; c < FM1_NCOL; c++)
                for (r = 1; r < 5u; r++) {
                    int32_t kid = FM1_KEYMAP[r][c];
                    if (kid < 0) continue;
                    id = (uint32_t)kid;
                    if (id < 14u ? (lb >> id) & 1u : (lk >> (id - 14u)) & 1u) nl[c] |= (uint8_t)(1u << r);
                    if (id >= 14u && (lg >> (id - 14u)) & 1u) nd[c] |= (uint8_t)(1u << r);
                }
            memcpy(fm1_led, nl, sizeof nl);
            memcpy(fm1_led_dim, nd, sizeof nd);
#else
            for (id = 0; id < 14u; id++)
                fm1_led_key(id, (lb >> id) & 1u);
            if (smk_midi_mode) {
                smk_led_physical(0, smk_midi_enc_bank != 0);
                smk_led_physical(1, smk_midi_fad_bank != 0);
                smk_led_physical(2, smk_midi_pad_bank == 0);
                smk_led_physical(3, smk_midi_pad_bank == 1);
                smk_led_physical(12, (fm1_ms / 250u) & 1u);
            } else {
                smk_led_physical(0, (fm1_in.buttons >> ACT_LFO) & 1u);
                smk_led_physical(1, smk_fader_bank != 0);
            }
            smk_led_physical(4, smk_kbd_oct < 0);
            smk_led_physical(5, smk_kbd_oct > 0);
            smk_led_physical(6, ui.multi);
            smk_led_physical(7, smk_root_note != 0);
            smk_led_physical(8, smk_scale_mode != 0);
            smk_led_physical(9, smk_chord_mode != 0);

            /* WS2812 RGB Pad LEDs */
            {
                static uint8_t smk_pad_prev_st[16];
                static uint32_t smk_pad_hit_ms[16];
                uint32_t p;
                for (p = 0; p < 16u; p++) {
                    uint8_t st = (fm1_in.pad_st[p] & 0x80) != 0;
                    if (st && !smk_pad_prev_st[p])
                        smk_pad_hit_ms[p] = fm1_ms;
                    smk_pad_prev_st[p] = st;
                }

                for (p = 0; p < 16u; p++) {
                    uint32_t note_k = SMK_WHITE_NOTE[p];
                    int is_down = (fm1_in.pad_st[p] & 0x80) != 0 || ((uint32_t)(fm1_ms - smk_pad_hit_ms[p]) < 90u);

                    if (ui.steps) {
                        /* Step sequencer mode: pads 0..15 = steps 1..16 of current bar */
                        int has_step = (lk >> note_k) & 1u;
                        int is_head = (lg >> note_k) & 1u;

                        if (is_down || is_head) {
                            /* Finger pressed or playhead cursor -> bright white */
                            padled_set(p, 180, 180, 180);
                        } else if (has_step) {
                            /* Active recorded step -> steady illuminated white */
                            padled_set(p, 60, 60, 60);
                        } else {
                            /* Inactive step -> off */
                            padled_set(p, 0, 0, 0);
                        }
                    } else {
                        /* Normal play mode: off by default, white when pressed or hit */
                        int is_lit = is_down || ((lk >> note_k) & 1u);
                        if (is_lit) {
                            padled_set(p, 180, 180, 180);
                        } else {
                            padled_set(p, 0, 0, 0);
                        }
                    }
                }
                padled_flush();
            }
#endif
        }
#if BOARD_SMK37
        {   /* SMK-37: Encoders mapped via SMK_ENC_MAP (K1..K8) */
            int32_t d;
            for (i = 0; i < 8u; i++) {
                if ((d = fm1_enc_take(i)) != 0) {
                    if (smk_midi_mode) {
                        static uint8_t smk_midi_enc_val[16];
                        uint32_t idx = (smk_midi_enc_bank ? 8u : 0u) + i;
                        int32_t step = accel(d);
                        int32_t nv = sp_clamp((int32_t)smk_midi_enc_val[idx] + step, 0, 127);
                        smk_midi_enc_val[idx] = (uint8_t)nv;
                        uint8_t cc = (smk_midi_enc_bank ? 28 : 20) + i;
                        midi_out_event(0x0Bu | (0xB0u << 8) | ((uint32_t)cc << 16) | ((uint32_t)nv << 24));
                        char m[24], *p = cat(m, "KNOB ");
                        p = num(p, i + 1, 1, 0);
                        p = cat(p, " CC");
                        p = num(p, cc, 2, 0);
                        p = cat(p, ":");
                        num(p, nv, 3, 0);
                        ui_say(m);
                        ui.force = 1;
                    } else {
                        uint8_t act = SMK_ENC_MAP[i];
                        switch (act) {
                        case ENC_ACT_KNOB1: knob(0, d); break;
                        case ENC_ACT_KNOB2: knob(1, d); break;
                        case ENC_ACT_KNOB3: knob(2, d); break;
                        case ENC_ACT_KNOB4: knob(3, d); break;
                        case ENC_ACT_TEMPO:
                            sq.bpm10 = (uint16_t)sp_clamp((int32_t)sq.bpm10 + d * 5, 400, 2400);
                            break;
                        case ENC_ACT_PADSEL:
                            ui.sel = (uint8_t)((ui.sel + SP_NSOUND + (d > 0 ? 1 : SP_NSOUND - 1)) % SP_NSOUND);
                            break;
                        case ENC_ACT_SAMPLE:
                            ui_preset(d);
                            break;
                        case ENC_ACT_MASTER:
                            smk_master = sp_clamp(smk_master + d * 24, 0, 1023);
                            break;
                        default:
                            break;
                        }
                    }
                }
            }
        }
#else
        {   /* KNOB 1-4 (encoders 2..5), SELECT (0): tempo, ALGORITHM (1): the pad, PRESETS (6): its sample */
            int32_t d;
            for (i = 0; i < 4u; i++)
                if ((d = fm1_enc_take(2u + i)) != 0)
                    knob(i, d);
            if ((d = fm1_enc_take(0)) != 0)
                sq.bpm10 = (uint16_t)sp_clamp((int32_t)sq.bpm10 + d * 5, 400, 2400);
            if ((d = fm1_enc_take(1)) != 0)
                ui.sel = (uint8_t)((ui.sel + SP_NSOUND + (d > 0 ? 1 : SP_NSOUND - 1)) % SP_NSOUND);
            if ((d = fm1_enc_take(6)) != 0)
                ui_preset(d);
        }
#endif
        while (mi_r != mi_w) {                          /* USB MIDI in: notes 36..67 play the 32 pads, */
            uint32_t pk = midi_in_q[mi_r % MQ], st = (pk >> 8) & 0xF0u, nt = (pk >> 16) & 0x7Fu, vel = pk >> 24 & 0x7Fu;
            mi_r++;                                     /* Program Change 0..15 picks loop 1..16 (as a black key) */
            if (st == 0x90u && vel && nt >= 36u && nt < 68u)
                pad_hit(nt - 36u, vel);
            else if (st == 0xC0u && nt < SQ_NSEG)
                ui_loop(nt);
        }
        {
            int32_t a = fm1_adc_read(FM1_ADC_MASTER);   /* the MASTER knob */
            if (a >= 0) {
                uint32_t k10;
                knob_avg += (a * 16 - knob_avg) / 8;
                k10 = (uint32_t)(knob_avg / 16);
                master_q12 = (int32_t)((k10 * k10) >> 8);
            }
        }
        fx_bpm10 = sq.bpm10;                            /* the delay follows the tempo */
        if ((uint32_t)(fm1_ms - t_frame) >= 16u) {
            t_frame = fm1_ms;
            ui_draw();
        }
        if (ui.factory_req) {                           /* SETUP > FACTORY: the kit's pads, mix, effects, no loops, no songs */
            ui.factory_req = 0;
            ui.back = 1;
            sp_smooth = 1;
            for (i = 0; i < SP_NSOUND; i++) sp_sound[i] = KIT_PADS[i];
            for (i = 0; i < SP_NCH; i++) ui.mix[i] = sp_mix[i] = 100;
            fxp.crate = 40; fxp.cdepth = 60; fxp.cmix = 127; fxp.dtime = 1; fxp.fdbk = 60; fxp.colr = 70; fxp.dmix = 90;
            fxp.size = 90; fxp.damp = 60; fxp.pre = 0;
            sq_init();
            memset(sq_songs, 0, sizeof sq_songs);
            memset(sq_song_len, 0, sizeof sq_song_len);
            sq.seg = 0; sq.song_mode = 0; sq.song_sel = 0; sq.swing = 0; sq.quant = 3; sq.bpm10 = 900;
            sq.click = 1; sq.cin_bars = 1; sq.dub_bar = 0; sp_mute = sp_solo = 0;
            ui.save_req = 1;
            ui.force = 1;
            ui_say("FACTORY RESET");
        }
        {   /* saving: SAVE, or by itself when stopped, silent and nothing changed for 3 s (an erase stops the audio) */
            uint32_t k, quiet = !sq.playing && !zl_busy();   /* (not while a backup is read or restored) */
            for (k = 0; k < SP_NCH; k++) quiet &= !sp_ch[k].on;
            if (fm1_ms - t_sig >= 200u) {
                uint32_t sg = zs_sig();
                t_sig = fm1_ms;
                if (sg != last_sig) { last_sig = sg; t_change = fm1_ms; }
            }
            if (ui.save_req && !quiet && sq.playing) {
                ui.save_req = 0;
                save_later = 1;
                ui_say("SAVES AT STOP");
            }
            if ((ui.save_req || save_later || (last_sig != zs_saved_sig && fm1_ms - t_change > 3000u)) && quiet) {
                ui.save_req = 0;
                save_later = 0;
                if (zs_save())
                    ui_say("SAVE ERROR");
                last_sig = zs_saved_sig;
            }
        }
    }
}

void fm1_cstart(void)
{
    extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
    extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];
    uint32_t *s, *d, p3, boot_mode;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    fm1_wdt_arm(0x0D);
    if ((p3 & 1u) && !(p3 & (4u | 0x40u)))
        bootguard_clear(&bootguard);
    boot_mode = bootguard_begin(&bootguard);
    if (boot_mode == BOOT_ROM)
        fm1_enter_uboot();
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++) *d = 0;
    for (d = _pool_start; d < _pool_end; d++) *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++) *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++) *d = *s;
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    if (boot_mode == BOOT_RECOVERY || recovery_key())
        recovery_main();                        /* OCT- at power-on: the USB rescue */
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
    if (flash_ok)
        fl_plain_window_init();                 /* flash above 0x93000 reads plain through XIP: the own samples */
    ota_boot_cleanup();
    lcd_init();
    lcd_fill(0, 0, 240, 240, C_BLACK);
#if BOARD_SMK37
    padled_init();
#endif
    fm1_input_init();
    fm1_adc_init();
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);   /* (abuf is zero: .bss) */
    usb_start();
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    zp12_main();
    for (;;)
        fm1_service();
}

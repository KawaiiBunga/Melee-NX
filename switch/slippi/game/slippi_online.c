/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native Slippi online play, the game side. Ported from Melee Unlocked's
 * sourceport/game/shim/mu_online.c (GPL-2.0-or-later).
 *
 * The console game gets online play from Slippi's patches: it exchanges pads with the host's Slippi
 * device every frame, saves and loads its state around rollbacks, and re-simulates the frames a
 * late remote input changed, all inside the engine loop (gm_801A4D34). This file is the native
 * equivalent, called from the same sites; each function names the patch it stands for. Nothing
 * here runs unless an online match is active.
 *
 * Frame numbers: the online pad frame N (the number sent with each pad) is consumed by the engine
 * body whose unpaused-frame counter (gm_80479D58.unk_8) is N at its start; savestates and
 * checksums are keyed by that counter.
 *
 * This file builds into melee_game_host, so all of its bookkeeping is outside rollback snapshots:
 * a load restores the game, never the record of which frames were predicted or confirmed.
 *
 * Left out from the donor: its test harness and self-test, early inputs, replay recording, the
 * game report, the in-game HUD text (the status line is drawn by the host), Teams, and the sound
 * reconciliation (see AXDriver_8038CFF4 in the hooks patch). */
#include "slippi_game.h"

#include <stdio.h>
#include <string.h>

#include "perf_switch.h"
#include "slippi_host.h"
#include <melee/cm/camera.h>
#include <melee/ft/ftlib.h>
#include <melee/ft/types.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gmvs.h>
#include <melee/lb/lb_0195.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/types.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/random.h>

void fn_800195FC(void); /* lb_0195.c: the pad alarm's handler */
extern StaticPlayer player_slots[];

enum {
    ROLLBACK_MAX = 7,
    LOCAL_INPUTS_COUNT = 2 * ROLLBACK_MAX,
    PREDICTED_INPUTS_COUNT = 2 * ROLLBACK_MAX,
    RXB_INPUTS_COUNT = ROLLBACK_MAX,
    REMOTES = 3,
    PAD_SIZE = 12,
    MIN_DELAY = 1,
    MAX_DELAY = 15,
    UNFREEZE_FRAME = 84,
    START_SYNC_FRAME = UNFREEZE_FRAME - 6,
    DESYNC_ENTRIES = ROLLBACK_MAX * 3,
    USED_PADS_COUNT = 16,
    RESP_NORMAL = 1,
    RESP_SKIP = 2,
    RESP_DISCONNECTED = 3,
    RESP_ADVANCE = 4,
    CMD_ONLINE_INPUTS = 0xB0,
    CMD_CAPTURE_SAVESTATE = 0xB1,
    CMD_LOAD_SAVESTATE = 0xB2,
    CMD_GET_MATCH_STATE = 0xB3,
    RESPONSE_CAPACITY = 4096,
    /* The match state response (Slippi's MSRB), byte offsets. */
    MSRB_LOCAL_PLAYER_INDEX = 3,
    MSRB_REMOTE_PLAYER_INDEX = 4,
    MSRB_RNG_OFFSET = 5,
    MSRB_DELAY_FRAMES = 9,
    MSRB_GAME_INFO_BLOCK = 598,
    MSRB_SIZE = 962,
    /* The inputs response (Slippi's RXB), byte offsets. */
    RXB_RESULT = 0,
    RXB_OPNT_COUNT = 1,
    RXB_OPNT_DESYNC = 2,
    RXB_OPNT_FRAME_NUMS = RXB_OPNT_DESYNC + 8 * REMOTES,
    RXB_SMALLEST_LATEST = RXB_OPNT_FRAME_NUMS + 4 * REMOTES,
    RXB_OPNT_INPUTS = RXB_SMALLEST_LATEST + 4,
    RXB_SHOULD_DESPAWN = RXB_OPNT_INPUTS + PAD_SIZE * RXB_INPUTS_COUNT * REMOTES,
    RXB_SIZE = RXB_SHOULD_DESPAWN + REMOTES,
    MODE_DIRECT = 2,
};

enum { ENGINE_CONTINUE = 0, ENGINE_RESIM = 1, ENGINE_ROLLBACK_DONE = 2 };

typedef struct DesyncLocal {
    int frame;
    unsigned int checksum;
} DesyncLocal;

static struct {
    int active;      /* an online match is running */
    int pending;     /* an online match has been negotiated and waits for its scene */
    int mode;        /* Slippi's mode id: 1 Unranked, 2 Direct */
    u8 local_index;  /* the local player's in-game port */
    u8 remote_index;
    u8 input_source; /* the physical port the local pad is read from */
    u8 delay;
    u8 frozen_stadium;
    int frame; /* next online pad frame to fetch */
    unsigned int rng_offset;
    u8 last_local[PAD_SIZE];
    u8 delay_index;
    u8 delay_buffer[MAX_DELAY][PAD_SIZE];
    u8 rxb[RXB_SIZE];
    int rollback_active, rollback_should_load, rollback_end_frame;
    u8 local_inputs_idx;
    u8 local_inputs[LOCAL_INPUTS_COUNT][PAD_SIZE];
    u8 predicted_read[REMOTES], predicted_write[REMOTES];
    u8 predicted[REMOTES][PREDICTED_INPUTS_COUNT][PAD_SIZE];
    int savestate_predicting, savestate_frame;
    int player_savestate_frame[REMOTES];
    u8 player_predicting[REMOTES];
    int stable_rollback_active, stable_rollback_end_frame, stable_should_load, stable_savestate_frame;
    int stable_finalized, finalized;
    int force_pad_renew;
    int frame_advance;
    int disconnected, disconnect_displayed, game_over, game_end_frame;
    int desync_displayed, desync_risk_displayed;
    int desync_last_frame;
    u8 desync_write_idx;
    DesyncLocal desync_local[DESYNC_ENTRIES];
    unsigned int tx_checksum; /* the checksum field of the last pad sent (Slippi reuses its buffer) */
    int direct_renew;         /* depth of pad renews the rollback logic called itself */
    int resim;                /* re-simulating after a rollback load */
    /* The pads each recent frame used, every port in wire form, and whether every remote pad in it
     * was confirmed: a re-simulated confirmed frame must get the same pads again. */
    int used_frame[USED_PADS_COUNT];
    u8 used_confirmed[USED_PADS_COUNT];
    u8 used_pads[USED_PADS_COUNT][4][PAD_SIZE];
    int loop_entered;   /* the scene's engine loop has reached its pad wait */
    int engine_started; /* the engine body of frame 0 has begun */
    /* Diagnostics, logged at match exit. */
    unsigned int rollbacks, resim_frames, max_depth, skips, advances, skip_run;
    unsigned int predictions, mispredictions, resim_input_mismatches;
    unsigned long long rollback_start_ns, rollback_ns_total;
    unsigned int rollback_us_max, rollback_over_frame;
    u8 match_state[RESPONSE_CAPACITY];
} online;

static void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char* fmt, ...)
{
    char line[320];
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    __builtin_va_end(args);
    melee_nx_slippi_log(line);
}

static u32 be32(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}
static void put32(u8* p, u32 v)
{
    p[0] = (u8) (v >> 24);
    p[1] = (u8) (v >> 16);
    p[2] = (u8) (v >> 8);
    p[3] = (u8) v;
}

int melee_nx_online_active(void)
{
    return online.active;
}
int melee_nx_online_resim(void)
{
    return online.resim;
}
int melee_nx_online_codes(void)
{
    return online.active || online.pending;
}
int melee_nx_online_mode(void)
{
    return online.active ? online.mode : -1;
}
int melee_nx_online_local_port(void)
{
    return online.active ? online.local_index : -1;
}
int melee_nx_online_frozen_stadium(void)
{
    return online.frozen_stadium;
}

static int match_finished; /* outside `online`: arming the next match clears that block */
int melee_nx_online_take_finished(void)
{
    int was = match_finished;
    match_finished = 0;
    if (was) {
        melee_nx_slippi_hint(360);
    }
    return was;
}

/* Slippi mode boots straight to VS mode's character select (Slippi's "Boot to CSS"), since that is
 * where online play starts. */
int melee_nx_online_boot_mode(int mode)
{
    if (!melee_nx_slippi_enabled()) {
        return mode;
    }
    melee_nx_slippi_hint(900);
    return GM_VS;
}

/* The patches' short-circuit: an online match is running and the scene is not being left. */
static int hooks_on(void)
{
    return online.active && melee_nx_engine_state()->unk_C == 0;
}

static int global_frame(void)
{
    return (int) melee_nx_engine_state()->unk_8;
}

/* ---- pads on the wire: Slippi's 12-byte report, the console PADStatus in big-endian order ---- */

static void pad_to_wire(u8* out, const PADStatus* pad)
{
    out[0] = (u8) (pad->button >> 8);
    out[1] = (u8) pad->button;
    out[2] = (u8) pad->stickX;
    out[3] = (u8) pad->stickY;
    out[4] = (u8) pad->substickX;
    out[5] = (u8) pad->substickY;
    out[6] = pad->triggerLeft;
    out[7] = pad->triggerRight;
    out[8] = pad->analogA;
    out[9] = pad->analogB;
    out[10] = (u8) pad->err;
    out[11] = 0;
}

static void wire_to_pad(PADStatus* pad, const u8* in)
{
    pad->button = (u16) (in[0] << 8 | in[1]);
    pad->stickX = (s8) in[2];
    pad->stickY = (s8) in[3];
    pad->substickX = (s8) in[4];
    pad->substickY = (s8) in[5];
    pad->triggerLeft = in[6];
    pad->triggerRight = in[7];
    pad->analogA = in[8];
    pad->analogB = in[9];
    pad->err = (s8) in[10];
}

/* ---- the finalized-state checksum (StartEngineLoop FN_COMPUTE_CHECKSUM) ---- */

static u32 float_bits(float f)
{
    u32 v;
    memcpy(&v, &f, 4);
    return v;
}

static u32 compute_checksum(void)
{
    u32 acc = 0;
    float sum = 0.0F;
    int i, k;
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 2; k++) {
            HSD_GObj* gobj = player_slots[i].player_entity[k];
            Fighter* fp;
            if (gobj == NULL) {
                continue;
            }
            fp = gobj->user_data;
            acc ^= (u32) fp->motion_id;
            acc ^= float_bits(fp->cur_pos.x);
            acc ^= float_bits(fp->cur_pos.y);
            acc ^= float_bits(fp->dmg.x1830_percent);
            acc ^= (u32) fp->x8_spawnNum;
            sum = sum + fp->cur_pos.x;
            sum = sum + fp->cur_pos.y;
            sum = sum + fp->dmg.x1830_percent;
        }
        acc ^= (u8) player_slots[i].stocks;
    }
    return (((acc >> 16) ^ (acc & 0xFFFF)) << 16) | ((u32) (s32) sum & 0xFFFF);
}

/* ---- host commands ---- */

static int command(unsigned int cmd, const u8* payload, unsigned int size, unsigned int* response_size)
{
    uint32_t got = 0;
    int result = mu_slippi_command((uint8_t) cmd, payload, size, online.match_state, RESPONSE_CAPACITY, &got);
    if (response_size != NULL) {
        *response_size = got;
    }
    if (result != 0) {
        logf_("online: host refused command %02X (%d)", cmd, result);
    }
    return result;
}

static void capture_state(int frame)
{
    u8 payload[32];
    memset(payload, 0, sizeof payload);
    put32(payload, (u32) frame);
    command(CMD_CAPTURE_SAVESTATE, payload, sizeof payload, NULL);
}

static void load_state(int frame)
{
    u8 payload[32];
    memset(payload, 0, sizeof payload); /* no preserve blocks: the bookkeeping is excluded */
    put32(payload, (u32) frame);
    command(CMD_LOAD_SAVESTATE, payload, sizeof payload, NULL);
}

/* ---- RenewInputs_Prefunction called by the rollback logic itself ---- */

static void direct_renew(void)
{
    online.direct_renew++;
    fn_800195FC();
    online.direct_renew--;
}

/* PreventPadAlarmDuringRollback (80019608), at the top of fn_800195FC. Nonzero: skip the renew. */
int melee_nx_online_pad_alarm_gate(void)
{
    if (!hooks_on()) {
        return 0;
    }
    online.frame_advance = 0;
    if (online.direct_renew != 0) {
        return 0; /* the rollback logic asked for these pads */
    }
    if (online.rollback_active) {
        /* The pad alarm fired during a rollback: renew at the earliest safe time instead. */
        online.force_pad_renew = 1;
        return 1;
    }
    return 0;
}

/* ForceInputRefetchOnAdvance (80019614), at the end of fn_800195FC. */
void melee_nx_online_after_pad_renew(void)
{
    if (hooks_on() && online.frame_advance) {
        direct_renew();
    }
}

/* SkipNewInputFetchOnRollback (80376A20): nonzero skips PADRead during a rollback. */
int melee_nx_online_skip_pad_read(void)
{
    return hooks_on() && online.rollback_active && !online.rollback_should_load;
}

/* ---- TriggerSendInput (80376A28), right after PADRead in HSD_PadRenewRawStatus ---- */

static int load_opponent_inputs(PADStatus* stat, int frame)
{
    int count, remote_port = 0, have_all = 1;
    u8* rxb = online.rxb;
    for (count = 0; count < REMOTES; count++, remote_port++) {
        int latest;
        const u8* slots = rxb + RXB_OPNT_INPUTS + count * RXB_INPUTS_COUNT * PAD_SIZE;
        const u8* src = slots; /* entry k is the pad of frame (latest - k) */
        if (remote_port == online.local_index) {
            remote_port++;
        }
        latest = (int) be32(rxb + RXB_OPNT_FRAME_NUMS + 4 * count);
        if (latest - frame >= 0) {
            if (latest - frame < RXB_INPUTS_COUNT) {
                src = slots + (latest - frame) * PAD_SIZE;
            }
        } else if (frame >= START_SYNC_FRAME - online.delay && !online.game_over) {
            /* Predicting: the newest confirmed pad, kept for comparison once the real one arrives. */
            u8 w = online.predicted_write[count];
            have_all = 0;
            memcpy(online.predicted[count][w], src, PAD_SIZE);
            online.predicted_write[count] = (u8) ((w + 1) % PREDICTED_INPUTS_COUNT);
            if (!online.player_predicting[count]) {
                online.player_savestate_frame[count] = frame;
                online.player_predicting[count] = 1;
                online.predicted_read[count] = w;
                if (!online.savestate_predicting) {
                    online.savestate_frame = frame;
                    online.savestate_predicting = 1;
                }
            }
        }
        if (remote_port < 4) {
            wire_to_pad(&stat[remote_port], src);
        }
    }
    if (have_all) {
        online.finalized = frame;
    }
    return have_all;
}

static int used_slot(int frame)
{
    int slot = frame % USED_PADS_COUNT;
    return slot < 0 ? slot + USED_PADS_COUNT : slot;
}

static void record_used_pads(const PADStatus* stat, int frame, int confirmed)
{
    int slot = used_slot(frame), p;
    online.used_frame[slot] = frame;
    online.used_confirmed[slot] = (u8) confirmed;
    for (p = 0; p < 4; p++) {
        pad_to_wire(online.used_pads[slot][p], &stat[p]);
    }
}

/* Section 9: compare earlier predictions with inputs that have now arrived. Returns 1 when a
 * rollback was triggered. */
static int check_predictions(int frame)
{
    u8* rxb = online.rxb;
    int rollback_required = online.rollback_active;
    int count, savestate, values_set = 0;

    if (!online.savestate_predicting) {
        online.player_predicting[0] = online.player_predicting[1] = online.player_predicting[2] = 0;
        return 0;
    }
    for (count = 0; count < REMOTES; count++) {
        while (online.player_predicting[count]) {
            int latest = (int) be32(rxb + RXB_OPNT_FRAME_NUMS + 4 * count);
            int ss = online.player_savestate_frame[count];
            int offset = latest - ss;
            const u8* actual;
            const u8* predicted;
            int t, differ = 0;
            if (offset < 0) {
                break; /* the inputs for the savestate frame have not arrived yet */
            }
            if (ss <= online.stable_finalized) {
                if (latest > ss) {
                    goto inputs_match; /* already finalized: advance without comparing */
                }
                break;
            }
            actual = rxb + RXB_OPNT_INPUTS + count * RXB_INPUTS_COUNT * PAD_SIZE + offset * PAD_SIZE;
            predicted = online.predicted[count][online.predicted_read[count]];
            /* The comparison Slippi makes: the buttons the game reads, both sticks, and the
             * triggers outside their dead zone. */
            if ((actual[0] & 0x1F) != (predicted[0] & 0x1F) || (actual[1] & 0x7F) != (predicted[1] & 0x7F) ||
                memcmp(actual + 2, predicted + 2, 4) != 0)
            {
                differ = 1;
            }
            for (t = 6; t < 8 && !differ; t++) {
                if (actual[t] <= 42 && predicted[t] <= 42) {
                    continue;
                }
                if (actual[t] != predicted[t]) {
                    differ = 1;
                }
            }
            online.predictions++;
            if (differ) {
                online.mispredictions++;
                rollback_required = 1;
                break;
            }
        inputs_match:
            online.player_savestate_frame[count] = ss + 1;
            online.predicted_read[count] = (u8) ((online.predicted_read[count] + 1) % PREDICTED_INPUTS_COUNT);
            if (online.predicted_read[count] == online.predicted_write[count]) {
                break; /* caught up with the prediction */
            }
        }
    }
    /* The savestate frame is the lowest frame any remote is still predicted from. */
    savestate = online.stable_finalized;
    for (count = 0; count < REMOTES; count++) {
        if (online.player_predicting[count] != 1) {
            continue;
        }
        if (!values_set || online.player_savestate_frame[count] < savestate) {
            savestate = online.player_savestate_frame[count];
        }
        values_set = 1;
    }
    online.savestate_frame = savestate;
    online.finalized = savestate;
    if (savestate > (int) be32(rxb + RXB_SMALLEST_LATEST)) {
        online.finalized = (int) be32(rxb + RXB_SMALLEST_LATEST);
    }
    if (rollback_required) {
        online.rollback_start_ns = melee_nx_perf_now();
        online.rollback_active = 1;
        online.rollback_should_load = 1;
        online.rollback_end_frame = frame;
        online.frame = frame + 1;
        online.rollbacks++;
        if ((unsigned int) (frame - savestate) > online.max_depth) {
            online.max_depth = (unsigned int) (frame - savestate);
        }
        return 1;
    }
    for (count = 0; count < REMOTES; count++) {
        if (online.player_predicting[count] == 1 && online.predicted_read[count] == online.predicted_write[count]) {
            online.player_predicting[count] = 0;
        }
    }
    for (count = 0; count < REMOTES; count++) {
        if (online.player_predicting[count] == 1) {
            return 0;
        }
    }
    online.savestate_predicting = 0;
    return 0;
}

static void clamp_stick_at_rest(PADStatus* pad)
{
    if (pad->stickX >= -2 && pad->stickX <= 2 && pad->stickY >= -2 && pad->stickY <= 2) {
        pad->stickX = pad->stickY = 0;
    }
    if (pad->substickX >= -2 && pad->substickX <= 2 && pad->substickY >= -2 && pad->substickY <= 2) {
        pad->substickX = pad->substickY = 0;
    }
}

int melee_nx_online_pad_renew(PADStatus* stat)
{
    int frame, i;
    u8 tx[25];
    unsigned int got = 0;
    PADStatus* source;

    if (!hooks_on()) {
        return 0;
    }
    frame = online.frame;

    /* The engine body of frame 0 takes a sample from before the match and online pad N goes to the
     * body of frame N. No online pad is fetched before the engine loop waits for pads, and when
     * the loop finds the queue empty body 0 gets a neutral sample of its own. */
    if (!online.engine_started) {
        if (!online.loop_entered) {
            return 1;
        }
        if (HSD_PadGetRawQueueCount() == 0) {
            memset(stat, 0, 4 * sizeof *stat);
            return 0;
        }
    }

    /* During a rollback the pads come from history; nothing new is sent. */
    if (online.rollback_active && !online.rollback_should_load) {
        int idx, p, slot = used_slot(frame), confirmed;
        int known = online.used_frame[slot] == frame;
        if (frame > online.rollback_end_frame) {
            return 1;
        }
        memset(stat, 0, 4 * sizeof *stat);
        idx = online.local_inputs_idx - (online.rollback_end_frame - frame + 1);
        if (idx < 0) {
            idx += LOCAL_INPUTS_COUNT;
        }
        wire_to_pad(&stat[online.local_index], online.local_inputs[idx]);
        confirmed = load_opponent_inputs(stat, frame);
        if (known && online.used_confirmed[slot]) {
            /* Every pad of this frame was confirmed the first time: they must not change. */
            u8 wire[PAD_SIZE];
            for (p = 0; p < 4; p++) {
                pad_to_wire(wire, &stat[p]);
                if (memcmp(wire, online.used_pads[slot][p], PAD_SIZE) != 0) {
                    if (online.resim_input_mismatches++ < 8) {
                        logf_("online: re-simulated frame %d port %d got other pads than it confirmed", frame, p);
                    }
                    break;
                }
            }
        }
        record_used_pads(stat, frame, confirmed);
        online.frame = frame + 1;
        online.resim_frames++;
        return 0;
    }

    /* Section 1: every port is zero until shortly before the match unfreezes, so both players'
     * games agree on finalized frames. */
    if (frame < START_SYNC_FRAME - online.delay) {
        memset(stat, 0, 4 * sizeof *stat);
    }
    source = &stat[online.input_source];
    /* Section 2: resting sticks read as exactly zero so noise does not cause rollbacks. */
    clamp_stick_at_rest(source);
    /* Section 3: a stale report repeats the previous one, as the game treats it locally. */
    if (source->err == -3) {
        wire_to_pad(source, online.last_local);
    }
    pad_to_wire(online.last_local, source);

    /* Section 4: send this frame's pad. */
    put32(tx, (u32) frame);
    put32(tx + 4, (u32) online.stable_finalized);
    /* The finalized frame's checksum; when it is not in the ring the previous value goes out
     * again, as the console's reused transfer buffer does. A zero would read as a desync. */
    for (i = 0; i < DESYNC_ENTRIES; i++) {
        if (online.desync_local[i].frame == online.stable_finalized) {
            online.tx_checksum = online.desync_local[i].checksum;
            break;
        }
    }
    put32(tx + 8, online.tx_checksum);
    tx[12] = online.delay;
    memcpy(tx + 13, online.last_local, PAD_SIZE);
    if (command(CMD_ONLINE_INPUTS, tx, sizeof tx, &got) == 0 && got >= RXB_SIZE) {
        memcpy(online.rxb, online.match_state, RXB_SIZE);
    } else {
        /* No answer, or the host's one-byte "disconnected" answer, means no match. The frame runs
         * with the last pads the buffer holds, so the frame that ends the game is reached. */
        if (!online.disconnected) {
            logf_("online: host reply %u bytes at frame %d, treating as disconnect", got, frame);
        }
        online.disconnected = 1;
        online.rxb[RXB_RESULT] = RESP_DISCONNECTED;
    }

    /* Section 5: skip (both games wait for each other), disconnect, or advance. */
    online.frame_advance = 0;
    switch (online.rxb[RXB_RESULT]) {
    case RESP_SKIP:
        if (!online.game_over) {
            if (online.skip_run++ == 0) {
                logf_("online: skip starts at frame %d", frame);
            }
            online.skips++;
            return 1;
        }
        break;
    case RESP_DISCONNECTED:
        online.disconnected = 1;
        break;
    case RESP_ADVANCE:
        online.frame_advance = 1;
        online.advances++;
        break;
    default:
        break;
    }
    if (online.skip_run > 0) {
        logf_("online: skip ends at frame %d after %u retraces", frame, online.skip_run);
        online.skip_run = 0;
    }

    /* Section 6: the local player's pad is the one from `delay` frames ago. */
    wire_to_pad(&stat[online.local_index], online.delay_buffer[online.delay_index]);
    /* Section 7: keep it for rollbacks. */
    memcpy(online.local_inputs[online.local_inputs_idx], online.delay_buffer[online.delay_index], PAD_SIZE);
    online.local_inputs_idx = (u8) ((online.local_inputs_idx + 1) % LOCAL_INPUTS_COUNT);
    /* Section 8: this frame's pad goes into the delay buffer. */
    memcpy(online.delay_buffer[online.delay_index], tx + 13, PAD_SIZE);
    online.delay_index = (u8) ((online.delay_index + 1) % online.delay);

    /* Section 9: rollback decision. */
    if (check_predictions(frame)) {
        return 1;
    }
    /* Section 10: remote pads for this frame, predicted where they have not arrived. */
    record_used_pads(stat, frame, load_opponent_inputs(stat, frame));
    online.frame = frame + 1;
    return 0;
}

/* ---- ForceEngineOnRollback (801A4DB4), the pad wait of gm_801A4D34 ----
 * Nonzero: run the engine now (with *count samples). */
int melee_nx_online_engine_gate(int* count)
{
    if (!hooks_on()) {
        return *count != 0;
    }
    online.loop_entered = 1;
    if (*count <= 0 && !online.rollback_active) {
        if (online.force_pad_renew) {
            online.force_pad_renew = 0;
            direct_renew();
        }
        melee_nx_slippi_idle(); /* waiting for the next pad sample or for the other game */
        return 0;
    }
    if (*count <= 0) {
        *count = 1; /* one iteration drives the whole rollback */
    }
    online.stable_rollback_active = online.rollback_active;
    online.stable_rollback_end_frame = online.rollback_end_frame;
    online.stable_should_load = online.rollback_should_load;
    online.stable_savestate_frame = online.savestate_frame;
    return 1;
}

/* ---- StartEngineLoop (801A4DE4), top of each engine body ---- */

void melee_nx_online_frame_begin(void)
{
    int frame, i;
    u8* rxb;

    if (!hooks_on()) {
        return;
    }
    online.engine_started = 1;
    frame = global_frame();
    rxb = online.rxb;

    if (!online.disconnect_displayed && online.disconnected && !online.game_over) {
        online.disconnect_displayed = 1;
        logf_("online: disconnected at frame %d", frame);
        melee_nx_slippi_status("Disconnected", 240);
        melee_nx_gmvs_request_online_end((int) online.remote_index);
    }

    if (online.stable_rollback_active) {
        if (online.stable_should_load) {
            int ss = online.stable_savestate_frame;
            if (frame < ss) {
                goto rollback_inputs_done;
            }
            if (frame > ss) {
                load_state(ss);
                online.resim = 1;
                melee_nx_slippi_resim_phase(1);
            }
            {
                /* Rewind the raw pad queue: the loaded state predates the samples in it. */
                PadLibData* p = &HSD_PadLibData;
                int queued = lb_80019894();
                int w = p->qwrite - queued;
                if (w < 0) {
                    w += p->qnum;
                }
                p->qwrite = (u8) w;
                p->qcount = 0;
            }
            frame = global_frame();
            online.frame = online.savestate_frame;
            online.savestate_predicting = 0;
            online.player_predicting[0] = online.player_predicting[1] = online.player_predicting[2] = 0;
            online.rollback_should_load = 0;
            online.stable_should_load = 0;
        }
        direct_renew(); /* the next pads of the rollback */
    }
rollback_inputs_done:

    /* The stable finalized frame never runs ahead of the frame being processed. */
    {
        int v = frame > online.finalized ? online.finalized : frame;
        if (v > online.stable_finalized) {
            online.stable_finalized = v;
        }
    }

    /* Checksum for this frame (overwritten when the frame is re-simulated). */
    {
        int offset = frame - (online.desync_last_frame + 1);
        int idx = online.desync_write_idx;
        if (offset >= 0) {
            online.desync_write_idx = (u8) ((online.desync_write_idx + 1) % DESYNC_ENTRIES);
            online.desync_last_frame = frame;
        }
        idx = (idx + offset) % DESYNC_ENTRIES;
        if (idx < 0) {
            idx += DESYNC_ENTRIES;
        }
        online.desync_local[idx].frame = frame;
        online.desync_local[idx].checksum = compute_checksum();
    }

    /* Compare the remote checksums of finalized frames with ours. */
    if (frame != 0 && !online.desync_displayed) {
        int remote_count = rxb[RXB_OPNT_COUNT];
        for (i = 0; i < remote_count && i < REMOTES; i++) {
            int cf = (int) be32(rxb + RXB_OPNT_DESYNC + 8 * i);
            u32 remote = be32(rxb + RXB_OPNT_DESYNC + 8 * i + 4);
            int k;
            if (cf > online.stable_finalized || cf <= UNFREEZE_FRAME) {
                continue;
            }
            for (k = 0; k < DESYNC_ENTRIES; k++) {
                u32 local;
                int diff;
                if (online.desync_local[k].frame != cf) {
                    continue;
                }
                local = online.desync_local[k].checksum;
                diff = (int) (s16) (local & 0xFFFF) - (int) (s16) (remote & 0xFFFF);
                if (diff < -1 || diff > 1) {
                    online.desync_displayed = 1;
                    logf_("online: DESYNC at frame %d (ours %08X, theirs %08X)", cf, (unsigned) local,
                          (unsigned) remote);
                    melee_nx_slippi_status("Desync detected", 240);
                    melee_nx_gmvs_request_online_end((int) online.remote_index);
                    goto desync_done;
                } else if (!online.desync_risk_displayed && (local >> 16) != (remote >> 16)) {
                    online.desync_risk_displayed = 1;
                    logf_("online: desync risk at frame %d (ours %08X, theirs %08X)", cf, (unsigned) local,
                          (unsigned) remote);
                }
                break;
            }
        }
    }

desync_done:
    /* Keep this frame while any remote input in it is predicted. */
    if (online.savestate_predicting && frame > online.stable_finalized) {
        capture_state(frame);
    }

    /* The match is over once it has stayed over past the rollback window. */
    if (!online.game_over) {
        if (gmVs_GetSceneState()->match_result == 0) {
            online.game_end_frame = 0;
        } else {
            if (online.game_end_frame == 0) {
                online.game_end_frame = frame;
            }
            if (frame - online.game_end_frame > ROLLBACK_MAX) {
                online.game_over = 1;
                logf_("online: match over at frame %d", frame);
            }
        }
    }
}

/* ---- LoopEngineForRollback (801A5014), end of each engine body ---- */

/* What the render pass would have done between two re-simulated frames that gameplay depends on:
 * Slippi's ExecCameraTasks (the camera values behind tag and offscreen positions, and each awake
 * fighter's offscreen state), plus the fighter draw's bone matrix setup, which gameplay reads raw
 * on the next frame. */
static void camera_tasks(void)
{
    HSD_GObj* gobj;
    Camera_8002A4AC(Camera_80030A50());
    HSD_CObjSetCurrent(Camera_80030A50()->hsd_obj);
    for (gobj = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER]; gobj != NULL; gobj = gobj->next) {
        Fighter* fp = gobj->user_data;
        if (!fp->x221F_b3 && ftLib_80086A8C(gobj)) {
            melee_nx_refresh_part_matrices(fp);
        }
    }
}

static void rollback_finished(void)
{
    online.rollback_active = 0;
    online.stable_rollback_active = 0;
    if (online.resim) {
        online.resim = 0;
        melee_nx_slippi_resim_phase(0);
    }
    if (online.rollback_start_ns) {
        unsigned int us = (unsigned int) ((melee_nx_perf_now() - online.rollback_start_ns) / 1000);
        online.rollback_start_ns = 0;
        online.rollback_ns_total += us;
        if (us > online.rollback_us_max) {
            online.rollback_us_max = us;
        }
        if (us > 16683) {
            online.rollback_over_frame++;
        }
    }
}

int melee_nx_online_frame_end(void)
{
    if (!hooks_on() || !online.stable_rollback_active) {
        return ENGINE_CONTINUE;
    }
    if (global_frame() - 1 < online.stable_rollback_end_frame) {
        camera_tasks();
        return ENGINE_RESIM;
    }
    rollback_finished();
    return ENGINE_ROLLBACK_DONE;
}

/* The scene loop can end on a re-simulated frame (a late remote input ends the match inside the
 * re-run): the rollback ends here, as it would have on its last frame. */
void melee_nx_online_scene_loop_exit(void)
{
    if (online.resim) {
        logf_("online: scene ended during a rollback at frame %d", global_frame());
        rollback_finished();
    }
}

/* ---- the match (InitOnlinePlay, 8016E748) ---- */

/* Slippi's SyncRNG: every frame the seed is the frame number (halves swapped) plus the match's
 * offset, so both games draw the same numbers whatever happened before. */
static void sync_rng_proc(HSD_GObj* gobj)
{
    u32 f = (u32) global_frame();
    (void) gobj;
    *HSD_RandSeedPtr = ((f << 16) | (f >> 16)) + online.rng_offset;
}

int melee_nx_online_lobby(VsModeData* vs)
{
    int i, port = -1;
    u64 mask = 0;
    struct GameCache* gc;
    const u8* info;

    for (i = 0; i < 4; i++) {
        if (vs->start.players[i].slot_type == Gm_PKind_Human) {
            port = i;
            break;
        }
    }
    /* Plain VS mode only: the special melee modes share this exit but not Slippi's rules. */
    if (port < 0 || gm_GetCurrentGameMode() != GM_VS || !melee_nx_slippi_enabled()) {
        return 1; /* offline */
    }
    memset(&online, 0, sizeof online);
    if (!melee_nx_slippi_lobby((uint8_t) vs->start.players[port].ckind, vs->start.players[port].color,
                               vs->start.rules.stkind, port, online.match_state, RESPONSE_CAPACITY))
    {
        return 0;
    }
    online.input_source = (u8) port;
    online.mode = melee_nx_slippi_mode();
    online.pending = 1;
    info = online.match_state + MSRB_GAME_INFO_BLOCK;
    melee_nx_apply_game_info(&vs->start, info);

    /* The files and sounds of the match as negotiated, the way Tournament mode names a match's
     * fighters after the character select (gmtou_1.c). */
    gc = &lbDvd_GetPreloadCacheScene()->game_cache;
    lbDvd_SetupVsPreloadCache();
    for (i = 0; i < 4; i++) {
        if (vs->start.players[i].slot_type != Gm_PKind_NA) {
            gc->entries[i].char_id = vs->start.players[i].ckind;
            gc->entries[i].color = vs->start.players[i].color;
            mask |= lbAudioAx_80026E84(vs->start.players[i].ckind);
        }
    }
    gc->stkind = vs->start.rules.stkind;
    lbDvd_80018254();
    lbAudioAx_80026F2C(20);
    lbAudioAx_8002702C(4, mask);
    lbAudioAx_80027168();
    return 1;
}

/* VS mode is left for the menu: the connection to the opponent goes with it. */
void melee_nx_online_leave(void)
{
    if (melee_nx_slippi_enabled()) {
        command(0xBA /* CMD_CLEANUP_CONNECTION */, NULL, 0, NULL);
    }
}

void melee_nx_online_start_melee(StartMeleeData* data)
{
    HSD_GObj* gobj;
    int i, delay;

    if (!online.pending) {
        return;
    }
    online.pending = 0;
    online.frame = 1;
    for (i = 0; i < USED_PADS_COUNT; i++) {
        online.used_frame[i] = -1;
    }
    online.local_index = online.match_state[MSRB_LOCAL_PLAYER_INDEX] & 3;
    online.remote_index = online.match_state[MSRB_REMOTE_PLAYER_INDEX] & 3;
    online.rng_offset = be32(online.match_state + MSRB_RNG_OFFSET);
    online.frozen_stadium = online.match_state[MSRB_SIZE - 1];
    *HSD_RandSeedPtr = online.rng_offset;
    melee_nx_apply_game_info(data, online.match_state + MSRB_GAME_INFO_BLOCK);
    delay = online.match_state[MSRB_DELAY_FRAMES];
    if (delay < MIN_DELAY) {
        delay = MIN_DELAY;
    }
    if (delay > MAX_DELAY) {
        delay = MAX_DELAY;
    }
    online.delay = (u8) delay;
    /* No transformation from a held A: clear every port's copied buttons. */
    for (i = 0; i < 4; i++) {
        HSD_PadCopyStatus[i].button = 0;
    }
    gobj = GObj_Create(4, 7, 0);
    HSD_GObj_SetupProc(gobj, sync_rng_proc, 0);
    online.active = 1;
    logf_("online: match starts, local port %d (pad %d), delay %d, rng offset %08X", online.local_index,
          online.input_source, online.delay, online.rng_offset);
}

void melee_nx_online_match_exit(void)
{
    if (!online.active) {
        return;
    }
    logf_("online: match exit: %u rollbacks (deepest %u frames), %u re-simulated frames, %u skips, %u advances",
          online.rollbacks, online.max_depth, online.resim_frames, online.skips, online.advances);
    logf_("online: %u predictions compared, %u wrong; %u re-simulated frames got other pads", online.predictions,
          online.mispredictions, online.resim_input_mismatches);
    logf_("online: rollback time: longest %u us, average %u us, %u over a frame", online.rollback_us_max,
          online.rollbacks ? (unsigned) (online.rollback_ns_total / online.rollbacks) : 0, online.rollback_over_frame);
    if (online.resim) {
        online.resim = 0;
        melee_nx_slippi_resim_phase(0);
    }
    online.active = 0;
    match_finished = 1;
}

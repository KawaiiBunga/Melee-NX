/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi's gameplay codes that need code of their own: UCF 0.84, neutral spawns, offscreen damage,
 * and the match's game info block. The one-line codes sit at their sites in the patched sources.
 * Ported from Melee Unlocked (sourceport/game/shim/mu_gecko.c, mu_replay.c and its gmvs.c patch,
 * GPL-2.0-or-later). All of it runs only while melee_nx_online_codes() is nonzero.
 *
 * This file is game state: it is built into melee_game, so the UCF pad ring is inside rollback
 * snapshots like the fighters it describes. */
#include "slippi_game.h"

#include <string.h>

#include <melee/ft/fighter.h>
#include <melee/ft/ftcommon.h>
#include <melee/ft/kinds/ftCommon/ftCo_Attack100.h>
#include <melee/it/types.h>
#include <melee/ft/kinds/ftCommon/ftCo_0A01.h>
#include <melee/ft/types.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gm_unsplit.h>
#include <melee/gr/stage.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/controller.h>

/* ---- the game info block (console layout, 0x138 bytes) into the match about to start ---- */

static u32 be32(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}
static u16 be16(const u8* p)
{
    return (u16) (p[0] << 8 | p[1]);
}
static float bef(const u8* p)
{
    u32 v = be32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

#define BIT(b, n) ((g[b] >> (7 - (n))) & 1)

static void convert_rules(StartMeleeRules* r, const u8* g)
{
    r->match_kind = g[0] >> 5;
    r->x0_3 = (g[0] >> 2) & 7;
    r->timer_enabled = BIT(0, 6);
    r->timer_counts_up = BIT(0, 7);
    r->x1_0 = BIT(1, 0); r->x1_1 = BIT(1, 1); r->x1_2 = BIT(1, 2); r->x1_3 = BIT(1, 3);
    r->x1_4 = BIT(1, 4); r->x1_5 = BIT(1, 5); r->timer_shows_hours = BIT(1, 6);
    r->friendly_fire = BIT(1, 7);
    r->is_stock = BIT(2, 0); r->x2_1 = BIT(2, 1); r->x2_2 = BIT(2, 2); r->single_button = BIT(2, 3);
    r->disable_pausing = BIT(2, 4); r->x2_5 = BIT(2, 5); r->x2_6 = BIT(2, 6); r->x2_7 = BIT(2, 7);
    r->x3_0 = BIT(3, 0); r->x3_1 = BIT(3, 1); r->x3_2 = BIT(3, 2); r->x3_3 = BIT(3, 3);
    r->x3_4 = BIT(3, 4); r->x3_5 = BIT(3, 5); r->x3_6 = BIT(3, 6); r->x3_7 = BIT(3, 7);
    r->x4_0 = BIT(4, 0); r->is_vs = BIT(4, 1); r->x4_2 = BIT(4, 2); r->x4_3 = BIT(4, 3);
    r->x4_4 = BIT(4, 4); r->x4_5 = BIT(4, 5); r->x4_6 = BIT(4, 6); r->x4_7 = BIT(4, 7);
    r->x5_0 = BIT(5, 0); r->x5_1 = BIT(5, 1); r->x5_2 = BIT(5, 2); r->x5_3 = BIT(5, 3);
    r->x5_4 = BIT(5, 4); r->x5_5 = BIT(5, 5); r->x5_6 = BIT(5, 6); r->x5_7 = BIT(5, 7);
    r->x6 = g[6];
    r->x7 = g[7];
    r->is_teams = g[8];
    r->x9 = g[9];
    r->xA = g[0xA];
    r->item_freq = (s8) g[0xB];
    r->sd_penalty = (s8) g[0xC];
    r->xD = g[0xD];
    r->stkind = be16(g + 0xE);
    r->time_limit = be32(g + 0x10);
    r->x14 = g[0x14];
    r->x18 = be32(g + 0x18);
    r->x1C_pad[0] = be32(g + 0x1C);
    r->x20 = (u64) be32(g + 0x20) << 32 | be32(g + 0x24);
    r->x28 = (int) be32(g + 0x28);
    r->x2C = bef(g + 0x2C);
    r->x30 = bef(g + 0x30);
    r->game_speed = bef(g + 0x34);
    /* The block's nine pointer slots are console addresses (zero in a negotiated match). */
    r->on_unpause_override = NULL;
    r->on_pause_override = NULL;
    r->check_for_pauser_override = NULL;
    r->on_match_start = NULL;
    r->on_frame_start = NULL;
    r->on_frame_end = NULL;
    r->on_match_end = NULL;
    r->x54 = NULL;
    r->x58 = NULL;
}

static void convert_player(PlayerInitData* p, const u8* g)
{
    p->ckind = (s8) g[0];
    p->slot_type = g[1];
    p->stocks = (s8) g[2];
    p->color = g[3];
    p->slot = g[4];
    p->spawn_pos = (s8) g[5];
    p->spawn_dir = (s8) g[6];
    p->sub_color = g[7];
    p->handicap = (s8) g[8];
    p->team = g[9];
    p->nametag = g[0xA];
    p->xB = g[0xB];
    p->rumble_enabled = BIT(0xC, 0); p->xC_b1 = BIT(0xC, 1); p->vs_metal = BIT(0xC, 2);
    p->xC_b3 = BIT(0xC, 3); p->vs_invisible = BIT(0xC, 4); p->xC_b5 = BIT(0xC, 5);
    p->xC_b6 = BIT(0xC, 6); p->xC_b7 = BIT(0xC, 7);
    p->xD_b0 = BIT(0xD, 0); p->xD_b1 = BIT(0xD, 1); p->xD_b2 = BIT(0xD, 2); p->xD_b3 = BIT(0xD, 3);
    p->xD_b4 = BIT(0xD, 4); p->xD_b5 = BIT(0xD, 5); p->xD_b6 = BIT(0xD, 6); p->xD_b7 = BIT(0xD, 7);
    p->cpu_kind = g[0xE];
    p->cpu_level = g[0xF];
    p->damage = be16(g + 0x10);
    p->damage1 = be16(g + 0x12);
    p->hp = be16(g + 0x14);
    p->attack_ratio = bef(g + 0x18);
    p->defense_ratio = bef(g + 0x1C);
    p->model_scale = bef(g + 0x20);
    if (p->slot_type == 0) {
        p->nametag = 0x78; /* no name tag: the other player's is not on this card */
    }
}

void melee_nx_apply_game_info(StartMeleeData* data, const unsigned char* info)
{
    int i;
    convert_rules(&data->rules, info);
    for (i = 0; i < 6; i++) {
        convert_player(&data->players[i], info + 0x60 + 0x24 * i);
    }
}

/* ---- "BrawlOffscreenDamage" (8006a880): outside the stage's camera limits, in place of the
 * magnifier-bubble test (which depends on each player's own camera) ---- */

int melee_nx_offscreen_damage_zone(Fighter* fp)
{
    float x = fp->cur_pos.x, y = fp->cur_pos.y;
    if (fp->x221F_b1) {
        return 0; /* dead */
    }
    if (fp->motion_id == 4 || fp->motion_id == 6) {
        return 0; /* star KO, screen KO */
    }
    return x < Stage_GetCamBoundsLeftOffset() || x > Stage_GetCamBoundsRightOffset() ||
           y > Stage_GetCamBoundsTopOffset() || y < Stage_GetCamBoundsBottomOffset();
}

/* ---- UCF 0.84 ----
 * The pad-buffer code's data, per controller port: the raw stick of the last four fighter-input
 * frames, the index of the newest sample, and the fast-down-flick counter. Humans only. */
static struct {
    signed char s[4][2];
    unsigned char idx;
    unsigned char flick;
} ucf_ring[4];

static int ucf_port(const Fighter* fp)
{
    return fp->x618_player_id < 4 ? fp->x618_player_id : -1;
}

/* (newest - two samples earlier) squared, for one axis: the codes' "fast movement" test. */
static int ucf_delta2(int port, int axis)
{
    int idx = ucf_ring[port].idx;
    int d = ucf_ring[port].s[idx][axis] - ucf_ring[port].s[(idx - 2) & 3][axis];
    return d * d;
}

/* fctiwz(fmsubs(|v|, 80, 0.0001)) + 2: a normalized axis in stick units, as the codes square it. */
static int ucf_units(float v)
{
    float mag = __builtin_fabsf(v);
    float units = (float) ((double) mag * 80.0 - (double) 0.0001f);
    return (int) units + 2;
}

/* 1.0 cardinals: a raw axis at 80 or beyond with the other axis within 6 becomes exactly +-1, 0. */
static void ucf_cardinal(int x, int y, Vec2* out)
{
    if ((unsigned char) (x + 79) > 158) {
        if ((unsigned char) (y + 6) > 12) {
            return;
        }
        out->x = x < 0 ? -1.0f : 1.0f;
        out->y = 0.0f;
        return;
    }
    if ((unsigned char) (y + 79) <= 158) {
        return;
    }
    if ((unsigned char) (x + 6) > 12) {
        return;
    }
    out->x = 0.0f;
    out->y = y < 0 ? -1.0f : 1.0f;
}

/* UCF Pad Buffer + 1.0 Cardinals, in Fighter_Spaghetti_8006AD10 before the trigger duration
 * counts up. Reads the raw pad the frame's input came from. */
void melee_nx_ucf_pad_buffer(Fighter* fp)
{
    const PADStatus* raw;
    int port, qread, idx, flick;

    if (!melee_nx_online_codes() || ftCo_IsCpuControlled(fp) || (port = ucf_port(fp)) < 0) {
        return;
    }
    qread = HSD_PadLibData.qread != 0 ? HSD_PadLibData.qread - 1 : 4;
    raw = &HSD_PadLibData.queue[qread].stat[port];
    idx = (ucf_ring[port].idx + 1) & 3;
    ucf_ring[port].idx = (unsigned char) idx;
    ucf_ring[port].s[idx][0] = raw->stickX;
    ucf_ring[port].s[idx][1] = raw->stickY;

    /* Not during Zelda's up special start, which reads the stick angle as given. */
    if (!(fp->kind == 19 && fp->motion_id == 349)) {
        ucf_cardinal(raw->stickX, raw->stickY, &fp->input.lstick[0]);
        ucf_cardinal(raw->substickX, raw->substickY, &fp->input.cstick[0]);
    }

    flick = 0;
    if (!(fp->input.lstick[0].y > -0.609375f)) {
        int ux = ucf_units(fp->input.lstick[0].x);
        int uy = ucf_units(fp->input.lstick[0].y);
        if (ux * ux + uy * uy > 6400) {
            flick = ucf_ring[port].flick;
            if (flick != 0) {
                flick = (unsigned char) (flick + 1);
            } else if (fp->active_timer.lstick.y <= 1 && ucf_delta2(port, 1) > 1936) {
                flick = 1;
            }
        }
    }
    ucf_ring[port].flick = (unsigned char) flick;
}

/* UCF Dashback, in ftCo_Turn_IASA right after the turn flips facing_dir: on the turn's second
 * frame, a stick that crossed the dash threshold within a frame and moved fast becomes a dash. */
void melee_nx_ucf_dashback(Fighter* fp)
{
    unsigned int frame_bits;
    float toward;
    int port;
    HSD_GObj* partner;

    if (!melee_nx_online_codes() || fp->is_sub_fighter) {
        return;
    }
    memcpy(&frame_bits, &fp->cur_anim_frame, sizeof frame_bits);
    if (frame_bits != 0x40000000u) {
        return;
    }
    toward = fp->facing_dir * fp->input.lstick[0].x;
    if (toward < p_ftCommonData->dash_smash_stick_threshold) {
        return;
    }
    if (fp->active_timer.lstick.x > 1 || (port = ucf_port(fp)) < 0) {
        return;
    }
    if (ucf_delta2(port, 0) <= 5625) {
        return;
    }
    fp->mv.co.turn.has_turned = true;
    fp->mv.co.turn.just_turned = true;
    /* Ice Climbers: the partner copies the leader's recorded input, so turn its record too. */
    partner = Player_GetEntityAtIndex(fp->player_id, 1);
    if (partner != NULL) {
        Fighter* nana = partner->user_data;
        unsigned int facing_bits;
        memcpy(&facing_bits, &fp->facing_dir, sizeof facing_bits);
        if (nana != NULL && nana->cpu.x444 != NULL) {
            nana->cpu.x444->facing_dir = fp->facing_dir;
            nana->cpu.x444->lstick.x = (s8) ((facing_bits >> 31) + 127);
        }
    }
}

/* UCF DBOOC SquatRv Fix: the threshold ftCo_SquatRv_CheckInput compares the stick with. */
float melee_nx_ucf_squatrv_threshold(Fighter* fp, float vanilla)
{
    int ux, uy;
    if (!melee_nx_online_codes() || fp->active_timer.lstick.x >= 1) {
        return vanilla;
    }
    ux = ucf_units(fp->input.lstick[0].x);
    uy = ucf_units(fp->input.lstick[0].y);
    return ux * ux + uy * uy > 6400 ? 0.59f : vanilla;
}

/* UCF SDI: a second chance when the vanilla window has passed, for a stick that was inside the SDI
 * deadzone last frame and moved fast since. Nonzero means SDI. */
int melee_nx_ucf_sdi(Fighter* fp)
{
    float yy, prev, th;
    int port;
    if (!melee_nx_online_codes()) {
        return 0;
    }
    if (fp->active_sticky.lstick.x > 1 && fp->active_sticky.lstick.y > 1) {
        return 0;
    }
    yy = fp->input.lstick[1].y * fp->input.lstick[1].y;
    prev = (float) ((double) fp->input.lstick[1].x * (double) fp->input.lstick[1].x + (double) yy);
    th = p_ftCommonData->sdi_min_stick_mag * p_ftCommonData->sdi_min_stick_mag;
    if (th <= prev || (port = ucf_port(fp)) < 0) {
        return 0;
    }
    return ucf_delta2(port, 0) + ucf_delta2(port, 1) > 3844;
}

/* UCF Shield SDI: the same second chance for shield SDI, on the X axis. */
int melee_nx_ucf_shield_sdi(Fighter* fp)
{
    int port;
    if (!melee_nx_online_codes() || fp->active_sticky.lstick.x > 1) {
        return 0;
    }
    if (fp->input.lstick[1].x >= p_ftCommonData->sdi_min_stick_mag || (port = ucf_port(fp)) < 0) {
        return 0;
    }
    return ucf_delta2(port, 0) > 3844;
}

/* UCF Shield Drop, at the start of the spot dodge from shield: on a platform, a control-stick input
 * past the stick ring that is not a straight-down spot dodge input is left to the shield drop
 * check instead. Nonzero means cancel the spot dodge. */
int melee_nx_ucf_shield_drop(Fighter* fp)
{
    int ux, uy;
    if (!melee_nx_online_codes()) {
        return 0;
    }
    if (fp->input.cstick[0].y <= p_ftCommonData->x314) {
        return 0;
    }
    if (fp->active_timer.lstick.x < p_ftCommonData->x320) {
        return 0;
    }
    if (fp->input.lstick[0].y <= -0.8f) {
        return 0;
    }
    if (fp->coll_data.floor.index == -1 || !(fp->coll_data.floor.flags & 0x100)) {
        return 0;
    }
    uy = ucf_units(fp->input.lstick[0].y);
    ux = ucf_units(fp->input.lstick[0].x);
    return uy * uy + ux * ux > 6400;
}

/* UCF Shield Drop Extended: the shield drop check also accepts a stick short of the vanilla
 * threshold once the pad buffer has counted a fast downward flick for two frames. */
int melee_nx_ucf_shield_drop_extended(Fighter* fp)
{
    int port = ucf_port(fp);
    return melee_nx_online_codes() && port >= 0 && ucf_ring[port].flick > 1;
}

/* UCF Tumble: whether the wiggle out of tumble happens (asked only with the codes on). Frame 0
 * wiggles, frame 1 only for a stick that was inside the deadzone last frame and moved fast. */
int melee_nx_ucf_tumble_wiggle(Fighter* fp)
{
    int timer = fp->active_timer.lstick.x, port;
    if (timer == 0) {
        return 1;
    }
    if (timer != 1) {
        return 0;
    }
    if (__builtin_fabsf(fp->input.lstick[1].x) >= p_ftCommonData->x210 || (port = ucf_port(fp)) < 0) {
        return 0;
    }
    return ucf_delta2(port, 0) > 5625;
}

/* ---- "NeutralSpawn": for singles, and for teams of exactly two, the Nth player (in slot order,
 * or team by team) starts at the Nth neutral position of the six tournament stages, or of the
 * stage's own spawn points (teams mapped 0, 3, 1, 2) elsewhere, facing the middle ---- */

static const struct {
    s32 stkind;
    float singles[4][2];
    float teams[4][2];
} neutral_spawns[] = {
    { 0x20, { { -60, 10 }, { 60, 10 }, { -20, 10 }, { 20, 10 } },
            { { -60, 10 }, { -20, 10 }, { 60, 10 }, { 20, 10 } } },
    { 0x1F, { { -38.8f, 35.2f }, { 38.8f, 35.2f }, { 0, 8 }, { 0, 62.4f } },
            { { -38.8f, 35.2f }, { -38.8f, 5 }, { 38.8f, 35.2f }, { 38.8f, 5 } } },
    { 0x08, { { -42, 26.6f }, { 42, 28 }, { 0, 46.9f }, { 0, 4.9f } },
            { { -42, 26.6f }, { -42, 5 }, { 42, 28 }, { 42, 5 } } },
    { 0x1C, { { -46.6f, 37.2f }, { 47.4f, 37.3f }, { 0, 7 }, { 0, 58.5f } },
            { { -46.6f, 37.2f }, { -46.6f, 5 }, { 47.4f, 37.3f }, { 47.4f, 5 } } },
    { 0x02, { { -41.25f, 21 }, { 41.25f, 27 }, { 0, 5.25f }, { 0, 48 } },
            { { -41.25f, 21 }, { -41.25f, 5 }, { 41.25f, 27 }, { 41.25f, 5 } } },
    { 0x03, { { -40, 32 }, { 40, 32 }, { 70, 7 }, { -70, 7 } },
            { { -40, 32 }, { -40, 5 }, { 40, 32 }, { 40, 5 } } },
};

static void neutral_spawn_at(int slot, int index, u8 teams)
{
    static const u8 team_order[4] = { 0, 3, 1, 2 };
    Vec3 pos;
    int e;
    for (e = 0; e < (int) (sizeof neutral_spawns / sizeof neutral_spawns[0]); e++) {
        if (neutral_spawns[e].stkind == (s32) Stage_80225194()) {
            break;
        }
    }
    if (e < (int) (sizeof neutral_spawns / sizeof neutral_spawns[0])) {
        const float* p = teams ? neutral_spawns[e].teams[index & 3] : neutral_spawns[e].singles[index & 3];
        pos.x = p[0];
        pos.y = p[1];
        pos.z = 0.0f;
        Player_80032768(slot, &pos);
    } else {
        Stage_80224E64(teams == 1 ? team_order[index & 3] : index, &pos);
        Player_80032768(slot, &pos);
    }
    Player_LoadPlayerCoords(slot, &pos);
    Player_SetFacingDirection(slot, pos.x <= 0.0f ? 1.0F : -1.0F);
}

void melee_nx_neutral_spawn(int slot, u8 teams)
{
    int index, s, team;
    if (!melee_nx_online_codes() || slot >= 5) {
        return;
    }
    if (teams != 1) {
        index = 0;
        for (s = 0; s <= 4; s++) {
            if (Player_GetPlayerSlotType(s) == Gm_PKind_NA) {
                continue;
            }
            if (s == slot) {
                break;
            }
            index++;
        }
        neutral_spawn_at(slot, index, teams);
        return;
    }
    for (team = 0; team < 3; team++) {
        int count = 0;
        for (s = 0; s < 4; s++) {
            if (Player_GetPlayerSlotType(s) != Gm_PKind_NA && Player_GetTeam(s) == team) {
                count++;
            }
        }
        if (count == 1 || count > 2) {
            return;
        }
    }
    {
        int order[4] = { -1, -1, -1, -1 };
        int n = 0;
        for (team = 0; team < 3; team++) {
            for (s = 0; s < 4; s++) {
                if (Player_GetPlayerSlotType(s) != Gm_PKind_NA && Player_GetTeam(s) == team && n < 4) {
                    order[n++] = s;
                }
            }
        }
        for (index = 0; index < 4; index++) {
            if (order[index] == slot) {
                break;
            }
        }
        neutral_spawn_at(slot, index, 1);
    }
}

/* ---- "PreventWobbling": the 4th distinct hit by an Ice Climbers grabber breaks the grab (not in
 * Teams). The counter and the last hit's id live in the grabbed fighter's state variables
 * (fp+0x2384 and fp+0x2386 on the console), so they are game state. ---- */

#define WOBBLE_COUNT(fp) (*(u8*) ((u8*) &(fp)->mv + 0x44))
#define WOBBLE_LAST(fp) (*(u16*) ((u8*) &(fp)->mv + 0x46))

/* At the start of a grab (the two capture states): reset the counter. */
void melee_nx_wobble_reset(Fighter* fp)
{
    if (melee_nx_online_codes()) {
        WOBBLE_COUNT(fp) = 0;
        WOBBLE_LAST(fp) = 0xFFFF;
    }
}

/* On a hit taken while grabbed. Returns nonzero when the grab broke. */
int melee_nx_wobble_check(Fighter_GObj* gobj)
{
    Fighter* fp = gobj->user_data;
    Fighter_GObj* grabber;
    Fighter* gfp;
    HSD_GObj* src;
    u16 id, prev;
    Fighter_GObj* nana;
    if (!melee_nx_online_codes() || fp->motion_id < 0xDF || fp->motion_id > 0xE4) {
        return 0;
    }
    grabber = fp->victim_gobj;
    if (grabber == NULL || !(gfp = grabber->user_data)->x2222_b5) {
        return 0;
    }
    src = fp->dmg.x1868_source;
    if (src == NULL) {
        return 0;
    }
    if (src == (HSD_GObj*) grabber) {
        id = gfp->x2074.x2088;
    } else if (src->classifier == HSD_GOBJ_CLASS_ITEM && ((Item*) src->user_data)->owner == (HSD_GObj*) grabber) {
        id = ((Item*) src->user_data)->xDA8_short;
    } else {
        return 0;
    }
    if (id == WOBBLE_LAST(fp)) {
        return 0;
    }
    prev = WOBBLE_LAST(fp);
    WOBBLE_LAST(fp) = id;
    WOBBLE_COUNT(fp)++;
    if (gm_8016B168() || WOBBLE_COUNT(fp) <= 3) {
        return 0;
    }
    ftCo_800DA698(grabber, prev != 0);
    nana = Player_GetEntityAtIndex(gfp->player_id, 1);
    if (nana != NULL) {
        Fighter* nfp = nana->user_data;
        if (!nfp->x221F_b1 && !nfp->x2219_b5 && nfp->x2070.x2071_b0_3 != 13) {
            if (nfp->ground_or_air == GA_Ground) {
                ftCo_800DA698(nana, false);
            } else {
                ftCommon_8007D5D4(nfp);
                nfp->self_vel.x = -nfp->facing_dir * p_ftCommonData->x374;
                nfp->self_vel.y = p_ftCommonData->x378;
                nfp->mv.co.buryjump.x0 = 0;
                Fighter_ChangeMotionState(nana, ftCo_MS_CaptureJump, 0, 0.0F, 1.0F, 0.0F, NULL);
            }
        }
    }
    return 1;
}
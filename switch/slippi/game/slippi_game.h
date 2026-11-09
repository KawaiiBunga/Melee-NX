/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Game side of Slippi online play: what the patched game sources call (hook sites in
 * switch/patches/melee-switch-slippi-hooks.patch). Ported from Melee Unlocked's
 * sourceport/game/shim (GPL-2.0-or-later); see docs/SLIPPI.md.
 *
 * Every function is inert unless an online match is pending or running, so offline play is the
 * game as it was. */
#pragma once

#include <string.h>

#include <dolphin/pad.h>
#include <melee/ft/forward.h>
#include <melee/gm/types.h>
#include <melee/mn/types.h>

/* ---- the online match (slippi_online.c; its state is outside rollback snapshots) ---- */

int melee_nx_online_active(void); /* an online match is running */
int melee_nx_online_resim(void);  /* re-simulating after a rollback load */
/* Slippi Dolphin always plays with its gameplay codes (UCF 0.84, neutral spawns, the determinism
 * fixes); nonzero while an online match is pending or running, so this game does too. */
int melee_nx_online_codes(void);
int melee_nx_online_mode(void);       /* Slippi's mode id (1 Unranked, 2 Direct), -1 offline */
int melee_nx_online_local_port(void); /* the local player's in-game port, -1 offline */
int melee_nx_online_frozen_stadium(void); /* the match's frozen Pokemon Stadium choice */
/* 1 once after an online match ended (VS mode then returns to the character select). */
int melee_nx_online_take_finished(void);
int melee_nx_online_boot_mode(int mode); /* the game mode the boot scene hands over to */
/* The port's own frozen Stadium option never applies to an online match. */
#define melee_nx_pc_frozen_stadium() (pc_is_frozen_stadium_enabled() && !melee_nx_online_codes())

/* VS mode, leaving the stage select: find the opponent and agree on the match (blocks, with the
 * status on screen). 1: `vs` now holds the negotiated match (or play is offline and nothing
 * changed); 0: cancelled or failed, go back to the character select. */
int melee_nx_online_lobby(VsModeData* vs);
void melee_nx_online_leave(void);                       /* back to the menu from the character select */
void melee_nx_online_start_melee(StartMeleeData* data); /* InitOnlinePlay */
void melee_nx_online_match_exit(void);

/* Engine loop (gm_801A4D34) and pad sampling. */
int melee_nx_online_engine_gate(int* count); /* ForceEngineOnRollback */
void melee_nx_online_frame_begin(void);      /* StartEngineLoop */
int melee_nx_online_frame_end(void);         /* LoopEngineForRollback: 0 continue, 1 resim, 2 done */
void melee_nx_online_scene_loop_exit(void);
int melee_nx_online_pad_alarm_gate(void);       /* PreventPadAlarmDuringRollback */
void melee_nx_online_after_pad_renew(void);     /* ForceInputRefetchOnAdvance */
int melee_nx_online_skip_pad_read(void);        /* SkipNewInputFetchOnRollback */
int melee_nx_online_pad_renew(PADStatus* stat); /* TriggerSendInput: nonzero drops the sample */

/* Provided by the patched game sources. */
struct gm_80479D58_t* melee_nx_engine_state(void);    /* gmscene.c */
void melee_nx_gmvs_request_online_end(int pauser);    /* gmvs.c */
void melee_nx_refresh_part_matrices(Fighter* fp);     /* ftparts.c */
void pc_os_alarms_hold(void);                         /* src/pc/os.c */
void pc_os_alarms_release(void);

/* ---- Slippi's gameplay codes (slippi_codes.c; game state, inside snapshots) ---- */

void melee_nx_apply_game_info(StartMeleeData* data, const unsigned char* info);
void melee_nx_ucf_pad_buffer(Fighter* fp);
void melee_nx_ucf_dashback(Fighter* fp);
float melee_nx_ucf_squatrv_threshold(Fighter* fp, float vanilla);
int melee_nx_ucf_sdi(Fighter* fp);
int melee_nx_ucf_shield_sdi(Fighter* fp);
int melee_nx_ucf_shield_drop(Fighter* fp);
int melee_nx_ucf_shield_drop_extended(Fighter* fp);
int melee_nx_ucf_tumble_wiggle(Fighter* fp);
int melee_nx_offscreen_damage_zone(Fighter* fp);
void melee_nx_neutral_spawn(int slot, u8 teams);
void melee_nx_wobble_reset(Fighter* fp);
int melee_nx_wobble_check(Fighter_GObj* gobj);

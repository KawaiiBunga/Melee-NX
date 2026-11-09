/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The host side of native Slippi online play: answers the Slippi commands the game-side online code
 * sends (inputs, savestates, match state, matchmaking, selections, chat, reports), the way Slippi
 * Dolphin's EXI device does. Ported from Melee Unlocked's port/runtime/hle/slippi_online.cpp and
 * native_slippi_bridge.h (GPL-2.0-or-later); see docs/SLIPPI.md "Online play port plan".
 *
 * The game side calls these through the same names Melee Unlocked's shims use. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One Slippi command. Returns 0 on success, -1 unknown command, -2 bad payload or response size.
 * The reply (possibly empty) goes to `response`, its size to `*response_size`. */
int32_t mu_slippi_command(uint8_t command, const uint8_t* payload, uint32_t payload_size, uint8_t* response,
                          uint32_t response_capacity, uint32_t* response_size);
/* The game enters (1) or leaves (0) re-simulation after a rollback load. */
void melee_nx_slippi_resim_phase(int32_t entering);
/* Vertical retraces since boot, for the online code's frame pacing checks. */
uint32_t melee_nx_slippi_retrace_count(void);
/* A short sleep for the engine loop's pad wait during an online match. */
void melee_nx_slippi_idle(void);
/* One line to the runtime log. */
void melee_nx_slippi_log(const char* text);

/* The launcher chose Slippi (launch.cfg mode=slippi). With code= the lobby plays that opponent in
 * Direct mode, without it it searches Unranked. */
int32_t melee_nx_slippi_enabled(void);
/* Slippi's mode id of the last search: 1 Unranked, 2 Direct. */
int32_t melee_nx_slippi_mode(void);
/* The lobby: finds the opponent and waits until both players have picked, presenting
 * frames with the status line while it does. `character`, `color` and `stage` are the local
 * player's picks; B on `pad_port` cancels. Returns 1 with Slippi's match state reply in `state`
 * (962 bytes), 0 when cancelled or failed. */
int32_t melee_nx_slippi_lobby(uint8_t character, uint8_t color, uint16_t stage, int32_t pad_port, uint8_t* state,
                              uint32_t capacity);
/* The status line drawn over the game for `frames` retraces; the overlay reads it back each frame
 * ("" when there is none). */
void melee_nx_slippi_status(const char* text, uint32_t frames);
const char* melee_nx_slippi_status_text(void);
/* Shows what to do next on the character select (Slippi mode only). */
void melee_nx_slippi_hint(uint32_t frames);
/* 1 while the lobby waits (the port keeps the last frame on screen meanwhile). */
int32_t melee_nx_slippi_lobby_open(void);
/* Tests only. */
void melee_nx_slippi_test_setup(const char* dir, int32_t local_index, uint16_t local_port, const char* remote,
                                const char* opponent_code);

#ifdef __cplusplus
}
#endif

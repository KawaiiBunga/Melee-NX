/* YouGame's run-wide SPSC datagram channel. No address is sent to the app.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YG_ENET_SOCKET_H
#define YG_ENET_SOCKET_H
#include "enet/enet.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { YG_SLOTS=256, YG_STRIDE=1280, YG_DATAGRAM=1200,
       YG_OUT=128, YG_RING_SIZE=128+256*1280,
       YG_IN=128+YG_RING_SIZE, YG_SECTION_SIZE=128+2*YG_RING_SIZE };
/* wait: 0 timeout, 1 inbound doorbell, 2 local wake, 3 interrupt, -1 error.
 * Memory and callbacks outlive all virtual sockets. Only tests attach plain memory. */
typedef struct YGDoorbells {
    void *context;
    void (*notify)(void *);
    void (*wake)(void *);
    int (*wait)(void *, enet_uint32);
} YGDoorbells;
int yg_enet_attach(void *memory, size_t size, YGDoorbells bells);
#ifdef _WIN32
int yg_enet_attach_handles(uintptr_t section, uintptr_t toApp, uintptr_t toGame);
#else
/* Borrow app-owned inherited fds for the run; never close them, even on failure.
 * Attachment sets NONBLOCK/CLOEXEC and ignores SIGPIPE process-wide. Only the
 * mapping and private self-pipe belong to this backend. */
int yg_enet_attach_posix(int section, int wake, int wait, size_t size);
#endif
/* Selection is per run; disabling dead links must never restore native UDP. */
void yg_enet_select(int direct);
int yg_enet_selected(void);
void yg_enet_enable(int enabled);
int yg_enet_enabled(void);
int yg_enet_channel_ready(void);
int yg_enet_link_state(unsigned link);
int yg_enet_link_address(unsigned link, ENetAddress *address);
int yg_enet_is_socket(ENetSocket socket);
unsigned yg_enet_mtu(ENetSocket socket, unsigned ordinary);
ENetSocket yg_enet_create(void);
void yg_enet_destroy(ENetSocket socket);
int yg_enet_bind(ENetSocket socket, const ENetAddress *address);
int yg_enet_get_address(ENetSocket socket, ENetAddress *address);
int yg_enet_send(ENetSocket socket, const ENetAddress *address, const ENetBuffer *buffers, size_t count);
int yg_enet_receive(ENetSocket socket, ENetAddress *address, ENetBuffer *buffers, size_t count);
int yg_enet_wait(ENetSocket socket, enet_uint32 *condition, enet_uint32 timeout);
#ifdef __cplusplus
}
#endif
#endif

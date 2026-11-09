/* melee-nx: Melee Unlocked's ENet routes sockets through an optional YouGame datagram channel
 * (yg_enet_socket.c, not vendored: it needs mmap and POSIX fds the Switch does not have). These
 * stand-ins say "never selected, never a YouGame socket", so every call takes ENet's own UDP path.
 * SPDX-License-Identifier: MIT */
#include "enet/yg_enet_socket.h"

int yg_enet_selected(void) { return 0; }
int yg_enet_is_socket(ENetSocket socket) { (void)socket; return 0; }
unsigned yg_enet_mtu(ENetSocket socket, unsigned ordinary) { (void)socket; return ordinary; }
ENetSocket yg_enet_create(void) { return ENET_SOCKET_NULL; }
void yg_enet_destroy(ENetSocket socket) { (void)socket; }
int yg_enet_bind(ENetSocket socket, const ENetAddress *address) { (void)socket; (void)address; return -1; }
int yg_enet_get_address(ENetSocket socket, ENetAddress *address) { (void)socket; (void)address; return -1; }
int yg_enet_send(ENetSocket socket, const ENetAddress *address, const ENetBuffer *buffers, size_t count) {
    (void)socket; (void)address; (void)buffers; (void)count; return -1;
}
int yg_enet_receive(ENetSocket socket, ENetAddress *address, ENetBuffer *buffers, size_t count) {
    (void)socket; (void)address; (void)buffers; (void)count; return -1;
}
int yg_enet_wait(ENetSocket socket, enet_uint32 *condition, enet_uint32 timeout) {
    (void)socket; (void)condition; (void)timeout; return -1;
}

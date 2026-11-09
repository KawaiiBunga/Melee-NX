/** 
 @file  unix.h
 @brief ENet Unix header
*/
#ifndef __ENET_UNIX_H__
#define __ENET_UNIX_H__

#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <arpa/inet.h> /* melee-nx: libnx's newlib declares htons/htonl here only */
#endif

#ifdef MSG_MAXIOVLEN
#define ENET_BUFFER_MAXIMUM MSG_MAXIOVLEN
#endif

typedef int ENetSocket;

#define ENET_SOCKET_NULL -1

#define ENET_HOST_TO_NET_16(value) (htons (value)) /**< macro that converts host to net byte-order of a 16-bit value */
#define ENET_HOST_TO_NET_32(value) (htonl (value)) /**< macro that converts host to net byte-order of a 32-bit value */

#define ENET_NET_TO_HOST_16(value) (ntohs (value)) /**< macro that converts net to host byte-order of a 16-bit value */
#define ENET_NET_TO_HOST_32(value) (ntohl (value)) /**< macro that converts net to host byte-order of a 32-bit value */

typedef struct
{
    void * data;
    size_t dataLength;
} ENetBuffer;

#define ENET_CALLBACK

#define ENET_API extern

typedef fd_set ENetSocketSet;

#define ENET_SOCKETSET_EMPTY(sockset)          FD_ZERO (& (sockset))
#define ENET_SOCKETSET_ADD(sockset, socket)    do { if ((socket) >= 0 && (socket) < FD_SETSIZE) FD_SET (socket, & (sockset)); } while (0)
#define ENET_SOCKETSET_REMOVE(sockset, socket) do { if ((socket) >= 0 && (socket) < FD_SETSIZE) FD_CLR (socket, & (sockset)); } while (0)
#define ENET_SOCKETSET_CHECK(sockset, socket)  ((socket) >= 0 && (socket) < FD_SETSIZE && FD_ISSET (socket, & (sockset)))
    
#endif /* __ENET_UNIX_H__ */


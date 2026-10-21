#pragma once

#include "SocketManager.hpp"

// Provider-owned runtime layouts; borrowed peer and retained queue/file edges
// follow SocketDebugStats.hpp. Trailing packet data is not part of metadata.
struct socket_ancillary_t
{
    socket_ancillary_t*  next;
    size_t               count;
    file_t*              files[253];
    socket_credentials_t credentials;
    bool                 explicitCredentials;
    uint32_t             senderUid;
};

struct socket_packet_t
{
    socket_packet_t*    next;
    size_t              size;
    size_t              offset;
    socket_address_t    source;
    uint32_t            sourceLength;
    socket_ancillary_t* control;
    uint8_t             data[];
};

struct socket_endpoint_t
{
    inode_t            inode;
    SocketManager*     manager;
    file_t*            file;
    socket_endpoint_t* next;
    socket_endpoint_t* peer;
    socket_endpoint_t* pendingHead;
    socket_endpoint_t* pendingTail;
    socket_endpoint_t* pendingNext;
    socket_packet_t*   head;
    socket_packet_t*   tail;
    inode_t*           node;
    socket_address_t   address;
    socket_address_t   peerAddress;
    uint32_t           addressLength;
    uint32_t           peerAddressLength;
    uint32_t           type;
    uint32_t           backlog;
    uint32_t           pendingCount;
    uint32_t           packetCount;
    uint32_t           lastReadiness;
    uint32_t           pendingReadiness;
    uint32_t           sendLimit;
    uint32_t           receiveLimit;
    size_t             collectionIndex;
    size_t             internalReferences;
    size_t             queued;
    bool               bound;
    bool               binding;
    bool               connected;
    bool               listening;
    bool               readShutdown;
    bool               writeShutdown;
    bool               passCredentials;
    int32_t            credentials[3];
    int32_t            peerCredentials[3];
};


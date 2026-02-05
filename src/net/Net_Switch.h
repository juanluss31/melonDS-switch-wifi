/*
    Copyright 2016-2021 Arisotura

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef NET_SWITCH_H
#define NET_SWITCH_H

#include "../types.h"
#include "../Savestate.h"
#include "../FIFO.h"
#include "NetDriver.h"
#include <functional>
#include <map>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#endif

// Network driver for Nintendo Switch with full TCP/UDP/DHCP/DNS support
// Provides NAT-like functionality similar to libslirp
class Net_Switch : public NetDriver
{
public:
    using SendPacketCallback = std::function<void(const u8* data, int len)>;

    explicit Net_Switch(const SendPacketCallback& callback);
    ~Net_Switch() override;

    int SendPacket(u8* data, int len) override;
    void RecvCheck() override;

private:
    // Connection tracking structures
    struct TCPConnection
    {
        int socket;
        u32 clientIP;
        u16 clientPort;
        u32 destIP;
        u16 destPort;
        bool connected;
        u32 serverSeq;
        u32 clientSeq;
        u32 serverSeqNext;
        u64 lastActivity;
        u64 connectStartTime; // When connect() was initiated
        bool connecting;
        std::vector<u8> recvBuffer;
    };

    struct UDPConnection
    {
        int socket;
        u32 clientIP;
        u16 clientPort;
        u32 destIP;
        u16 destPort;
        u64 lastActivity;
        bool isDNS; // Track if this is a DNS connection
    };

    SendPacketCallback Callback;
    FIFO<u32, (0x8000 >> 2)> RXBuffer;
    u32 IPv4ID;
    bool Initialized;
    u64 CurrentTime;

    // Connection tracking
    std::map<u32, TCPConnection> TCPConnections; // Key: clientPort | (clientIP << 16)
    std::map<u32, UDPConnection> UDPConnections; // Key: clientPort | (clientIP << 16)

#ifdef __SWITCH__
    SocketInitConfig socketConfig;
#endif

    // Protocol handlers
    void HandleARPFrame(u8* data, int len);
    void HandleIPFrame(u8* data, int len);
    void HandleICMPFrame(u8* ipHeader, int ipLen);
    void HandleTCPFrame(u8* ipHeader, int ipLen);
    void HandleUDPFrame(u8* ipHeader, int ipLen);
    void HandleDHCPFrame(u8* udpData, int udpLen, u32 srcIP);
    void HandleDNSFrame(u8* udpData, int udpLen, u32 srcIP, u16 srcPort);
    void ForwardUDPPacket(u32 srcIP, u16 srcPort, u32 dstIP, u16 dstPort, u8* data, int len);

    // TCP/UDP connection management
    void ProcessTCPConnections();
    void ProcessUDPConnections();
    void CleanupOldConnections();

    // Packet construction helpers
    void SendIPPacket(u8* ethDest, u8 protocol, u32 srcIP, u32 dstIP, u8* payload, int payloadLen);
    void SendUDPPacket(u32 srcIP, u16 srcPort, u32 dstIP, u16 dstPort, u8* data, int len);
    void SendTCPPacket(u32 srcIP, u16 srcPort, u32 dstIP, u16 dstPort, 
                       u32 seq, u32 ack, u8 flags, u8* data, int len);
    void SendICMPPacket(u32 srcIP, u32 dstIP, u8 type, u8 code, u8* data, int len);
    void FinishUDPFrame(u8* data, int len);

    // Utility functions
    u16 IPChecksum(u8* data, int len);
    u16 TCPChecksum(u32 srcIP, u32 dstIP, u8* tcpData, int tcpLen);
    u16 UDPChecksum(u32 srcIP, u32 dstIP, u8* udpData, int udpLen);
    u32 MakeConnectionKey(u32 ip, u16 port);
    u64 GetMonotonicTime();
};

#endif // NET_SWITCH_H

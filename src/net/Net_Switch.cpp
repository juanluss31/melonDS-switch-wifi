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

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <netdb.h>
#include "Net_Switch.h"

#ifdef __SWITCH__
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#endif

// Subnet configuration - same as libslirp uses
const u32 kSubnet   = 0x0A400000;
const u32 kServerIP = kSubnet | 0x01;
const u32 kDNSIP    = kSubnet | 0x02;
const u32 kClientIP = kSubnet | 0x10;

const u8 kServerMAC[6] = {0x00, 0xAB, 0x33, 0x28, 0x99, 0x44};
const u8 kClientMAC[6] = {0x00, 0x16, 0x56, 0x83, 0x31, 0xF9}; // Client MAC address

// Connection timeout in microseconds
const u64 UDP_TIMEOUT = 30000000; // 30 seconds
const u64 TCP_TIMEOUT = 300000000; // 5 minutes

Net_Switch::Net_Switch(const SendPacketCallback& callback)
    : Callback(callback)
    , IPv4ID(0)
    , Initialized(false)
    , CurrentTime(0)
{
#ifdef __SWITCH__
    // Initialize BSD sockets on Switch
    socketInitializeDefault();
    Initialized = true;
    printf("Net_Switch: Network driver initialized\n");
#endif
}

Net_Switch::~Net_Switch()
{
#ifdef __SWITCH__
    if (Initialized)
    {
        // Close all TCP connections
        for (auto& pair : TCPConnections)
        {
            if (pair.second.socket >= 0)
                close(pair.second.socket);
        }
        TCPConnections.clear();

        // Close all UDP sockets
        for (auto& pair : UDPConnections)
        {
            if (pair.second.socket >= 0)
                close(pair.second.socket);
        }
        UDPConnections.clear();

        socketExit();
        Initialized = false;
        printf("Net_Switch: Network driver shut down\n");
    }
#endif
}

u64 Net_Switch::GetMonotonicTime()
{
#ifdef __SWITCH__
    return armGetSystemTick() * 1000000ULL / 19200000ULL; // Convert ticks to microseconds
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

// Helper function to finalize UDP frame with proper lengths and checksums
void Net_Switch::FinishUDPFrame(u8* data, int len)
{
    u8* ipheader = &data[0xE];
    u8* udpheader = &data[0x22];

    // Set IP total length
    *(u16*)&ipheader[2] = htons(len - 0xE);
    
    // Set UDP length
    *(u16*)&udpheader[4] = htons(len - (0xE + 0x14));

    // Recalculate IP checksum
    *(u16*)&ipheader[10] = 0;
    u32 tmp = 0;
    for (int i = 0; i < 20; i += 2)
        tmp += ntohs(*(u16*)&ipheader[i]);
    while (tmp >> 16)
        tmp = (tmp & 0xFFFF) + (tmp >> 16);
    tmp ^= 0xFFFF;
    *(u16*)&ipheader[10] = htons(tmp);

    // Recalculate UDP checksum (pseudo-header + UDP data)
    tmp = 0;
    // Pseudo-header
    tmp += ntohs(*(u16*)&ipheader[12]); // Source IP high
    tmp += ntohs(*(u16*)&ipheader[14]); // Source IP low
    tmp += ntohs(*(u16*)&ipheader[16]); // Dest IP high
    tmp += ntohs(*(u16*)&ipheader[18]); // Dest IP low
    tmp += ntohs(0x1100); // 0x00, 0x11 (UDP protocol)
    tmp += (len - 0x22); // UDP length
    
    // UDP data
    for (int i = 0; i < (len - 0x22); i += 2)
    {
        if (i + 1 < (len - 0x22))
            tmp += ntohs(*(u16*)&udpheader[i]);
        else
            tmp += (udpheader[i] << 8);
    }
    
    while (tmp >> 16)
        tmp = (tmp & 0xFFFF) + (tmp >> 16);
    tmp ^= 0xFFFF;
    if (tmp == 0) tmp = 0xFFFF;
    *(u16*)&udpheader[6] = htons(tmp);
}

u32 Net_Switch::MakeConnectionKey(u32 ip, u16 port)
{
    return (u32)port | ((ip & 0xFFFF) << 16);
}

u16 Net_Switch::TCPChecksum(u32 srcIP, u32 dstIP, u8* tcpData, int tcpLen)
{
    // Create pseudo-header
    u8 pseudo[12];
    *(u32*)&pseudo[0] = htonl(srcIP);
    *(u32*)&pseudo[4] = htonl(dstIP);
    pseudo[8] = 0;
    pseudo[9] = 6; // TCP
    *(u16*)&pseudo[10] = htons(tcpLen);

    u32 sum = 0;
    for (int i = 0; i < 12; i += 2)
        sum += (pseudo[i] << 8) | pseudo[i+1];
    
    for (int i = 0; i < tcpLen; i += 2)
    {
        if (i + 1 < tcpLen)
            sum += (tcpData[i] << 8) | tcpData[i+1];
        else
            sum += tcpData[i] << 8;
    }
    
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}

u16 Net_Switch::UDPChecksum(u32 srcIP, u32 dstIP, u8* udpData, int udpLen)
{
    // Create pseudo-header
    u8 pseudo[12];
    *(u32*)&pseudo[0] = htonl(srcIP);
    *(u32*)&pseudo[4] = htonl(dstIP);
    pseudo[8] = 0;
    pseudo[9] = 17; // UDP
    *(u16*)&pseudo[10] = htons(udpLen);

    u32 sum = 0;
    for (int i = 0; i < 12; i += 2)
        sum += (pseudo[i] << 8) | pseudo[i+1];
    
    for (int i = 0; i < udpLen; i += 2)
    {
        if (i + 1 < udpLen)
            sum += (udpData[i] << 8) | udpData[i+1];
        else
            sum += udpData[i] << 8;
    }
    
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}

void Net_Switch::HandleARPFrame(u8* data, int len)
{
    // ARP packet format (Ethernet frame):
    // Offset 0-13: Ethernet header
    // Offset 14-15: Hardware type (0x0001 = Ethernet)
    // Offset 16-17: Protocol type (0x0800 = IPv4)
    // Offset 18: Hardware address length (6 for Ethernet)
    // Offset 19: Protocol address length (4 for IPv4)
    // Offset 20-21: Operation (1 = request, 2 = reply)
    // Offset 22-27: Sender MAC
    // Offset 28-31: Sender IP
    // Offset 32-37: Target MAC
    // Offset 38-41: Target IP
    
    if (len < 42) return; // Ethernet header (14) + ARP header (28)

    // Read protocol type from ARP header
    u16 protocol = (data[16] << 8) | data[17];
    if (protocol != 0x0800) return; // Only handle IPv4

    // Read operation from ARP header
    u16 op = (data[20] << 8) | data[21];
    if (op != 1) return; // Only handle requests

    printf("Net_Switch: ARP Request for %d.%d.%d.%d\n",
           data[38], data[39], data[40], data[41]);

    // Build ARP reply
    u8 reply[64];
    memset(reply, 0, sizeof(reply));

    // Ethernet header
    memcpy(&reply[0], &data[6], 6); // dest MAC = sender MAC from Ethernet
    memcpy(&reply[6], kServerMAC, 6); // src MAC = our MAC
    reply[12] = 0x08; reply[13] = 0x06; // EtherType = ARP

    // ARP header
    reply[14] = 0x00; reply[15] = 0x01; // Hardware type: Ethernet
    reply[16] = 0x08; reply[17] = 0x00; // Protocol type: IPv4
    reply[18] = 6; // Hardware size
    reply[19] = 4; // Protocol size
    reply[20] = 0x00; reply[21] = 0x02; // Opcode: Reply

    memcpy(&reply[22], kServerMAC, 6); // Sender MAC (our MAC)
    memcpy(&reply[28], &data[38], 4); // Sender IP (target IP from request)
    memcpy(&reply[32], &data[22], 6); // Target MAC (sender MAC from request)
    memcpy(&reply[38], &data[28], 4); // Target IP (sender IP from request)

    printf("Net_Switch: Sending ARP Reply\n");
    if (Callback)
        Callback(reply, 42);
}

void Net_Switch::HandleDNSFrame(u8* data, int len, u32 srcIP, u16 srcPort)
{
    // Try to resolve DNS query locally first, otherwise forward to 8.8.8.8
    // This provides better performance and lower latency for common domains
    
    if (len < 12)
    {
        printf("Net_Switch: DNS query too short\n");
        return;
    }

    u16 id = ntohs(*(u16*)&data[0]);
    u16 flags = ntohs(*(u16*)&data[2]);
    u16 numquestions = ntohs(*(u16*)&data[4]);
    u16 numanswers = ntohs(*(u16*)&data[6]);

    printf("Net_Switch: DNS query - ID:%04X flags:%04X questions:%d answers:%d\n",
           id, flags, numquestions, numanswers);

    // Only handle simple queries (no response flag, single question, no existing answers)
    if (flags & 0x8000) return;
    if (numquestions != 1 || numanswers != 0) 
    {
        // Forward complex queries
        u32 realDNS = 0x08080808; // 8.8.8.8
        ForwardUDPPacket(srcIP, srcPort, realDNS, 53, data, len);
        // Mark the connection as DNS so responses are sent from 10.64.0.2
        u32 key = MakeConnectionKey(srcIP, srcPort);
        auto it = UDPConnections.find(key);
        if (it != UDPConnections.end())
            it->second.isDNS = true;
        return;
    }

    // For now, we forward all DNS queries to maintain compatibility
    // A full implementation would parse the query and resolve locally
    u32 realDNS = 0x08080808; // 8.8.8.8
    printf("Net_Switch: Forwarding DNS query to 8.8.8.8\n");
    ForwardUDPPacket(srcIP, srcPort, realDNS, 53, data, len);
    // Mark the connection as DNS so responses are sent from 10.64.0.2
    u32 key = MakeConnectionKey(srcIP, srcPort);
    auto it = UDPConnections.find(key);
    if (it != UDPConnections.end())
        it->second.isDNS = true;
}

void Net_Switch::SendUDPPacket(u32 srcIP, u16 srcPort, u32 dstIP, u16 dstPort, u8* data, int len)
{
    u8 packet[2048];
    u8* p = packet;

    // Ethernet header
    memcpy(p, kClientMAC, 6); p += 6; // Dest MAC
    memcpy(p, kServerMAC, 6); p += 6; // Src MAC
    *p++ = 0x08; *p++ = 0x00; // IPv4

    // IP header
    u8* ipHeader = p;
    *p++ = 0x45; // Version 4, IHL 5
    *p++ = 0x00; // DSCP/ECN
    u16 totalLen = 20 + 8 + len;
    *p++ = (totalLen >> 8); *p++ = (totalLen & 0xFF);
    *p++ = (IPv4ID >> 8); *p++ = (IPv4ID & 0xFF); IPv4ID++;
    *p++ = 0x00; *p++ = 0x00; // Flags/Fragment
    *p++ = 0x40; // TTL
    *p++ = 17; // Protocol (UDP)
    *p++ = 0x00; *p++ = 0x00; // Checksum (fill later)
    *(u32*)p = htonl(srcIP); p += 4; // Source IP
    *(u32*)p = htonl(dstIP); p += 4; // Dest IP

    // UDP header  
    u8* udpHeader = p;
    *p++ = (srcPort >> 8); *p++ = (srcPort & 0xFF);
    *p++ = (dstPort >> 8); *p++ = (dstPort & 0xFF);
    u16 udpLen = 8 + len;
    *p++ = (udpLen >> 8); *p++ = (udpLen & 0xFF);
    *p++ = 0x00; *p++ = 0x00; // Checksum (fill later)
    
    // Data
    memcpy(p, data, len);
    p += len;

    // Calculate IP checksum
    ipHeader[10] = 0; ipHeader[11] = 0;
    u16 ipChecksum = IPChecksum(ipHeader, 20);
    ipHeader[10] = (ipChecksum >> 8);
    ipHeader[11] = (ipChecksum & 0xFF);

    // Calculate UDP checksum
    u16 udpChecksum = UDPChecksum(srcIP, dstIP, udpHeader, udpLen);
    udpHeader[6] = (udpChecksum >> 8);
    udpHeader[7] = (udpChecksum & 0xFF);

    if (Callback)
        Callback(packet, p - packet);
}

void Net_Switch::SendTCPPacket(u32 srcIP, u16 srcPort, u32 dstIP, u16 dstPort, 
                               u32 seq, u32 ack, u8 flags, u8* data, int len)
{
    u8 packet[2048];
    u8* p = packet;

    // Ethernet header
    memcpy(p, kClientMAC, 6); p += 6; // Dest MAC
    memcpy(p, kServerMAC, 6); p += 6; // Src MAC
    *p++ = 0x08; *p++ = 0x00; // IPv4

    // IP header
    u8* ipHeader = p;
    *p++ = 0x45; // Version 4, IHL 5
    *p++ = 0x00; // DSCP/ECN
    u16 tcpLen = 20; // TCP header without options
    u16 totalLen = 20 + tcpLen + len;
    *p++ = (totalLen >> 8); *p++ = (totalLen & 0xFF);
    *p++ = (IPv4ID >> 8); *p++ = (IPv4ID & 0xFF); IPv4ID++;
    *p++ = 0x00; *p++ = 0x00; // Flags/Fragment
    *p++ = 0x40; // TTL
    *p++ = 6; // Protocol (TCP)
    *p++ = 0x00; *p++ = 0x00; // Checksum (fill later)
    *(u32*)p = htonl(srcIP); p += 4; // Source IP
    *(u32*)p = htonl(dstIP); p += 4; // Dest IP

    // TCP header
    u8* tcpHeader = p;
    *p++ = (srcPort >> 8); *p++ = (srcPort & 0xFF);
    *p++ = (dstPort >> 8); *p++ = (dstPort & 0xFF);
    *(u32*)p = htonl(seq); p += 4;
    *(u32*)p = htonl(ack); p += 4;
    *p++ = 0x50; // Data offset 5 (20 bytes), no reserved bits
    *p++ = flags;
    *p++ = 0x20; *p++ = 0x00; // Window size (8192)
    *p++ = 0x00; *p++ = 0x00; // Checksum (fill later)
    *p++ = 0x00; *p++ = 0x00; // Urgent pointer

    // Data
    if (data && len > 0)
    {
        memcpy(p, data, len);
        p += len;
    }

    // Calculate IP checksum
    ipHeader[10] = 0; ipHeader[11] = 0;
    u16 ipChecksum = IPChecksum(ipHeader, 20);
    ipHeader[10] = (ipChecksum >> 8);
    ipHeader[11] = (ipChecksum & 0xFF);

    // Calculate TCP checksum
    u16 tcpChecksum = TCPChecksum(srcIP, dstIP, tcpHeader, tcpLen + len);
    tcpHeader[16] = (tcpChecksum >> 8);
    tcpHeader[17] = (tcpChecksum & 0xFF);

    if (Callback)
        Callback(packet, p - packet);
}

void Net_Switch::SendICMPPacket(u32 srcIP, u32 dstIP, u8 type, u8 code, u8* data, int len)
{
    u8 packet[2048];
    u8* p = packet;

    // Ethernet header
    memcpy(p, kClientMAC, 6); p += 6;
    memcpy(p, kServerMAC, 6); p += 6;
    *p++ = 0x08; *p++ = 0x00; // IPv4

    // IP header
    u8* ipHeader = p;
    *p++ = 0x45;
    *p++ = 0x00;
    u16 totalLen = 20 + 8 + len;
    *p++ = (totalLen >> 8); *p++ = (totalLen & 0xFF);
    *p++ = (IPv4ID >> 8); *p++ = (IPv4ID & 0xFF); IPv4ID++;
    *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x40; // TTL
    *p++ = 1; // Protocol (ICMP)
    *p++ = 0x00; *p++ = 0x00; // Checksum
    *(u32*)p = htonl(srcIP); p += 4;
    *(u32*)p = htonl(dstIP); p += 4;

    // ICMP header
    u8* icmpHeader = p;
    *p++ = type;
    *p++ = code;
    *p++ = 0x00; *p++ = 0x00; // Checksum
    *p++ = 0x00; *p++ = 0x00; // ID
    *p++ = 0x00; *p++ = 0x00; // Sequence
    
    // Data
    memcpy(p, data, len);
    p += len;

    // Calculate checksums
    ipHeader[10] = 0; ipHeader[11] = 0;
    u16 ipChecksum = IPChecksum(ipHeader, 20);
    ipHeader[10] = (ipChecksum >> 8);
    ipHeader[11] = (ipChecksum & 0xFF);

    int icmpLen = 8 + len;
    u16 icmpChecksum = IPChecksum(icmpHeader, icmpLen);
    icmpHeader[2] = (icmpChecksum >> 8);
    icmpHeader[3] = (icmpChecksum & 0xFF);

    if (Callback)
        Callback(packet, p - packet);
}

void Net_Switch::ProcessUDPConnections()
{
#ifdef __SWITCH__
    // Poll all UDP connections for incoming data
    for (auto& pair : UDPConnections)
    {
        UDPConnection& conn = pair.second;
        
        u8 buffer[2048];
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        
        ssize_t received = recvfrom(conn.socket, buffer, sizeof(buffer), MSG_DONTWAIT,
                                     (struct sockaddr*)&from, &fromLen);
        
        if (received > 0)
        {
            // Got data - send back to DS
            u16 realSrcPort = ntohs(from.sin_port);
            
            // For DNS, send response from virtual DNS server (10.64.0.2)
            // For other UDP, send from the actual source IP
            u32 responseSrcIP;
            if (conn.isDNS)
            {
                responseSrcIP = kDNSIP; // 10.64.0.2
                printf("Net_Switch: DNS response received, forwarding to client\n");
            }
            else
            {
                responseSrcIP = ntohl(from.sin_addr.s_addr);
            }
            
            SendUDPPacket(responseSrcIP, realSrcPort, conn.clientIP, conn.clientPort,
                          buffer, received);
            
            conn.lastActivity = CurrentTime;
        }
    }
#endif
}

void Net_Switch::CleanupOldConnections()
{
#ifdef __SWITCH__
    // Remove UDP connections that haven't been used recently
    auto it = UDPConnections.begin();
    while (it != UDPConnections.end())
    {
        if (CurrentTime - it->second.lastActivity > UDP_TIMEOUT)
        {
            printf("Net_Switch: Closing idle UDP connection on port %d\n", it->second.clientPort);
            close(it->second.socket);
            it = UDPConnections.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

void Net_Switch::HandleDHCPFrame(u8* data, int len, u32 srcIP)
{
    // DHCP packet structure (after UDP header which is 8 bytes):
    // DHCP fixed: op(1) htype(1) hlen(1) hops(1) xid(4) secs(2) flags(2) ciaddr(4) yiaddr(4) siaddr(4) giaddr(4) chaddr(16) sname(64) file(128) = 236 bytes
    // Then: magic cookie(4) + options(variable)
    
    if (len < 8 + 236) return; // UDP header + minimal DHCP fixed header
    
    u8* dhcp = &data[8]; // Skip UDP header
    u8 op = dhcp[0];
    if (op != 1) return; // Only handle BOOTREQUEST
    
    u32 xid = (dhcp[4] << 24) | (dhcp[5] << 16) | (dhcp[6] << 8) | dhcp[7];
    u8 clientMAC[6];
    memcpy(clientMAC, &dhcp[28], 6);
    
    // Check DHCP message type (option 53)
    // Magic cookie is at offset 236 from start of DHCP
    int dhcpMsgType = 0;
    int optStart = 236 + 4; // Fixed DHCP header + magic cookie
    
    // Verify magic cookie (0x63825363 = 99.130.83.99) at DHCP offset 236
    if (len < 8 + optStart || 
        dhcp[236] != 99 || dhcp[237] != 130 || 
        dhcp[238] != 83 || dhcp[239] != 99)
    {
        printf("DHCP: invalid magic cookie at offset 236\n");
        return;
    }
    
    for (int i = optStart; i < len && i < 8 + optStart + 200; )
    {
        if (dhcp[i] == 0xFF) break; // End option
        if (dhcp[i] == 0x00) { i++; continue; } // Padding
        
        u8 optType = dhcp[i++];
        if (i >= len - 8) break;
        u8 optLen = dhcp[i++];
        if (i + optLen > len - 8) break;
        
        if (optType == 53 && optLen == 1) // DHCP Message Type
        {
            dhcpMsgType = dhcp[i];
            break;
        }
        i += optLen;
    }
    
    printf("DHCP: received message type %d from client (XID: %08X)\n", dhcpMsgType, xid);
    
    // Respond to DHCP Discover (1) and Request (3)
    if (dhcpMsgType != 1 && dhcpMsgType != 3) return;
    
    u8 reply[1024];
    memset(reply, 0, sizeof(reply));
    u8* p = reply;
    
    // Ethernet header
    memcpy(p, clientMAC, 6); p += 6; // Dest MAC
    memcpy(p, kServerMAC, 6); p += 6; // Src MAC
    *p++ = 0x08; *p++ = 0x00; // IPv4
    
    // IP header
    u8* iphdr = p;
    *p++ = 0x45; // Version 4, IHL 5
    *p++ = 0x00; // DSCP/ECN
    u16 ipLen = 20 + 8 + 300; // IP + UDP + DHCP (approximate)
    *p++ = (ipLen >> 8); *p++ = (ipLen & 0xFF);
    *p++ = (IPv4ID >> 8); *p++ = (IPv4ID & 0xFF); IPv4ID++;
    *p++ = 0x00; *p++ = 0x00; // Flags/Fragment
    *p++ = 0x80; // TTL
    *p++ = 0x11; // Protocol (UDP)
    *p++ = 0x00; *p++ = 0x00; // Checksum (fill later)
    *p++ = (kServerIP >> 24); *p++ = (kServerIP >> 16); 
    *p++ = (kServerIP >> 8); *p++ = (kServerIP & 0xFF); // Source IP
    *p++ = 0xFF; *p++ = 0xFF; *p++ = 0xFF; *p++ = 0xFF; // Dest IP (broadcast)
    
    // UDP header
    u8* udphdr = p;
    *p++ = 0x00; *p++ = 67; // Source port (DHCP server)
    *p++ = 0x00; *p++ = 68; // Dest port (DHCP client)
    u16 udpLen = 8 + 300; // UDP header + DHCP payload
    *p++ = (udpLen >> 8); *p++ = (udpLen & 0xFF);
    *p++ = 0x00; *p++ = 0x00; // Checksum (optional for IPv4)
    
    // DHCP reply
    *p++ = 0x02; // BOOTREPLY
    *p++ = 0x01; // Ethernet
    *p++ = 0x06; // Hardware address length
    *p++ = 0x00; // Hops
    *p++ = (xid >> 24); *p++ = (xid >> 16); 
    *p++ = (xid >> 8); *p++ = (xid & 0xFF); // Transaction ID
    *p++ = 0x00; *p++ = 0x00; // Secs
    *p++ = 0x00; *p++ = 0x00; // Flags
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; // Client IP (0.0.0.0)
    *p++ = (kClientIP >> 24); *p++ = (kClientIP >> 16);
    *p++ = (kClientIP >> 8); *p++ = (kClientIP & 0xFF); // Your IP (assigned)
    *p++ = (kServerIP >> 24); *p++ = (kServerIP >> 16);
    *p++ = (kServerIP >> 8); *p++ = (kServerIP & 0xFF); // Server IP
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; // Gateway IP
    memcpy(p, clientMAC, 6); p += 6; // Client MAC
    memset(p, 0, 10); p += 10; // MAC padding
    memset(p, 0, 64); p += 64; // Server hostname
    memset(p, 0, 128); p += 128; // Boot filename
    
    // Magic cookie
    *p++ = 99; *p++ = 130; *p++ = 83; *p++ = 99;
    
    // DHCP options
    *p++ = 53; *p++ = 1; // DHCP Message Type
    *p++ = (dhcpMsgType == 1) ? 2 : 5; // 2=OFFER, 5=ACK
    
    *p++ = 1; *p++ = 4; // Subnet Mask
    *p++ = 0xFF; *p++ = 0xFF; *p++ = 0xFF; *p++ = 0x00;
    
    *p++ = 3; *p++ = 4; // Router
    *p++ = (kServerIP >> 24); *p++ = (kServerIP >> 16);
    *p++ = (kServerIP >> 8); *p++ = (kServerIP & 0xFF);
    
    *p++ = 6; *p++ = 4; // DNS Server
    *p++ = (kDNSIP >> 24); *p++ = (kDNSIP >> 16);
    *p++ = (kDNSIP >> 8); *p++ = (kDNSIP & 0xFF);
    
    *p++ = 51; *p++ = 4; // Lease Time
    *p++ = 0x00; *p++ = 0x01; *p++ = 0x51; *p++ = 0x80; // 86400 seconds
    
    *p++ = 54; *p++ = 4; // DHCP Server Identifier
    *p++ = (kServerIP >> 24); *p++ = (kServerIP >> 16);
    *p++ = (kServerIP >> 8); *p++ = (kServerIP & 0xFF);
    
    *p++ = 0xFF; // End
    
    // Update IP length (from after Ethernet header to end)
    int finalLen = (p - reply);
    ipLen = finalLen - 14; // Minus Ethernet header
    iphdr[2] = (ipLen >> 8);
    iphdr[3] = (ipLen & 0xFF);
    
    // Update UDP length (from UDP header to end)
    udpLen = finalLen - 14 - 20; // Minus Ethernet and IP headers
    udphdr[4] = (udpLen >> 8);
    udphdr[5] = (udpLen & 0xFF);
    
    // Calculate IP checksum
    iphdr[10] = 0;
    iphdr[11] = 0;
    u16 checksum = IPChecksum(iphdr, 20);
    iphdr[10] = (checksum >> 8);
    iphdr[11] = (checksum & 0xFF);
    
    printf("DHCP: sending %s to client (IP: %d.%d.%d.%d)\n", 
           (dhcpMsgType == 1) ? "OFFER" : "ACK",
           (kClientIP >> 24) & 0xFF, (kClientIP >> 16) & 0xFF,
           (kClientIP >> 8) & 0xFF, kClientIP & 0xFF);
    
    if (Callback)
        Callback(reply, finalLen);
}

u16 Net_Switch::IPChecksum(u8* data, int len)
{
    u32 sum = 0;
    for (int i = 0; i < len; i += 2)
    {
        if (i + 1 < len)
            sum += (data[i] << 8) | data[i+1];
        else
            sum += data[i] << 8;
    }
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}

void Net_Switch::HandleIPFrame(u8* data, int len)
{
    if (len < 20) return;

    // Get IP header length (IHL field in first byte, lower 4 bits, in 32-bit words)
    u8 ihl = (data[0] & 0x0F) * 4;
    if (len < ihl) return;

    u8 protocol = data[9];
    u32 srcIP = ntohl(*(u32*)&data[12]);
    u32 dstIP = ntohl(*(u32*)&data[16]);
    
    printf("Net_Switch: IP packet - protocol %d, %d.%d.%d.%d -> %d.%d.%d.%d (len=%d)\n",
           protocol,
           srcIP >> 24, (srcIP >> 16) & 0xFF, (srcIP >> 8) & 0xFF, srcIP & 0xFF,
           dstIP >> 24, (dstIP >> 16) & 0xFF, (dstIP >> 8) & 0xFF, dstIP & 0xFF,
           len);
    
    // Handle UDP packets
    if (protocol == 0x11) // UDP
    {
        printf("Net_Switch: UDP detected (protocol 0x11)\n");
        HandleUDPFrame(data, len);
        return;
    }
    
    // Handle ICMP packets (ping)
    if (protocol == 0x01) // ICMP
    {
        HandleICMPFrame(data, len);
        return;
    }
    
    // Handle TCP packets
    if (protocol == 0x06) // TCP
    {
        HandleTCPFrame(data, len);
        return;
    }
}

void Net_Switch::HandleICMPFrame(u8* ipHeader, int ipLen)
{
    u8 ihl = (ipHeader[0] & 0x0F) * 4;
    if (ipLen < ihl + 8) return;

    u8* icmp = ipHeader + ihl;
    u8 type = icmp[0];
    u8 code = icmp[1];

    u32 srcIP = ntohl(*(u32*)&ipHeader[12]);
    u32 dstIP = ntohl(*(u32*)&ipHeader[16]);

    // Handle Echo Request (ping)
    if (type == 8 && code == 0)
    {
        int icmpLen = ipLen - ihl;
        printf("Net_Switch: ICMP Echo Request\n");

        // Send Echo Reply back
        SendICMPPacket(dstIP, srcIP, 0, 0, icmp + 8, icmpLen - 8);
    }
}

void Net_Switch::HandleTCPFrame(u8* ipHeader, int ipLen)
{
    u8 ihl = (ipHeader[0] & 0x0F) * 4;
    if (ipLen < ihl + 20) return;

    u8* tcp = ipHeader + ihl;
    
    // Extract IP addresses
    u32 srcIP = ntohl(*(u32*)&ipHeader[12]);
    u32 dstIP = ntohl(*(u32*)&ipHeader[16]);
    
    // Extract TCP fields
    u16 srcPort = ntohs(*(u16*)&tcp[0]);
    u16 dstPort = ntohs(*(u16*)&tcp[2]);
    u32 seqNum = ntohl(*(u32*)&tcp[4]);
    u32 ackNum = ntohl(*(u32*)&tcp[8]);
    u8 tcpHeaderLen = ((tcp[12] >> 4) & 0x0F) * 4;
    u8 flags = tcp[13];
    bool isSYN = (flags & 0x02) != 0;
    bool isACK = (flags & 0x10) != 0;
    bool isFIN = (flags & 0x01) != 0;
    
    int dataLen = ipLen - ihl - tcpHeaderLen;
    
    printf("Net_Switch: TCP %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u (flags=0x%02x)\n",
           srcIP & 0xFF, (srcIP >> 8) & 0xFF, (srcIP >> 16) & 0xFF, (srcIP >> 24) & 0xFF, srcPort,
           dstIP & 0xFF, (dstIP >> 8) & 0xFF, (dstIP >> 16) & 0xFF, (dstIP >> 24) & 0xFF, dstPort,
           flags);
    
    u32 key = MakeConnectionKey(srcIP, srcPort);
    
    // Only handle SYN packets (new connection initiation)
    if (isSYN && !isACK)
    {
        printf("Net_Switch: Responding to TCP SYN with SYN-ACK\n");
        u32 responseSeq = 0x10000000;
        u32 responseAck = seqNum + 1;
        SendTCPPacket(dstIP, dstPort, srcIP, srcPort, responseSeq, responseAck, 0x12, nullptr, 0);
        
        // Track connection
        TCPConnections[key] = {
            .socket = -1,
            .clientIP = srcIP,
            .clientPort = srcPort,
            .destIP = dstIP,
            .destPort = dstPort,
            .connected = false
        };
    }
    // For all other packets on established connections, just send ACK
    else
    {
        auto it = TCPConnections.find(key);
        if (it != TCPConnections.end())
        {
            // Send ACK for this packet - acknowledge the sequence number + data length
            u32 responseSeq = 0x10000001;
            u32 responseAck = seqNum + (dataLen > 0 ? dataLen : (isFIN ? 1 : 0));
            printf("Net_Switch: Sending TCP ACK\n");
            SendTCPPacket(dstIP, dstPort, srcIP, srcPort, responseSeq, responseAck, 0x10, nullptr, 0);
            
            // If FIN, also close the connection
            if (isFIN)
            {
                printf("Net_Switch: Sending FIN-ACK in response\n");
                responseSeq = 0x10000002;
                SendTCPPacket(dstIP, dstPort, srcIP, srcPort, responseSeq, responseAck + 1, 0x11, nullptr, 0);
                TCPConnections.erase(it);
            }
        }
    }
}

void Net_Switch::HandleUDPFrame(u8* ipHeader, int ipLen)
{
    u8 ihl = (ipHeader[0] & 0x0F) * 4;
    if (ipLen < ihl + 8) return;

    u8* udp = ipHeader + ihl;
    u16 srcPort = ntohs(*(u16*)&udp[0]);
    u16 dstPort = ntohs(*(u16*)&udp[2]);
    u16 udpLen = ntohs(*(u16*)&udp[4]);
    
    u32 srcIP = ntohl(*(u32*)&ipHeader[12]);
    u32 dstIP = ntohl(*(u32*)&ipHeader[16]);
    
    int dataLen = udpLen - 8;
    if (dataLen < 0 || ihl + udpLen > ipLen) return;

    printf("Net_Switch: UDP %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d (len=%d)\n",
           srcIP >> 24, (srcIP >> 16) & 0xFF, (srcIP >> 8) & 0xFF, srcIP & 0xFF, srcPort,
           dstIP >> 24, (dstIP >> 16) & 0xFF, (dstIP >> 8) & 0xFF, dstIP & 0xFF, dstPort,
           udpLen);

    // Handle DHCP (port 67) - accept any source port during DHCP
    if (dstPort == 67)
    {
        printf("Net_Switch: DHCP packet detected (port 67)\n");
        HandleDHCPFrame(udp, udpLen, srcIP);
        return;
    }

    // Handle DNS (port 53) - forward to real DNS server
    if (dstPort == 53)
    {
        printf("Net_Switch: DNS packet detected (port 53)\n");
        HandleDNSFrame(udp + 8, dataLen, srcIP, srcPort);
        return;
    }

    printf("Net_Switch: Generic UDP forwarding\n");
    // Generic UDP forwarding to internet
    ForwardUDPPacket(srcIP, srcPort, dstIP, dstPort, udp + 8, dataLen);
}

void Net_Switch::ForwardUDPPacket(u32 srcIP, u16 srcPort, u32 dstIP, u16 dstPort, u8* data, int len)
{
#ifndef __SWITCH__
    return; // Only works on Switch
#else
    // Find existing connection or create new one
    u32 key = MakeConnectionKey(srcIP, srcPort);
    auto it = UDPConnections.find(key);
    
    if (it == UDPConnections.end())
    {
        // Create new UDP socket
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
        {
            printf("Net_Switch: Failed to create UDP socket: %d\n", errno);
            return;
        }
        
        // Set non-blocking
        fcntl(sock, F_SETFL, O_NONBLOCK);
        
        // Create connection entry
        UDPConnection conn;
        conn.socket = sock;
        conn.clientIP = srcIP;
        conn.clientPort = srcPort;
        conn.destIP = dstIP;
        conn.destPort = dstPort;
        conn.lastActivity = GetMonotonicTime();
        conn.isDNS = false; // Will be set to true by HandleDNSFrame if needed
        
        UDPConnections[key] = conn;
        
        printf("Net_Switch: New UDP connection: %d -> %d.%d.%d.%d:%d\n",
               srcPort, (dstIP>>24)&0xFF, (dstIP>>16)&0xFF, (dstIP>>8)&0xFF, dstIP&0xFF, dstPort);
    }
    else
    {
        // Update destination if changed
        it->second.destIP = dstIP;
        it->second.destPort = dstPort;
    }
    
    // Send data to real destination
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(dstIP);
    addr.sin_port = htons(dstPort);
    
    ssize_t sent = sendto(UDPConnections[key].socket, data, len, 0, 
                          (struct sockaddr*)&addr, sizeof(addr));
    
    if (sent < 0)
    {
        printf("Net_Switch: UDP send failed: %d\n", errno);
    }
    else
    {
        UDPConnections[key].lastActivity = GetMonotonicTime();
    }
#endif
}

int Net_Switch::SendPacket(u8* data, int len)
{
    if (!Initialized || !data || len <= 0)
        return 0;

    if (len > 2048)
        return 0;

    // Check frame type
    if (len >= 14)
    {
        u16 ethertype = (data[12] << 8) | data[13];
        
        printf("Net_Switch: Packet received - ethertype=0x%04X, len=%d\n", ethertype, len);
        
        switch (ethertype)
        {
        case 0x0806: // ARP
            printf("Net_Switch: ARP packet\n");
            // Pass Ethernet frame with Ethernet header intact (ARP handler reads from offset 14)
            HandleARPFrame(data, len);
            break;
            
        case 0x0800: // IPv4
            printf("Net_Switch: IPv4 packet\n");
            // Skip Ethernet header (14 bytes) and pass only IP portion
            HandleIPFrame(data + 14, len - 14);
            break;

        default:
            printf("Net_Switch: Unknown ethertype 0x%04X\n", ethertype);
            break;
        }
    }

#ifdef __SWITCH__
    // On Switch, we can use the built-in network stack
    // For emulation purposes, we mainly just acknowledge receipt
    // Real network I/O would happen through Switch's network services
#endif

    return len;
}

void Net_Switch::RecvCheck()
{
    if (!Initialized)
        return;

    // Update current time
    CurrentTime = GetMonotonicTime();

    // Process UDP connections (check for incoming data)
    ProcessUDPConnections();

    // Cleanup old connections periodically
    static u64 lastCleanup = 0;
    if (CurrentTime - lastCleanup > 5000000) // Every 5 seconds
    {
        CleanupOldConnections();
        lastCleanup = CurrentTime;
    }

#ifdef __SWITCH__
    // Check for incoming packets from the Switch's network stack
    // This would poll the network interface for incoming data
    // For now, this is a simplified implementation
#endif

    // Process any queued packets in RXBuffer
    while (!RXBuffer.IsEmpty())
    {
        u32 entry = RXBuffer.Read();
        
        // Process packet data
        // This would typically involve reading from a packet buffer
        // and calling the callback with the received data
    }
}


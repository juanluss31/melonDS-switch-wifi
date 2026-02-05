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
#include "Net_Switch.h"

#ifdef __SWITCH__
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

// Subnet configuration - same as libslirp uses
const u32 kSubnet   = 0x0A400000;
const u32 kServerIP = kSubnet | 0x01;
const u32 kDNSIP    = kSubnet | 0x02;
const u32 kClientIP = kSubnet | 0x10;

const u8 kServerMAC[6] = {0x00, 0xAB, 0x33, 0x28, 0x99, 0x44};

Net_Switch::Net_Switch(const SendPacketCallback& callback)
    : Callback(callback)
    , IPv4ID(0)
    , Initialized(false)
{
#ifdef __SWITCH__
    // Initialize BSD sockets on Switch
    socketInitializeDefault();
    Initialized = true;
#endif
}

Net_Switch::~Net_Switch()
{
#ifdef __SWITCH__
    if (Initialized)
    {
        socketExit();
        Initialized = false;
    }
#endif
}

void Net_Switch::HandleARPFrame(u8* data, int len)
{
    // Basic ARP handling - respond to ARP requests
    if (len < 28) return;

    u16 protocol = (data[2] << 8) | data[3];
    if (protocol != 0x0800) return; // Only handle IPv4

    u16 op = (data[6] << 8) | data[7];
    if (op != 1) return; // Only handle requests

    // Build ARP reply
    u8 reply[64];
    memset(reply, 0, sizeof(reply));

    // Ethernet header
    memcpy(&reply[0], &data[8+6], 6); // dest MAC = sender MAC from ARP
    memcpy(&reply[6], kServerMAC, 6); // src MAC = our MAC
    reply[12] = 0x08; reply[13] = 0x06; // ARP

    // ARP header
    reply[14] = 0x00; reply[15] = 0x01; // Hardware type: Ethernet
    reply[16] = 0x08; reply[17] = 0x00; // Protocol type: IPv4
    reply[18] = 6; // Hardware size
    reply[19] = 4; // Protocol size
    reply[20] = 0x00; reply[21] = 0x02; // Opcode: Reply

    memcpy(&reply[22], kServerMAC, 6); // Sender MAC
    memcpy(&reply[28], &data[8+14], 4); // Sender IP (target IP from request)
    memcpy(&reply[32], &data[8+6], 6); // Target MAC
    memcpy(&reply[38], &data[8+10], 4); // Target IP (sender IP from request)

    if (Callback)
        Callback(reply, 42);
}

void Net_Switch::HandleDNSFrame(u8* data, int len)
{
    // Simple DNS handling - minimal implementation
    // Most DNS queries will just be dropped, real DNS would be handled by Switch OS
    // This is mainly to prevent crashes and provide basic functionality
}

void Net_Switch::HandleIPFrame(u8* data, int len)
{
    if (len < 20) return;

    u8 protocol = data[9];
    
    // For now, we mainly handle the frame reception
    // Actual protocol handling would need more complex implementation
    // The Switch's network stack will handle most of this
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
        
        switch (ethertype)
        {
        case 0x0806: // ARP
            HandleARPFrame(data, len);
            break;
            
        case 0x0800: // IPv4
            HandleIPFrame(data, len);
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

#ifdef __SWITCH__
    // Check for incoming packets from the Switch's network stack
    // This would poll the network interface for incoming data
    // For now, this is a simplified implementation
#endif

    // Process any queued packets in RXBuffer
    while (!RXBuffer.IsEmpty())
    {
        u32 entry;
        RXBuffer.Read(entry);
        
        // Process packet data
        // This would typically involve reading from a packet buffer
        // and calling the callback with the received data
    }
}

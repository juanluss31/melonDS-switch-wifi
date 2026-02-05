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

#ifdef __SWITCH__
#include <switch.h>
#endif

// Simple network driver for Nintendo Switch using built-in network stack
// This provides basic networking capabilities using the Switch's native network interface
class Net_Switch : public NetDriver
{
public:
    using SendPacketCallback = std::function<void(const u8* data, int len)>;

    explicit Net_Switch(const SendPacketCallback& callback);
    ~Net_Switch() override;

    int SendPacket(u8* data, int len) override;
    void RecvCheck() override;

private:
    SendPacketCallback Callback;
    FIFO<u32, (0x8000 >> 2)> RXBuffer;
    u32 IPv4ID;
    bool Initialized;

#ifdef __SWITCH__
    SocketInitConfig socketConfig;
#endif

    void HandleARPFrame(u8* data, int len);
    void HandleIPFrame(u8* data, int len);
    void HandleDNSFrame(u8* data, int len);
};

#endif // NET_SWITCH_H

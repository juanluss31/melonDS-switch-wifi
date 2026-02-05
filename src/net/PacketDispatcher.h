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

#ifndef PACKETDISPATCHER_H
#define PACKETDISPATCHER_H

#include "../types.h"
#include "../FIFO.h"

class PacketDispatcher
{
public:
    PacketDispatcher();
    ~PacketDispatcher();

    void registerInstance(int inst);
    void unregisterInstance(int inst);

    void clear();

    void sendPacket(const u8* header, int headerlen, const void* data, int len, int seqno, u16 instancemask);
    bool recvPacket(u8* header, int* headerlen, u8* data, int* len, int instancenum);

private:
    struct Packet
    {
        u32 Timestamp;
        u8 Data[2048];
        int Length;
        u16 SenderID;
        u16 DestMask;
    };

    FIFO<Packet, 64> PacketQueue;
    int InstanceMask;
};

#endif // PACKETDISPATCHER_H

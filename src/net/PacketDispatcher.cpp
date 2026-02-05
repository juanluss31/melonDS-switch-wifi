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

#include <string.h>
#include "PacketDispatcher.h"

PacketDispatcher::PacketDispatcher()
{
    InstanceMask = 0;
}

PacketDispatcher::~PacketDispatcher()
{
}

void PacketDispatcher::registerInstance(int inst)
{
    InstanceMask |= (1 << inst);
}

void PacketDispatcher::unregisterInstance(int inst)
{
    InstanceMask &= ~(1 << inst);
}

void PacketDispatcher::clear()
{
    PacketQueue.Clear();
}

void PacketDispatcher::sendPacket(const u8* header, int headerlen, const void* data, int len, int seqno, u16 instancemask)
{
    if (PacketQueue.IsFull()) return;

    Packet pkt;
    pkt.Timestamp = seqno;
    
    int pos = 0;
    if (header && headerlen)
    {
        memcpy(&pkt.Data[pos], header, headerlen);
        pos += headerlen;
    }
    
    if (data && len)
    {
        memcpy(&pkt.Data[pos], data, len);
        pos += len;
    }
    
    pkt.Length = pos;
    pkt.SenderID = 0;
    pkt.DestMask = instancemask;

    PacketQueue.Write(pkt);
}

bool PacketDispatcher::recvPacket(u8* header, int* headerlen, u8* data, int* len, int instancenum)
{
    if (PacketQueue.IsEmpty()) return false;

    Packet pkt = PacketQueue.Read();

    if (!(pkt.DestMask & (1 << instancenum)))
    {
        // Packet not for this instance, put it back
        return false;
    }

    if (data && len)
    {
        if (header && headerlen)
        {
            memcpy(header, pkt.Data, *headerlen);
            memcpy(data, &pkt.Data[*headerlen], pkt.Length - *headerlen);
            *len = pkt.Length - *headerlen;
        }
        else
        {
            memcpy(data, pkt.Data, pkt.Length);
            *len = pkt.Length;
        }
    }

    return true;
}

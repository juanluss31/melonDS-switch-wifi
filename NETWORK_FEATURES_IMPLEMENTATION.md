# Complete Network Features Implementation for melonDS-switch-wifi

## Overview
This document provides a complete implementation plan to bring all networking features from the official melonDS repository to the Switch version.

## Architecture Comparison

### Official melonDS (Desktop)
- **libslirp**: Complete user-mode TCP/IP stack
  - Full DHCP server with all options
  - TCP connection tracking and proxying
  - UDP socket forwarding
  - DNS proxy
  - TFTP server
  - NAT/routing with connection state tracking
  
### Current Switch Implementation
- Basic ARP handling ✅
- Minimal IP frame processing ⚠️
- Started DHCP (incomplete) ⚠️
- No TCP support ❌
- No UDP forwarding ❌
- No DNS proxy ❌

## Implementation Plan

### Phase 1: Core Infrastructure (PRIORITY)
The most critical missing piece is **UDP forwarding** because:
- DNS queries use UDP
- Many games use UDP for communication
- WFC uses UDP extensively

### Phase 2: Complete DHCP
- Full option support
- Proper lease management
- Support for renewal

### Phase 3: TCP Support  
- Connection tracking
- Socket proxying
- State management

### Phase 4: DNS Proxy
- Forward DNS queries to real DNS servers
- Cache responses
- Handle special cases

## Detailed Implementation

### 1. UDP Forwarding (CRITICAL)

The key is to create a mapping between:
- Client (DS) UDP port → Real internet socket
- Track connection state
- Forward packets bidirectionally

```cpp
// Add to Net_Switch.h
struct UDPConnection
{
    int socket;          // Real socket to internet
    u32 clientIP;        // DS IP (10.64.0.16)
    u16 clientPort;      // DS source port
    u32 destIP;          // Real destination IP
    u16 destPort;        // Real destination port  
    u64 lastActivity;    // For timeout
    bool needsDNS;       // If destIP needs resolution
};

std::map<u16, UDPConnection> UDPConnections; // Key: client port
```

#### UDP Packet Flow

**Outgoing (DS → Internet):**
1. DS sends UDP packet to any destination
2. Extract: srcPort, dstIP, dstPort, data
3. Check if connection exists for srcPort
4. If not, create new real UDP socket
5. Forward data to real destination via socket
6. Store mapping

**Incoming (Internet → DS):**
1. Poll all UDP sockets for data
2. When data arrives, lookup which DS port it belongs to
3. Construct UDP/IP/Ethernet packet
4. Send back to DS via callback

```cpp
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
    
    // Handle DHCP (port 67)
    if (dstPort == 67) {
        HandleDHCPFrame(udp, udpLen, srcIP);
        return;
    }
    
    // Handle DNS (port 53) - forward to real DNS
    if (dstPort == 53) {
        HandleDNSForward(udp, udpLen, srcIP, srcPort, dstIP);
        return;
    }
    
    // Generic UDP forwarding
    ForwardUDPPacket(srcIP, srcPort, dstIP, dstPort, udp + 8, udpLen - 8);
}

void Net_Switch::ForwardUDPPacket(u32 srcIP, u16 srcPort, u32 dstIP, u16 dstPort, u8* data, int len)
{
    // Find or create connection
    auto it = UDPConnections.find(srcPort);
    
    if (it == UDPConnections.end()) {
        // Create new UDP socket
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            printf("Failed to create UDP socket\n");
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
        
        UDPConnections[srcPort] = conn;
        it = UDPConnections.find(srcPort);
        
        printf("UDP: New connection %d -> %d.%d.%d.%d:%d\n",
               srcPort, (dstIP>>24)&0xFF, (dstIP>>16)&0xFF, (dstIP>>8)&0xFF, dstIP&0xFF, dstPort);
    }
    
    // Send data to real destination
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(dstIP);
    addr.sin_port = htons(dstPort);
    
    sendto(it->second.socket, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    it->second.lastActivity = GetMonotonicTime();
}
```

#### Receiving UDP Data

```cpp
void Net_Switch::RecvCheck()
{
    CurrentTime = GetMonotonicTime();
    
    // Check all UDP connections for incoming data
    for (auto& pair : UDPConnections) {
        UDPConnection& conn = pair.second;
        
        u8 buffer[2048];
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        
        ssize_t received = recvfrom(conn.socket, buffer, sizeof(buffer), MSG_DONTWAIT,
                                     (struct sockaddr*)&from, &fromLen);
        
        if (received > 0) {
            // Got data - send back to DS
            u32 realSrcIP = ntohl(from.sin_addr.s_addr);
            u16 realSrcPort = ntohs(from.sin_port);
            
            SendUDPPacket(realSrcIP, realSrcPort, conn.clientIP, conn.clientPort,
                          buffer, received);
            
            conn.lastActivity = CurrentTime;
        }
    }
    
    // Cleanup old connections
    CleanupOldConnections();
}
```

### 2. DNS Forwarding

Instead of trying to implement a DNS server, forward queries to real DNS:

```cpp
void Net_Switch::HandleDNSForward(u8* udp, int udpLen, u32 srcIP, u16 srcPort, u32 dstIP)
{
    // Extract DNS query data
    u8* dnsData = udp + 8;
    int dnsLen = udpLen - 8;
    
    // Forward to real DNS server (e.g., 8.8.8.8 or Switch's configured DNS)
    u32 realDNS = 0x08080808; // 8.8.8.8 or get from Switch config
    
    ForwardUDPPacket(srcIP, srcPort, realDNS, 53, dnsData, dnsLen);
}
```

### 3. TCP Support (Complex)

TCP requires:
- Connection state tracking (SYN, SYN-ACK, ACK, FIN, etc.)
- Sequence number management
- Retransmission
- Flow control

**Simplified approach for Switch:**
- Create real TCP socket when DS initiates connection
- Proxy data bidirectionally
- Translate TCP packets to/from real sockets

```cpp
struct TCPConnection
{
    int socket;
    u32 clientIP;
    u16 clientPort;
    u32 destIP;
    u16 destPort;
    u32 clientSeq;   // DS sequence number
    u32 clientAck;   // DS acknowledgment
    u32 serverSeq;   // Our sequence number  
    enum { CLOSED, SYN_SENT, ESTABLISHED, FIN_WAIT, CLOSED } state;
};

void Net_Switch::HandleTCPFrame(u8* ipHeader, int ipLen)
{
    u8 ihl = (ipHeader[0] & 0x0F) * 4;
    if (ipLen < ihl + 20) return;
    
    u8* tcp = ipHeader + ihl;
    u16 srcPort = ntohs(*(u16*)&tcp[0]);
    u16 dstPort = ntohs(*(u16*)&tcp[2]);
    u32 seq = ntohl(*(u32*)&tcp[4]);
    u32 ack = ntohl(*(u32*)&tcp[8]);
    u8 flags = tcp[13];
    
    // ... complex state machine here ...
}
```

## Implementation Priority

1. **UDP Forwarding** - Implement ASAP (enables DNS, most games)
2. **Complete DHCP** - Already started, finish it
3. **ICMP** - Simple, good for debugging
4. **TCP** - More complex, implement if games need it
5. **DNS Caching** - Optimization, not critical

## Testing Plan

1. Test UDP forwarding with simple DNS query
2. Test game connectivity
3. Test TCP if implemented
4. Performance testing
5. Memory leak checking

## Code Integration

To integrate:
1. Update `Net_Switch.h` with new structures
2. Add UDP forwarding methods to `Net_Switch.cpp`  
3. Update `RecvCheck()` to poll sockets
4. Add connection cleanup
5. Test incrementally

## Notes

- libslirp is ~15,000 lines of code - full port is massive
- Focus on what games actually need (mostly UDP)
- Switch's BSD sockets handle the hard parts (TCP state, retransmission)
- Our job is just packet translation and forwarding

## Next Steps

Would you like me to:
1. Implement UDP forwarding first (most critical)?
2. Complete the DHCP server?
3. Add TCP support?
4. All of the above in phases?

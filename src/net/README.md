# Network Module for melonDS-switch-wifi

This directory contains the network infrastructure for melonDS on Nintendo Switch.

## Files

### Core Network Classes
- **Net.h/cpp**: Main network class that manages packet routing and driver interfaces
- **NetDriver.h**: Abstract base class for network drivers
- **PacketDispatcher.h/cpp**: Handles packet routing between different emulator instances

### Network Drivers
- **Net_Switch.h/cpp**: Nintendo Switch-specific network driver that uses the Switch's built-in network stack

## Architecture

The networking system follows a layered architecture:

1. **Network Driver Layer** (`NetDriver`): Abstract interface for different network backends
2. **Packet Management Layer** (`PacketDispatcher`): Routes packets between emulator instances  
3. **Application Layer** (`Net`): High-level interface used by the emulator core

## Implementation Notes

The Switch implementation (`Net_Switch`) uses Nintendo Switch's BSD socket API for network I/O. It provides:

- ARP handling for address resolution
- Basic IPv4 packet processing
- DNS frame handling (minimal implementation)
- Integration with Switch's network services

## Usage

Network initialization is handled in `Platform.cpp`:

```cpp
InitNet();  // Initializes the network stack
Net_SendPacket(data, len);  // Send a packet
Net_RecvPacket(data);  // Receive a packet
DeInitNet();  // Clean up network resources
```

## Comparison with Main melonDS

The main melonDS project has additional network drivers:
- `Net_PCap`: Direct network access via libpcap (not available on Switch)
- `Net_Slirp`: User-mode network stack using libslirp (could be ported if needed)
- `LAN.cpp`: Local multiplayer over LAN using ENet
- `Netplay.cpp`: Netplay functionality

For Switch, we use a simplified approach with `Net_Switch` that integrates with the console's native networking.

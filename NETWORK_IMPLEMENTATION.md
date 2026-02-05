# Network Capabilities Implementation for melonDS-switch-wifi

This document describes the network capabilities that have been added to melonDS-switch-wifi, based on the networking infrastructure from the main melonDS project.

## Overview

The emulator now has comprehensive networking support that allows DS/DSi games to access the internet through the Nintendo Switch's network connection. This implementation is based on the architecture used in the main melonDS project but adapted for the Nintendo Switch platform.

## Architecture

### Core Components

1. **Net Layer** (`src/net/Net.h`, `src/net/Net.cpp`)
   - Main network management class
   - Handles packet routing between emulator and network driver
   - Manages multiple emulator instances if needed

2. **NetDriver Interface** (`src/net/NetDriver.h`)
   - Abstract base class for network drivers
   - Defines `SendPacket()` and `RecvCheck()` methods
   - Allows different network backend implementations

3. **PacketDispatcher** (`src/net/PacketDispatcher.h`, `src/net/PacketDispatcher.cpp`)
   - Routes packets between different emulator instances
   - Manages packet queues (FIFO-based)
   - Handles packet filtering and delivery

4. **Net_Switch Driver** (`src/net/Net_Switch.h`, `src/net/Net_Switch.cpp`)
   - Switch-specific network driver implementation
   - Uses Nintendo Switch's BSD socket API
   - Handles ARP, IPv4, and DNS frames
   - Integrates with Switch's native network services

### Integration Points

**Platform Interface** (`src/Platform.h`, `src/frontend/switch/Platform.cpp`)
- Added `Net_SendPacket()` and `Net_RecvPacket()` functions
- Updated `LAN_*` functions to use the new network infrastructure
- Added initialization and cleanup functions (`InitNet()`, `DeInitNet()`)

**WifiAP Integration** (`src/WifiAP.cpp`)
- Already uses `Platform::LAN_SendPacket()` and `Platform::LAN_RecvPacket()`
- Automatically benefits from the new network capabilities
- No changes needed - works transparently with new infrastructure

## Comparison with Main melonDS

The main melonDS project has three network driver implementations:

1. **Net_PCap**: Direct network access via libpcap (Windows/Linux/macOS)
   - Not available on Switch (no libpcap)
   - Requires elevated privileges on most systems
   
2. **Net_Slirp**: User-mode network stack using libslirp
   - Could potentially be ported to Switch
   - Currently not implemented in this version
   
3. **LAN**: Local multiplayer over LAN using ENet
   - Not yet ported to Switch
   - Could be added in the future for multiplayer support

Our implementation uses:
- **Net_Switch**: Native Switch networking using BSD sockets
  - Uses Switch's built-in network stack
  - No special privileges required
  - Integrates seamlessly with Switch's network management

## Features

### Current Implementation

✅ **Internet Connectivity**
- DS/DSi games can access the internet
- HTTP/HTTPS support through Switch's network stack
- DNS resolution handled by Switch OS

✅ **WFC (Nintendo Wi-Fi Connection) Emulation**  
- Games can connect to WFC replacement servers
- Full 802.11 frame handling
- ARP and IPv4 support

✅ **Access Point Emulation**
- Virtual AP with configurable SSID ("melonAP")
- Authentication and association handling
- Broadcast and unicast packet support

### Network Configuration

The implementation uses a subnet configuration similar to libslirp:
- Subnet: `10.64.0.0/24`
- Gateway/Server IP: `10.64.0.1`
- DNS Server IP: `10.64.0.2`
- Client IP: `10.64.0.16`

These are handled internally and don't require user configuration.

## Usage

### For Users

1. **Enable Network Access**
   - Ensure your Switch has an active internet connection
   - The emulator will automatically use it

2. **Playing Online-Capable Games**
   - Load a DS/DSi game that supports Wi-Fi
   - The game should detect the virtual access point "melonAP"
   - Connect as you would with a real DS

3. **WFC Replacement Servers**
   - Configure your DNS to point to WFC replacement services (e.g., Wiimmfi)
   - Or patch ROMs to use alternative servers

### For Developers

Network initialization happens automatically in `Platform.cpp`:

```cpp
InitNet();  // Called when LAN_Init() is invoked
```

Sending and receiving packets:

```cpp
// Send packet to network
Platform::LAN_SendPacket(data, length);

// Receive packet from network  
int len = Platform::LAN_RecvPacket(buffer);
```

The `WifiAP` module handles all 802.11 frame processing and calls these functions automatically.

## Technical Details

### Packet Flow

1. **Outgoing Packets** (DS → Internet):
   ```
   DS WiFi → Wifi.cpp → WifiAP.cpp → Platform::LAN_SendPacket()
      → Net::SendPacket() → Net_Switch::SendPacket() → Switch Network Stack
   ```

2. **Incoming Packets** (Internet → DS):
   ```
   Switch Network Stack → Net_Switch::RecvCheck() → Net::RecvPacket()
      → Platform::LAN_RecvPacket() → WifiAP.cpp → Wifi.cpp → DS WiFi
   ```

### Frame Handling

**Net_Switch** handles three types of Ethernet frames:

1. **ARP (0x0806)**
   - Responds to ARP requests
   - Maintains virtual MAC-to-IP mappings
   - Server MAC: `00:AB:33:28:99:44`

2. **IPv4 (0x0800)**
   - Routes IP packets through Switch's network stack
   - Handles TCP/UDP protocols
   - Supports fragmentation if needed

3. **Other Frames**
   - Dropped or passed through depending on type
   - Could be extended for IPv6 support

### Performance Considerations

- Packet buffering uses FIFO queues (8KB RX buffer)
- Non-blocking I/O to prevent emulation slowdown
- Minimal overhead - network checks only when needed
- Switch's hardware handles actual network I/O

## Future Enhancements

### Potential Additions

1. **libslirp Integration**
   - Port Net_Slirp for more complete networking
   - Would provide better compatibility with some games

2. **Local Multiplayer** 
   - Implement LAN.cpp for local wireless multiplayer
   - Would use ENet for reliable networking

3. **Netplay Support**
   - Port Netplay.cpp for online multiplayer between emulator instances
   - Could enable DS multiplayer over internet

4. **Configuration Options**
   - UI for network settings
   - DNS server configuration
   - Network adapter selection (if multiple available)

5. **IPv6 Support**
   - Handle 0x86DD Ethernet frames
   - Full dual-stack networking

## Building

The network module is automatically included in the build:

```cmake
# In src/CMakeLists.txt
add_library(core STATIC
    ...
    net/Net.cpp
    net/Net_Switch.cpp  
    net/PacketDispatcher.cpp
    ...
)
```

No additional dependencies are required beyond the Nintendo Switch SDK.

## Troubleshooting

### Common Issues

**Problem**: Games can't connect to Wi-Fi
- **Solution**: Check that your Switch has an active internet connection
- **Solution**: Verify the game supports Wi-Fi (not all DS games do)

**Problem**: WFC servers don't work
- **Solution**: WFC was shut down in 2014; use WFC replacement servers (e.g., Wiimmfi)
- **Solution**: Some games may need ROM patches for replacement servers

**Problem**: Slow network performance  
- **Solution**: Ensure a strong Wi-Fi signal on your Switch
- **Solution**: Check for interference from other devices

## References

- Main melonDS networking: https://github.com/melonDS-emu/melonDS/tree/main/src/net
- Nintendo Switch Network API: Part of devkitPro SDK
- BSD Sockets documentation: Standard POSIX sockets API
- 802.11 Frame format: IEEE 802.11 specification

## Credits

Network implementation based on:
- melonDS by Arisotura and contributors
- Network architecture from the main melonDS project
- Adapted for Nintendo Switch by the melonDS-switch-wifi team

## License

This code is licensed under GPLv3, consistent with the main melonDS project.

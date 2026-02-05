# Network module for melonDS-switch-wifi
# This provides the networking infrastructure for the emulator

add_library(net INTERFACE)

target_sources(net INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Net.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Net_Switch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PacketDispatcher.cpp
)

target_include_directories(net INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

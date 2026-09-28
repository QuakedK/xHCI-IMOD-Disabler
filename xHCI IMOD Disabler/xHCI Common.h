#pragma once

#include <Windows.h>
#include <cstdint>

namespace xHCI
{
    constexpr const char* VERSION = "V1.1";

    // IMOD interval values, in 250 ns units.
    constexpr uint32_t DESIRED_IMOD_INTERVAL = 0x0000;      // Moderation off.
    constexpr uint32_t TEST_IMOD_INTERVAL = 0xFA00;         // 16 ms, about 62.5 Hz.

    // The IMOD register holds the interval (IMODI) in bits 15:0 and a live
    // countdown (IMODC) in bits 31:16, so only the low 16 bits are compared.
    constexpr uint32_t IMOD_INTERVAL_MASK = 0x0000FFFF;

    // PCI configuration space.
    constexpr DWORD PCI_REG_VENDOR_DEVICE = 0x00;
    constexpr DWORD PCI_REG_COMMAND = 0x04;
    constexpr DWORD PCI_REG_CLASS_CODE = 0x08;
    constexpr DWORD PCI_REG_BAR0 = 0x10;
    constexpr DWORD PCI_REG_BAR1 = 0x14;
    constexpr DWORD PCI_COMMAND_MEMORY_SPACE = 0x0002;
    constexpr DWORD PCI_CLASS_XHCI = 0x0C0330;

    // xHCI register layout (xHCI specification, sections 5.3 and 5.5).
    constexpr uint32_t CAPABILITY_WINDOW_SIZE = 0x1000;
    constexpr uint32_t HCSPARAMS1_OFFSET = 0x04;
    constexpr uint32_t RTSOFF_OFFSET = 0x18;
    constexpr uint32_t INTERRUPTER_OFFSET = 0x20;
    constexpr uint32_t INTERRUPTER_SIZE = 0x20;
    constexpr uint32_t IMOD_OFFSET = 0x04;
    constexpr uint32_t MAX_INTERRUPTERS = 1024;

    // Exports used from WinRing0x64.dll.
    using InitializeOls_t = BOOL(WINAPI*)();
    using DeinitializeOls_t = VOID(WINAPI*)();
    using ReadPciConfigDwordEx_t = BOOL(WINAPI*)(DWORD pciAddress, DWORD regAddress, PDWORD value);

    // Exports used from inpoutx64.dll.
    using MapPhysToLin_t = PBYTE(WINAPI*)(PBYTE physicalAddress, DWORD size, HANDLE* physicalMemoryHandle);
    using UnmapPhysicalMemory_t = BOOL(WINAPI*)(HANDLE physicalMemoryHandle, PBYTE linearAddress);
    using IsInpOutDriverOpen_t = BOOL(WINAPI*)();
}

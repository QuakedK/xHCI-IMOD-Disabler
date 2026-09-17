#pragma once

#include <Windows.h>
#include <cstdint>

namespace xHCI
{
    constexpr uint32_t DESIRED_IMOD_INTERVAL = 0x00000000;

    using InitializeOls_t = BOOL(WINAPI*)();
    using DeinitializeOls_t = VOID(WINAPI*)();
    using GetDllStatus_t = DWORD(WINAPI*)();

    using ReadPciConfigDwordEx_t =
        BOOL(WINAPI*)(DWORD pciAddress, DWORD regAddress, PDWORD value);

    using MapPhysToLin_t =
        PBYTE(WINAPI*)(PBYTE physicalAddress,
            DWORD size,
            HANDLE* physicalMemoryHandle);

    using UnmapPhysicalMemory_t =
        BOOL(WINAPI*)(HANDLE physicalMemoryHandle,
            PBYTE linearAddress);
}

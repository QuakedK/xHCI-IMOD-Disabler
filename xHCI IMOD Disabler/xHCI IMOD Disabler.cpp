// xHCI IMOD Disabler
//
// Disables xHCI Interrupt Moderation (IMOD) by writing 0 to the IMOD register
// of every interrupter on the USB xHCI controllers the user selects.
//
// Run with no arguments for the menu, or with --Silent (used by the startup
// entries) to disable IMOD on the selected controllers without a window.
// Exit code 0 means success.
//
// Sections, in order: output helpers, utilities, drivers, controllers,
// controller details, backup file, install location, controller selection,
// IMOD actions, startup, menus, entry point.
// 
// Credit: Andrew contributed substantial fixes and improvements to controller
// discovery, PCI/BAR handling, MMIO mapping, startup handling, and
// overall reliability.

#include "xHCI Common.h"

#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <conio.h>
#include <initguid.h>
#include <devpkey.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace
{
    // Output helpers

    // Status symbols as UTF-8 byte sequences (main switches the console to
    // UTF-8). Spelled out in hex so the source stays plain ASCII.
    namespace Glyph
    {
        constexpr const char* CHECK = "\xE2\x9C\x94";       // U+2714 heavy check mark
        constexpr const char* CROSS = "\xE2\x9C\x96";       // U+2716 heavy multiplication x
        constexpr const char* ARROW = "\xE2\x86\x92";       // U+2192 rightwards arrow
        constexpr const char* POINTER = "\xE2\x96\xBA";     // U+25BA black right-pointing pointer
        constexpr const char* LEFT_POINTER = "\xE2\x97\x84"; // U+25C4 black left-pointing pointer
    }

    void PrintStatus(bool succeeded, const std::string& message)
    {
        std::cout << (succeeded ? Glyph::CHECK : Glyph::CROSS) << "  : " << message << "\n";
    }

    // Controller results are indented under the controller's name.
    void PrintControllerError(const char* message)
    {
        std::cout << "  " << Glyph::CROSS << "  : " << message << "\n";
    }

    // Utilities

    std::string ToUtf8(const std::wstring& value)
    {
        if (value.empty())
            return "";

        const int wideLength = static_cast<int>(value.size());
        const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), wideLength, nullptr, 0, nullptr, nullptr);

        if (length <= 0)
            return "";

        std::string result(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), wideLength, result.data(), length, nullptr, nullptr);
        return result;
    }

    std::string HexString(uint64_t value)
    {
        std::ostringstream stream;
        stream << "0x" << std::uppercase << std::hex << value;
        return stream.str();
    }

    // "error 5: Access is denied" style text for a Win32 error code.
    std::string SystemErrorString(DWORD error)
    {
        char buffer[512] = {};
        const DWORD length = FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer, sizeof(buffer), nullptr);

        std::string message(buffer, length);

        while (!message.empty() && std::strchr("\r\n .", message.back()))
            message.pop_back();

        return "error " + std::to_string(error) + (message.empty() ? "" : ": " + message);
    }

    bool GetExecutablePath(std::filesystem::path& executablePath)
    {
        // GetModuleFileNameW truncates silently, so grow the buffer until the path fits.
        std::vector<wchar_t> buffer(MAX_PATH);

        while (buffer.size() <= 32768)
        {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

            if (length == 0)
                return false;

            if (length < buffer.size())
            {
                executablePath = std::wstring(buffer.data(), length);
                return true;
            }

            buffer.resize(buffer.size() * 2);
        }

        return false;
    }

    bool GetExecutableDirectory(std::filesystem::path& directory)
    {
        std::filesystem::path executablePath;

        if (!GetExecutablePath(executablePath))
            return false;

        directory = executablePath.parent_path();
        return !directory.empty();
    }

    // Drivers
    //
    // Both DLLs are loaded by full path from the executable's folder, never
    // through the DLL search path.

    HMODULE LoadDriverLibrary(const wchar_t* fileName, std::string& error)
    {
        std::filesystem::path directory;

        if (!GetExecutableDirectory(directory))
        {
            error = "Could not determine the executable path.";
            return nullptr;
        }

        const std::filesystem::path libraryPath = directory / fileName;
        HMODULE module = LoadLibraryExW(libraryPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

        if (!module)
            error = ToUtf8(libraryPath.wstring()) + " (" + SystemErrorString(GetLastError()) + ")";

        return module;
    }

    template <typename Function>
    Function GetExport(HMODULE module, const char* name)
    {
        return reinterpret_cast<Function>(GetProcAddress(module, name));
    }

    // InpOutX64 maps physical memory, which is how the xHCI registers are read
    // and written. It is required.
    class InpOut
    {
    public:
        InpOut() = default;
        InpOut(const InpOut&) = delete;
        InpOut& operator=(const InpOut&) = delete;

        ~InpOut()
        {
            if (module)
                FreeLibrary(module);
        }

        bool Load(std::string& error)
        {
            module = LoadDriverLibrary(L"inpoutx64.dll", error);

            if (!module)
                return false;

            mapPhysToLin = GetExport<xHCI::MapPhysToLin_t>(module, "MapPhysToLin");
            unmapPhysicalMemory = GetExport<xHCI::UnmapPhysicalMemory_t>(module, "UnmapPhysicalMemory");

            const auto isDriverOpen = GetExport<xHCI::IsInpOutDriverOpen_t>(module, "IsInpOutDriverOpen");

            if (!mapPhysToLin || !unmapPhysicalMemory)
                error = "inpoutx64.dll does not export MapPhysToLin/UnmapPhysicalMemory.";
            else if (isDriverOpen && !isDriverOpen())
                error = "The inpoutx64.sys driver could not be started.";
            else
                return true;

            FreeLibrary(module);
            module = nullptr;
            return false;
        }

        PBYTE Map(uint64_t physicalAddress, DWORD size, HANDLE& handle) const
        {
            handle = nullptr;
            return mapPhysToLin(reinterpret_cast<PBYTE>(static_cast<uintptr_t>(physicalAddress)), size, &handle);
        }

        void Unmap(HANDLE handle, PBYTE linearAddress) const
        {
            if (linearAddress)
                unmapPhysicalMemory(handle, linearAddress);
        }

    private:
        HMODULE module = nullptr;
        xHCI::MapPhysToLin_t mapPhysToLin = nullptr;
        xHCI::UnmapPhysicalMemory_t unmapPhysicalMemory = nullptr;
    };

    // WinRing0 reads PCI configuration space, which gives the controller's real
    // BAR0 and memory-enable state. It is the primary way the controller's
    // registers are located. If it is not present or cannot start (Defender
    // often quarantines WinRing0x64.sys), the address Windows assigned is used
    // as a fallback.
    class WinRing0
    {
    public:
        WinRing0() = default;
        WinRing0(const WinRing0&) = delete;
        WinRing0& operator=(const WinRing0&) = delete;

        ~WinRing0()
        {
            if (initialized)
                deinitializeOls();

            if (module)
                FreeLibrary(module);
        }

        bool Load()
        {
            std::string error;
            module = LoadDriverLibrary(L"WinRing0x64.dll", error);

            if (!module)
                return false;

            const auto initializeOls = GetExport<xHCI::InitializeOls_t>(module, "InitializeOls");
            deinitializeOls = GetExport<xHCI::DeinitializeOls_t>(module, "DeinitializeOls");
            readPciConfigDwordEx = GetExport<xHCI::ReadPciConfigDwordEx_t>(module, "ReadPciConfigDwordEx");

            if (initializeOls && deinitializeOls && readPciConfigDwordEx && initializeOls())
            {
                initialized = true;
                return true;
            }

            FreeLibrary(module);
            module = nullptr;
            return false;
        }

        bool IsLoaded() const { return initialized; }

        // pciAddress is bus << 8 | device << 3 | function.
        bool ReadConfig(DWORD pciAddress, DWORD offset, DWORD& value) const
        {
            return initialized && readPciConfigDwordEx(pciAddress, offset, &value);
        }

    private:
        HMODULE module = nullptr;
        bool initialized = false;
        xHCI::DeinitializeOls_t deinitializeOls = nullptr;
        xHCI::ReadPciConfigDwordEx_t readPciConfigDwordEx = nullptr;
    };

    // Controllers

    // An xHCI controller as Windows PnP reports it.
    struct USBController
    {
        std::wstring caption;
        std::wstring deviceId;          // Device instance path, e.g. PCI\VEN_8086&DEV_9D2F&...
        DEVINST devInst = 0;            // Device tree node, used to list attached USB devices.

        bool hasLocation = false;
        DWORD bus = 0;
        DWORD device = 0;
        DWORD function = 0;

        uint64_t resourceAddress = 0;   // First memory range Windows assigned (BAR0).
    };

    std::vector<BYTE> GetDeviceProperty(HDEVINFO deviceInfo, SP_DEVINFO_DATA& deviceData, DWORD property, DWORD& type)
    {
        DWORD size = 0;
        SetupDiGetDeviceRegistryPropertyW(deviceInfo, &deviceData, property, &type, nullptr, 0, &size);

        if (size == 0)
            return {};

        // Two spare zeroed wchar_t so strings are always terminated.
        std::vector<BYTE> buffer(size + sizeof(wchar_t) * 2, 0);

        if (!SetupDiGetDeviceRegistryPropertyW(deviceInfo, &deviceData, property, &type, buffer.data(), size, nullptr))
            return {};

        return buffer;
    }

    std::wstring GetDeviceString(HDEVINFO deviceInfo, SP_DEVINFO_DATA& deviceData, DWORD property)
    {
        DWORD type = 0;
        const std::vector<BYTE> buffer = GetDeviceProperty(deviceInfo, deviceData, property, type);

        if (buffer.empty() || (type != REG_SZ && type != REG_MULTI_SZ))
            return L"";

        return reinterpret_cast<const wchar_t*>(buffer.data());
    }

    std::vector<std::wstring> GetDeviceStrings(HDEVINFO deviceInfo, SP_DEVINFO_DATA& deviceData, DWORD property)
    {
        DWORD type = 0;
        const std::vector<BYTE> buffer = GetDeviceProperty(deviceInfo, deviceData, property, type);
        std::vector<std::wstring> values;

        if (buffer.empty() || type != REG_MULTI_SZ)
            return values;

        for (auto entry = reinterpret_cast<const wchar_t*>(buffer.data()); *entry; entry += values.back().size() + 1)
            values.emplace_back(entry);

        return values;
    }

    bool GetDeviceDword(HDEVINFO deviceInfo, SP_DEVINFO_DATA& deviceData, DWORD property, DWORD& value)
    {
        DWORD type = 0;
        return SetupDiGetDeviceRegistryPropertyW(deviceInfo, &deviceData, property, &type,
            reinterpret_cast<PBYTE>(&value), sizeof(value), nullptr) && type == REG_DWORD;
    }

    bool IsXhciDevice(HDEVINFO deviceInfo, SP_DEVINFO_DATA& deviceData)
    {
        // PnP IDs carry the PCI class code, e.g. "PCI\CC_0C0330" in the compatible IDs.
        for (DWORD property : { SPDRP_HARDWAREID, SPDRP_COMPATIBLEIDS })
        {
            for (std::wstring id : GetDeviceStrings(deviceInfo, deviceData, property))
            {
                CharUpperBuffW(id.data(), static_cast<DWORD>(id.size()));

                if (id.find(L"CC_0C0330") != std::wstring::npos)
                    return true;
            }
        }

        return false;
    }

    // The first memory range in the allocated configuration is BAR0.
    uint64_t GetAllocatedMemoryAddress(DEVINST deviceInstance)
    {
        LOG_CONF logConf = 0;

        if (CM_Get_First_Log_Conf(&logConf, deviceInstance, ALLOC_LOG_CONF) != CR_SUCCESS)
            return 0;

        uint64_t address = 0;
        RES_DES descriptor = 0;
        RESOURCEID resourceId = 0;
        CONFIGRET result = CM_Get_Next_Res_Des(&descriptor, logConf, ResType_All, &resourceId, 0);

        while (result == CR_SUCCESS && address == 0)
        {
            ULONG size = 0;

            if ((resourceId == ResType_Mem || resourceId == ResType_MemLarge) &&
                CM_Get_Res_Des_Data_Size(&size, descriptor, 0) == CR_SUCCESS && size > 0)
            {
                std::vector<BYTE> data(size);

                if (CM_Get_Res_Des_Data(descriptor, data.data(), size, 0) == CR_SUCCESS)
                {
                    if (resourceId == ResType_Mem && size >= sizeof(MEM_DES))
                        address = reinterpret_cast<const MEM_DES*>(data.data())->MD_Alloc_Base;
                    else if (resourceId == ResType_MemLarge && size >= sizeof(MEM_LARGE_DES))
                        address = reinterpret_cast<const MEM_LARGE_DES*>(data.data())->MLD_Alloc_Base;
                }
            }

            RES_DES next = 0;

            if (address == 0)
                result = CM_Get_Next_Res_Des(&next, descriptor, ResType_All, &resourceId, 0);

            CM_Free_Res_Des_Handle(descriptor);
            descriptor = next;
        }

        CM_Free_Log_Conf_Handle(logConf);
        return address;
    }

    // Every present PCI device with class code 0C0330 (USB xHCI).
    std::vector<USBController> FindUSBControllers()
    {
        std::vector<USBController> controllers;
        HDEVINFO deviceInfo = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);

        if (deviceInfo == INVALID_HANDLE_VALUE)
            return controllers;

        SP_DEVINFO_DATA deviceData = {};
        deviceData.cbSize = sizeof(deviceData);

        for (DWORD index = 0; SetupDiEnumDeviceInfo(deviceInfo, index, &deviceData); index++)
        {
            if (!IsXhciDevice(deviceInfo, deviceData))
                continue;

            // Only controllers Windows is running. One that is disabled in Device
            // Manager, or whose driver failed to start, has nothing working on it.
            ULONG status = 0;
            ULONG problem = 0;

            if (CM_Get_DevNode_Status(&status, &problem, deviceData.DevInst, 0) != CR_SUCCESS || !(status & DN_STARTED))
                continue;

            wchar_t instanceId[MAX_DEVICE_ID_LEN] = {};

            if (!SetupDiGetDeviceInstanceIdW(deviceInfo, &deviceData, instanceId, MAX_DEVICE_ID_LEN, nullptr) || !instanceId[0])
                continue;

            USBController controller;
            controller.deviceId = instanceId;
            controller.devInst = deviceData.DevInst;
            controller.caption = GetDeviceString(deviceInfo, deviceData, SPDRP_FRIENDLYNAME);

            if (controller.caption.empty())
                controller.caption = GetDeviceString(deviceInfo, deviceData, SPDRP_DEVICEDESC);

            if (controller.caption.empty())
                controller.caption = L"USB xHCI Compliant Host Controller";


            DWORD bus = 0;
            DWORD address = 0;

            // For PCI devices SPDRP_ADDRESS is (device << 16) | function.
            if (GetDeviceDword(deviceInfo, deviceData, SPDRP_BUSNUMBER, bus) &&
                GetDeviceDword(deviceInfo, deviceData, SPDRP_ADDRESS, address))
            {
                controller.hasLocation = true;
                controller.bus = bus;
                controller.device = address >> 16;
                controller.function = address & 0xFFFF;
            }

            controller.resourceAddress = GetAllocatedMemoryAddress(deviceData.DevInst);
            controllers.push_back(controller);
        }

        SetupDiDestroyDeviceInfoList(deviceInfo);
        return controllers;
    }

    // Reads the 4 hex digits after a key such as "VEN_" in a device instance path.
    bool ParseIdField(const std::wstring& deviceId, const wchar_t* key, DWORD& value)
    {
        size_t position = deviceId.find(key);

        if (position == std::wstring::npos)
            return false;

        position += wcslen(key);

        if (position + 4 > deviceId.size())
            return false;

        value = 0;

        for (size_t i = position; i < position + 4; i++)
        {
            const wchar_t c = deviceId[i];
            DWORD digit = 0;

            if (c >= L'0' && c <= L'9')
                digit = c - L'0';
            else if (c >= L'A' && c <= L'F')
                digit = c - L'A' + 10;
            else if (c >= L'a' && c <= L'f')
                digit = c - L'a' + 10;
            else
                return false;

            value = (value << 4) | digit;
        }

        return true;
    }

    // Guards against the bus/device/function pointing at a different device,
    // e.g. on systems with more than one PCI segment.
    bool MatchesDeviceId(const std::wstring& deviceId, DWORD vendorDevice)
    {
        DWORD vendor = 0;
        DWORD device = 0;

        if (!ParseIdField(deviceId, L"VEN_", vendor) || !ParseIdField(deviceId, L"DEV_", device))
            return true;

        return (vendorDevice & 0xFFFF) == vendor && (vendorDevice >> 16) == device;
    }

    enum class AddressResult
    {
        Found,
        MemoryDisabled,
        NotFound
    };

    // Physical address of the controller's registers (BAR0). Prefers BAR0
    // straight from PCI config space; anything unexpected falls through to the
    // address Windows assigned.
    AddressResult GetControllerAddress(const WinRing0& winRing0, const USBController& controller, uint64_t& physicalAddress)
    {
        physicalAddress = 0;

        if (winRing0.IsLoaded() && controller.hasLocation &&
            controller.bus < 256 && controller.device < 32 && controller.function < 8)
        {
            const DWORD pciAddress = (controller.bus << 8) | (controller.device << 3) | controller.function;
            DWORD vendorDevice = 0xFFFFFFFF;
            DWORD classCode = 0;
            DWORD command = 0;
            DWORD bar0 = 0;

            if (winRing0.ReadConfig(pciAddress, xHCI::PCI_REG_VENDOR_DEVICE, vendorDevice) &&
                (vendorDevice & 0xFFFF) != 0xFFFF &&
                MatchesDeviceId(controller.deviceId, vendorDevice) &&
                winRing0.ReadConfig(pciAddress, xHCI::PCI_REG_CLASS_CODE, classCode) &&
                (classCode >> 8) == xHCI::PCI_CLASS_XHCI)
            {
                if (winRing0.ReadConfig(pciAddress, xHCI::PCI_REG_COMMAND, command) &&
                    !(command & xHCI::PCI_COMMAND_MEMORY_SPACE))
                {
                    return AddressResult::MemoryDisabled;
                }

                // Bit 0 set means an I/O BAR; bits 2:1 == 10b means a 64-bit BAR spanning BAR0 and BAR1.
                if (winRing0.ReadConfig(pciAddress, xHCI::PCI_REG_BAR0, bar0) && !(bar0 & 0x1))
                {
                    physicalAddress = bar0 & 0xFFFFFFF0u;

                    if (((bar0 >> 1) & 0x3) == 0x2)
                    {
                        DWORD bar1 = 0;

                        if (winRing0.ReadConfig(pciAddress, xHCI::PCI_REG_BAR1, bar1))
                            physicalAddress |= static_cast<uint64_t>(bar1) << 32;
                        else
                            physicalAddress = 0;
                    }

                    if (physicalAddress != 0)
                        return AddressResult::Found;
                }
            }
        }

        physicalAddress = controller.resourceAddress;
        return physicalAddress ? AddressResult::Found : AddressResult::NotFound;
    }

    struct MapStatus
    {
        enum class Kind
        {
            Mapped,
            Skipped,    // Not a controller this tool should touch.
            Failed
        };

        Kind kind = Kind::Failed;
        const char* message = "";
    };

    // One mapping from BAR0 through the last interrupter's IMOD register. BAR0
    // is page-aligned but RTSOFF is only 32-byte aligned, so the runtime
    // registers are reached through this window rather than mapped on their own.
    class ControllerRegisters
    {
    public:
        explicit ControllerRegisters(const InpOut& inpOut) : inpOut(inpOut) {}
        ControllerRegisters(const ControllerRegisters&) = delete;
        ControllerRegisters& operator=(const ControllerRegisters&) = delete;

        ~ControllerRegisters()
        {
            inpOut.Unmap(memoryHandle, linearAddress);
        }

        MapStatus Map(uint64_t address)
        {
            using Kind = MapStatus::Kind;

            inpOut.Unmap(memoryHandle, linearAddress);
            linearAddress = nullptr;
            maxInterrupters = 0;

            HANDLE capabilityHandle = nullptr;
            PBYTE capabilities = inpOut.Map(address, xHCI::CAPABILITY_WINDOW_SIZE, capabilityHandle);

            if (!capabilities)
                return { Kind::Failed, "Windows/InpOut could not map the controller's physical MMIO region." };

            const auto read = [capabilities](uint32_t offset) { return *reinterpret_cast<volatile uint32_t*>(capabilities + offset); };
            const uint32_t header = read(0x00);
            const uint32_t hcsparams1 = read(xHCI::HCSPARAMS1_OFFSET);
            const uint32_t rtsoff = read(xHCI::RTSOFF_OFFSET) & ~0x1Fu;
            inpOut.Unmap(capabilityHandle, capabilities);

            // All ones means nothing answered, e.g. the controller is powered down.
            if (header == 0xFFFFFFFF)
                return { Kind::Skipped, "Unable to access this xHCI's registers." };

            const uint32_t capLength = header & 0xFF;
            const uint32_t hciVersion = header >> 16;

            // Only xHCI 0.96 through 1.2 is touched.
            if (hciVersion < 0x0096 || hciVersion > 0x0120 || capLength < 0x20)
                return { Kind::Skipped, "Skipped, controller is not a supported xHCI controller." };

            const uint32_t interrupters = (hcsparams1 >> 8) & 0x7FF;

            if (interrupters == 0 || interrupters > xHCI::MAX_INTERRUPTERS)
                return { Kind::Failed, "The xHCI capability header was read, but it reported an invalid interrupter count." };

            if (rtsoff < capLength)
                return { Kind::Failed, "The xHCI capability header was read, but it reported an invalid runtime register offset." };

            uint64_t size = static_cast<uint64_t>(rtsoff) + xHCI::INTERRUPTER_OFFSET +
                static_cast<uint64_t>(interrupters) * xHCI::INTERRUPTER_SIZE;
            size = (size + 0xFFF) & ~0xFFFull;

            if (size > 0x01000000)
                return { Kind::Failed, "The calculated xHCI runtime register area is too large." };

            linearAddress = inpOut.Map(address, static_cast<DWORD>(size), memoryHandle);

            if (!linearAddress)
                return { Kind::Failed, "Windows/InpOut could not map the controller's runtime register area." };

            physicalAddress = address;
            runtimeOffset = rtsoff;
            maxInterrupters = interrupters;
            return { Kind::Mapped, "" };
        }

        uint32_t InterrupterCount() const { return maxInterrupters; }
        uint64_t ImodAddress(uint32_t interrupter) const { return physicalAddress + ImodOffset(interrupter); }
        uint32_t ReadImod(uint32_t interrupter) const { return *Register(interrupter); }

        // IMAN.IE, set on the interrupters Windows actually uses (usually just 0).
        bool InterruptsEnabled(uint32_t interrupter) const { return (*(Register(interrupter) - 1) & 0x2) != 0; }
        void WriteImod(uint32_t interrupter, uint32_t value) const { *Register(interrupter) = value; }

    private:
        uint32_t ImodOffset(uint32_t interrupter) const
        {
            return runtimeOffset + xHCI::INTERRUPTER_OFFSET + xHCI::INTERRUPTER_SIZE * interrupter + xHCI::IMOD_OFFSET;
        }

        volatile uint32_t* Register(uint32_t interrupter) const
        {
            return reinterpret_cast<volatile uint32_t*>(linearAddress + ImodOffset(interrupter));
        }

        const InpOut& inpOut;
        HANDLE memoryHandle = nullptr;
        PBYTE linearAddress = nullptr;
        uint64_t physicalAddress = 0;
        uint32_t runtimeOffset = 0;
        uint32_t maxInterrupters = 0;
    };

    // Controller details
    //
    // Where a controller sits relative to the CPU: inside the CPU, in the
    // chipset, or on an add-in chip. Windows doesn't report this, so it's looked
    // up by PCI vendor/device ID. The table is adapted from cpu-direct-usb by
    // Marius Heier (MIT, see THIRD-PARTY-NOTICES.txt), which built it from the
    // Linux kernel's xhci-pci.c and the pci.ids database.

    struct ControllerModel
    {
        WORD vendor;
        WORD device;
        const char* location;
        const char* platform;
    };

    constexpr const char* LOC_CPU = "CPU-direct";
    constexpr const char* LOC_TB = "Thunderbolt";
    constexpr const char* LOC_CHIPSET = "Chipset";
    constexpr const char* LOC_ADDIN = "Add-in card";

    constexpr ControllerModel CONTROLLER_MODELS[] =
    {
        // Intel, inside the CPU
        { 0x8086, 0x8A13, LOC_CPU, "Intel Ice Lake" },
        { 0x8086, 0x9A13, LOC_CPU, "Intel Tiger Lake-LP" },
        { 0x8086, 0x9A17, LOC_CPU, "Intel Tiger Lake-H" },
        { 0x8086, 0x461E, LOC_CPU, "Intel Alder Lake-P" },
        { 0x8086, 0x464E, LOC_CPU, "Intel Alder Lake-N" },
        { 0x8086, 0xA71E, LOC_CPU, "Intel Raptor Lake-P" },
        { 0x8086, 0x7EC0, LOC_CPU, "Intel Meteor Lake-P" },
        { 0x8086, 0xA831, LOC_CPU, "Intel Lunar Lake-M" },
        // Intel, chipset (PCH)
        { 0x8086, 0x7F6E, LOC_CHIPSET, "Intel 800 Series PCH" },
        { 0x8086, 0x7A60, LOC_CHIPSET, "Intel 700 Series PCH" },
        { 0x8086, 0x7A61, LOC_CHIPSET, "Intel 700 Series PCH" },
        { 0x8086, 0x7AE0, LOC_CHIPSET, "Intel 600 Series PCH (Desktop)" },
        { 0x8086, 0x51ED, LOC_CHIPSET, "Intel 600 Series PCH" },
        { 0x8086, 0x54ED, LOC_CHIPSET, "Intel Alder Lake-N PCH" },
        { 0x8086, 0x7E7D, LOC_CHIPSET, "Intel Meteor Lake PCH" },
        { 0x8086, 0x777D, LOC_CHIPSET, "Intel Arrow Lake" },
        { 0x8086, 0xA87D, LOC_CHIPSET, "Intel Lunar Lake PCH" },
        { 0x8086, 0xA0ED, LOC_CHIPSET, "Intel 500 Series PCH" },
        { 0x8086, 0x43ED, LOC_CHIPSET, "Intel 500 Series PCH-H" },
        { 0x8086, 0xA3AF, LOC_CHIPSET, "Intel 400 Series PCH" },
        { 0x8086, 0x02ED, LOC_CHIPSET, "Intel 400 Series PCH-LP" },
        { 0x8086, 0x06ED, LOC_CHIPSET, "Intel 400 Series PCH" },
        { 0x8086, 0xA36D, LOC_CHIPSET, "Intel 300 Series PCH" },
        { 0x8086, 0x9DED, LOC_CHIPSET, "Intel 300 Series PCH-LP" },
        { 0x8086, 0xA2AF, LOC_CHIPSET, "Intel 200 Series PCH" },
        { 0x8086, 0xA12F, LOC_CHIPSET, "Intel 100 Series PCH" },
        { 0x8086, 0x9D2F, LOC_CHIPSET, "Intel 100 Series PCH-LP" },
        { 0x8086, 0x8CB1, LOC_CHIPSET, "Intel 9 Series PCH" },
        { 0x8086, 0x9CB1, LOC_CHIPSET, "Intel 9 Series PCH-LP" },
        { 0x8086, 0x8C31, LOC_CHIPSET, "Intel 8 Series PCH" },
        { 0x8086, 0x9C31, LOC_CHIPSET, "Intel 8 Series PCH-LP" },
        { 0x8086, 0x1E31, LOC_CHIPSET, "Intel 7 Series PCH" },
        { 0x8086, 0x8D31, LOC_CHIPSET, "Intel X99/C610" },
        { 0x8086, 0xA1AF, LOC_CHIPSET, "Intel C620" },
        // Intel Thunderbolt
        { 0x8086, 0x5782, LOC_TB, "Intel Barlow Ridge Host 80G" },
        { 0x8086, 0x5785, LOC_TB, "Intel Barlow Ridge Host 40G" },
        { 0x8086, 0x5787, LOC_TB, "Intel Barlow Ridge Hub 80G" },
        { 0x8086, 0x57A5, LOC_TB, "Intel Barlow Ridge Hub 40G" },
        { 0x8086, 0x1138, LOC_TB, "Intel Maple Ridge 4C" },
        { 0x8086, 0x1135, LOC_TB, "Intel Maple Ridge 2C" },
        { 0x8086, 0x0B27, LOC_TB, "Intel Goshen Ridge" },
        { 0x8086, 0x15E9, LOC_TB, "Intel Titan Ridge 2C" },
        { 0x8086, 0x15EC, LOC_TB, "Intel Titan Ridge 4C" },
        { 0x8086, 0x15F0, LOC_TB, "Intel Titan Ridge DD" },
        { 0x8086, 0x15B5, LOC_TB, "Intel Alpine Ridge 2C" },
        { 0x8086, 0x15B6, LOC_TB, "Intel Alpine Ridge 4C" },
        { 0x8086, 0x15C1, LOC_TB, "Intel Alpine Ridge LP" },
        { 0x8086, 0x15D4, LOC_TB, "Intel Alpine Ridge 4C" },
        { 0x8086, 0x15DB, LOC_TB, "Intel Alpine Ridge 2C" },
        // AMD, inside the CPU
        { 0x1022, 0x15B6, LOC_CPU, "AMD Ryzen 7000/9000 (AM5)" },
        { 0x1022, 0x15B7, LOC_CPU, "AMD Ryzen 7000/9000 (AM5)" },
        { 0x1022, 0x15B8, LOC_CPU, "AMD Ryzen 7000/9000 (AM5)" },
        { 0x1022, 0x1587, LOC_CPU, "AMD Strix Halo" },
        { 0x1022, 0x1588, LOC_CPU, "AMD Strix Halo" },
        { 0x1022, 0x1589, LOC_CPU, "AMD Strix Halo" },
        { 0x1022, 0x158B, LOC_CPU, "AMD Strix Halo" },
        { 0x1022, 0x158D, LOC_CPU, "AMD Strix Halo" },
        { 0x1022, 0x158E, LOC_CPU, "AMD Strix Halo" },
        { 0x1022, 0x161A, LOC_CPU, "AMD Ryzen 6000 Mobile" },
        { 0x1022, 0x161B, LOC_CPU, "AMD Ryzen 6000 Mobile" },
        { 0x1022, 0x161C, LOC_CPU, "AMD Ryzen 6000 Mobile" },
        { 0x1022, 0x161D, LOC_CPU, "AMD Ryzen 6000 Mobile" },
        { 0x1022, 0x161E, LOC_CPU, "AMD Ryzen 6000 Mobile" },
        { 0x1022, 0x161F, LOC_CPU, "AMD Ryzen 6000 Mobile" },
        { 0x1022, 0x15D6, LOC_CPU, "AMD Ryzen 6000 Mobile" },
        { 0x1022, 0x15D7, LOC_CPU, "AMD Ryzen 6000 Mobile" },
        { 0x1022, 0x162E, LOC_CPU, "AMD Ryzen 6000 Mobile" },
        { 0x1022, 0x162F, LOC_CPU, "AMD Ryzen 6000 Mobile" },
        { 0x1022, 0x15C4, LOC_CPU, "AMD Ryzen 7040 Mobile" },
        { 0x1022, 0x15C5, LOC_CPU, "AMD Ryzen 7040 Mobile" },
        { 0x1022, 0x1668, LOC_CPU, "AMD Pink Sardine" },
        { 0x1022, 0x1669, LOC_CPU, "AMD Pink Sardine" },
        { 0x1022, 0x1639, LOC_CPU, "AMD Ryzen 4000/5000 APU" },
        { 0x1022, 0x15E0, LOC_CPU, "AMD Ryzen 2000 APU" },
        { 0x1022, 0x15E1, LOC_CPU, "AMD Ryzen 2000 APU" },
        { 0x1022, 0x15E5, LOC_CPU, "AMD Ryzen 3000 APU" },
        { 0x1022, 0x149C, LOC_CPU, "AMD Ryzen 3000/5000 (AM4)" },
        { 0x1022, 0x148C, LOC_CPU, "AMD EPYC Rome / Threadripper 3000" },
        { 0x1022, 0x145F, LOC_CPU, "AMD Ryzen 1000" },
        { 0x1022, 0x145C, LOC_CPU, "AMD Ryzen 1000" },
        { 0x1022, 0x162C, LOC_CPU, "AMD Steam Deck (Van Gogh)" },
        { 0x1022, 0x163A, LOC_CPU, "AMD Steam Deck (Van Gogh)" },
        { 0x1022, 0x163B, LOC_CPU, "AMD Steam Deck (Van Gogh)" },
        { 0x1022, 0x15D4, LOC_CPU, "AMD FireFlight" },
        { 0x1022, 0x15D5, LOC_CPU, "AMD FireFlight" },
        { 0x1022, 0x13ED, LOC_CPU, "AMD Ariel" },
        { 0x1022, 0x13EE, LOC_CPU, "AMD Ariel" },
        { 0x1022, 0x1557, LOC_CPU, "AMD EPYC Turin" },
        // AMD, chipset
        { 0x1022, 0x43FC, LOC_CHIPSET, "AMD X870/B850 (AM5)" },
        { 0x1022, 0x43FD, LOC_CHIPSET, "AMD X870/B850 (AM5)" },
        { 0x1022, 0x43F7, LOC_CHIPSET, "AMD X670/B650 (AM5)" },
        { 0x1022, 0x43EE, LOC_CHIPSET, "AMD X570/B550 (AM4)" },
        { 0x1022, 0x43EC, LOC_CHIPSET, "AMD A520 (AM4)" },
        { 0x1022, 0x43D5, LOC_CHIPSET, "AMD X470/B450 (AM4)" },
        { 0x1022, 0x43B9, LOC_CHIPSET, "AMD X370 (AM4)" },
        { 0x1022, 0x43BA, LOC_CHIPSET, "AMD X399 (Threadripper)" },
        { 0x1022, 0x43BB, LOC_CHIPSET, "AMD B350 (AM4)" },
        { 0x1022, 0x43BC, LOC_CHIPSET, "AMD A320 (AM4)" },
        { 0x1022, 0x7814, LOC_CHIPSET, "AMD FCH" },
        { 0x1022, 0x7812, LOC_CHIPSET, "AMD FCH" },
    };

    struct AddInVendor
    {
        WORD vendor;
        const char* name;
    };

    constexpr AddInVendor ADDIN_VENDORS[] =
    {
        { 0x1B21, "ASMedia" },
        { 0x1106, "VIA" },
        { 0x1B73, "Fresco Logic" },
        { 0x1912, "Renesas" },
        { 0x1B6F, "Etron" },
        { 0x104C, "Texas Instruments" },
    };

    // "Chipset -> Intel 100 Series PCH-LP", "Add-in card -> ASMedia" or "Unknown".
    std::string ControllerLocation(DWORD vendor, DWORD device)
    {
        const std::string arrow = std::string(" ") + Glyph::ARROW + " ";

        for (const ControllerModel& model : CONTROLLER_MODELS)
        {
            if (model.vendor == vendor && model.device == device)
                return model.location + arrow + model.platform;
        }

        for (const AddInVendor& addIn : ADDIN_VENDORS)
        {
            if (addIn.vendor == vendor)
                return LOC_ADDIN + arrow + addIn.name;
        }

        return "Unknown";
    }

    // How many chips sit between the controller and the CPU, counted the same
    // way as cpu-direct-usb: 0 for CPU-direct and Thunderbolt, 1 for the
    // chipset or an add-in card, -1 when the controller is not in the table.
    int ChipsToCpu(DWORD vendor, DWORD device)
    {
        for (const ControllerModel& model : CONTROLLER_MODELS)
        {
            if (model.vendor == vendor && model.device == device)
                return model.location == LOC_CPU || model.location == LOC_TB ? 0 : 1;
        }

        for (const AddInVendor& addIn : ADDIN_VENDORS)
        {
            if (addIn.vendor == vendor)
                return 1;
        }

        return -1;
    }

    // The interrupt mode Windows is set to use for the controller, from the same
    // registry value Device Manager tools edit: "MSI", "Line-based" or "".
    std::string InterruptMode(const USBController& controller)
    {
        const std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\" + controller.deviceId +
            L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";
        DWORD value = 0;
        DWORD size = sizeof(value);

        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"MSISupported", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
            return "";

        return value ? "MSI" : "Line-based";
    }

    // "Disabled (0x0)", "0xC8 (50 us)" or "Unknown", from the interrupters
    // Windows has enabled (usually just interrupter 0).
    std::string CurrentImod(const InpOut& inpOut, const WinRing0& winRing0, const USBController& controller)
    {
        uint64_t physicalAddress = 0;

        if (GetControllerAddress(winRing0, controller, physicalAddress) != AddressResult::Found)
            return "Unknown";

        ControllerRegisters registers(inpOut);

        if (registers.Map(physicalAddress).kind != MapStatus::Kind::Mapped)
            return "Unknown";

        std::set<uint32_t> intervals;

        for (uint32_t i = 0; i < registers.InterrupterCount(); i++)
        {
            if (registers.InterruptsEnabled(i))
                intervals.insert(registers.ReadImod(i) & xHCI::IMOD_INTERVAL_MASK);
        }

        if (intervals.empty())
            intervals.insert(registers.ReadImod(0) & xHCI::IMOD_INTERVAL_MASK);

        if (intervals.size() > 1)
            return "Mixed across interrupters";

        const uint32_t interval = *intervals.begin();

        if (interval == 0)
            return "Disabled (0x0)";

        // IMODI counts in 250 ns steps.
        std::ostringstream text;
        text << HexString(interval) << " (" << interval * 0.25 << " us)";
        return text.str();
    }

    // USB devices
    //
    // Walks the device tree below a controller: root hub, external hubs, then
    // each physical USB device with the classes of everything below it (a gaming
    // mouse is often "Mouse, Keyboard" because of its macro interface).

    struct UsbDevice
    {
        std::wstring name;
        std::string types;              // e.g. "Mouse, Keyboard"
        int port = 0;                   // 0 when Windows doesn't report it.
        std::vector<std::wstring> hubs; // External hubs between the device and the root port.
    };

    std::wstring GetNodeString(DEVINST deviceInstance, const DEVPROPKEY& key)
    {
        DEVPROPTYPE type = 0;
        ULONG size = 0;
        CM_Get_DevNode_PropertyW(deviceInstance, &key, &type, nullptr, &size, 0);

        if (size == 0)
            return L"";

        std::vector<BYTE> buffer(size + sizeof(wchar_t), 0);

        if (CM_Get_DevNode_PropertyW(deviceInstance, &key, &type, buffer.data(), &size, 0) != CR_SUCCESS ||
            type != DEVPROP_TYPE_STRING)
        {
            return L"";
        }

        return reinterpret_cast<const wchar_t*>(buffer.data());
    }

    std::wstring NodeName(DEVINST deviceInstance)
    {
        std::wstring name = GetNodeString(deviceInstance, DEVPKEY_Device_BusReportedDeviceDesc);

        if (name.empty())
            name = GetNodeString(deviceInstance, DEVPKEY_Device_FriendlyName);

        if (name.empty())
            name = GetNodeString(deviceInstance, DEVPKEY_Device_DeviceDesc);

        return name.empty() ? L"Unknown device" : name;
    }

    std::vector<DEVINST> NodeChildren(DEVINST deviceInstance)
    {
        std::vector<DEVINST> children;
        DEVINST child = 0;

        if (CM_Get_Child(&child, deviceInstance, 0) != CR_SUCCESS)
            return children;

        do
        {
            // A plugged-in device is either running or flagged with a problem
            // (e.g. its driver is disabled). A removed device can linger in the
            // tree as neither until Windows rescans; it is skipped.
            ULONG status = 0;
            ULONG problem = 0;

            if (CM_Get_DevNode_Status(&status, &problem, child, 0) == CR_SUCCESS && (status & (DN_STARTED | DN_HAS_PROBLEM)) != 0)
                children.push_back(child);
        } while (CM_Get_Sibling(&child, child, 0) == CR_SUCCESS);

        return children;
    }

    bool IsUsbHub(DEVINST deviceInstance)
    {
        const std::wstring service = GetNodeString(deviceInstance, DEVPKEY_Device_Service);
        return _wcsicmp(service.c_str(), L"USBHUB3") == 0 || _wcsicmp(service.c_str(), L"usbhub") == 0;
    }

    // Maps a device class to a short type, or nullptr for classes not worth naming.
    const char* DeviceType(const std::wstring& deviceClass)
    {
        struct ClassType
        {
            const wchar_t* deviceClass;
            const char* type;
        };

        static const ClassType types[] =
        {
            { L"Mouse", "Mouse" },
            { L"Keyboard", "Keyboard" },
            { L"MEDIA", "Audio" },
            { L"AudioEndpoint", "Audio" },
            { L"DiskDrive", "Storage" },
            { L"Camera", "Camera" },
            { L"Image", "Camera" },
            { L"Bluetooth", "Bluetooth" },
            { L"Net", "Network" },
            { L"XnaComposite", "Game controller" },
            { L"XboxComposite", "Game controller" },
            { L"XUSBClass", "Game controller" },
            { L"Printer", "Printer" },
            { L"HIDClass", "HID" },
        };

        for (const ClassType& entry : types)
        {
            if (_wcsicmp(deviceClass.c_str(), entry.deviceClass) == 0)
                return entry.type;
        }

        return nullptr;
    }

    void CollectTypes(DEVINST deviceInstance, std::set<std::string>& types)
    {
        if (const char* type = DeviceType(GetNodeString(deviceInstance, DEVPKEY_Device_Class)))
            types.insert(type);

        for (DEVINST child : NodeChildren(deviceInstance))
            CollectTypes(child, types);
    }

    std::string TypeList(DEVINST deviceInstance)
    {
        std::set<std::string> types;
        CollectTypes(deviceInstance, types);

        // "HID" only when nothing more specific was found.
        if (types.size() > 1)
            types.erase("HID");

        if (types.empty())
            return "USB device";

        std::string list;

        for (const std::string& type : types)
            list += (list.empty() ? "" : ", ") + type;

        return list;
    }

    // "Port_#0002.Hub_#0001" -> 2
    int PortNumber(DEVINST deviceInstance)
    {
        const std::wstring location = GetNodeString(deviceInstance, DEVPKEY_Device_LocationInfo);
        const size_t position = location.find(L"Port_#");
        return position == std::wstring::npos ? 0 : _wtoi(location.c_str() + position + 6);
    }

    void CollectUsbDevices(DEVINST hub, std::vector<std::wstring>& hubChain, std::vector<UsbDevice>& devices)
    {
        for (DEVINST child : NodeChildren(hub))
        {
            if (IsUsbHub(child))
            {
                hubChain.push_back(NodeName(child));
                CollectUsbDevices(child, hubChain, devices);
                hubChain.pop_back();
                continue;
            }

            UsbDevice device;
            device.name = NodeName(child);
            device.types = TypeList(child);
            device.port = PortNumber(child);
            device.hubs = hubChain;
            devices.push_back(device);
        }
    }

    // Every USB device attached to a controller, through any number of hubs.
    std::vector<UsbDevice> FindUsbDevices(const USBController& controller)
    {
        std::vector<UsbDevice> devices;
        std::vector<std::wstring> hubChain;

        for (DEVINST rootHub : NodeChildren(controller.devInst))
            CollectUsbDevices(rootHub, hubChain, devices);

        return devices;
    }

    // Backup file
    //
    // The backup is small, fixed-layout JSON written by this tool:
    //
    //   {
    //     "version": 1,
    //     "controllers": [
    //       {
    //         "device_path": "PCI\\VEN_8086&DEV_9D2F&...",
    //         "registers": [
    //           { "address": "0xE1132024", "value": "0xC8" },
    //           ...
    //
    // The reader only understands that layout (one key per line) and rejects
    // anything it cannot parse completely, so a damaged file is never restored.

    struct BackupRegister
    {
        uint64_t address = 0;   // Informational; restore matches registers by index.
        uint32_t value = 0;
    };

    struct BackupController
    {
        std::wstring deviceId;
        std::vector<BackupRegister> registers;
    };

    const std::string DEVICE_KEY = "\"device_path\": \"";
    const std::string ADDRESS_KEY = "\"address\": \"";
    const std::string VALUE_KEY = "\"value\": \"";

    // Device instance paths are ASCII; anything else is dropped.
    std::string JsonEscape(const std::wstring& value)
    {
        std::string result;

        for (wchar_t c : value)
        {
            if (c == L'\\' || c == L'"')
                result += '\\';

            if (c >= 0x20 && c <= 0x7E)
                result += static_cast<char>(c);
        }

        return result;
    }

    // Reads the quoted string that starts at position, undoing JSON escapes.
    bool ReadJsonString(const std::string& line, size_t position, std::wstring& value)
    {
        value.clear();

        for (size_t i = position; i < line.size(); i++)
        {
            if (line[i] == '"')
                return true;

            if (line[i] == '\\' && i + 1 < line.size())
                i++;

            value += static_cast<wchar_t>(static_cast<unsigned char>(line[i]));
        }

        return false;
    }

    // Parses the whole field as a number (e.g. "0xFA0"), or fails.
    bool ParseNumber(const std::string& line, size_t position, uint64_t& value)
    {
        const size_t end = line.find('"', position);

        if (end == std::string::npos || end == position)
            return false;

        const std::string text = line.substr(position, end - position);

        // std::stoull would also accept leading spaces and a minus sign.
        if (text[0] < '0' || text[0] > '9')
            return false;

        try
        {
            size_t parsed = 0;
            value = std::stoull(text, &parsed, 0);
            return parsed == text.size();
        }
        catch (...)
        {
            return false;
        }
    }

    // "xHCI IMOD Backup.json" beside the executable.
    bool GetBackupPath(std::filesystem::path& backupPath)
    {
        std::filesystem::path directory;

        if (!GetExecutableDirectory(directory))
            return false;

        backupPath = directory / L"xHCI IMOD Backup.json";
        return true;
    }

    bool WriteBackupFile(const std::filesystem::path& backupPath, const std::vector<BackupController>& backups)
    {
        // Write to a temporary file and swap it in, so an existing backup is
        // never left half-written.
        std::filesystem::path temporaryPath = backupPath;
        temporaryPath += L".tmp";

        {
            std::ofstream file(temporaryPath, std::ios::out | std::ios::trunc | std::ios::binary);

            if (!file)
                return false;

            file << "{\n  \"version\": 1,\n  \"controllers\": [\n";

            for (size_t c = 0; c < backups.size(); c++)
            {
                file << "    {\n      \"device_path\": \"" << JsonEscape(backups[c].deviceId) << "\",\n"
                     << "      \"registers\": [\n";

                const auto& registers = backups[c].registers;

                for (size_t r = 0; r < registers.size(); r++)
                {
                    file << "        { \"address\": \"" << HexString(registers[r].address)
                         << "\", \"value\": \"" << HexString(registers[r].value) << "\" }"
                         << (r + 1 < registers.size() ? ",\n" : "\n");
                }

                file << "      ]\n    }" << (c + 1 < backups.size() ? ",\n" : "\n");
            }

            file << "  ]\n}\n";
            file.flush();

            if (!file.good())
            {
                file.close();
                DeleteFileW(temporaryPath.c_str());
                return false;
            }
        }

        if (!MoveFileExW(temporaryPath.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(temporaryPath.c_str());
            return false;
        }

        return true;
    }

    bool ReadBackupFile(const std::filesystem::path& backupPath, std::vector<BackupController>& backups)
    {
        backups.clear();

        std::ifstream file(backupPath, std::ios::in | std::ios::binary);

        if (!file)
            return false;

        std::string line;

        while (std::getline(file, line))
        {
            const size_t devicePosition = line.find(DEVICE_KEY);

            if (devicePosition != std::string::npos)
            {
                BackupController controller;

                if (!ReadJsonString(line, devicePosition + DEVICE_KEY.size(), controller.deviceId) ||
                    controller.deviceId.empty())
                {
                    return false;
                }

                backups.push_back(std::move(controller));
                continue;
            }

            const size_t addressPosition = line.find(ADDRESS_KEY);
            const size_t valuePosition = line.find(VALUE_KEY);

            if (addressPosition == std::string::npos || valuePosition == std::string::npos)
                continue;

            uint64_t address = 0;
            uint64_t value = 0;

            if (backups.empty() ||
                !ParseNumber(line, addressPosition + ADDRESS_KEY.size(), address) ||
                !ParseNumber(line, valuePosition + VALUE_KEY.size(), value) ||
                value > 0xFFFFFFFF)
            {
                return false;
            }

            backups.back().registers.push_back({ address, static_cast<uint32_t>(value) });
        }

        if (file.bad() || backups.empty())
            return false;

        for (const auto& controller : backups)
        {
            if (controller.registers.empty())
                return false;
        }

        return true;
    }

    // Install location

    // Used for the task name, the Run value name and the install folder.
    const wchar_t* const STARTUP_NAME = L"xHCI IMOD Disabler";
    const wchar_t* const INSTALLED_EXECUTABLE_NAME = L"xHCI IMOD Disabler.exe";

    // C:\xHCI IMOD Disabler, on whichever drive Windows is installed.
    std::filesystem::path GetInstallDirectory()
    {
        wchar_t windowsDirectory[MAX_PATH] = {};
        const UINT length = GetSystemWindowsDirectoryW(windowsDirectory, MAX_PATH);
        std::wstring root = L"C:\\";

        if (length >= 3 && length < MAX_PATH && windowsDirectory[1] == L':')
            root.assign(windowsDirectory, 3);

        return std::filesystem::path(root) / STARTUP_NAME;
    }

    // Controller selection
    //
    // Controllers the user unticks are saved by device instance path, one
    // "skip=" line each, in SELECTION_FILE beside the executable. Anything not
    // listed is ticked, so a newly added controller gets IMOD disabled by
    // default. Startup runs read the copy in the install folder, which saving
    // keeps in sync.

    const wchar_t* const SELECTION_FILE = L"Controller Selection.txt";
    const std::string SKIP_PREFIX = "skip=";

    std::wstring FromUtf8(const std::string& value)
    {
        if (value.empty())
            return L"";

        const int byteLength = static_cast<int>(value.size());
        const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), byteLength, nullptr, 0);

        if (length <= 0)
            return L"";

        std::wstring result(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), byteLength, result.data(), length);
        return result;
    }

    // Device instance paths are case-insensitive, so they're compared upper-cased.
    std::wstring UpperCase(std::wstring value)
    {
        CharUpperBuffW(value.data(), static_cast<DWORD>(value.size()));
        return value;
    }

    bool IsSelected(const std::set<std::wstring>& skipped, const USBController& controller)
    {
        return skipped.count(UpperCase(controller.deviceId)) == 0;
    }

    std::set<std::wstring> LoadSkippedControllers()
    {
        std::set<std::wstring> skipped;
        std::filesystem::path directory;

        if (!GetExecutableDirectory(directory))
            return skipped;

        std::ifstream file(directory / SELECTION_FILE);
        std::string line;

        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.rfind(SKIP_PREFIX, 0) == 0)
                skipped.insert(UpperCase(FromUtf8(line.substr(SKIP_PREFIX.size()))));
        }

        return skipped;
    }

    bool WriteSelectionFile(const std::filesystem::path& path, const std::set<std::wstring>& skipped)
    {
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        file << "# xHCI IMOD Disabler controller selection.\n"
                "# IMOD is disabled on every controller except the ones listed below.\n";

        for (const std::wstring& deviceId : skipped)
            file << SKIP_PREFIX << ToUtf8(deviceId) << "\n";

        return file.good();
    }

    // Saves beside the executable, and to the installed copy so startup runs follow it.
    bool SaveSkippedControllers(const std::set<std::wstring>& skipped)
    {
        std::filesystem::path directory;

        if (!GetExecutableDirectory(directory))
            return false;

        bool saved = WriteSelectionFile(directory / SELECTION_FILE, skipped);

        const std::filesystem::path installDirectory = GetInstallDirectory();
        std::error_code error;

        if (std::filesystem::is_directory(installDirectory, error) &&
            !std::filesystem::equivalent(directory, installDirectory, error))
        {
            saved = WriteSelectionFile(installDirectory / SELECTION_FILE, skipped) && saved;
        }

        return saved;
    }

    // IMOD actions

    enum class ImodAction
    {
        Backup,     // Save every IMOD register to the backup file.
        Restore,    // Write the saved values back.
        Test,       // Set the 62.5 Hz test interval.
        Disable     // Set the interval to 0.
    };

    enum class ControllerResult
    {
        Changed,
        AlreadySet,
        Skipped,
        Failed
    };

    // Writes each interrupter's target interval and reads it back to verify.
    // Registers that already hold the target are left alone.
    ControllerResult WriteImodValues(const ControllerRegisters& registers,
        const std::vector<uint32_t>& targets, const char* alreadySetLabel)
    {
        bool anyChanged = false;
        bool anyFailed = false;

        for (uint32_t i = 0; i < registers.InterrupterCount(); i++)
        {
            const uint32_t target = targets[i] & xHCI::IMOD_INTERVAL_MASK;
            const uint32_t before = registers.ReadImod(i);

            if ((before & xHCI::IMOD_INTERVAL_MASK) == target)
            {
                std::cout << "    Register " << i + 1 << ": [" << alreadySetLabel << "]\n";
                continue;
            }

            registers.WriteImod(i, target);

            const uint32_t after = registers.ReadImod(i);
            const bool succeeded = (after & xHCI::IMOD_INTERVAL_MASK) == target;

            std::cout << "    Register " << i + 1 << ": " << HexString(registers.ImodAddress(i))
                      << " = " << HexString(before) << " " << Glyph::ARROW << " " << HexString(after)
                      << " [" << (succeeded ? Glyph::CHECK : Glyph::CROSS) << "  ]\n";

            if (succeeded)
                anyChanged = true;
            else
                anyFailed = true;
        }

        if (anyFailed)
            return ControllerResult::Failed;

        return anyChanged ? ControllerResult::Changed : ControllerResult::AlreadySet;
    }

    ControllerResult BackupValues(const ControllerRegisters& registers, const USBController& controller,
        std::vector<BackupController>& backups)
    {
        BackupController backup;
        backup.deviceId = controller.deviceId;

        std::cout << "  Current IMOD values:\n";

        for (uint32_t i = 0; i < registers.InterrupterCount(); i++)
        {
            const BackupRegister saved = { registers.ImodAddress(i), registers.ReadImod(i) & xHCI::IMOD_INTERVAL_MASK };
            backup.registers.push_back(saved);

            std::cout << "    Register " << i + 1 << ": " << HexString(saved.address)
                      << " = " << HexString(saved.value) << " [" << Glyph::CHECK << "  ]\n";
        }

        backups.push_back(std::move(backup));
        return ControllerResult::Changed;
    }

    const BackupController* FindBackup(const std::vector<BackupController>& savedBackups, const USBController& controller)
    {
        for (const auto& backup : savedBackups)
        {
            if (_wcsicmp(backup.deviceId.c_str(), controller.deviceId.c_str()) == 0)
                return &backup;
        }

        return nullptr;
    }

    ControllerResult RestoreValues(const ControllerRegisters& registers, const BackupController& backup)
    {
        if (backup.registers.size() != registers.InterrupterCount())
        {
            PrintControllerError("The backup does not contain the expected number of IMOD registers for this controller.");
            return ControllerResult::Failed;
        }

        std::vector<uint32_t> targets;

        for (const auto& saved : backup.registers)
            targets.push_back(saved.value);

        std::cout << "  Restoring saved IMOD values:\n";
        return WriteImodValues(registers, targets, "Already Restored");
    }

    // selected is false for controllers unticked in Controller Selection; Test
    // and Disable leave those alone, Backup and Restore cover every controller.
    ControllerResult ProcessController(ImodAction action, const InpOut& inpOut, const WinRing0& winRing0,
        const USBController& controller, bool selected, const std::vector<BackupController>& savedBackups,
        std::vector<BackupController>& backups)
    {
        std::cout << "\n" << ToUtf8(controller.caption) << "\n"
                  << "  Device Path: " << ToUtf8(controller.deviceId) << "\n";

        if (!selected && (action == ImodAction::Test || action == ImodAction::Disable))
        {
            PrintControllerError("Skipped, not selected in Controller Selection.");
            return ControllerResult::Skipped;
        }


        uint64_t physicalAddress = 0;

        switch (GetControllerAddress(winRing0, controller, physicalAddress))
        {
        case AddressResult::MemoryDisabled:
            PrintControllerError("Skipped, this controller's memory space is disabled.");
            return ControllerResult::Skipped;

        case AddressResult::NotFound:
            PrintControllerError("Windows PnP did not provide a physical MMIO resource address for this controller.");
            return ControllerResult::Failed;

        case AddressResult::Found:
            break;
        }

        const BackupController* backup = nullptr;

        if (action == ImodAction::Restore)
        {
            backup = FindBackup(savedBackups, controller);

            if (!backup)
            {
                PrintControllerError("No saved IMOD values were found for this controller.");
                return ControllerResult::Skipped;
            }
        }

        ControllerRegisters registers(inpOut);
        const MapStatus status = registers.Map(physicalAddress);

        if (status.kind != MapStatus::Kind::Mapped)
        {
            PrintControllerError(status.message);
            return status.kind == MapStatus::Kind::Skipped ? ControllerResult::Skipped : ControllerResult::Failed;
        }

        switch (action)
        {
        case ImodAction::Backup:
            return BackupValues(registers, controller, backups);

        case ImodAction::Restore:
            return backup ? RestoreValues(registers, *backup) : ControllerResult::Failed;

        case ImodAction::Test:
            std::cout << "  IMOD interval: " << HexString(xHCI::TEST_IMOD_INTERVAL) << " (62.5Hz)\n";
            return WriteImodValues(registers,
                std::vector<uint32_t>(registers.InterrupterCount(), xHCI::TEST_IMOD_INTERVAL), "Already Set");

        case ImodAction::Disable:
        default:
            std::cout << "  IMOD interval: " << HexString(xHCI::DESIRED_IMOD_INTERVAL) << "\n";
            return WriteImodValues(registers,
                std::vector<uint32_t>(registers.InterrupterCount(), xHCI::DESIRED_IMOD_INTERVAL), "Already Disabled");
        }
    }

    // Saves the collected values, unless they are all 0 and a backup already
    // exists: after IMOD has been disabled, re-running Backup would otherwise
    // replace the Windows defaults with zeros and Restore could never undo it.
    bool SaveBackups(const std::filesystem::path& backupPath, const std::vector<BackupController>& backups)
    {
        if (backups.empty())
        {
            std::cout << "\n" << Glyph::CROSS << "  : No IMOD values could be backed up.\n";
            return false;
        }

        bool allDisabled = true;

        for (const auto& backup : backups)
        {
            for (const auto& saved : backup.registers)
                allDisabled = allDisabled && saved.value == xHCI::DESIRED_IMOD_INTERVAL;
        }

        const std::string path = ToUtf8(backupPath.wstring());
        std::error_code error;

        if (allDisabled && std::filesystem::exists(backupPath, error))
        {
            std::cout << "\n" << Glyph::CROSS << "  : IMOD is already disabled, so the existing backup was kept: " << path << "\n";
            return true;
        }

        if (!WriteBackupFile(backupPath, backups))
        {
            std::cout << "\n" << Glyph::CROSS << "  : Failed to save the IMOD backup file: " << path << "\n";
            return false;
        }

        std::cout << "\n" << Glyph::CHECK << "  : Backup saved to " << path << "\n";

        if (allDisabled)
            std::cout << Glyph::CROSS << "  : IMOD was already disabled, so this backup holds the disabled values.\n";

        return true;
    }

    // Runs one action on every xHCI controller (Test and Disable only on the
    // selected ones) and prints the result of each register. Returns true if at
    // least one controller succeeded and none failed.
    bool RunImodAction(ImodAction action)
    {
        std::filesystem::path backupPath;

        if (!GetBackupPath(backupPath))
        {
            std::cout << "\n" << Glyph::CROSS << "  : Could not determine the executable path.\n";
            return false;
        }

        std::vector<BackupController> savedBackups;

        if (action == ImodAction::Restore && !ReadBackupFile(backupPath, savedBackups))
        {
            std::cout << "\n" << Glyph::CROSS << "  : Could not read the IMOD backup file: " << ToUtf8(backupPath.wstring()) << "\n";
            return false;
        }

        const std::vector<USBController> controllers = FindUSBControllers();

        if (controllers.empty())
        {
            std::cout << "\n" << Glyph::CROSS << "  : Windows did not report any xHCI USB controllers.\n";
            return false;
        }

        InpOut inpOut;
        std::string inpOutError;

        if (!inpOut.Load(inpOutError))
        {
            std::cout << "\n" << Glyph::CROSS << "  : Failed to load InpOutX64.\n"
                      << "  " << inpOutError << "\n"
                      << "  Make sure inpoutx64.dll and inpoutx64.sys are present beside the executable.\n";
            return false;
        }

        // If WinRing0 fails to load, GetControllerAddress falls back to the PnP resource.
        WinRing0 winRing0;
        winRing0.Load();

        const std::set<std::wstring> skipped = LoadSkippedControllers();
        const bool changesImod = action == ImodAction::Test || action == ImodAction::Disable;
        bool anySelected = false;
        bool anySucceeded = false;
        bool anyFailed = false;
        std::vector<BackupController> backups;

        for (const auto& controller : controllers)
        {
            const bool selected = IsSelected(skipped, controller);
            anySelected = anySelected || selected;

            switch (ProcessController(action, inpOut, winRing0, controller, selected, savedBackups, backups))
            {
            case ControllerResult::Changed:
            case ControllerResult::AlreadySet:
                anySucceeded = true;
                break;

            case ControllerResult::Failed:
                anyFailed = true;
                break;

            case ControllerResult::Skipped:
                break;
            }
        }

        if (action == ImodAction::Backup && !SaveBackups(backupPath, backups))
            return false;

        // Unticking every controller is a valid choice, not a failure.
        if (changesImod && !anySelected)
        {
            std::cout << "\n" << Glyph::CROSS << "  : No controllers are selected, nothing was changed.\n";
            return true;
        }

        return anySucceeded && !anyFailed;
    }

    // Startup

    enum class StartupMethod
    {
        TaskScheduler,
        RegistryRun
    };

    const wchar_t* const RUN_KEY = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";

    // The same task the basic schtasks command creates (SYSTEM, at logon,
    // highest privileges), plus what that command cannot set:
    //  - battery conditions off, otherwise the task never runs on a
    //    laptop that is unplugged at logon;
    //  - a second trigger on resume from sleep or hibernate (System log,
    //    Power-Troubleshooter event 1), since IMOD can be reset when the
    //    controller powers back up. The delay lets the USB driver finish
    //    restoring the controller first.
    const wchar_t* const TASK_XML = LR"(<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <Triggers>
    <LogonTrigger />
    <EventTrigger>
      <Subscription>&lt;QueryList&gt;&lt;Query Id="0" Path="System"&gt;&lt;Select Path="System"&gt;*[System[Provider[@Name='Microsoft-Windows-Power-Troubleshooter'] and EventID=1]]&lt;/Select&gt;&lt;/Query&gt;&lt;/QueryList&gt;</Subscription>
      <Delay>PT5S</Delay>
    </EventTrigger>
  </Triggers>
  <Principals>
    <Principal>
      <UserId>S-1-5-18</UserId>
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
  </Settings>
  <Actions>
    <Exec>
      <Command>{EXE}</Command>
      <Arguments>--Silent</Arguments>
    </Exec>
  </Actions>
</Task>
)";

    std::wstring GetSchtasksPath()
    {
        wchar_t systemDirectory[MAX_PATH] = {};
        const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);

        if (length == 0 || length >= MAX_PATH)
            return L"";

        return std::wstring(systemDirectory) + L"\\schtasks.exe";
    }

    // Runs schtasks by full path, so a schtasks.exe placed beside this
    // executable can never be started with admin rights instead.
    int RunSchtasks(const std::wstring& arguments)
    {
        const std::wstring schtasksPath = GetSchtasksPath();

        if (schtasksPath.empty())
            return -1;

        std::wstring commandLine = L"\"" + schtasksPath + L"\" " + arguments;
        STARTUPINFOW startupInfo = { sizeof(startupInfo) };
        PROCESS_INFORMATION processInfo = {};

        if (!CreateProcessW(schtasksPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo))
        {
            return -1;
        }

        WaitForSingleObject(processInfo.hProcess, INFINITE);

        DWORD exitCode = 1;
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        return static_cast<int>(exitCode);
    }

    std::wstring XmlEscape(const std::wstring& value)
    {
        std::wstring result;

        for (wchar_t c : value)
        {
            switch (c)
            {
            case L'&': result += L"&amp;"; break;
            case L'<': result += L"&lt;"; break;
            case L'>': result += L"&gt;"; break;
            case L'"': result += L"&quot;"; break;
            case L'\'': result += L"&apos;"; break;
            default: result += c; break;
            }
        }

        return result;
    }

    // Copies source to destination, or does nothing if they are the same file
    // (the startup option was chosen from the installed copy itself).
    bool CopyStartupFile(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        std::error_code error;

        if (std::filesystem::exists(destination, error) && std::filesystem::equivalent(source, destination, error))
            return true;

        return CopyFileW(source.c_str(), destination.c_str(), FALSE) != FALSE;
    }

    // Copies the executable, drivers and controller selection to the install
    // folder. The executable is always installed under its original name so the
    // startup entries stay the same even if the downloaded file was renamed.
    // WinRing0 is copied when present; without it the installed copy uses the
    // fallback (see the WinRing0 class).
    bool InstallStartupFiles(const std::filesystem::path& destinationDirectory)
    {
        std::filesystem::path executablePath;

        if (!GetExecutablePath(executablePath))
        {
            std::cout << "\n";
            PrintStatus(false, "Could not determine the executable path.");
            return false;
        }

        const std::filesystem::path sourceDirectory = executablePath.parent_path();
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> copies;
        copies.emplace_back(executablePath, destinationDirectory / INSTALLED_EXECUTABLE_NAME);

        for (const wchar_t* fileName : { L"inpoutx64.dll", L"inpoutx64.sys" })
        {
            const std::filesystem::path source = sourceDirectory / fileName;
            std::error_code error;

            if (!std::filesystem::is_regular_file(source, error))
            {
                std::cout << "\n";
                PrintStatus(false, "Required file is missing: " + ToUtf8(source.wstring()));
                return false;
            }

            copies.emplace_back(source, destinationDirectory / fileName);
        }

        for (const wchar_t* fileName : { L"WinRing0x64.dll", L"WinRing0x64.sys", SELECTION_FILE })
        {
            const std::filesystem::path source = sourceDirectory / fileName;
            std::error_code error;

            if (std::filesystem::is_regular_file(source, error))
                copies.emplace_back(source, destinationDirectory / fileName);
        }

        std::error_code error;
        std::filesystem::create_directories(destinationDirectory, error);

        if (error)
        {
            std::cout << "\n";
            PrintStatus(false, "Failed to create " + ToUtf8(destinationDirectory.wstring()));
            return false;
        }

        // No selection file means every controller is ticked, so an older copy
        // in the install folder must not keep skipping controllers.
        const std::filesystem::path installedSelection = destinationDirectory / SELECTION_FILE;

        if (!std::filesystem::is_regular_file(sourceDirectory / SELECTION_FILE, error) &&
            !std::filesystem::equivalent(sourceDirectory, destinationDirectory, error))
        {
            std::filesystem::remove(installedSelection, error);
        }

        for (const auto& [source, destination] : copies)
        {
            if (!CopyStartupFile(source, destination))
            {
                const DWORD copyError = GetLastError();
                std::cout << "\n";
                PrintStatus(false, "Failed to copy " + ToUtf8(source.filename().wstring()) + " to " +
                    ToUtf8(destinationDirectory.wstring()) + " (" + SystemErrorString(copyError) + ").");
                return false;
            }
        }

        return true;
    }

    // schtasks can only take these settings from an XML task definition,
    // which is written to %TEMP% and deleted once the task is registered.
    bool ConfigureTaskScheduler(const std::wstring& executablePath)
    {
        wchar_t temporaryDirectory[MAX_PATH + 1] = {};
        const DWORD length = GetTempPathW(MAX_PATH + 1, temporaryDirectory);

        if (length == 0 || length > MAX_PATH)
            return false;

        std::wstring taskXml = TASK_XML;
        taskXml.replace(taskXml.find(L"{EXE}"), 5, XmlEscape(executablePath));

        const std::filesystem::path xmlPath = std::filesystem::path(temporaryDirectory) / L"xHCI IMOD Disabler Task.xml";

        {
            std::ofstream file(xmlPath, std::ios::out | std::ios::trunc | std::ios::binary);

            if (!file)
                return false;

            // UTF-16 LE with a byte order mark, matching the XML declaration.
            const unsigned char byteOrderMark[] = { 0xFF, 0xFE };
            file.write(reinterpret_cast<const char*>(byteOrderMark), sizeof(byteOrderMark));
            file.write(reinterpret_cast<const char*>(taskXml.data()), static_cast<std::streamsize>(taskXml.size() * sizeof(wchar_t)));

            if (!file.good())
            {
                file.close();
                DeleteFileW(xmlPath.c_str());
                return false;
            }
        }

        const int exitCode = RunSchtasks(
            L"/create /tn \"" + std::wstring(STARTUP_NAME) + L"\" /xml \"" + xmlPath.wstring() + L"\" /f");

        DeleteFileW(xmlPath.c_str());
        return exitCode == 0;
    }

    bool ConfigureRunKey(const std::wstring& executablePath)
    {
        const std::wstring command = L"\"" + executablePath + L"\" --Silent";

        return RegSetKeyValueW(HKEY_LOCAL_MACHINE, RUN_KEY, STARTUP_NAME, REG_SZ, command.c_str(),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }

    const wchar_t* const UAC_POLICY_KEY = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";

    bool IsUacEnabled()
    {
        DWORD value = 1;
        DWORD size = sizeof(value);

        // A missing value means the Windows default, which is enabled.
        if (RegGetValueW(HKEY_LOCAL_MACHINE, UAC_POLICY_KEY, L"EnableLUA", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
            return true;

        return value != 0;
    }

    // Takes effect after a restart.
    bool DisableUac()
    {
        const DWORD value = 0;
        return RegSetKeyValueW(HKEY_LOCAL_MACHINE, UAC_POLICY_KEY, L"EnableLUA", REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
    }

    // Copies the executable and drivers to C:\xHCI IMOD Disabler and registers
    // the installed copy to run with --Silent using the chosen method.
    bool ConfigureStartup(StartupMethod method)
    {
        const bool taskScheduler = method == StartupMethod::TaskScheduler;
        const std::filesystem::path installDirectory = GetInstallDirectory();

        if (!InstallStartupFiles(installDirectory))
            return false;

        const std::wstring installedExecutable = (installDirectory / INSTALLED_EXECUTABLE_NAME).wstring();

        if (!(taskScheduler ? ConfigureTaskScheduler(installedExecutable) : ConfigureRunKey(installedExecutable)))
        {
            std::cout << "\n";
            PrintStatus(false, taskScheduler ? "Failed to configure Task Scheduler startup." : "Failed to configure Registry Run startup.");
            return false;
        }

        std::cout << "\n";
        PrintStatus(true, "Installed to " + ToUtf8(installDirectory.wstring()));
        PrintStatus(true, taskScheduler ? "Startup configured using Task Scheduler." : "Startup configured using the Registry Run key.");

        // Windows does not start Run entries that require admin rights while
        // UAC is on, so the entry would silently do nothing at logon. UAC is
        // turned off for it; Task Scheduler is the option that works with UAC on.
        if (!taskScheduler && IsUacEnabled())
        {
            if (DisableUac())
            {
                PrintStatus(true, "UAC disabled so the Registry Run entry can start as admin at logon. "
                    "Restart your PC for this to take effect.");
            }
            else
            {
                PrintStatus(false, "Failed to disable UAC, so Windows will not start the Registry Run entry as admin at logon. "
                    "Use the Task Scheduler option instead.");
            }
        }

        return true;
    }

    // Deletes both the Task Scheduler entry and the Registry Run value, whichever exist.
    void RemoveFromStartup()
    {
        const bool taskRemoved = RunSchtasks(L"/delete /tn \"" + std::wstring(STARTUP_NAME) + L"\" /f") == 0;
        const LSTATUS runKeyResult = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, RUN_KEY, STARTUP_NAME);

        std::cout << "\n";
        PrintStatus(taskRemoved, taskRemoved ? "Removed from Task Scheduler." : "No Task Scheduler entry was found.");

        if (runKeyResult == ERROR_SUCCESS)
            PrintStatus(true, "Removed from the Registry Run key.");
        else if (runKeyResult == ERROR_FILE_NOT_FOUND)
            PrintStatus(false, "No Registry Run entry was found.");
        else
            PrintStatus(false, "Failed to remove the Registry Run entry.");
    }

    // Menus
    //
    // Every menu works two ways: type an option's number and press Enter, or
    // move the highlight with Up/Down and press Enter. When input is redirected
    // (a file or pipe instead of a console), whole lines are read instead.

    constexpr int MENU_INPUT_CLOSED = -1;
    constexpr int MENU_REDRAW = -2;
    constexpr int MENU_DEVICES_CHANGED = -3;

    void ClearConsole()
    {
        HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info = {};

        // Nothing to clear when output is redirected to a file or pipe.
        if (console == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(console, &info))
            return;

        std::cout.flush();

        const DWORD cellCount = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
        const COORD origin = { 0, 0 };
        DWORD written = 0;

        FillConsoleOutputCharacterW(console, L' ', cellCount, origin, &written);
        FillConsoleOutputAttribute(console, info.wAttributes, cellCount, origin, &written);
        SetConsoleCursorPosition(console, origin);
    }

    // Console colors
    //
    // Text meant for the menus can carry color markers (Color::GREEN etc.);
    // PrintColored switches the color at each one. The colors are cpu-direct-usb's
    // palette, drawn with true-color escape sequences; consoles without them
    // get the nearest of the 16 classic colors. When output is not a console,
    // the markers are dropped and the text is printed plain.

    namespace Color
    {
        constexpr const char* GREEN = "\x01" "G";
        constexpr const char* ORANGE = "\x01" "O";
        constexpr const char* RED = "\x01" "R";
        constexpr const char* WHITE = "\x01" "W";
        constexpr const char* DIM = "\x01" "D";
        constexpr const char* RESET = "\x01" "0";
    }

    const char* ColorSequence(char marker)
    {
        switch (marker)
        {
        case 'G': return "\x1B[38;2;0;255;135m";
        case 'O': return "\x1B[38;2;255;179;71m";
        case 'R': return "\x1B[38;2;255;107;107m";
        case 'W': return "\x1B[97m";
        case 'D': return "\x1B[90m";
        default: return "\x1B[39m";
        }
    }

    WORD ColorAttribute(char marker, WORD normal)
    {
        const WORD background = normal & 0xFFF0;

        switch (marker)
        {
        case 'G': return background | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case 'O': return background | FOREGROUND_RED | FOREGROUND_GREEN;
        case 'R': return background | FOREGROUND_RED | FOREGROUND_INTENSITY;
        case 'W': return background | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        case 'D': return background | FOREGROUND_INTENSITY;
        default: return normal;
        }
    }

    // Turns on escape-sequence support for the console (Windows 10 and later).
    bool EnableVirtualTerminal(HANDLE console)
    {
        static const bool enabled = [console]
        {
            DWORD mode = 0;

            if (!GetConsoleMode(console, &mode))
                return false;

            return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
                SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != FALSE;
        }();

        return enabled;
    }

    void PrintColored(const std::string& text)
    {
        HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info = {};
        const bool isConsole = GetConsoleScreenBufferInfo(console, &info) != FALSE;
        const bool trueColor = isConsole && EnableVirtualTerminal(console);
        size_t start = 0;

        while (true)
        {
            const size_t marker = text.find('\x01', start);
            std::cout << text.substr(start, marker == std::string::npos ? std::string::npos : marker - start);

            if (marker == std::string::npos || marker + 1 >= text.size())
                break;

            if (trueColor)
            {
                std::cout << ColorSequence(text[marker + 1]);
            }
            else if (isConsole)
            {
                std::cout.flush();
                SetConsoleTextAttribute(console, ColorAttribute(text[marker + 1], info.wAttributes));
            }

            start = marker + 2;
        }

        if (trueColor)
        {
            std::cout << ColorSequence('0') << std::flush;
        }
        else if (isConsole)
        {
            std::cout.flush();
            SetConsoleTextAttribute(console, info.wAttributes);
        }
    }

    // Clears the screen and prints the title, an optional header, the options,
    // an optional status and the prompt. The highlighted option gets a white
    // pointer after its first line, unless input is redirected and the arrow
    // keys can't be used.
    void PrintMenu(const std::vector<std::string>& options, int highlighted, const std::string& status, bool invalidSelection,
                   const std::string& header = "")
    {
        DWORD inputMode = 0;
        const bool arrowKeys = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &inputMode) != FALSE;

        ClearConsole();

        std::cout << Glyph::POINTER << " xHCI IMOD Disabler " << xHCI::VERSION << "\n\n";

        if (!header.empty())
        {
            PrintColored(header);
            std::cout << "\n\n";
        }

        for (size_t i = 0; i < options.size(); i++)
        {
            const size_t firstLineEnd = options[i].find('\n');
            const std::string firstLine = options[i].substr(0, firstLineEnd);

            PrintColored(firstLine);

            if (static_cast<int>(i) == highlighted && arrowKeys)
                PrintColored(std::string("  ") + Color::WHITE + Glyph::LEFT_POINTER + Color::RESET);

            if (firstLineEnd != std::string::npos)
                PrintColored(options[i].substr(firstLineEnd));

            std::cout << "\n";
        }

        std::cout << "\n";

        if (!status.empty())
        {
            PrintColored(status);
            std::cout << "\n\n";
        }

        if (invalidSelection)
            std::cout << Glyph::CROSS << "  : Invalid selection.\n\n";

        std::cout << Glyph::ARROW << " " << std::flush;
    }

    // "3" -> 3 when it is a valid option number, otherwise 0. Leading zeros are
    // fine; at most 4 digits.
    int ParseMenuNumber(const std::string& text, int optionCount)
    {
        const size_t first = text.find_first_not_of(" \t\r");
        const size_t last = text.find_last_not_of(" \t\r");

        if (first == std::string::npos || last - first >= 4)
            return 0;

        int choice = 0;

        for (size_t i = first; i <= last; i++)
        {
            if (text[i] < '0' || text[i] > '9')
                return 0;

            choice = choice * 10 + (text[i] - '0');
        }

        return choice >= 1 && choice <= optionCount ? choice : 0;
    }

    // Reads one choice. Returns 1..optionCount, 0 for an invalid entry,
    // MENU_REDRAW after Up/Down moved the highlight, or MENU_INPUT_CLOSED once
    // redirected input has ended. With refresh set, it is called once a second
    // while nothing has been typed, and MENU_DEVICES_CHANGED is returned when
    // it reports a change so the menu can redraw.
    int ReadMenuChoice(int optionCount, int& highlighted, const std::function<bool()>& refresh = nullptr)
    {
        DWORD mode = 0;

        if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode))
        {
            std::string line;

            if (!std::getline(std::cin, line))
                return MENU_INPUT_CLOSED;

            return ParseMenuNumber(line, optionCount);
        }

        std::string typed;

        while (true)
        {
            ULONGLONG nextRefresh = GetTickCount64() + 1000;

            while (refresh && typed.empty() && !_kbhit())
            {
                Sleep(50);

                if (GetTickCount64() < nextRefresh)
                    continue;

                if (refresh())
                    return MENU_DEVICES_CHANGED;

                nextRefresh = GetTickCount64() + 1000;
            }

            const int key = _getch();

            // Arrow keys arrive as 0 or 0xE0 followed by a scan code: 72 Up, 80 Down.
            if (key == 0 || key == 0xE0)
            {
                const int code = _getch();

                if (code == 72 || code == 80)
                {
                    highlighted = (highlighted + (code == 72 ? optionCount - 1 : 1)) % optionCount;
                    return MENU_REDRAW;
                }

                continue;
            }

            if (key == '\r')
            {
                std::cout << "\n";

                if (typed.empty())
                    return highlighted + 1;

                const int choice = ParseMenuNumber(typed, optionCount);

                if (choice != 0)
                    highlighted = choice - 1;

                return choice;
            }

            if (key == '\b')
            {
                if (!typed.empty())
                {
                    typed.pop_back();
                    std::cout << "\b \b" << std::flush;
                }

                continue;
            }

            if (key >= 0x20 && key < 0x7F && typed.size() < 16)
            {
                typed += static_cast<char>(key);
                std::cout << static_cast<char>(key) << std::flush;
            }
        }
    }

    // Waits for any key, or for a line when input is redirected.
    void PauseAfterAction()
    {
        std::cout << "\n" << Glyph::ARROW << " Press any key to return..." << std::flush;

        DWORD mode = 0;

        if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode))
        {
            std::string line;
            std::getline(std::cin, line);
            return;
        }

        // Arrow and function keys arrive as two codes; read both.
        const int key = _getch();

        if (key == 0 || key == 0xE0)
            (void)_getch();
    }

    void RunImodMenu()
    {
        const std::vector<std::string> options =
        {
            "1. Backup Current IMOD Values.",
            "2. Restore IMOD Values.",
            "3. Test IMOD. (62.5Hz)",
            "4. Disable IMOD.",
            "5. Back."
        };

        int highlighted = 0;
        bool invalidSelection = false;

        while (true)
        {
            PrintMenu(options, highlighted, "", invalidSelection);

            const int choice = ReadMenuChoice(5, highlighted);
            invalidSelection = choice == 0;

            if (choice == MENU_INPUT_CLOSED || choice == 5)
                return;

            if (choice <= 0)
                continue;

            constexpr ImodAction actions[] = { ImodAction::Backup, ImodAction::Restore, ImodAction::Test, ImodAction::Disable };
            RunImodAction(actions[choice - 1]);
            PauseAfterAction();
        }
    }

    // Everything shown for one controller; read once when the screen opens.
    struct ControllerView
    {
        USBController controller;
        std::string location;
        int chips = -1;                 // Chips between the controller and the CPU, see ChipsToCpu.
        std::string interruptMode;
        std::string imod;
        std::vector<UsbDevice> devices;
    };

    std::vector<ControllerView> LoadControllerViews()
    {
        InpOut inpOut;
        std::string inpOutError;
        const bool haveInpOut = inpOut.Load(inpOutError);

        // If WinRing0 fails to load, GetControllerAddress falls back to the PnP resource.
        WinRing0 winRing0;

        if (haveInpOut)
            winRing0.Load();

        std::vector<ControllerView> views;

        for (const USBController& controller : FindUSBControllers())
        {
            DWORD vendor = 0;
            DWORD device = 0;
            ParseIdField(controller.deviceId, L"VEN_", vendor);
            ParseIdField(controller.deviceId, L"DEV_", device);

            ControllerView view;
            view.controller = controller;
            view.location = ControllerLocation(vendor, device);
            view.chips = ChipsToCpu(vendor, device);
            view.interruptMode = InterruptMode(controller);
            view.imod = haveInpOut ? CurrentImod(inpOut, winRing0, controller) : "Unknown";
            view.devices = FindUsbDevices(controller);
            views.push_back(view);
        }

        return views;
    }

    // Green for a direct path to the CPU, orange for one chip in between, red
    // for two or more; no color when the controller's location is unknown.
    std::string ChipColor(int chips)
    {
        if (chips < 0)
            return "";

        return chips == 0 ? Color::GREEN : chips == 1 ? Color::ORANGE : Color::RED;
    }

    int DeviceChips(const ControllerView& view, const UsbDevice& usb)
    {
        return view.chips < 0 ? -1 : view.chips + static_cast<int>(usb.hubs.size());
    }

    // One controller's entry in Controller Selection: the numbered line, then
    // its details and devices, then a blank line.
    std::string ControllerOption(size_t index, const ControllerView& view, bool selected)
    {
        const USBController& controller = view.controller;
        DWORD vendor = 0;
        DWORD device = 0;
        ParseIdField(controller.deviceId, L"VEN_", vendor);
        ParseIdField(controller.deviceId, L"DEV_", device);

        char ids[96];
        sprintf_s(ids, "VID %04lX DID %04lX", vendor, device);
        std::string details = ids;

        if (controller.hasLocation)
        {
            char pci[32];
            sprintf_s(pci, " | PCI %02lX:%02lX.%lX", controller.bus, controller.device, controller.function);
            details += pci;
        }

        if (!view.interruptMode.empty())
            details += std::string(" | ") + (view.interruptMode == "MSI" ? Color::GREEN : Color::RED) + view.interruptMode + Color::RESET;

        std::ostringstream text;

        // No brackets around the mark: Windows Terminal draws it wider than one
        // cell, and a character right after it would clip it.
        text << index + 1 << ". " << (selected ? Color::GREEN : Color::RED) << (selected ? Glyph::CHECK : Glyph::CROSS) << Color::RESET
             << "  " << ToUtf8(controller.caption) << "\n"
             << "      Location: " << ChipColor(view.chips) << view.location << Color::RESET << "\n"
             << "      " << details << "\n"
             << "      IMOD: " << view.imod << "\n";

        if (view.devices.empty())
        {
            text << "      Devices: None\n";
            return text.str();
        }

        text << "      Devices:";

        for (size_t d = 0; d < view.devices.size(); d++)
        {
            const UsbDevice& usb = view.devices[d];
            text << "\n        " << (d + 1 == view.devices.size() ? "'-" : "|-") << " " << ChipColor(DeviceChips(view, usb))
                 << ToUtf8(usb.name) << " " << Glyph::ARROW << " " << usb.types;

            // "Port 2", or "Port 1 via Hub A > Hub B" for devices behind hubs.
            std::string where = usb.port > 0 ? "Port " + std::to_string(usb.port) : "";

            if (!usb.hubs.empty())
            {
                std::string chain;

                for (const std::wstring& hub : usb.hubs)
                    chain += (chain.empty() ? "" : " > ") + ToUtf8(hub);

                where += (where.empty() ? "via " : " via ") + chain;
            }

            if (!where.empty())
                text << " (" << where << ")";

            text << Color::RESET;
        }

        text << "\n";
        return text.str();
    }

    // The color key under the controller list, drawn like cpu-direct-usb's:
    // how many chips sit between a device and the CPU.
    std::string ChipLegend()
    {
        std::ostringstream text;
        text << "  " << Color::GREEN << "0 CHIPS" << Color::RESET << "  device " << Color::GREEN << "---" << Color::RESET << " [CPU]\n"
             << "  " << Color::ORANGE << "1 CHIP " << Color::RESET << "  device -" << Color::ORANGE << "[CHIPSET]" << Color::RESET << "- [CPU]\n"
             << "  " << Color::RED << "2 CHIPS" << Color::RESET << "  device -" << Color::RED << "[HUB]" << Color::RESET << "-[CHIPSET]- [CPU]\n";

        return text.str();
    }

    // How likely a device is to be the mouse: 2 clearly, 1 maybe, 0 no. Many
    // gaming keyboards also report a mouse function and many mice a keyboard
    // one, so the name decides when it says which it is.
    int MouseLikelihood(const UsbDevice& usb)
    {
        if (usb.types.find("Mouse") == std::string::npos)
            return 0;

        const std::wstring name = UpperCase(usb.name);

        if (name.find(L"MOUSE") != std::wstring::npos)
            return 2;

        if (name.find(L"KEYBOARD") != std::wstring::npos || name.find(L"KBD") != std::wstring::npos)
            return 0;

        return usb.types.find("Keyboard") == std::string::npos ? 2 : 1;
    }

    // Word-wraps text at 90 characters, starting each further line with
    // newLine (a line break plus the indent that lines it up).
    std::string WrapText(const std::string& text, const std::string& newLine)
    {
        std::string wrapped;
        size_t lineLength = 0;
        size_t start = 0;

        while (start < text.size())
        {
            size_t end = text.find(' ', start);

            if (end == std::string::npos)
                end = text.size();

            const std::string word = text.substr(start, end - start);
            start = end + 1;

            if (word.empty())
                continue;

            if (lineLength > 0 && lineLength + 1 + word.size() > 90)
            {
                wrapped += newLine;
                lineLength = 0;
            }
            else if (lineLength > 0)
            {
                wrapped += ' ';
                lineLength++;
            }

            wrapped += word;
            lineLength += word.size();
        }

        return wrapped;
    }

    // One tip on where the mouse is best plugged in, from what is plugged in
    // where: the fewest chips to the CPU (CPU-direct when the PC has it), no
    // hub, and ideally a controller shared only with the keyboard. Empty when
    // no controller's location is known.
    std::string ConnectionTip(const std::vector<ControllerView>& views)
    {
        int bestChips = -1;

        for (const ControllerView& view : views)
        {
            if (view.chips >= 0 && (bestChips < 0 || view.chips < bestChips))
                bestChips = view.chips;
        }

        if (bestChips < 0)
            return "";

        // The device most likely to be the mouse (the closest to the CPU among
        // equals), and the controller it is on.
        int mouseLikelihood = 0;
        int mouseChips = -1;
        size_t mouseController = 0;

        for (size_t i = 0; i < views.size(); i++)
        {
            for (const UsbDevice& usb : views[i].devices)
            {
                const int likelihood = MouseLikelihood(usb);
                const int chips = DeviceChips(views[i], usb);

                if (likelihood == 0 || chips < 0)
                    continue;

                if (likelihood > mouseLikelihood || (likelihood == mouseLikelihood && chips < mouseChips))
                {
                    mouseLikelihood = likelihood;
                    mouseChips = chips;
                    mouseController = i;
                }
            }
        }

        // Dedicated: nothing but mice and keyboards on the mouse's controller.
        const bool dedicated = mouseChips >= 0 &&
            std::all_of(views[mouseController].devices.begin(), views[mouseController].devices.end(), [](const UsbDevice& usb)
            {
                return usb.types.find("Mouse") != std::string::npos || usb.types.find("Keyboard") != std::string::npos;
            });

        const std::string noOthers = "with no other devices (audio, storage, webcams, etc.) connected to";
        std::string tip;

        if (mouseChips == 0 && dedicated)
            tip = "Your mouse is plugged into a CPU-direct USB port on a dedicated USB controller. This is the ideal setup.";
        else if (mouseChips == 0)
            tip = "Your mouse is plugged into a CPU-direct USB port, but other devices share its controller. "
                "Ideally keep only your mouse and keyboard on that controller, " + noOthers + " it.";
        else if (bestChips == 0)
            tip = "Plug your mouse into a CPU-direct USB port, ideally on a dedicated USB controller shared only with your keyboard, " +
                noOthers + " that controller.";
        else if (mouseChips == bestChips && dedicated)
            tip = "No CPU-direct USB controller was found, and your mouse is on a dedicated USB controller. This is the best setup for this PC.";
        else if (mouseChips == bestChips)
            tip = "No CPU-direct USB controller was found. Ideally keep only your mouse and keyboard on the controller your mouse is on, " +
                noOthers + " it.";
        else if (mouseChips > bestChips)
            tip = "Your mouse is connected through a USB hub. Plug it directly into a USB port instead, ideally on a dedicated USB "
                "controller shared only with your keyboard.";
        else
            tip = "Plug your mouse directly into a USB port, ideally on a dedicated USB controller shared only with your keyboard, " +
                noOthers + " that controller.";

        return std::string(Color::DIM) + Glyph::POINTER + " Tip: " + WrapText(tip, "\n       ") + Color::RESET;
    }

    bool SameDevices(const std::vector<UsbDevice>& a, const std::vector<UsbDevice>& b)
    {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const UsbDevice& x, const UsbDevice& y)
        {
            return x.name == y.name && x.types == y.types && x.port == y.port && x.hubs == y.hubs;
        });
    }

    // Re-reads which devices are on each controller and returns whether
    // anything changed. When a controller itself appeared or went away (e.g. a
    // Thunderbolt dock), everything is reloaded.
    bool RefreshControllerViews(std::vector<ControllerView>& views)
    {
        const std::vector<USBController> controllers = FindUSBControllers();
        bool sameControllers = controllers.size() == views.size();

        for (size_t i = 0; sameControllers && i < views.size(); i++)
            sameControllers = UpperCase(controllers[i].deviceId) == UpperCase(views[i].controller.deviceId);

        if (!sameControllers)
        {
            views = LoadControllerViews();
            return true;
        }

        bool changed = false;

        for (ControllerView& view : views)
        {
            std::vector<UsbDevice> devices = FindUsbDevices(view.controller);

            if (!SameDevices(devices, view.devices))
            {
                view.devices = std::move(devices);
                changed = true;
            }
        }

        return changed;
    }
    // Lists every controller with its devices. Choosing a controller ticks or
    // unticks it, and Select All / Deselect All does every one. Save writes the
    // selection used by Test, Disable and the startup entries. The list follows
    // devices being plugged in or removed.
    void RunControllerMenu()
    {
        ClearConsole();
        std::cout << Glyph::POINTER << " xHCI IMOD Disabler " << xHCI::VERSION << "\n\nReading USB controllers...\n" << std::flush;

        std::vector<ControllerView> views = LoadControllerViews();

        const std::string header =
            "Choose the controllers to disable IMOD on, then Save.\n" +
            std::string(Color::GREEN) + Glyph::CHECK + Color::RESET + "  : Disable IMOD on this controller.\n" +
            Color::RED + Glyph::CROSS + Color::RESET + "  : Leave this controller alone.";

        std::set<std::wstring> skipped = LoadSkippedControllers();
        std::set<std::wstring> saved = skipped;
        std::string message;
        int highlighted = 0;
        bool invalidSelection = false;

        while (true)
        {
            const int allOption = static_cast<int>(views.size()) + 1;
            const int saveOption = allOption + 1;
            const int backOption = saveOption + 1;
            highlighted = (std::min)(highlighted, backOption - 1);

            std::vector<std::string> options;
            bool allSelected = !views.empty();

            for (size_t i = 0; i < views.size(); i++)
            {
                const bool selected = IsSelected(skipped, views[i].controller);
                allSelected = allSelected && selected;
                options.push_back(ControllerOption(i, views[i], selected));
            }

            // The key goes under the last controller, above Select All, Save and Back.
            if (!views.empty())
                options.back() += "\n" + ChipLegend();

            options.push_back(std::to_string(allOption) + (allSelected ? ". Deselect All." : ". Select All."));
            options.push_back(std::to_string(saveOption) + ". Save.");
            options.push_back(std::to_string(backOption) + ". Back.");

            std::string footer = message;

            if (views.empty())
                footer = std::string(Glyph::CROSS) + "  : Windows did not report any xHCI USB controllers.";
            else if (footer.empty() && skipped != saved)
                footer = std::string(Glyph::CROSS) + "  : Unsaved changes.";

            const std::string tip = ConnectionTip(views);

            if (!tip.empty())
                footer = tip + (footer.empty() ? "" : "\n\n" + footer);

            PrintMenu(options, highlighted, footer, invalidSelection, header);

            const int choice = ReadMenuChoice(backOption, highlighted, [&views] { return RefreshControllerViews(views); });

            if (choice == MENU_DEVICES_CHANGED)
                continue;

            invalidSelection = choice == 0;
            message.clear();

            if (choice == MENU_INPUT_CLOSED || choice == backOption)
                return;

            if (choice <= 0)
                continue;

            if (choice == saveOption)
            {
                if (SaveSkippedControllers(skipped))
                {
                    saved = skipped;
                    message = std::string(Glyph::CHECK) + "  : Controller selection saved.";
                }
                else
                {
                    message = std::string(Glyph::CROSS) + "  : Failed to save the controller selection.";
                }

                continue;
            }

            if (choice == allOption)
            {
                for (const ControllerView& view : views)
                {
                    const std::wstring id = UpperCase(view.controller.deviceId);

                    if (allSelected)
                        skipped.insert(id);
                    else
                        skipped.erase(id);
                }

                continue;
            }

            const std::wstring id = UpperCase(views[choice - 1].controller.deviceId);

            if (!skipped.erase(id))
                skipped.insert(id);
        }
    }

    void RunStartupMenu()
    {
        const std::vector<std::string> options =
        {
            "1. Task Scheduler.",
            "2. Registry Run.",
            "3. Remove From Startup.",
            "4. Back."
        };

        int highlighted = 0;
        bool invalidSelection = false;

        while (true)
        {
            PrintMenu(options, highlighted, "", invalidSelection);

            const int choice = ReadMenuChoice(4, highlighted);
            invalidSelection = choice == 0;

            if (choice == MENU_INPUT_CLOSED || choice == 4)
                return;

            if (choice == 1)
                ConfigureStartup(StartupMethod::TaskScheduler);
            else if (choice == 2)
                ConfigureStartup(StartupMethod::RegistryRun);
            else if (choice == 3)
                RemoveFromStartup();
            else
                continue;

            PauseAfterAction();
        }
    }

    void RunMainMenu()
    {
        const std::vector<std::string> options =
        {
            "1. IMOD Options.",
            "2. Controller Selection.",
            "3. Startup Options.",
            "4. Exit."
        };

        int highlighted = 0;
        bool invalidSelection = false;

        // Stops at Exit, or when redirected input closes so a piped run cannot loop forever.
        while (!std::cin.eof())
        {
            PrintMenu(options, highlighted, "", invalidSelection);

            const int choice = ReadMenuChoice(4, highlighted);
            invalidSelection = choice == 0;

            if (choice == MENU_INPUT_CLOSED || choice == 4)
                return;

            if (choice == 1)
                RunImodMenu();
            else if (choice == 2)
                RunControllerMenu();
            else if (choice == 3)
                RunStartupMenu();
        }
    }
}

// Entry point

int main(int argc, char* argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (_stricmp(argv[i], "--Silent") == 0)
        {
            // Detach from the console window and discard all output.
            FreeConsole();
            std::cout.setstate(std::ios::failbit);
            return RunImodAction(ImodAction::Disable) ? 0 : 1;
        }
    }

    // Output is UTF-8; restore the caller's code page on exit so a parent
    // cmd window is left as it was.
    const UINT originalCodePage = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);

    RunMainMenu();

    std::cout.flush();

    if (originalCodePage != 0)
        SetConsoleOutputCP(originalCodePage);

    return 0;
}

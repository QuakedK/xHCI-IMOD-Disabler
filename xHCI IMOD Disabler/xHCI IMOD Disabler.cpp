// xHCI IMOD Disabler
//
// Disables xHCI Interrupt Moderation (IMOD) by writing 0 to the IMOD register
// of every interrupter on every USB xHCI controller.
//
// Run with no arguments for the menu, or with --Silent (used by the startup
// entries) to disable IMOD without a window. Exit code 0 means success.
//
// Sections, in order: output helpers, utilities, drivers, controllers,
// backup file, IMOD actions, startup, menus, entry point.
// 
// Credit: Andrew contributed substantial fixes and improvements to controller
// discovery, PCI/BAR handling, MMIO mapping, startup handling, and
// overall reliability.

#include "xHCI Common.h"

#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    }

    void PrintStatus(bool succeeded, const std::string& message)
    {
        std::cout << (succeeded ? Glyph::CHECK : Glyph::CROSS) << " : " << message << "\n";
    }

    // Controller results are indented under the controller's name.
    void PrintControllerError(const char* message)
    {
        std::cout << "  " << Glyph::CROSS << " : " << message << "\n";
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
        DWORD problemCode = 0;          // Device Manager problem code, 0 if none.

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

            wchar_t instanceId[MAX_DEVICE_ID_LEN] = {};

            if (!SetupDiGetDeviceInstanceIdW(deviceInfo, &deviceData, instanceId, MAX_DEVICE_ID_LEN, nullptr) || !instanceId[0])
                continue;

            USBController controller;
            controller.deviceId = instanceId;
            controller.caption = GetDeviceString(deviceInfo, deviceData, SPDRP_FRIENDLYNAME);

            if (controller.caption.empty())
                controller.caption = GetDeviceString(deviceInfo, deviceData, SPDRP_DEVICEDESC);

            if (controller.caption.empty())
                controller.caption = L"USB xHCI Compliant Host Controller";

            ULONG status = 0;
            ULONG problem = 0;

            if (CM_Get_DevNode_Status(&status, &problem, deviceData.DevInst, 0) == CR_SUCCESS && (status & DN_HAS_PROBLEM))
                controller.problemCode = problem;

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
                << " [" << (succeeded ? Glyph::CHECK : Glyph::CROSS) << " ]\n";

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
                << " = " << HexString(saved.value) << " [" << Glyph::CHECK << " ]\n";
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

    ControllerResult ProcessController(ImodAction action, const InpOut& inpOut, const WinRing0& winRing0,
        const USBController& controller, const std::vector<BackupController>& savedBackups,
        std::vector<BackupController>& backups)
    {
        std::cout << "\n" << ToUtf8(controller.caption) << "\n"
            << "  Device Path: " << ToUtf8(controller.deviceId) << "\n";

        if (controller.problemCode == CM_PROB_DISABLED)
        {
            PrintControllerError("Skipped, Windows reports this controller as disabled.");
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
            return RestoreValues(registers, *backup);

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
            std::cout << "\n" << Glyph::CROSS << " : No IMOD values could be backed up.\n";
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
            std::cout << "\n" << Glyph::CROSS << " : IMOD is already disabled, so the existing backup was kept: " << path << "\n";
            return true;
        }

        if (!WriteBackupFile(backupPath, backups))
        {
            std::cout << "\n" << Glyph::CROSS << " : Failed to save the IMOD backup file: " << path << "\n";
            return false;
        }

        std::cout << "\n" << Glyph::CHECK << " : Backup saved to " << path << "\n";

        if (allDisabled)
            std::cout << Glyph::CROSS << " : IMOD was already disabled, so this backup holds the disabled values.\n";

        return true;
    }

    // Runs one action on every xHCI controller and prints the result of each
    // register. Returns true if at least one controller succeeded and none failed.
    bool RunImodAction(ImodAction action)
    {
        std::filesystem::path backupPath;

        if (!GetBackupPath(backupPath))
        {
            std::cout << "\n" << Glyph::CROSS << " : Could not determine the executable path.\n";
            return false;
        }

        std::vector<BackupController> savedBackups;

        if (action == ImodAction::Restore && !ReadBackupFile(backupPath, savedBackups))
        {
            std::cout << "\n" << Glyph::CROSS << " : Could not read the IMOD backup file: " << ToUtf8(backupPath.wstring()) << "\n";
            return false;
        }

        const std::vector<USBController> controllers = FindUSBControllers();

        if (controllers.empty())
        {
            std::cout << "\n" << Glyph::CROSS << " : Windows did not report any xHCI USB controllers.\n";
            return false;
        }

        InpOut inpOut;
        std::string inpOutError;

        if (!inpOut.Load(inpOutError))
        {
            std::cout << "\n" << Glyph::CROSS << " : Failed to load InpOutX64.\n"
                << "  " << inpOutError << "\n"
                << "  Make sure inpoutx64.dll and inpoutx64.sys are present beside the executable.\n";
            return false;
        }

        // If WinRing0 fails to load, GetControllerAddress falls back to the PnP resource.
        WinRing0 winRing0;
        winRing0.Load();

        bool anySucceeded = false;
        bool anyFailed = false;
        std::vector<BackupController> backups;

        for (const auto& controller : controllers)
        {
            switch (ProcessController(action, inpOut, winRing0, controller, savedBackups, backups))
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

        return anySucceeded && !anyFailed;
    }

    // Startup

    enum class StartupMethod
    {
        TaskScheduler,
        RegistryRun
    };

    // Used for both the task name and the Run value name.
    const wchar_t* const STARTUP_NAME = L"xHCI IMOD Disabler";
    const wchar_t* const INSTALLED_EXECUTABLE_NAME = L"xHCI IMOD Disabler.exe";
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

    // The root of the Windows drive.
    std::filesystem::path GetInstallDirectory()
    {
        wchar_t windowsDirectory[MAX_PATH] = {};
        const UINT length = GetSystemWindowsDirectoryW(windowsDirectory, MAX_PATH);
        std::wstring root = L"C:\\";

        if (length >= 3 && length < MAX_PATH && windowsDirectory[1] == L':')
            root.assign(windowsDirectory, 3);

        return std::filesystem::path(root) / STARTUP_NAME;
    }

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

    // Copies the executable and drivers to the install folder. The executable
    // is always installed under its original name so the startup entries stay
    // the same even if the downloaded file was renamed. WinRing0 is copied when
    // present; without it the installed copy uses the fallback (see the
    // WinRing0 class).
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

        for (const wchar_t* fileName : { L"WinRing0x64.dll", L"WinRing0x64.sys" })
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

    constexpr int MENU_INPUT_CLOSED = -1;

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

    // Clears the screen and prints the title, the numbered options and the prompt.
    void PrintMenu(const char* options, bool invalidSelection)
    {
        ClearConsole();

        std::cout << Glyph::POINTER << " xHCI IMOD Disabler " << xHCI::VERSION << "\n\n" << options << "\n";

        if (invalidSelection)
            std::cout << Glyph::CROSS << " : Invalid selection.\n\n";

        std::cout << Glyph::ARROW << " " << std::flush;
    }

    // Reads one line: returns 1..optionCount, 0 for anything else, or
    // MENU_INPUT_CLOSED once standard input has ended.
    int ReadMenuChoice(int optionCount)
    {
        std::string line;

        if (!std::getline(std::cin, line))
            return MENU_INPUT_CLOSED;

        const size_t first = line.find_first_not_of(" \t\r");
        const size_t last = line.find_last_not_of(" \t\r");

        if (first == std::string::npos || first != last)
            return 0;

        const int choice = line[first] - '0';
        return choice >= 1 && choice <= optionCount ? choice : 0;
    }

    void PauseAfterAction()
    {
        std::cout << "\nPress Enter to return..." << std::flush;

        std::string line;
        std::getline(std::cin, line);
    }

    void RunImodMenu()
    {
        bool invalidSelection = false;

        while (true)
        {
            PrintMenu(
                "1. Backup Current IMOD Values.\n"
                "2. Restore IMOD Values.\n"
                "3. Test IMOD. (62.5Hz)\n"
                "4. Disable IMOD.\n"
                "5. Back.\n",
                invalidSelection);

            const int choice = ReadMenuChoice(5);
            invalidSelection = choice == 0;

            if (choice == MENU_INPUT_CLOSED || choice == 5)
                return;

            if (choice == 0)
                continue;

            constexpr ImodAction actions[] = { ImodAction::Backup, ImodAction::Restore, ImodAction::Test, ImodAction::Disable };
            RunImodAction(actions[choice - 1]);
            PauseAfterAction();
        }
    }

    void RunStartupMenu()
    {
        bool invalidSelection = false;

        while (true)
        {
            PrintMenu(
                "1. Task Scheduler.\n"
                "2. Registry Run.\n"
                "3. Remove From Startup.\n"
                "4. Back.\n",
                invalidSelection);

            const int choice = ReadMenuChoice(4);
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
        bool invalidSelection = false;

        // Stops at Exit, or when standard input closes so a piped run cannot loop forever.
        while (!std::cin.eof())
        {
            PrintMenu(
                "1. IMOD Options.\n"
                "2. Startup Options.\n"
                "3. Exit.\n",
                invalidSelection);

            const int choice = ReadMenuChoice(3);
            invalidSelection = choice == 0;

            if (choice == MENU_INPUT_CLOSED || choice == 3)
                return;

            if (choice == 1)
                RunImodMenu();
            else if (choice == 2)
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

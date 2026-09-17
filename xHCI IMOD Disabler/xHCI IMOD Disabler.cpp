#include "xHCI Common.h"
#include <Windows.h>
#include <Wbemidl.h>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "wbemuuid.lib")

// Windows PnP information used to locate each xHCI controller.

struct USBController
{
    std::wstring caption;
    std::wstring deviceId;
    LONG configManagerErrorCode = -1;
};

struct PNPResource
{
    std::wstring deviceId;
    uint64_t physicalAddress = 0;
};

// Runtime APIs used to map the controller's physical MMIO region.
struct InpOutAPI
{
    HMODULE module = nullptr;

    xHCI::MapPhysToLin_t MapPhysToLin = nullptr;
    xHCI::UnmapPhysicalMemory_t UnmapPhysicalMemory = nullptr;
};

// Initialize COM and connect to WMI.
bool InitializeWMI(
    IWbemServices** services,
    IWbemLocator** locator)
{
    HRESULT hr = CoInitializeEx(
        nullptr,
        COINIT_MULTITHREADED
    );

    if (FAILED(hr))
        return false;

    hr = CoInitializeSecurity(
        nullptr,
        -1,
        nullptr,
        nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        EOAC_NONE,
        nullptr
    );

    if (FAILED(hr) && hr != RPC_E_TOO_LATE)
    {
        CoUninitialize();
        return false;
    }

    hr = CoCreateInstance(
        CLSID_WbemLocator,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator,
        reinterpret_cast<void**>(locator)
    );

    if (FAILED(hr))
    {
        CoUninitialize();
        return false;
    }

    BSTR namespaceName =
        SysAllocString(L"ROOT\\CIMV2");

    if (!namespaceName)
    {
        (*locator)->Release();
        *locator = nullptr;

        CoUninitialize();

        return false;
    }

    hr = (*locator)->ConnectServer(
        namespaceName,
        nullptr,
        nullptr,
        nullptr,
        0,
        nullptr,
        nullptr,
        services
    );

    SysFreeString(namespaceName);

    if (FAILED(hr))
    {
        (*locator)->Release();
        *locator = nullptr;

        CoUninitialize();

        return false;
    }

    hr = CoSetProxyBlanket(
        *services,
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        nullptr,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        EOAC_NONE
    );

    if (FAILED(hr))
    {
        (*services)->Release();
        *services = nullptr;

        (*locator)->Release();
        *locator = nullptr;

        CoUninitialize();

        return false;
    }

    return true;
}

// WMI may escape backslashes in DeviceID strings.
std::wstring NormalizeDeviceId(
    std::wstring deviceId)
{
    std::wstring::size_type position = 0;

    while ((position =
        deviceId.find(L"\\\\", position))
        != std::wstring::npos)
    {
        deviceId.replace(
            position,
            2,
            L"\\"
        );

        position++;
    }

    return deviceId;
}

// Extract the value portion from a WMI reference.
std::wstring ExtractAfterEquals(
    const std::wstring& value)
{
    size_t equalPosition =
        value.find(L"=");

    if (equalPosition == std::wstring::npos)
        return L"";

    std::wstring result =
        value.substr(equalPosition + 1);

    if (!result.empty() &&
        result.front() == L'"')
    {
        result.erase(result.begin());
    }

    if (!result.empty() &&
        result.back() == L'"')
    {
        result.pop_back();
    }

    return result;
}

// Enumerate the USB host controllers reported by Windows.
std::vector<USBController> GetUSBControllers(
    IWbemServices* services)
{
    std::vector<USBController> controllers;

    IEnumWbemClassObject* enumerator = nullptr;

    BSTR queryLanguage =
        SysAllocString(L"WQL");

    BSTR query =
        SysAllocString(
            L"SELECT DeviceID, Caption, "
            L"ConfigManagerErrorCode "
            L"FROM Win32_USBController"
        );

    HRESULT hr = services->ExecQuery(
        queryLanguage,
        query,
        WBEM_FLAG_FORWARD_ONLY |
        WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        &enumerator
    );

    SysFreeString(queryLanguage);
    SysFreeString(query);

    if (FAILED(hr))
        return controllers;

    while (true)
    {
        IWbemClassObject* object = nullptr;
        ULONG returned = 0;

        hr = enumerator->Next(
            WBEM_INFINITE,
            1,
            &object,
            &returned
        );

        if (FAILED(hr) || returned == 0)
            break;

        USBController controller;

        VARIANT value;
        VariantInit(&value);

        if (SUCCEEDED(object->Get(
            L"DeviceID",
            0,
            &value,
            nullptr,
            nullptr)))
        {
            if (value.vt == VT_BSTR &&
                value.bstrVal)
            {
                controller.deviceId =
                    value.bstrVal;
            }
        }

        VariantClear(&value);

        VariantInit(&value);

        if (SUCCEEDED(object->Get(
            L"Caption",
            0,
            &value,
            nullptr,
            nullptr)))
        {
            if (value.vt == VT_BSTR &&
                value.bstrVal)
            {
                controller.caption =
                    value.bstrVal;
            }
        }

        VariantClear(&value);

        VariantInit(&value);

        if (SUCCEEDED(object->Get(
            L"ConfigManagerErrorCode",
            0,
            &value,
            nullptr,
            nullptr)))
        {
            if (value.vt == VT_I4)
            {
                controller.configManagerErrorCode =
                    value.lVal;
            }
        }

        VariantClear(&value);

        controllers.push_back(controller);

        object->Release();
    }

    enumerator->Release();

    return controllers;
}

// Map each controller DeviceID to the physical resource reported by PnP.
std::unordered_map<std::wstring, PNPResource>
GetPNPResourceMap(
    IWbemServices* services)
{
    std::unordered_map<
        std::wstring,
        PNPResource
    > resourceMap;

    IEnumWbemClassObject* enumerator = nullptr;

    BSTR queryLanguage =
        SysAllocString(L"WQL");

    BSTR query =
        SysAllocString(
            L"SELECT Antecedent, Dependent "
            L"FROM Win32_PNPAllocatedResource"
        );

    HRESULT hr = services->ExecQuery(
        queryLanguage,
        query,
        WBEM_FLAG_FORWARD_ONLY |
        WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        &enumerator
    );

    SysFreeString(queryLanguage);
    SysFreeString(query);

    if (FAILED(hr))
        return resourceMap;

    while (true)
    {
        IWbemClassObject* object = nullptr;
        ULONG returned = 0;

        hr = enumerator->Next(
            WBEM_INFINITE,
            1,
            &object,
            &returned
        );

        if (FAILED(hr) || returned == 0)
            break;

        std::wstring antecedent;
        std::wstring dependent;

        VARIANT value;
        VariantInit(&value);

        if (SUCCEEDED(object->Get(
            L"Antecedent",
            0,
            &value,
            nullptr,
            nullptr)))
        {
            if (value.vt == VT_BSTR &&
                value.bstrVal)
            {
                antecedent =
                    value.bstrVal;
            }
        }

        VariantClear(&value);

        VariantInit(&value);

        if (SUCCEEDED(object->Get(
            L"Dependent",
            0,
            &value,
            nullptr,
            nullptr)))
        {
            if (value.vt == VT_BSTR &&
                value.bstrVal)
            {
                dependent =
                    value.bstrVal;
            }
        }

        VariantClear(&value);

        std::wstring deviceId =
            NormalizeDeviceId(
                ExtractAfterEquals(dependent)
            );

        std::wstring addressString =
            ExtractAfterEquals(antecedent);

        if (!deviceId.empty() &&
            !addressString.empty())
        {
            try
            {
                uint64_t physicalAddress =
                    std::stoull(
                        addressString,
                        nullptr,
                        0
                    );

                if (resourceMap.find(deviceId) ==
                    resourceMap.end())
                {
                    PNPResource resource;

                    resource.deviceId =
                        deviceId;

                    resource.physicalAddress =
                        physicalAddress;

                    resourceMap.emplace(
                        deviceId,
                        resource
                    );
                }
            }
            catch (...)
            {
            }
        }

        object->Release();
    }

    enumerator->Release();

    return resourceMap;
}

// Load InpOutX64 and resolve the MMIO mapping functions.
bool LoadInpOut(
    InpOutAPI& api)
{
    api.module =
        LoadLibraryW(L"inpoutx64.dll");

    if (!api.module)
        return false;

    api.MapPhysToLin =
        reinterpret_cast<
        xHCI::MapPhysToLin_t
        >(
            GetProcAddress(
                api.module,
                "MapPhysToLin"
            )
            );

    api.UnmapPhysicalMemory =
        reinterpret_cast<
        xHCI::UnmapPhysicalMemory_t
        >(
            GetProcAddress(
                api.module,
                "UnmapPhysicalMemory"
            )
            );

    if (!api.MapPhysToLin ||
        !api.UnmapPhysicalMemory)
    {
        FreeLibrary(api.module);
        api.module = nullptr;

        return false;
    }

    return true;
}

// Release InpOutX64.
void UnloadInpOut(
    InpOutAPI& api)
{
    if (api.module)
    {
        FreeLibrary(api.module);
        api.module = nullptr;
    }

    api.MapPhysToLin = nullptr;
    api.UnmapPhysicalMemory = nullptr;
}

// Read the xHCI registers and disable IMOD when it is not already zero.
bool ProcessController(
    const InpOutAPI& api,
    const USBController& controller,
    uint64_t physicalAddress,
    uint32_t desiredImodInterval)
{
    HANDLE physicalMemoryHandle = nullptr;

    PBYTE linearAddress =
        api.MapPhysToLin(
            reinterpret_cast<PBYTE>(
                static_cast<uintptr_t>(
                    physicalAddress
                    )
                ),
            0x2000,
            &physicalMemoryHandle
        );

    if (!linearAddress)
    {
        std::wcout
            << L"\n"
            << controller.caption
            << L"\n"
            << L"  Device Path: "
            << controller.deviceId
            << L"\n"
            << L"  \xE2\x9C\x96 : Windows/InpOut could not map "
            L"the controller's physical MMIO region.\n";

        return false;
    }

    uint32_t capabilityHeader =
        *reinterpret_cast<volatile uint32_t*>(
            linearAddress + 0x00
            );

    if (capabilityHeader == 0xFFFFFFFF)
    {
        std::wcout
            << L"\n"
            << controller.caption
            << L"\n"
            << L"  Device Path: "
            << controller.deviceId
            << L"\n"
            << L"  \xE2\x9C\x96 : Unable to access this xHCI's registers.\n";

        api.UnmapPhysicalMemory(
            physicalMemoryHandle,
            linearAddress
        );

        return false;
    }

    uint32_t hcsparams1 =
        *reinterpret_cast<volatile uint32_t*>(
            linearAddress + 0x04
            );

    uint32_t rtsoff =
        *reinterpret_cast<volatile uint32_t*>(
            linearAddress + 0x18
            );

    uint32_t maxInterrupters =
        (hcsparams1 >> 8) & 0x7FF;

    if (maxInterrupters == 0 ||
        maxInterrupters > 1024)
    {
        std::wcout
            << L"\n"
            << controller.caption
            << L"\n"
            << L"  Device Path: "
            << controller.deviceId
            << L"\n"
            << L"  \xE2\x9C\x96 : The xHCI capability header was "
            L"read, but it reported an invalid "
            L"interrupter count.\n";

        api.UnmapPhysicalMemory(
            physicalMemoryHandle,
            linearAddress
        );

        return false;
    }

    uint64_t lastImodOffset =
        0x24ULL +
        (0x20ULL * (maxInterrupters - 1)) +
        sizeof(uint32_t);

    if (rtsoff + lastImodOffset > 0x2000)
    {
        std::wcout
            << L"\n"
            << controller.caption
            << L"\n"
            << L"  Device Path: "
            << controller.deviceId
            << L"\n"
            << L"  \xE2\x9C\x96 : The xHCI runtime register area "
            L"extends beyond the mapped MMIO region.\n";

        api.UnmapPhysicalMemory(
            physicalMemoryHandle,
            linearAddress
        );

        return false;
    }

    std::wcout
        << L"\n"
        << controller.caption
        << L"\n";

    std::wcout
        << L"  Device Path: "
        << controller.deviceId
        << L"\n";

    std::cout
        << "  IMOD interval: 0x"
        << std::hex
        << desiredImodInterval
        << std::dec
        << "\n";

    bool operationSucceeded = true;

    for (uint32_t i = 0;
        i < maxInterrupters;
        i++)
    {
        // Each interrupter's IMOD register is 0x20 bytes after the previous one.
        size_t imodOffset =
            static_cast<size_t>(
                rtsoff +
                0x24 +
                (0x20 * i)
                );

        volatile uint32_t* imodRegister =
            reinterpret_cast<
            volatile uint32_t*
            >(
                linearAddress +
                imodOffset
                );

        uint32_t currentValue =
            *imodRegister;

        if (currentValue ==
            desiredImodInterval)
        {
            std::cout
                << "    Register "
                << (i + 1)
                << ": [Already Disabled]\n";

            continue;
        }

        *imodRegister =
            desiredImodInterval;

        uint32_t verifyValue =
            *imodRegister;

        uint64_t registerAddress =
            physicalAddress +
            imodOffset;

        std::cout
            << "    Register "
            << (i + 1)
            << ": 0x"
            << std::uppercase
            << std::hex
            << registerAddress
            << " = 0x"
            << currentValue
            << " \xE2\x86\x92 0x"
            << verifyValue
            << std::dec;

        if (verifyValue ==
            desiredImodInterval)
        {
            std::cout
                << " ["
                << "\xE2\x9C\x94 "
                << "]\n";
        }
        else
        {
            operationSucceeded = false;

            std::cout
                << " ["
                << "\xE2\x9C\x96 "
                << "]\n";
        }
    }

    api.UnmapPhysicalMemory(
        physicalMemoryHandle,
        linearAddress
    );

    return operationSucceeded;
}

// Get the folder containing the running executable.
bool GetExecutableDirectory(
    std::filesystem::path& directory)
{
    wchar_t pathBuffer[MAX_PATH];
    DWORD length =
        GetModuleFileNameW(
            nullptr,
            pathBuffer,
            MAX_PATH
        );

    if (length == 0 ||
        length >= MAX_PATH)
    {
        return false;
    }

    directory =
        std::filesystem::path(pathBuffer).parent_path();

    return !directory.empty();
}

// Copy the executable and InpOut runtime files to the startup folder.
bool InstallStartupFiles(
    const std::filesystem::path& sourceDirectory,
    const std::filesystem::path& destinationDirectory)
{
    const std::vector<std::wstring> files =
    {
        L"xHCI IMOD Disabler.exe",
        L"inpoutx64.dll",
        L"inpoutx64.sys",
        L"WinRing0x64.dll",
        L"WinRing0x64.sys"
    };

    for (const auto& fileName : files)
    {
        std::filesystem::path sourcePath =
            sourceDirectory / fileName;

        if (!std::filesystem::exists(sourcePath) ||
            !std::filesystem::is_regular_file(sourcePath))
        {
            return false;
        }
    }

    std::error_code error;
    std::filesystem::create_directories(
        destinationDirectory,
        error
    );

    if (error)
        return false;

    for (const auto& fileName : files)
    {
        std::filesystem::path sourcePath =
            sourceDirectory / fileName;

        std::filesystem::path destinationPath =
            destinationDirectory / fileName;

        std::filesystem::copy_file(
            sourcePath,
            destinationPath,
            std::filesystem::copy_options::overwrite_existing,
            error
        );

        if (error)
            return false;
    }

    return true;
}

// Add or update the Windows Run registry entry.
bool ConfigureRunKey(
    const std::wstring& executablePath)
{
    HKEY key = nullptr;

    LONG result =
        RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
            0,
            nullptr,
            0,
            KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr
        );

    if (result != ERROR_SUCCESS)
        return false;

    std::wstring command =
        L"\"" + executablePath + L"\" --Silent";

    result =
        RegSetValueExW(
            key,
            L"xHCI IMOD Disabler",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>(
                (command.size() + 1) * sizeof(wchar_t)
                )
        );

    RegCloseKey(key);

    return result == ERROR_SUCCESS;
}

// Create or update the Task Scheduler startup task.
bool ConfigureTaskScheduler(
    const std::wstring& executablePath)
{
    std::wstring commandLine =
        L"schtasks.exe /create /tn \"xHCI IMOD Disabler\" /tr \"\\\"" +
        executablePath +
        L"\\\" --Silent\" /sc onstart /ru SYSTEM /rl highest /f";

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);

    PROCESS_INFORMATION processInfo = {};

    std::vector<wchar_t> commandBuffer(
        commandLine.begin(),
        commandLine.end()
    );
    commandBuffer.push_back(L'\0');

    BOOL created =
        CreateProcessW(
            nullptr,
            commandBuffer.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo
        );

    if (!created)
        return false;

    WaitForSingleObject(
        processInfo.hProcess,
        INFINITE
    );

    DWORD exitCode = 1;

    GetExitCodeProcess(
        processInfo.hProcess,
        &exitCode
    );

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    return exitCode == 0;
}

// Handle the startup installation command-line arguments before normal IMOD processing.
bool HandleStartupArguments(
    int argc,
    char* argv[],
    bool& handled,
    bool silent)
{
    bool runKeyStartup = false;
    bool taskSchedulerStartup = false;

    for (int i = 1; i < argc; i++)
    {
        if (std::string(argv[i]) == "--RK-Startup")
        {
            runKeyStartup = true;
        }
        else if (std::string(argv[i]) == "--TS-Startup")
        {
            taskSchedulerStartup = true;
        }
    }

    if (!runKeyStartup && !taskSchedulerStartup)
    {
        handled = false;
        return true;
    }

    handled = true;

    if (runKeyStartup && taskSchedulerStartup)
    {
        if (!silent)
        {
            std::cout
                << "\n\xE2\x9C\x96 : Use either --RK-Startup or "
                "--TS-Startup, not both.\n";
        }

        return false;
    }

    std::filesystem::path sourceDirectory;
    const std::filesystem::path destinationDirectory =
        L"C:\\xHCI IMOD Disabler";

    if (!GetExecutableDirectory(sourceDirectory))
    {
        if (!silent)
        {
            std::cout
                << "\n\xE2\x9C\x96 : Could not determine the executable path.\n";
        }

        return false;
    }

    // Validate all required files before creating or changing startup entries.
    const std::vector<std::wstring> requiredFiles =
    {
        L"xHCI IMOD Disabler.exe",
        L"inpoutx64.dll",
        L"inpoutx64.sys",
        L"WinRing0x64.dll",
        L"WinRing0x64.sys"
    };

    for (const auto& fileName : requiredFiles)
    {
        std::filesystem::path sourcePath =
            sourceDirectory / fileName;

        if (!std::filesystem::exists(sourcePath) ||
            !std::filesystem::is_regular_file(sourcePath))
        {
            if (!silent)
            {
                std::cout
                    << "\n\xE2\x9C\x96 : Required file is missing: "
                    << sourcePath.string()
                    << "\n";
            }

            return false;
        }
    }

    if (!InstallStartupFiles(
        sourceDirectory,
        destinationDirectory))
    {
        if (!silent)
        {
            std::cout
                << "\n\xE2\x9C\x96 : Failed to create or copy files to "
                "C:\\xHCI IMOD Disabler.\n";
        }

        return false;
    }

    std::filesystem::path installedExecutable =
        destinationDirectory /
        L"xHCI IMOD Disabler.exe";

    bool configured =
        runKeyStartup
        ? ConfigureRunKey(installedExecutable.wstring())
        : ConfigureTaskScheduler(installedExecutable.wstring());

    if (!configured)
    {
        if (!silent)
        {
            std::cout
                << "\n\xE2\x9C\x96 : Failed to configure "
                << (runKeyStartup ? "Registry Run startup.\n" :
                    "Task Scheduler startup.\n");
        }

        return false;
    }

    if (!silent)
    {
        std::cout
            << "\xE2\x96\xBA xHCI IMOD Disabler V1.0\n"
            << "\n\xE2\x9C\x94 : Installed to "
            << destinationDirectory.string()
            << "\n"
            << "\xE2\x9C\x94 : Startup configured using "
            << (runKeyStartup ? "the Registry Run key.\n" :
                "Task Scheduler.\n");

        std::cout
            << "\n\xE2\x86\x92 Press Enter to exit..."
            << std::flush;

        std::cin.get();
    }

    return true;
}

// Program entry point.
int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);

    // Check for the optional silent command-line argument.
    bool silent = false;
    bool test = false;

    for (int i = 1; i < argc; i++)
    {
        if (std::string(argv[i]) == "--Silent")
        {
            silent = true;
        }
        else if (std::string(argv[i]) == "--Test")
        {
            test = true;
        }
    }

    bool startupArgumentHandled = false;

    if (!HandleStartupArguments(
        argc,
        argv,
        startupArgumentHandled,
        silent))
    {
        return 1;
    }

    if (startupArgumentHandled)
    {
        return 0;
    }

    // Use the normal disabled value unless --Test is specified.
    uint32_t desiredImodInterval =
        test ? 0x0000FA00 : xHCI::DESIRED_IMOD_INTERVAL;

    // Suppress console output when --Silent is used.
    if (silent)
    {
        std::cout.setstate(std::ios::failbit);
        std::wcout.setstate(std::ios::failbit);
    }

    if (!silent)
    {
        std::cout
            << "\xE2\x96\xBA xHCI IMOD Disabler V1.0\n";
    }

    IWbemServices* services = nullptr;
    IWbemLocator* locator = nullptr;

    if (!InitializeWMI(
        &services,
        &locator))
    {
        std::cout
            << "\n\xE2\x9C\x96 : Failed to initialize Windows "
            "Management Instrumentation (WMI).\n";

        return 1;
    }

    std::vector<USBController> controllers =
        GetUSBControllers(services);

    if (controllers.empty())
    {
        std::cout
            << "\n\xE2\x9C\x96 : Windows WMI did not report any "
            "USB controllers.\n";

        services->Release();
        locator->Release();
        CoUninitialize();

        return 1;
    }

    auto resourceMap =
        GetPNPResourceMap(services);

    InpOutAPI inpOut;
    bool allControllersSucceeded = true;

    if (!LoadInpOut(inpOut))
    {
        std::cout
            << "\n\xE2\x9C\x96 : Failed to load InpOutX64.\n";

        std::cout
            << "  Make sure inpoutx64.dll is present "
            "beside the executable.\n";

        services->Release();
        locator->Release();
        CoUninitialize();

        return 1;
    }

    for (const auto& controller : controllers)
    {
        if (controller.configManagerErrorCode == 22)
        {
            std::wcout
                << L"\n"
                << controller.caption
                << L"\n"
                << L"  Device Path: "
                << controller.deviceId
                << L"\n"
                << L"  \xE2\x9C\x96 : Skipped, Windows reports this "
                L"controller as disabled.\n";

            continue;
        }

        auto iterator =
            resourceMap.find(
                controller.deviceId
            );

        if (iterator == resourceMap.end())
        {
            allControllersSucceeded = false;

            std::wcout
                << L"\n"
                << controller.caption
                << L"\n"
                << L"  Device Path: "
                << controller.deviceId
                << L"\n"
                << L"  \xE2\x9C\x96 : Windows PnP did not provide "
                L"a physical MMIO resource address "
                L"for this controller.\n";

            continue;
        }

        if (!ProcessController(
            inpOut,
            controller,
            iterator->second.physicalAddress,
            desiredImodInterval
        ))
        {
            allControllersSucceeded = false;
        }
    }

    UnloadInpOut(inpOut);

    services->Release();
    locator->Release();

    CoUninitialize();

    if (!silent)
    {
        std::cout
            << "\n\xE2\x86\x92 Press Enter to exit..."
            << std::flush;

        std::cin.get();
    }

    return allControllersSucceeded ? 0 : 1;
}
# xHCI IMOD Disabler
xHCI IMOD Disabler: Disables xHCI Interrupt Moderation **(IMOD)** on every USB Host Controller found in your system, by patching/modifying each interrupter's IMOD register to **0** via PCI/MMIO access using **WinRing0** and **InpOutX64** drivers. The default IMOD Intervel is `0xFA0` / `4000 decimal` which translates to `1,000,000 ns` / `1 ms`, but by disabling IMOD the **1 ms** becomes **0 ns/0 ms**.

<img width="1280" height="720" alt="Github Picture" src="https://github.com/user-attachments/assets/67455a2c-197f-4b30-b1a2-0a53d722ed8f" />

![GitHub Release Downloads](https://img.shields.io/github/downloads/QuakedK/xHCI-IMOD-Disabler/total)

# Why disable IMOD?
**Interrupt Moderation **(IMOD)** controls the amount of time the **USB xHCI controller** waits before generating an **interrupt** for USB events.** A higher **IMOD** value means the controller waits **longer** before generating the interrupt, while a lower value means it waits for **less** time.  The default IMOD Intervel is specified in `250 ns` increments.

Disabling IMOD by setting it to `0x0` removes this waiting period. This is particularly relevant for high polling rate devices such as 8K Hz mice, which generate a new report every `125 µs` **(125,000 ns)**. Since the IMOD interval can introduce an additional **waiting period** before USB events are reported to the CPU, removing that delay allows high-frequency input events to be processed more **immediately**, reducing potential latency and avoiding unnecessary delay between individual mouse reports and their processing.

An xHCI controller typically handles multiple USB devices and their events, rather than being dedicated to a single device. As the number of active devices and USB events handled by the controller **increases**, more events can occur during the **moderation interval** before the controller generates an interrupt. Disabling **IMOD** removes this intentional **waiting period** across the controller, allowing USB events from multiple devices to be reported without that **additional moderation delay**.

**Learn more information here** → [Vally of Doom's IMOD Documentation](https://github.com/valleyofdoom/PC-Tuning#1139-xhci-interrupt-moderation-imod-permalink).

# Usage 
Simply follow the quick and easy steps below ↓

1. Download [xHCI IMOD Disabler V1.1.zip](https://github.com/QuakedK/xHCI-IMOD-Disabler/releases/download/IMOD/xHCI-IMOD-Disabler-V1.1.zip).
2. Right-click, Extract & run the exe as admin!

# Startup Options
In xHCI IMOD Disabler, you can select 2 different Startup Options which are Task Scheduler and Registry Run. Each option automatically add's xHCI IMOD Disabler to startup, and disables IMOD with the `--Silent` Command-line Argument!

> [!WARNING]
> However using the Registry Run option, may lead to BSOD's **(Blue Screens)** depending on the user. The exact cause remains fully unknown, however I speculate it has something to do with execution time on startup. You can still safely test the Registry Run and see if the issue occurs for you. If it does occur, simply disabling it from startup in Task Manager quickly enough can prevent it from being a infinite BSOD Loop. Or entering Safe Boot/Safe Mode and disabling it from there can, fix the BSOD's too.
> Although maybe not the exact program, you can find more information here ➔ [Registry Run BSOD Information](https://github.com/QuakedK/IMOD-Disabler/blob/main/Help/IMOD%20Disabler%20Fixes.md#4-bsod-on-startup).

# Windows Version Support
Due to the [April 2026 Windows Driver Policy update](https://support.microsoft.com/en-us/windows/hardware/drivers/the-windows-driver-policy), Microsoft stopped trusting legacy cross-signed drivers by default, causing Windows to block drivers that don't meet its current signing requirements. Effectively blocking the usage of inpoutx64 and WinRing0x64 drivers, which prevents xHCI IMOD Disabler from working.

**Affected Windows Versions** - (Assuming the April 2026 was installed or was released/made after)
- Windows 11 24H2
- Windows 11 25H2
- Windows 11 26H1
- Windows 11 26H2
- Windows Server 2025

**Work Around:**

Simply installing a Windows 11 build, before the April 2026 Windows Driver Policy update would stop Windows from blocking the required drivers for xHCI IMOD Disabler. 
Eg: Using [RG-Adguard](https://files.rg-adguard.net/version/f0bd8307-d897-ef77-dbd6-216fefbe94c5), you could download specific Windows versions down to the Build and Revision numbers. Making it easy to download Windows 11 24H2, 25H2 and 26H1 before the April 2026 update.

**Tutorial:**

When purposefully installing an older Windows version, you need to prevent/block Windows from updating. Otherwise your older Windows version gets updated to a newer Windows version, via a cumulative update. However nowadays even when setting up a Windows installation, it will attempt to update to the latest revision/cumulative update. So in order to prevent this, Windows must be installed offline! This can be done using the [Offline Account Method](https://youtu.be/VOtOEEGxbu4?si=Q9WdHbVFJQExuPk8) during Windows installation, however it doesn't stop there. As once you get into windows and either replug in your ethernet or connect your Wi-Fi Windows will try updating automatically, including such revision/cumulative updates. So in order to prevent this, before we ever connect to the internet after completing our Windows install we must pause Windows updates.
However most times it ends up unpausing itself, so running [Pause Windows Updates](https://github.com/QuakedK/Downloads/blob/main/Pause%20updates%20until%20the%20year%203000.reg) and restarting then connecting to internet finally should prevent Windows from updating!

More Information ➔ [Offline Windows Install](https://github.com/QuakedK/Scripting-Station/blob/main/System%20Docs/Offline%20Windows%20Install.md#offline-windows-install).

# Command-line Argument

`--Silent` | Runs xHCI IMOD Disabler sliently without display or output.

xHCI IMOD Disabler can be opened/ran in CMD, Task Scheduler and Run Registry with the `--Silent` Command-line Argument.

**CMD Example** | ```start "" "C:\Users\QuakedOPS\Downloads\xHCI IMOD Disabler V1.0\xHCI IMOD Disabler.exe" --Silent```

# Build Instructions
1. Prerequisites: Visual Studio 2022/2026+ with the "Desktop development with C++" workload.
2. Open Visual Studio and click clone a repository and copy/paste →
`https://github.com/QuakedK/xHCI-IMOD-Disabler.git`.
4. Once cloned, open **xHCI IMOD Disabler.sln** and then switch **Debug** to **Release** and Build Solution.
5. The **xHCI IMOD Disabler** will be built in `xHCI IMOD Disabler\x64\Release\xHCI IMOD Disabler.exe`.
6. Go to `\source\repos\xHCI-IMOD-Disabler` not `\source\repos\xHCI-IMOD-Disabler\xHCI IMOD Disabler\` and open the **Drivers** folder and copy **all 4** drivers
and paste them in `xHCI IMOD Disabler\x64\Release\xHCI IMOD Disabler.exe`.
7. Then open **xHCI IMOD Disabler.exe** <3



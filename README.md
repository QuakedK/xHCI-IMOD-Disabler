# xHCI IMOD Disabler
xHCI IMOD Disabler: Disables xHCI Interrupt Moderation **(IMOD)** on every USB Host Controller found in your system, by patching/modifying each interrupter's IMOD register to **0** via PCI/MMIO access using **WinRing0** and **InpOutX64** drivers. The default IMOD Intervel is `0xFA0` / `4000 decimal` which translates to **250 ns**, but by disabling IMOD the **250 ns** becomes **0 ns/0 ms**.

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



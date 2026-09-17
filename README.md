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

1. Download [xHCI IMOD Disabler V1.0.zip](https://github.com/QuakedK/xHCI-IMOD-Disabler/releases/download/xHCI/xHCI-IMOD-Disabler-V1.0.zip).
2. Right-click, Extract & run the exe as admin!

# Command-line Arguments

`--Silent` | Runs xHCI IMOD Disabler sliently without display or output.

`--Test` | Runs xHCI IMOD Disabler using a test IMOD Intervel value of `0xFA00` which is equal to **16 ms**.

`--TS-Startup` | Add's xHCI IMOD Disabler to **Task Scheduler**, so it can automatically run on startup.

`--RK-Startup` | Add's xHCI IMOD Disabler to the **Run Registry Key**, so it can automatically run on startup.

xHCI IMOD Disabler can be opened in cmd, and ran with such commands.

1. Copy the path of **xHCI IMOD Disabler.exe** - `Eg: "C:\Users\QuakedOPS\Downloads\xHCI IMOD Disabler V1.0\xHCI IMOD Disabler.exe"`
2. Open CMD as admin, and paste the following ↓

**Format** | ```start "" "pathtoexe" --"Command-line Arguments"```

**Example** | ```start "" "C:\Users\QuakedOPS\Downloads\xHCI IMOD Disabler V1.0\xHCI IMOD Disabler.exe" --Silent```

# Build Instructions
1. Prerequisites: Visual Studio 2022/2026+ with the "Desktop development with C++" workload.
2. Open Visual Studio and click clone a repository and copy/paste →
`https://github.com/QuakedK/xHCI-IMOD-Disabler.git`.
4. Once cloned, open **xHCI IMOD Disabler.sln** and then switch **Debug** to **Release** and Build Solution.
5. The **xHCI IMOD Disabler** will be built in `xHCI IMOD Disabler\x64\Release\xHCI IMOD Disabler.exe`.
6. Go to `\source\repos\xHCI-IMOD-Disabler` not `\source\repos\xHCI-IMOD-Disabler\xHCI IMOD Disabler\` and open the **Drivers** folder and copy **all 4** drivers
and paste them in `xHCI IMOD Disabler\x64\Release\xHCI IMOD Disabler.exe`.
7. Then open **xHCI IMOD Disabler.exe** <3



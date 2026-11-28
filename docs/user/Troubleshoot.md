# User Handbook - Troubleshooting

## Vulkan initialization error

- Ensure you have the latest drivers
- Uninstall old drivers, for Windows you can use [Display Driver Uninstaller](https://www.guru3d.com/download/display-driver-uninstaller-download/)
- Change backend manually in the settings file (set it from `0` to `1` or vice versa).
- Disconnect your second monitor, if any

## Shaders fail with "out of host memory" on Windows

*Platform(s) Affected*: **Windows**, mostly PCs with 16 GB of RAM.

**Symptoms:** the log has lines like these, and parts of the image are missing (in Ocarina of
Time, the intro video). The game may then close with exception `0xc0000409` in `ucrtbase.dll`.

```text
Driver rejected compute shader F9EA14A22394F5B1: VK_ERROR_OUT_OF_HOST_MEMORY
Graphics pipeline build failed: VK_ERROR_OUT_OF_HOST_MEMORY (...)
The GPU driver ran out of host memory building a pipeline: Windows commit is 31.8 of 31.9 GiB. ...
```

**Cause:** Windows lets programs commit only as much memory as RAM plus the page file (the
*commit limit*). A Switch 2 game commits 14 GiB of guest memory when it starts, and the first run
compiles shaders, several at once, some of which need gigabytes each (more on GPUs without matrix
cores, where tensor-core shaders are emulated). With 16 GB of RAM and a small page file, the limit
is reached. Little of it is ever written to disk: what runs out is the limit, not free memory.

**Fix:** enlarge the page file.

1. Open *System Properties* → *Advanced* → *Performance: Settings* → *Advanced* → *Virtual
   memory: Change*.
2. Clear *Automatically manage paging file size for all drives*, select the drive for the page
   file (usually `C:`), and choose *Custom size*. Enter an *Initial size* in MB so that RAM plus
   page file is at least 48 GB (on 16 GB of RAM, a 32 GB page file: `32768`), and a *Maximum
   size* at least that large (`49152` or more; don't lower an existing maximum). Click *Set*,
   then *OK*, and reboot.
3. Check *Task Manager* → *Performance* → *Memory* → *Committed* (used / limit) while the game
   runs. The *Commit size* of the emulator in the *Details* tab doesn't include guest memory, so
   use the system-wide figure.

On an RX 6750 GRE with 16 GB of RAM, a 31.9 GB commit limit failed 25 pipelines in Ocarina of
Time's intro, and 53.9 GB failed none ([#462](https://github.com/zortos293/Lichii/issues/462)).

## Linux / Steam Deck: crashes on loading screens

*Platform(s) Affected*: **Linux**, especially the Steam Deck (SteamOS) and other
16 GB handhelds.

**Symptoms:** Lichii closes during a loading screen or when entering a new zone
(in Ocarina of Time, a zone load). The process is gone rather than frozen, and
the journal shows earlyoom sending SIGTERM/SIGKILL to `AppRun` (the Lichii
AppImage) with swap essentially full, for example:

```text
earlyoom: mem avail:    59 of  2271 MiB ( 2.64%), swap free:    1 of 8422 MiB ( 0.01%)
earlyoom: low memory! at or below SIGTERM limits: mem  2.70%, swap  4.75%
earlyoom: sending SIGTERM to process ... uid 1000 "AppRun": ... VmRSS 5445 MiB, cmdline ".../lichii.AppImage"
```

**Cause:** A Switch 2 game can push Lichii's resident memory to around 5.5–6.5
GiB during loads. On a Steam Deck, usable RAM is about 14.5 GiB, zram is about
7.2 GB, and the stock `/home/swapfile` is only 1 GiB (8.2 GB of swap in total).
When that swap fills, earlyoom kills Lichii.

**Fix:** enlarge `/home/swapfile` to 16 GiB. Leave zram alone. Close Lichii
first, and make sure `/home` has enough free space for a fully allocated 16 GiB
file.

```bash
sudo swapoff /home/swapfile
sudo rm /home/swapfile
sudo fallocate -l 16G /home/swapfile
# if fallocate isn't available:
# sudo dd if=/dev/zero of=/home/swapfile bs=1M count=16384 status=progress
sudo chmod 600 /home/swapfile
sudo mkswap /home/swapfile
sudo swapon /home/swapfile
```

Use `fallocate` or `dd` so the file has no holes. A sparse file will be
deleted: SteamOS's `swapfile.service` runs
`holo-create-swapfile /home/swapfile 1024M`, which only recreates the file when
it is missing or sparse, so a dense 16 GiB file survives reboots. After
`swapon`, the file should sit at a low priority (here, `-1`) so zram (priority
100) is used first.

To undo and return to the stock 1 GiB file without a reboot:

```bash
sudo swapoff /home/swapfile
sudo rm /home/swapfile
sudo fallocate -l 1G /home/swapfile
# or: sudo dd if=/dev/zero of=/home/swapfile bs=1M count=1024 status=progress
sudo chmod 600 /home/swapfile
sudo mkswap /home/swapfile
sudo swapon /home/swapfile
```

Do not change `vm.compaction_proactiveness`, `vm.page_lock_unfairness`, or
`khugepaged` defrag. Setting those to 0, 1, and 0 caused heavy stutter on a
Steam Deck; the SteamOS defaults (20, 5, and 1) should be left alone.

On a Steam Deck with SteamOS 3.9.5 and Lichii nx2 build 386, growing
`/home/swapfile` to 16 GiB stopped the loading-screen earlyoom kills, with zram
left at 7.2 GB ([#2172](https://github.com/zortos293/Lichii/issues/2172)).

## This mod only works on an Emulator

- Enable RNG seed
- Set RNG seed to 0

## Eden Fails to Launch and Does Not Leave Any Logs

**[See here](https://evilperson1337.notion.site/Windows-Eden-Fails-to-Launch-and-Does-Not-Leave-Any-Logs-2b057c2edaf68156b640cf1ac549870a) for an illustrated guide.**

### Error Details

*Behavior*:  Program appears not to launch or exits immediately without leaving any log entries.
*Platform(s) Affected*:
- **Windows**

**Error Log Entries:** 

```text
None
```
**Example Error Message Entry in Windows Event Viewer**
```text
Faulting application name: eden.exe, version: 0.0.0.0, time stamp: 0x6795dc3c
Faulting module name: ntdll.dll, version: 10.0.26100.3037, time stamp: 0x95e6c489
Exception code: 0xc0000005
Fault offset: 0x0000000000014778
Faulting process id: 0x2AF0
Faulting application start time: 0x1DB7C30D2972402
Faulting application path: C:\temp\Eden-Windows\eden.exe
Faulting module path: C:\WINDOWS\SYSTEM32\ntdll.dll
Report Id: 4c8a6e13-9637-438c-b4d0-e802d279af66
Faulting package full name: 
Faulting package-relative application ID:
```

---

### Causes

<aside>

#### Issue 1: Missing C++ Redistributable

*Eden requires the latest C++ redistributable from Microsoft in order to run.  Like many other programs, it relies on aspects and libraries included in this runtime, without it - the program cannot run.*

1. Download the [Latest C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170#visual-studio-2015-2017-2019-and-2022) from Microsoft for your machine.
2. Double Click the downloaded executable file and wait for the software to install.
3. Restart the computer.
4. Launch Eden again, if the main window appears, you are good to go.  If not, proceed to the next issue.
</aside>

<aside>

#### Issue 2: Corrupted System Files

*A corruption of necessary system files can cause odd behaviors when Eden tries to access them.  It is a very rare case and you would likely see other programs misbehaving if this is what your issue is, but you can try if you have no other options.*

1. Launch Eden to generate a crash.
2. Confirm there are no logs created in the log directory.
    1. See the [How to Access Logs](./HowToAccessLogs.md) page for the log location if you need it.
    2. If there are any entries in here since you tried step 1, this is likely not your issue.
3. Navigate to your *Windows Event Viewer* (Start Menu > **eventvwr.msc)**. 
4. Expand **Windows Logs** and select **Application.**
    
5. Look for an entry with the Level of Error, and look for a message similar to the following
    
    ```text
    Faulting application name: Eden.exe, version: 0.0.0.0, time stamp: 0x6795dc3c
    Faulting module name: ntdll.dll, version: 10.0.26100.3037, time stamp: 0x95e6c489
    Exception code: 0xc0000005
    Fault offset: 0x0000000000014778
    Faulting process id: 0x2AF0
    Faulting application start time: 0x1DB7C30D2972402
    Faulting application path: C:\temp\Eden-Windows\Eden.exe
    Faulting module path: C:\WINDOWS\SYSTEM32\ntdll.dll
    Report Id: 4c8a6e13-9637-438c-b4d0-e802d279af66
    Faulting package full name: 
    Faulting package-relative application ID:
    ```
    
6. Run a Command Prompt terminal Window as Administrator.
7. Enter the following command and wait for it to complete.  It will take a while, just be patient and do other things while it completes.
    
    ```bat
    DISM /Online /Cleanup-Image /RestoreHealth
    ```
    
8. Reboot your computer.
9. Launch Eden and verify it is now working.
</aside>

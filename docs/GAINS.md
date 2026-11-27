# Measured gains

[Back to README](../README.md)

What a PC gains from Lares, and what Lares costs it, is shown here as numbers that were measured. Nothing on this page is a promise: a result is one PC on one day, and yours will differ. Windows 11 only for now.

Each row carries one of the project's status words:

| Status | Meaning |
| :--- | :--- |
| confirmed | Measured with `tools/measure.ps1` by the method below, and the run is recorded in a pull request |
| reported | Measured by someone else with the same script, who sent the snapshots |
| planned | Not measured yet |
| blocked | Cannot be measured for now, with the reason |

## What is measured

`tools/measure.ps1` is one PowerShell script with no dependencies. It runs on the PowerShell that Windows 11 ships with, needs no administrator rights, changes no setting and leaves nothing running. It reads its counters twice, when the idle window opens and when it closes, so it adds no load in between.

| Reading | How it is read |
| :--- | :--- |
| Memory in use | Installed memory less what Windows reports as available |
| Memory committed | The system's commit charge |
| Processes, threads | The system's own counts |
| Processor use at rest | Share of the idle window in which the processors were not idle, all of them together |
| Graphics card use at rest | The busiest graphics engine over the idle window, added up over every program using it |
| Graphics memory in use | Dedicated graphics memory in use, all cards together |
| Start-up entries | How many entries the two `Run` registry keys and the two Startup folders hold, counted, never named, and split into turned on and turned off |
| Named programs | For each program you name: how many are running, their own (private) memory, their working set, and the processor time they used during the idle window |

The file it writes holds numbers, the label you gave, the time, the Windows build, the processor count and the amount of memory. It holds no user, computer or device name, no serial number, no path, no network detail, and no program or start-up entry name other than the ones you asked for. Before writing, the script looks for this PC's account and computer name in its own result and refuses to write if it finds either. Read the file anyway before you post it.

## Method

Change one thing between the two sets of snapshots, and nothing else.

1. Plug the PC in. Restart it, sign in, and wait 10 minutes for Windows to finish starting.
2. Close every window. If Windows Update, an antivirus scan or a backup is running, wait until it ends.
3. Take three snapshots, without touching the PC while each one runs:

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\measure.ps1 -Out before1.json -Label before -IdleSeconds 300
   ```

   `-ExecutionPolicy Bypass` applies to that one run and changes no setting. Use `before2.json` and `before3.json` for the next two.
4. Make the change: install Lares, or turn one part of it on. Name its programs with `-Processes`, for example `-Processes lares,lares-pad`, in both sets, so the table shows their own cost.
5. Repeat steps 1 to 3 with `-Label after` and `after1.json` to `after3.json`.
6. Compare. With several snapshots on a side, each reading is the middle value:

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\measure.ps1 -Before before1.json,before2.json,before3.json -After after1.json,after2.json,after3.json
   ```

It prints a table ready to paste into an issue or a pull request.

What counts: the same PC, the same power mode, the same screens and devices plugged in, the same idle window, and the same wait after signing in on both sides. A change smaller than the difference between two snapshots of the same side is noise, not a gain. A PC that is being used during the idle window gives system-wide numbers that mean nothing; only the named programs' rows hold up then.

## Results

### What Lares itself costs on Windows

One PC (Windows 11, 28 logical processors, 32 GB of memory), 27 November 2026, three snapshots a side with a 60 second idle window. *Before* is with the Lares window closed; *after* is with it open on Overview in a dry run (`LARES_DRY_RUN=1`). `lares-pad.exe`, the background helper, was running on both sides. The PC was in use, so only the rows of Lares' own programs are given.

| Program | Reading | Middle value | Lowest to highest | Status |
| :--- | :--- | ---: | ---: | :--- |
| `lares.exe`, window open on Overview | Own memory | 12.9 MB | 12.7 to 13.0 MB | confirmed |
| `lares.exe`, window open on Overview | Working set | 32.6 MB | 32.4 to 32.7 MB | confirmed |
| `lares.exe`, window open on Overview | Processor time in 60 s | 0.78 s | 0.64 to 0.80 s | confirmed |
| `lares-pad.exe`, background helper | Own memory | 3.7 MB | 3.7 to 3.7 MB | confirmed |
| `lares-pad.exe`, background helper | Working set | 14.8 MB | 14.8 to 14.8 MB | confirmed |
| `lares-pad.exe`, background helper | Processor time in 60 s | 0.02 s | 0 to 0.03 s | confirmed |
| `lares.exe`, window closed | Own memory | 0 MB, it is not running | | confirmed |
| `lares-share.exe`, while sending or receiving | Own memory, processor time | | | planned |

Overview draws live load graphs, so 0.78 s in 60 s is the window at work: about 1.3% of one processor, or 0.05% of this PC's 28. The helper's rows are for six snapshots, the window's for three.

### What a PC gains

| Change | Reading | Status |
| :--- | :--- | :--- |
| Lares installed, every part off | Every system-wide reading, against the same PC without Lares | planned: needs a freshly restarted PC left idle, which this first run was not |
| The desktop shell (top bar, dock, launcher) in place of the Windows taskbar | Memory, processor and graphics card use at rest | planned: the shell is not in this repository yet |
| Each "lighter Windows" switch | The reading that switch is meant to move, on the user's own PC | planned |
| Lares on Windows 11 on ARM | Every reading | planned |

## Limits

- Processor use is time not idle. Task Manager shows a different figure (processor utility) that also weighs clock speed, so the two do not match exactly.
- Start-up entries leave out scheduled tasks, services and the start-up tasks of Store apps.
- A PC or virtual machine without graphics counters gets "not measured" for the two graphics readings.
- The script measures a PC at rest. It says nothing about how fast anything opens or runs.

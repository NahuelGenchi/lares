# Crash safety on Windows

[Back to README](../README.md) · Issue [#127](https://github.com/NahuelGenchi/lares/issues/127)

Lares for Windows will be able to put its own bar and dock in place of the Windows taskbar. This page is about what happens when something goes wrong: a crash, a kill, a hang, a power cut. The rule is that the user always keeps a working desktop, and the Windows taskbar comes back by itself.

Each claim is marked **confirmed** (a recorded check establishes it), **reported** (documented by Microsoft, not checked here), **planned** or **blocked**. The bar and dock themselves are not in this repository yet, so everything here was checked with a stand-in for them, `lares-test-shell.exe`, which is built for tests and never installed.

## The guard

`lares-guard.exe` is a small program with no window. It starts the bar ("the shell") and owns one fact: *the Windows taskbar is hidden*.

- The shell says "ready", and the guard hides the Windows taskbar and switches its auto-hide on, after writing down what auto-hide was.
- The shell says "quitting", and the guard gives the taskbar back, with auto-hide as it was, and leaves too.
- The shell ends in any other way (a crash, a kill, a hang the guard has to end): the guard gives the taskbar back at once, then starts the shell again.
- Two such endings within five minutes: the guard stops restarting, leaves the Windows taskbar on screen and shows one message with **Try again** and **Keep Windows' own taskbar**.

```
lares-guard run <shell program and its arguments>
lares-guard restore        gives the taskbar back from the saved state, when no guard is running
```

### When the guard itself is killed

Three things stand behind the guard, and this is the design chosen for "the guard can be killed too":

1. **A twin.** The guard starts a second copy of itself, `lares-guard twin`, that does nothing but wait on the guard. If the guard ends without having written "stopped", the twin gives the taskbar back and starts a new guard. The guard waits on the twin the same way and starts a new one if it ends.
2. **A job object.** The shell runs in a Windows job that ends it when the guard's handle closes, so a shell never runs on with nobody watching it. Programs the shell starts are not in the job.
3. **State that outlives every process.** `%LOCALAPPDATA%\Lares\guard.state` says whether the taskbar's windows are hidden, and the change journal (below) says what auto-hide was before. Both are written and flushed *before* the taskbar is touched; the state file is replaced in one step, so a power cut leaves either the old state or the new one. The next guard to start, or `lares-guard restore`, gives the taskbar back from them.

The limit of this design: if the guard and its twin are both killed in the same instant (a script that ends every process by name does this), neither has time to act. The taskbar then stays hidden until a guard next starts or `lares-guard restore` is run. Windows' own Start menu (the Windows key) and Alt+Tab still work in that state. Measured: one millisecond between the two kills is already enough for the taskbar to come back by itself.

### What the shell has to do

Three calls, in `win/guard/guard_client.h`: `lares_guard_client_ready` once the bar is on screen, `lares_guard_client_message` first thing in its window procedure, `lares_guard_client_quit` before leaving on purpose. They cover saying "ready" and "quitting", saying "still answering" every two seconds from the thread that draws the bar, hiding the taskbar again when Explorer makes it anew or shows it again or the screens change, and giving it back at the end of the session. A shell started without a guard does none of this and leaves the Windows taskbar alone.

A shell must also end itself at once when it crashes, as the stand-in does, or Windows' own crash handling keeps the dead process around and the guard waiting.

### Small on purpose

The guard and the twin use `kernel32` alone: no window, no C runtime, and one wait on process handles, so no polling. While the taskbar is hidden that wait ends every four seconds to see whether the shell still says "still answering". Everything that needs Windows' window library (hiding, giving back, the message) runs in a short-lived copy of the same program, so the two that stay never load it. Asking Explorer about auto-hide waits on Explorer; a copy that has not finished after five seconds is ended, so an Explorer that stops answering cannot stop the guard or its twin. That is the design and it is **not checked**: no test makes Explorer hang.

| Reading | Value | Where | Status |
| :--- | ---: | :--- | :--- |
| `lares-guard.exe`, own memory | 564 KB | GitHub's Windows runner, a clean Windows Server | confirmed |
| Its twin, own memory | 468 KB | same | confirmed |
| Guard, processor time in 30 idle seconds | 0.0 ms | same | confirmed |
| Guard, working set | 3.4 MB | same | confirmed |
| `lares-guard.exe`, own memory | 2.4 MB | the Windows 11 development PC | confirmed, see below |
| Guard, processor time in 60 idle seconds | 15.6 ms or less (one tick of the clock) | the Windows 11 development PC | confirmed |
| `lares-guard.exe`, size on disk | 19 KB | | confirmed |

The target is under 1 MB of own memory. It is met on a clean Windows. On the development PC a third-party program loads itself, and Windows' window and drawing libraries with it, into every process that starts; the 2.4 MB there is that program's cost, in the guard as in every other process, and nothing the guard can avoid. `lares-guard-test --measure <seconds>` prints these readings, and the Checks workflow runs it on every pull request.

## Safe mode

Safe mode starts Lares with every shell part off and leaves the Windows desktop exactly as it is. Three things turn it on (issue [#128](https://github.com/NahuelGenchi/lares/issues/128)):

- **`--safe`**, on `lares.exe` (with anything else on its command line, or alone) or as `lares-guard run --safe <shell>`.
- **Shift held while signing in.** The guard looks at Shift only when it is started with `--sign-in`, which is what Lares' start-up entry will pass; at any other start, typing a capital letter would be enough. Hold Shift until the desktop is fully shown.
- **A mark on the session** that either of the two leaves behind, so that nothing starts the bar again by itself later in the same session. The mark is a Windows global atom: no file, no process, and it is gone at the next sign-in. Reading the keyboard and the mark both need Windows' window library, so one of the guard's short-lived copies decides at each start; the guard itself still never loads it.

In safe mode the guard starts no shell. A guard that is already running is asked to end the bar and give the taskbar back (`lares-guard stop` does the same on its own), and anything an earlier guard left hidden comes back. Lares' Settings shows a **Safe mode** card while it is on, with **Turn safe mode off**.

| Case | How | Result | Status |
| :--- | :--- | :--- | :--- |
| `lares-guard run --safe` | automatic, stand-in taskbar | no guard, twin or shell stays; the taskbar is never touched; the session is marked | confirmed |
| A plain start in a marked session | automatic | nothing starts | confirmed |
| `--safe` while the bar is running | automatic | bar ended, taskbar back, nothing counted as a crash | confirmed |
| `lares-guard stop` | automatic | the same, and nothing to do when no guard runs | confirmed |
| The decision (switch, Shift, mark; Shift only at sign-in) | unit test | | confirmed |
| `lares --safe`, in any position on the command line | unit test | | confirmed |
| The Safe mode card and **Turn safe mode off** | window check in a dry run, light and dark | the card shows, and goes when the button is clicked | confirmed |
| Shift held at a real sign-in | | | **not checked**: it needs a sign-out on a PC with Lares' start-up entry, which does not exist until the bar is in this repository. Only the decision is tested, with Shift as a plain yes or no |

## The change journal

Every Windows setting Lares changes is written down first, in one small file per user, `%LOCALAPPDATA%\Lares\journal.log` (issue [#129](https://github.com/NahuelGenchi/lares/issues/129)). An entry says what was changed, the value before, the value after, when, and which feature asked. It is written and flushed to disk **before** the setting is touched and marked done after, so a crash or a power cut at any point leaves something undo can act on:

| The program died... | What is on disk | What undo does |
| :--- | :--- | :--- |
| while writing the entry | a line cut short, which fails its checksum | ignores it: the setting was never touched |
| after the entry, before the change | an entry with no "done" mark, the setting still as before | finds the old value and closes the entry |
| after the change, before the "done" mark | an entry with no "done" mark, the setting changed | puts the old value back |

One routine, `lares_journal_undo` in `win/journal/journal.h`, serves all three callers: **Undo all** in Settings (and `lares undo` on the command line), the guard when it gives the taskbar's auto-hide back, and the uninstaller when there is one. It works newest entry first, for one feature, one entry or everything. A setting that no longer has the value Lares gave it was changed by you or another program since: undo leaves it alone, says so, and keeps the entry. When nothing is left to undo, the file is removed.

The interface is C with no C runtime, so the guard links it:

```c
lares_journal journal;
lares_journal_open(&journal, NULL);                      /* the user's own journal */
lares_journal_add_backend(&journal, &lares_journal_registry);

lares_journal_value value;
lares_journal_value_dword(&value, 1);
lares_journal_change(&journal, "feature-name", "registry",
                     L"HKCU\\Software\\Example|Value", &value, NULL);

lares_journal_undo(&journal, NULL, 0, 0, &result);        /* everything, newest first */
```

A backend is a pair of functions that read and write one kind of setting. Two exist: `registry` (`win/journal/journal_win.h`, a value under `HKCU` or `HKLM`, including one that was not there) and `taskbar` (`win/guard/taskbar_journal.h`, the taskbar's auto-hide). The guard's own state file now holds only whether the taskbar's windows are hidden; auto-hide, the part that outlives a session, is in the journal.

The file is plain text, one record a line, each with a CRC-32. It holds setting names and values, which can include paths; it stays on the PC like every other Lares file.

| Case | How | Result | Status |
| :--- | :--- | :--- | :--- |
| A change, the list, undo, the file removed when empty | unit test, made-up backend | | confirmed |
| Several entries: newest first, by feature, by one entry | unit test | two changes of one setting go back to the first value | confirmed |
| A setting someone else changed since | unit test | left alone and kept in the journal; forced, it goes back | confirmed |
| A change or an undo that cannot be made | unit test | reported, nothing owed for a change that failed | confirmed |
| An entry with no completion mark, change made or not | unit test | put back, or found as it was | confirmed |
| A torn last record | unit test: the second entry's records cut at every byte | the torn record is never believed, everything before it undoes, the next record starts on its own line | confirmed |
| A record damaged in the middle | unit test | that record alone is lost | confirmed |
| Registry values: written where there was none, changed, a string, removed | automatic, under a test key in `HKCU` | undo restores all four | confirmed |
| Power cut between the journal write and the change | automatic: a copy of the test ends itself there | the setting was never changed; undo finds it as it was | confirmed |
| Power cut between the change and the completion mark | automatic, the same way | undo puts the old value back | confirmed |
| The guard's auto-hide through the journal | automatic, stand-in taskbar: every guard case above, plus three kinds of leftovers from a guard that died in the middle | auto-hide back each time, journal empty after | confirmed |
| **Undo all** in Settings | window check in a dry run, light and dark, with a journal of two registry values under a test key | "2 settings are back as before", the values gone, the file removed | confirmed |
| A real power cut (the machine losing power) | | | **not checked**: a killed process stands in for it. What the journal relies on is `FlushFileBuffers` reaching the disk before the change |

### What else Lares changes in Windows

Every place the Windows code changes something outside its own files was looked for (issue [#133](https://github.com/NahuelGenchi/lares/issues/133)). This is the list, and what each does with the journal:

| Change | Where | Journal | Why |
| :--- | :--- | :--- | :--- |
| The taskbar's auto-hide | `win/guard/` | yes, feature `shell` | a setting of Windows' own that outlives Lares |
| The helper's start-up entry, `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value `LaresPad` | `win/src/helper.cpp` | yes, feature `pad-helper` | a setting that outlives Lares. An entry made by a Lares from before the journal is written down as Lares' own the first time, so undo removes it instead of putting it back. **Undo all** also turns Run in the background off, or the next start would add the entry again, and stops the helper |
| The screen's size, for Remote | `win/src/remote.cpp` | no | changed without being saved (`ChangeDisplaySettingsExW` without `CDS_UPDATEREGISTRY`), so Windows forgets it at sign-out; Home puts it back |
| Tailscale up and down, Sunshine started and stopped | `win/src/remote.cpp` | no | switched on and off, not configured: the Remote and Home modes are the switch |
| Sunshine's service permission, so the user can start and stop it | `win/src/remote.cpp`, the **Allow** button | no, not yet | a lasting change, but made once with administrator rights from an elevated window; putting it back needs them too. Its own follow-up |
| The Wi-Fi adapter's WinUSB driver, for AirDrop | `win/share/driver_setup.cpp`, **Set up** | no, not yet | a lasting change made with administrator rights; Share's own **Give back** undoes it. Its own follow-up |

Nothing else in the Windows code writes the registry, a system setting or a service.

## What was checked

`win/tests/guard_test.c` builds `lares-guard-test.exe`, which runs the real `lares-guard.exe` with the stand-in shell and kills things. By default it points the guard at a stand-in taskbar (windows of its own, off screen, with one auto-hide setting for all of them as Windows has) and a temporary state folder, so it changes nothing on the PC it runs on and shows nothing on its desktop: with a stand-in, the guard's question after two crashes goes through a file in that folder instead of a window, and the test answers it. With `--real` it uses the PC's own taskbar, but only when `LARES_TEST_REAL_TASKBAR=1` is set, which only the Checks workflow does, on GitHub's throwaway Windows runner; on a PC someone works on, those cases do nothing.

| Case | How | Result | Status |
| :--- | :--- | :--- | :--- |
| Shell quits cleanly, auto-hide off or on before | automatic, stand-in taskbar | taskbar back, auto-hide as before, guard and twin leave | confirmed |
| Shell crashes, is killed, or leaves without a word | automatic, stand-in taskbar | taskbar back in under 20 ms, shell started again | confirmed |
| Shell hangs | automatic, stand-in taskbar | ended and taskbar back after about 12 seconds, shell started again | confirmed; this is the one case slower than two seconds |
| Guard killed | automatic, stand-in taskbar | shell ends with it, taskbar back in under 20 ms, new guard, twin and shell | confirmed |
| Twin killed, then guard | automatic, stand-in taskbar | new twin, taskbar stays hidden; then back when the guard is killed | confirmed |
| Shell, guard and twin killed in all 12 orders of two and three, 250 ms apart | automatic, stand-in taskbar | usable desktop (taskbar back, or a whole new guard, twin and shell) in under 20 ms of the last kill | confirmed |
| The same 12 orders with no time between the kills | automatic, stand-in taskbar | 4 to 10 of 12 come back by themselves, depending on the machine; the others come back from the state file with `lares-guard restore` | confirmed, and a known limit |
| Crash loop: two crashes | automatic, stand-in taskbar | taskbar stays visible, the question is asked, no shell is started; **Try again** starts it, **Keep Windows' own taskbar** ends guard and twin | confirmed for the logic; the message window itself is not shown in tests and was not looked at |
| A shell that cannot be started | automatic | the message, taskbar never hidden | confirmed |
| State left by a guard that is gone | automatic | `lares-guard restore`, and a guard that starts, both give the taskbar back | confirmed |
| Explorer restart | automatic, stand-in: a new taskbar window and the `TaskbarCreated` message | new taskbar hidden again; one shown again by its owner is hidden again | confirmed for the stand-in; a real Explorer restart is **not checked** |
| Display change | automatic, stand-in: a second taskbar and `WM_DISPLAYCHANGE` | both hidden, both back after a crash | confirmed for the stand-in; a real change of screens is **not checked** |
| Sign-out | automatic, stand-in: `WM_QUERYENDSESSION` and `WM_ENDSESSION` sent to the shell | taskbar and auto-hide back, nothing counted as a crash, nothing started again; a sign-out that is called off changes nothing | confirmed for the stand-in; a real sign-out is **not checked** |
| The real Windows taskbar: hidden, back after the shell is killed, back after the guard is killed, back after a clean quit | automatic, on GitHub's Windows runner (Windows Server) | back 51 ms after the shell was killed and 35 ms after the guard was | confirmed on Windows Server |
| The real Windows 11 taskbar | | | **not checked**: on the development PC another program already hides the taskbar, and tests there must not touch it |
| Sleep and wake, remote desktop sessions, more than one real screen | | | **not checked** |

### Manual checks still owed

These need a Windows 11 PC, or a test account, where nothing else hides the taskbar. None has been run.

1. **Real Windows 11 taskbar.** Run `lares-guard-test --real`. Expected: every line passes and the taskbar ends as it started.
2. **Explorer restart.** With `lares-guard run lares-test-shell.exe` running and the taskbar hidden, restart Explorer from Task Manager. Expected: the taskbar that comes back is hidden again within two seconds; ending the stand-in from Task Manager then brings it back.
3. **Display change.** In the same state, change the resolution, then plug in or unplug a second screen. Expected: no taskbar appears on any screen; ending the stand-in brings one back on each.
4. **Sign-out.** In the same state, with auto-hide off beforehand, sign out and in again. Expected: the taskbar is visible, auto-hide is off, and `guard.state` says nothing is hidden and counts no crash.

## Test switches in the guard

Two environment variables exist for the tests and are read by `lares-guard.exe` and the shell's side: `LARES_GUARD_TASKBAR_CLASS` names a window class to treat as the taskbar instead of the real one, and `LARES_GUARD_STATE_DIR` names the folder for `guard.state`. Without them the guard works on the real taskbar and the user's own folder.

## Sources

The taskbar handling is written only from Microsoft's public documentation; the pages are listed at the top of `win/guard/taskbar.c`, `win/guard/guard.c` and `win/guard/guard_client.c`. Microsoft has no reference page for the taskbar's two window class names; the files say which of Microsoft's own articles use them.

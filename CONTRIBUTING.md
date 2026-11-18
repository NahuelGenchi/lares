# Contributing

Keep the work small, traceable, and honest about its current limitations. Agents must also follow [AGENTS.md](AGENTS.md).

## One work item, one delivery trail

1. **Start with an issue.** Reuse the issue that already owns the work, or open the work-item form. Record the goal, scope, completion criteria, dependencies, and milestone before editing files. Bugs, enhancements, documentation, research and repository operations are work items too.
2. **Create a new branch for the issue.** Every work item is developed on its own new branch, created from the latest `main`:

   ```sh
   git fetch origin
   git switch -c feat/7-dualsense origin/main
   ```

   Name it `<type>/<issue>-<slug>`. Types are `feat`, `fix`, `enh`, `docs`, `research` and `ops`. One branch holds one issue: never reuse a merged or existing branch for new work, and never mix two issues on one branch. Use focused commits such as `feat: show a DualSense's battery (#7)`. `main` takes changes only through pull requests. Branches are deleted when their PR merges.
3. **Open a linked PR.** Include a standalone `Closes #N` line for each completed, same-repository issue. Set the PR to the same milestone as its primary issue. Split cross-milestone changes into separate PRs. Use `Refs #N` for unfinished parent or related work; it does not replace a closing reference to the scoped issue.
4. **Record the outcome.** Describe exactly what changed, which checks ran, and what was not run. Close the issue only when its stated scope is complete.

For non-file operations (repository settings, releases), put sanitized commands and results in the issue. When an operation produces no code change, deliver its lasting decision through a small documentation PR instead of an empty PR.

## What belongs where

**Issues** own scope, decisions, blockers, dependencies, and outcomes. **Milestones** group outcomes. **PRs** own the proposed diff and delivery record. **Documentation** owns reusable knowledge, not duplicate task status.

## Checks

`make check` builds Lares and its helper with warnings as errors, runs the unit tests in `tests/`, and parses the top-bar addon's JavaScript and tests its controller picture under Node. The Checks workflow runs the same on every PR, on Ubuntu 26.04 with GTK 4.22, and `main` requires it to pass. It shows that the code builds and that the plain-data parts work, not that the window behaves. A change to those parts comes with a test.

Window checks run Lares in a dry run (`LARES_DRY_RUN=1`), which pretends every change to the screen, services and Bluetooth and keeps its own settings, or in a separate, hidden GNOME Shell with its own settings; they never change your running session. Hardware checks name the device, its connection and its driver, and say whether it was real or simulated. Report each separately in the PR's delivery record, and say what was not run.

A change to how Lares looks is checked in light and dark before it is proposed.

## Private data

Keep Bluetooth addresses, serial numbers, Tailscale addresses and machine names, personal paths, logs that contain them and account credentials out of commits, issues, PRs and screenshots. Lares reads Sunshine's state file only to see whether a device is paired; never log or show anything else from it. If a secret is exposed, stop sharing it and revoke it.

## Documentation standard

Write clear English. Prefer specific paths and actionable instructions. Distinguish **implemented**, **planned**, **blocked**, and **unknown**. Do not invent commands, measurements, or compatibility. Update documentation in the same PR as the change it describes. Keep `README.md` approachable and `AGENTS.md` limited to what agents need in every session.

## Tracking automation and branch rules

The metadata workflow assigns missing milestones and labels PRs that need tracking. Branch rules on `main` require a pull request and the checks, and block force-pushes and deletion. See [docs/TRACKING.md](docs/TRACKING.md).

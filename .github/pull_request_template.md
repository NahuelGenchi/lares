## Tracking

Closes #<issue-number>
Milestone: <same milestone as the primary issue; also set the GitHub sidebar>
Branch: <type>/<issue>-<slug>, new from the latest main and used only for this issue

<!-- Use one standalone Closes #N line per completed same-repository issue. Use Refs #N for unfinished related work. -->

## Changes

<!-- Summarize the concrete change and any important decision. Link instead of repeating the issue. -->

## Delivery record

Build and tests: <none, or `make check` and its result>
Window checks: <none, or how Lares was run (dry run, hidden test shell or real session), the section, light or dark, and the result>
Top-bar and helper checks: <none, or how they were run and the result>
Hardware checks: <none, or the device, its connection and driver, and whether it was real or simulated>
Source/diff review: <actual scope>
Private data: none committed (no Bluetooth addresses, serial numbers, Tailscale addresses or names, personal paths or credentials)

## Before you post

- [ ] No screenshot of a personal desktop: pictures come from a dry run or a clean test account, cropped to Lares.
- [ ] No log, command output or error text with a file path, an account name or a network address in it.
- [ ] No device, computer, Wi-Fi or Tailscale name, here or in the linked issue.
- [ ] `node tools/privacy-gate.mjs` passes, and no program built on a personal PC is attached.

## Limitations and follow-up

<!-- State real blockers, unverified behavior, and follow-up issue links. Write None only when accurate. -->

# 1.8.3 hardware test results - A500, 2026-10-28

Machine: A500 + GVP A530 (68030), AmigaOS 3.2.3, `gvpscsi.device` unit 2 =
BlueSCSI Pico W, firmware with Toolbox capabilities **0x07** (large
transfers, large send, working directory; default dir `/shared`).
Driven unattended over NetHarness 1.18 (no monitor or mouse attached);
timings are wall-clock on the PC side unless marked, which adds roughly
0.8-2.7 s of harness overhead per command. Comparison binaries: the 1.8
builds that were installed on the machine (`SYS:SCSIToolbox`, `L:sharedfs`).

## Summary

| # | Test | 1.8 | 1.8.3 |
|---|------|-----|-------|
| 3 | CLI against empty units 5 and 6 | (not run; guru risk) | PASS - "Error 33 opening SCSI device", clean exit |
| 1 | SDTransfer idle while `List SYS: ALL >NIL:` runs (74 s alone) | **>120 s** (harness timeout, twice) | **74 s** (Amiga `Date`-timed) |
| 2 | Save requester Cancel | writes file to current dir | PASS - no file, gauge unchanged |
| 2 | Save requester Save to RAM: | - | PASS - `mui.key` 5696 bytes |
| 4 | Run `List` directly from `SHARED:` (LoadSeg) | **blocks**: gvpscsi.device "Unexpected Status $49/$46" requester, twice | 0.87 s, repeatable |
| 4 | `Type` 103 KB file `>NIL:` | 23.4 s | 22.9 s (Type-bound, not read-bound) |
| 4 | `Copy` 448 KB `SHARED:` -> `RAM:` | not reached | 2.5 s, size exact |
| 4 | `Copy` 56 KB `RAM:` -> `SHARED:` | not reached | 2.0 s, size exact |
| 4 | Copied executable runs from RAM: | - | PASS |
| 4 | Overwrite file, re-`Type` (cache invalidation) | - | PASS - new content, new length |
| 4 | Subfolder: `Copy SHARED:testdir RAM:td ALL` | - | PASS - 4 files, sizes match |
| 5 | Two uploads at once | silently truncates first | PASS - second refused "object is in use", first intact at 448280 |
| 6 | Mount with wrong unit | - | PASS - fails cleanly, no process left, ~13 KB (the seglist) retained by DOS as normal |
| 7 | SDTransfer 2 min idle soak | - | PASS, window stays up |

Not run: CDChanger Select/Eject (they change the mounted disc), the >100-file listing,
and the dismount/remount hygiene test - `Assign DISMOUNT` does not send
`ACTION_DIE`, so the `dn_Task` clear cannot be exercised that way.

## Notes

- **The old handler trips a gvpscsi.device error on LoadSeg.** Both apparent
  freezes happened with the 1.8 `sharedfs` serving a program load from the
  volume. With a monitor attached, the second one turned out to be a
  `gvpscsi.device: unit 002 Unexpected Status $49/$46` system requester
  waiting for Resume. Everything behind it was blocked, so the network went
  quiet; after Resume the machine carried on with RAM: intact. The new handler
  ran the identical command repeatedly without it. The page cache turns
  LoadSeg's dozens of small reads into one GET_FILE per 4 KB page, which is
  the only difference on that path. Why the firmware and driver produce that
  status under rapid repeated GET_FILE of the same page is not established.
- One unexplained reboot occurred with the *new* SDTransfer open idle while
  a 2-minute `List` ran and the harness was timing out and Ctrl-C-stopping it.
  A two-minute idle soak plus the same `List` repeated afterwards (timed on
  the Amiga, no harness timeout) did not reproduce it.
- `Type` to NIL: is dominated by Type itself on this machine (23 s for 103 KB
  with either handler), so it is not a useful read benchmark. Use the
  LoadSeg test or `Copy`.
- The firmware `SEND_FILE_PREP` on an existing name truncates and rewrites it;
  overwriting a file in place through `SHARED:` works.
- The A500 takes up to ~18 minutes after a power cycle before its network
  stack answers; the second boot was much faster.
- `Run >log Execute script` captures only Run's own line, not the script's
  output, so `tests/SharedSpeed` must be run from a Shell window (or its
  steps run one at a time over the harness and timed on the host).

## 1.9 GadTools GUIs (same session)

| Test | Where | Result |
|------|-------|--------|
| SDTransfer and CDChanger open, logo/list/buttons/status laid out | KS/WB 3.1, no ReAction (WinUAE `prism os31`, TESTMODE build) | PASS, CPU idle 0% |
| 1.8.2 SDTransfer on the same machine | same | stops at "Could not open window.class" (control) |
| SDTransfer opens centred under the pointer | A500 3.2.3 | PASS |
| Double-click "/" goes to the SD root, status "Dir: /" | A500 | PASS |
| Click selects (highlight), button descends into `shared/` | A500 | PASS |
| Save requester in save mode over the window; Cancel writes nothing | A500 | PASS |
| Download 5.6 KB and 448 KB to RAM:, progress bar (seen at 29%), final status | A500 | PASS, sizes exact |
| Resize larger and down to the minimum; gadgets rebuilt, selection kept | A500 | PASS, minimum 212x200 |
| Esc quits; working dir back at `/shared` | A500 | PASS |
| CDChanger on the CD-ROM unit (3) lists the ISOs; close gadget quits | A500 | PASS (Select/Eject not pressed) |

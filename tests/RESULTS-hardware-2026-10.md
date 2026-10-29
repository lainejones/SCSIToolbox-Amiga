# Hardware test results, 1.8.3 and 1.9 - A500 (BlueSCSI) and A1200 (ZuluSCSI), 2026-10-28

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

## A1200 + ZuluSCSI, and the upload-offset fix (2026-10-28)

Machine: A1200, TF1232 68030/50, AmigaOS 3.2.3, ZuluSCSI on Laine's own
`dataflyer.device` (CD = unit 2, FLY0 = 3, DATA2 = 4). Toolbox capabilities
0x03: no working directory, so flat folder only. Driven over NetHarness
through the A314 Pi (192.168.50.33:7802).

| Test | Result |
|------|--------|
| CLI INFO / DIR; empty units 0 and 6 | PASS - "Error 32 opening SCSI device", clean exit |
| SDTransfer: Zulu logo, flat list, Cancel writes nothing, Save exact, Esc | PASS |
| CDChanger on the CD unit lists the ISOs; close gadget | PASS (Select/Eject not pressed) |
| Device line after the 1.9 fix: "dataflyer.device, unit 3" in full | PASS |
| SHARED: list, read, run a program from it, copies both ways | PASS |
| Two uploads at once | PASS - second refused "object is in use", first intact |
| **Overwrite an existing file (1.9 before the fix)** | **FAIL** - 55973-byte file over itself -> 102565, then 143525; shorter over longer kept the old tail |
| Overwrite after the fix, SHARED: and CLI SEND | refused, "object already exists" / message; old file unchanged |
| Fresh uploads after the fix, 55 KB and 2.2 MB, read back | PASS - MD5 identical |
| Same on the A500 BlueSCSI: fresh, overwrite longer-then-shorter, big-with-small, CLI SEND twice | PASS - exact sizes, MD5 identical |

Cause of the overwrite failure, from the firmware sources: every firmware
(BlueSCSI v1 and v2, ZuluSCSI) applies the SEND_FILE_10 offset with
`seekCur`, relative to the last write. The client sent the absolute block
number, which worked on new files only because each seek ran past the end
and failed. ZuluSCSI also opens an existing file without truncating it, so
the seeks succeeded and opened gaps. The client now sends 0, and refuses to
overwrite on ZuluSCSI because the missing truncate cannot be fixed from the
Amiga side.

Old 1.8 handler against 1.9, same machine and files, timed on the Amiga
with `Date` (1 s resolution):

| Test | 1.8 | 1.9 |
|------|-----|-----|
| Run `List` (12 KB) from SHARED: x10 | 16 s | about 1 s |
| Copy 2.2 MB SHARED: -> RAM: x3 | 26 s | 26 s (about 255 KB/s) |
| `Type` 33 KB text >NIL: x10 | 26 s | 11 s |

The old handler did not raise the "Unexpected Status" requester here; that
was only seen with the A500's `gvpscsi.device`.

Test files left on the SD cards (the Toolbox has no delete): on the ZuluSCSI
`up_A.lha`, `up_C.lha`, `up_z_Roadie.lha`, `ow_test.lha`, `ow_cli.lha`,
`note.txt`, `List`, `fix_*`; on the BlueSCSI `/shared` `up_new_Roadie.lha`,
`up_A.lha`, `note.txt`, `List`, `fix_*`.

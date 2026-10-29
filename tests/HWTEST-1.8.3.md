# Hardware test plan for 1.8.3

Nothing in 1.8.3 can be tested in an emulator: vamos has no SCSI and WinUAE has
no BlueSCSI/ZuluSCSI. Every test below runs on a real Amiga with the device
attached. The A1200 (DataFlyer `ExpXDS.device`, ZuluSCSI on unit 2) is assumed;
adjust `DEVICE=`/`UNIT=` for other setups.

Keep the **1.8.2 binaries** in a separate drawer (e.g. `RAM:old/`) so each
speed test can be run with both versions back to back. Timing uses `Date`
(1-second resolution), so the loops are sized to take tens of seconds.

The script `tests/SharedSpeed` automates the timed parts (section 4).

---

## 1. SDTransfer idle CPU (busy-loop fix)

**Setup:** copy a CPU-bound command to RAM: so disk speed does not matter, e.g.
`Copy C:List RAM:` and a big directory to list, or use CPUMeter if installed.

1. Note the baseline: in a Shell, `Date` then `List SYS: ALL >NIL:` then
   `Date`. Write down the elapsed seconds.
2. Launch **1.8.2 SDTransfer** and leave it open, idle. Repeat step 1.
   Expected (old bug): noticeably slower, often 1.5-2x on a 68020.
3. Quit it, launch **1.8.3 SDTransfer**, leave it idle. Repeat step 1.
   Expected: same as the baseline.

Alternative without timing: with SDTransfer open, run `Status` (or Scout/
Xoper if installed). The old build shows the SDTransfer task constantly in the
ready/running state; the new one sits waiting.

Also confirm the loop still wakes up for everything it should:
- clicking rows, Open/Download, double-click navigation;
- dropping a file icon on the window (AppWindow signal);
- Ctrl-C in the Shell it was started from, and Esc in the window, both quit.

## 2. SDTransfer Save-requester cancel

1. `CD RAM:` in a Shell and launch SDTransfer from it (`SDTransfer DEVICE=ExpXDS.device UNIT=2`).
2. Select a file, press **Open / Download**, then press **Cancel** in the ASL
   requester.
3. Expected: `List RAM:` shows no new file, the gauge text is unchanged, and the
   window still responds. (1.8.2 wrote the file to RAM: under its remote name.)
4. Repeat and press **Save** this time: the file arrives where you chose, and
   the gauge reports the byte count.

## 3. CLI CloseDevice guard

Pick a unit that has no device on the bus (e.g. unit 5 here).

```
BlueSCSIToolbox DEVICE=ExpXDS.device UNIT=5 DIR
```

Expected: `Error <n> opening SCSI device ... unit 5`, prompt returns, no guru.
(1.8.2 could Software Failure #80000003 on exit, depending on the driver.)
Then confirm the normal path still works: `... UNIT=2 DIR` lists the shared
folder, and `... UNIT=2 INFO` prints API version and capabilities.

## 4. sharedfs page cache (speed)

Mount with the **old** handler first (`Copy RAM:old/sharedfs L:` then
`Mount SHARED:`), run `Execute tests/SharedSpeed`, note the times, then
`Assign SHARED: DISMOUNT` (or reboot), install the new `sharedfs`, mount, run
again.

What the script measures, and what to expect:

| Test | What it stresses | 1.8.2 -> 1.8.3 expectation |
|------|------------------|----------------------------|
| `Type` of a ~100 KB text file to NIL: 5x | sequential reads in small chunks | several times faster |
| Running a ~50-100 KB executable from `SHARED:` 10x | LoadSeg hunk parsing (4-byte reads) | large speedup, this was the worst case |
| `Copy` of a ~1 MB file `SHARED:` -> `RAM:` | large reads (Copy uses big buffers) | roughly unchanged (not the cache's target) |
| `Copy` of a ~1 MB file `RAM:` -> `SHARED:` | upload path | unchanged (512-byte blocks either way) |

Then verify correctness of what was read through the cache:
- run the copied executable from RAM: (hunk loader accepts it = intact);
- `Copy SHARED:big RAM:big2` then `Copy RAM:big RAM:big3` from the original
  source and compare sizes with `List`; if a checksum tool is available
  (`md5sum`, `Sum`), compare checksums, or pull the SD card and check on the PC.

Cache invalidation (must not serve a stale page):
1. `Type SHARED:note.txt`.
2. On the PC side (or via `BlueSCSIToolbox SEND=`), overwrite `note.txt` with
   different content of a different length.
3. `Type SHARED:note.txt` again. Expected: the new content. (Opening the file
   drops the cache; so does any other SCSI command.)

## 5. sharedfs upload guard (stress)

Two uploads at once, which 1.8.2 silently corrupted:

1. Open two Shells. In each: `Copy RAM:big SHARED:bigA` and
   `Copy RAM:big SHARED:bigB`, started as close together as possible (or
   `Run >NIL: Copy RAM:big SHARED:bigA` then immediately the second in the same
   Shell).
2. Expected: one copy completes, the other fails with **"object in use"** and
   writes nothing. `List SHARED:` shows the completed file with the full size.
   Nothing truncated.
3. Then copy the second file on its own: it succeeds (the guard clears on
   close).

Many files in sequence (no guard interference):

```
Copy RAM:somedrawer/#? SHARED: ALL
List SHARED:
```

Expected: every file present with the correct size.

Error reporting on close: hard to provoke on demand; a full SD card or pulling
the card mid-copy should make `Copy` report "disk full" instead of succeeding
silently.

## 6. sharedfs mount/unmount hygiene

1. Mount with a wrong unit in the mountlist (e.g. unit 5). Expected: Mount
   reports a failure, no handler process lingers (`Status`), and `Avail FLUSH`
   before/after shows memory returned.
2. Mount correctly, use it, then `Assign SHARED: DISMOUNT` (OS 3.2) and
   reference `SHARED:` again (`Dir SHARED:`). Expected: the handler is
   restarted cleanly by DOS, not a crash from a packet sent to the old port.
   (1.8.2 left `dn_Task` pointing at the dead process.)

## 7. List edge cases

- An empty shared folder: `Dir SHARED:` and SDTransfer both show an empty list,
  no garbage rows, no crash.
- More than 100 files in the shared folder (firmware caps listings at 100):
  listing shows 100, no crash. 1.8.3 additionally clamps at 102 entries on the
  client side.

---

## Reporting

For each section note PASS/FAIL and, for section 4, the `Date` deltas for old
vs new. Anything that fails: capture the Shell output, and for a guru the
error number.

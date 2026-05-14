# BlueSCSI Toolbox Amiga 1.3 — Build Notes

## Project Overview

Cross-compiled AmigaOS 3.x GUI tools for BlueSCSI/ZuluSCSI SCSI emulators:
- **CDChanger** — lists CD images on BlueSCSI, lets user switch the active image
- **SDTransfer** — lists files in the BlueSCSI shared folder, downloads them to Amiga

Target hardware: Amiga 1200, 68020, **AmigaOS 3.2.3** (Hyperion 68k branch — NOT AmigaOS 4.x which is PowerPC).

SCSI device: **ExpXDS.device** (Dataflyer SCSI adapter for A1200), **unit 2** (BlueSCSI).

A pre-compiled v1.3 binary from the original author works correctly on this hardware.

---

## Toolchain

- Cross-compiler: `/opt/amiga/bin/m68k-amigaos-gcc`
- NDK headers: `/opt/amiga/m68k-amigaos/ndk-include/`
- NDK version: **47.x** — this IS the correct SDK for AmigaOS 3.2.3 (Hyperion maintains
  both the 68k 3.x branch and PPC 4.x branch; version 47 refers to exec.library version,
  not OS generation)
- Build: `make 68020` from `src/`

Key CFLAGS: `-DNO_INLINE_STDARG -D__NOLIBBASE__ -O3 -std=c99 -m68020 -ffunction-sections -fdata-sections`

---

## Confirmed Struct Layout (compiler-verified)

Computed by compiling an offsetof test with the actual cross-compiler:

| Field | Offset |
|---|---|
| `sizeof(struct Task)` | **92** bytes |
| `sizeof(struct MsgPort)` | **34** bytes |
| `offsetof(struct Process, pr_ReturnAddr)` | **176** (0xB0) |
| `offsetof(struct Process, pr_PktWait)` | **180** (0xB4) |
| `offsetof(struct Process, pr_WindowPtr)` | **184** (0xB8) |

The NDK 47.x struct Task is IDENTICAL to the classic AmigaOS 3.x layout (92 bytes,
same fields, no extensions). The struct layouts match the 3.2.3 runtime.

---

## Problem 1: window.library auto-open crash — FIXED

**Symptom:** Program printed "window.library failed to load" and crashed before `main()`.

**Cause:** `libstubs.a` in NDK 47.x has an auto-open constructor that tries to open
`window.library` (an AmigaOS 4.x GUI class). This library does not exist on OS 3.x.

**Fix:** Added `-D__NOLIBBASE__` to CFLAGS. This suppresses libstubs.a auto-open
constructors. Requires:
1. `toolbox.h` included FIRST in every source file (provides mandatory externs)
2. All `*Base` globals defined with `= NULL` initializer (makes them STRONG symbols,
   preventing libstubs.a from pulling in its STRONG definitions):
   ```c
   struct Library *WindowBase = NULL, *LayoutBase = NULL, ...
   struct Library *UtilityBase = NULL, *IntuitionBase = NULL, ...
   ```
   Without `= NULL`, globals are COMMON symbols and the linker picks libstubs.a's
   STRONG definition, which includes the broken auto-open constructor.

---

## Problem 2: CloseDevice crash on failed OpenDevice — FIXED

**Symptom:** Pressing any button in the volume requester caused "Software Failure
error #80000003".

**Cause:** When the user pressed Cancel/Deny in the volume requester, `OpenDevice()`
returned failure. The program called `scsi_cleanup()`, which called `CloseDevice()` on
`io_ptr` even though the device was never successfully opened.

The Dataflyer ExpXDS.device sets `io_ptr->io_Device` (non-NULL) even when `Open()`
fails — non-standard driver behaviour. Checking `io_ptr->io_Device != NULL` was not
sufficient because the Dataflyer sets it regardless.

**Fix:** Added `static int scsi_deviceOpened = 0` flag in `scsi.c`. Set to `1` only
when `OpenDevice()` returns 0 (success). `scsi_cleanup()` calls `CloseDevice()` only
when `scsi_deviceOpened == 1`.

```c
// In scsi_setup():
if (OpenDevice(...) != 0) return -1;
scsi_deviceOpened = 1;          // ← only set on success

// In scsi_cleanup():
if (io_ptr) {
    if (scsi_deviceOpened) {
        CloseDevice((struct IORequest *)io_ptr);
        scsi_deviceOpened = 0;
    }
    DeleteIORequest(io_ptr);
}
```

---

## Problem 3: Volume requester appearing — UNSOLVED

**Symptom:** When CDChanger/SDTransfer run, an AmigaDOS requester appears:
"Please insert volume [NsH*****\n0a:] in any drive"
Buttons: **Retry**, Assign, Deny, Cancel.

**What the volume name means:** The garbled name (with embedded newline 0x0A) is the
AmigaDOS partition label stored in the Dataflyer's RDB (Rigid Disk Block) for unit 2.
It is NOT in our code. It was set when the SCSI hardware was originally configured.

**Why it appears:** The Amiga has an AmigaDOS filesystem handler process running for
ExpXDS.device unit 2. When our program opens the raw SCSI device, something triggers
the filesystem handler to check for its volume. It can't find/match it and shows the
requester.

**Why the pre-compiled v1.3 binary doesn't show it:** Unknown. The v1.3 binary likely
had a working suppression mechanism, or was compiled with different flags that avoid
triggering the filesystem handler.

### Suppression attempts — all failed or caused crashes

**Setting `pr_WindowPtr = -1` for our own process only:**
- Tried in `scsi_setup()` before `OpenDevice()` — requester still appeared
- Tried at start of `main()` before `OpenLibrary()` — requester still appeared (and
  later version crashed, possibly unrelated)
- Conclusion: requester does NOT come from our process context. It comes from the
  filesystem handler's own process context. Our `pr_WindowPtr` has no effect on it.

**Walking exec task lists with `Forbid()` to suppress ALL process `pr_WindowPtr`:**
- Walked `SysBase->TaskReady` and `SysBase->TaskWait`, set `pr_WindowPtr = -1` for
  every `NT_PROCESS` task
- **CRASHED to black screen (CPU exception, hard lockup)**
- Root cause unknown — writing to foreign process structures under `Forbid()` hit
  something fatal. Do NOT attempt this approach again.

**Inline asm writing to offset 184 of foreign processes:**
- Earlier attempts in previous session also caused black screen crashes
- Pattern: writing to `pr_WindowPtr` of ANY process other than our own causes hard
  lockups. Only safe to write to our own process.

**`LockDosList` / `NextDosEntry`:**
- NOT available in the NDK 47.x inline/dos.h headers. Cannot use this approach.

**`Inhibit(name, TRUE)`:**
- IS available in inline/dos.h
- Requires knowing the AmigaDOS device name (e.g. "CD0:") for unit 2
- User does not know their device name. The requester shows the VOLUME name, not the
  device name.
- Could be added as an optional CLI parameter if user finds their device name.

### Current behaviour (latest build)

- Requester appears
- **Retry**: AmigaDOS retries finding the volume. If BlueSCSI has a CD image mounted,
  it may succeed → `OpenDevice` succeeds → CDChanger should open. **Try this.**
- **Deny/Cancel**: `OpenDevice` fails → `scsi_setup` returns -1 → program exits
  cleanly (no crash, thanks to `scsi_deviceOpened` fix)

---

## What CRASHES the system (do not attempt)

1. **Writing `pr_WindowPtr` to any process other than our own** — always produces
   hard lockup / black screen requiring power cycle. Mechanism unknown but repeatable.
2. **`Forbid()` + task list walk + write to foreign process** — same hard lockup.
3. **C struct `->pr_WindowPtr` on FindTask result of foreign process** — same.
4. **Inline asm writing to hardcoded offset 184 of foreign process pointer** — same.

Only safe: `((struct Process *)FindTask(NULL))->pr_WindowPtr = (APTR)-1L` on our OWN
process. But this alone does not suppress the requester (different process context).

---

## Current State of Source Files

### `src/scsi.c`
- `static int scsi_deviceOpened = 0` — guards CloseDevice
- `scsi_setup()`: sets `scsi_deviceOpened = 1` after successful `OpenDevice()`
- `scsi_cleanup()`: uses `scsi_deviceOpened` flag before calling `CloseDevice()`
- No `suppress_dos_requester` code (removed — was causing crashes or doing nothing)

### `src/CDChanger.c` / `src/SDTransfer.c`
- All `*Base` globals have `= NULL` initializers
- `char scsi_msg[128]` (increased from 50 to prevent overflow)
- No `suppress_dos_requester` calls

### `src/toolbox.h`
- `-D__NOLIBBASE__` extern declarations for SysBase, DOSBase, IntuitionBase, UtilityBase
- Must be the FIRST include in every source file

---

## Next Steps to Try

1. **Test Retry button**: With `scsi_deviceOpened` fix in place, pressing Retry should
   be safe. If BlueSCSI has a CD image, AmigaDOS should find it and CDChanger should
   open. Report whether CDChanger window appears.

2. **If Retry works but requester is annoying**: Add optional `INHIBIT/K` CLI parameter
   to CDChanger/SDTransfer. User specifies their AmigaDOS CD device name (find it via
   `List DEVS:DosDrivers/` or HDToolBox). Program calls `Inhibit(name, TRUE)` before
   `scsi_setup()` and `Inhibit(name, FALSE)` at cleanup.

3. **If Retry doesn't work**: Investigate whether `OpenDevice` itself has issues
   independent of the requester. Try without a CD image loaded to see if OpenDevice
   succeeds when there's no media (and thus no volume mismatch).

4. **If program crashes after requester**: Report exactly WHEN the crash happens
   (before requester / after pressing Retry / after CDChanger window appears). This
   narrows down which part of the code is crashing.

---

## Notes on AmigaOS Version

- AmigaOS 3.2.3 = latest Hyperion 68k branch (newer by release date than 4.0)
- AmigaOS 4.x = PowerPC only (different hardware, different branch)
- NDK 47.x covers BOTH branches — it IS correct for 3.2.3 development
- struct layouts in NDK 47.x headers match AmigaOS 3.2.3 runtime exactly

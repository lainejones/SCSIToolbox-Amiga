# BlueSCSI Toolbox Amiga 1.3 — Build & Debug Notes

## Project Overview

Cross-compiled AmigaOS 3.x GUI tools for BlueSCSI/ZuluSCSI SCSI emulators:
- **CDChanger** — lists CD images on BlueSCSI, lets user switch the active image
- **SDTransfer** — lists files in the BlueSCSI shared folder, downloads them to Amiga
- **BlueSCSIToolbox** — CLI tool; `LISTCDS`, `SETCD N`, `DIR`, `RECEIVE`, etc.

Target hardware: Amiga 1200, 68020, **AmigaOS 3.2.3** (Hyperion 68k branch — NOT AmigaOS 4.x).

SCSI device: **ExpXDS.device** (Dataflyer SCSI adapter for A1200), **unit 2** (BlueSCSI).

---

## Toolchain

```powershell
# Must set PATH first in PowerShell every session:
$env:PATH = "C:\amiga-gcc\bin;" + $env:PATH

# Build from src/ directory:
C:\amiga-gcc\bin\_make.exe SHELL="C:\amiga-gcc\bin\sh.exe"          # 68000 (default)
C:\amiga-gcc\bin\_make.exe SHELL="C:\amiga-gcc\bin\sh.exe" 68020    # 68020/030
```

- Cross-compiler: `C:\amiga-gcc\bin\m68k-amigaos-gcc` (version 6.5.0b)
- NDK headers: `D:\Amiga\Include_H` (NDK 47.x — correct for AmigaOS 3.2.3)
- Key CFLAGS: `-DNO_INLINE_STDARG -O2 -std=c99 -m68000 -ffunction-sections -fdata-sections`
- Key LFLAGS: `-lamiga -Wl,--gc-sections -s`

**Note:** `make` is not in PATH by default under Windows — always call `_make.exe` directly.

---

## AmigaOS Version Notes

- AmigaOS 3.2.3 = latest Hyperion 68k branch
- NDK 47.x covers both 3.x and 4.x — it IS correct for 3.2.3 development
- Struct layouts in NDK 47.x match AmigaOS 3.2.3 runtime exactly

---

## Fixed Problems

### Fix 1 — window.library auto-open crash

**Symptom:** Crashed before `main()` with "window.library failed to load".

**Cause:** `libstubs.a` constructor tries to open `window.class` (AmigaOS 4.x only).

**Fix:** `-DNO_INLINE_STDARG` in CFLAGS + all `*Base` globals declared with `= NULL`
initializer (makes them STRONG symbols, preventing libstubs.a from inserting its own
STRONG auto-open definition):

```c
struct Library *WindowBase = NULL, *LayoutBase = NULL, *ListBrowserBase = NULL;
struct Library *UtilityBase = NULL, *IconBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
```

---

### Fix 2 — CloseDevice crash on failed OpenDevice

**Symptom:** "Software Failure #80000003" when pressing Cancel on volume requester.

**Cause:** `scsi_cleanup()` called `CloseDevice()` on `io_ptr` even when `OpenDevice()`
had failed. The Dataflyer ExpXDS.device sets `io_ptr->io_Device` non-NULL even on
failure, so checking `io_ptr->io_Device != NULL` is insufficient.

**Fix:** `static int scsi_deviceOpened = 0` flag in `scsi.c`. Set to 1 only on
successful `OpenDevice()`. `scsi_cleanup()` guards `CloseDevice()` with this flag.

---

### Fix 3 — Volume requester on startup

**Symptom:** "Please insert volume [NsH*****\n0a:] in any drive" requester appeared
when CDChanger/SDTransfer launched.

**Fix:** `myproc->pr_WindowPtr = (APTR)-1L` at the very start of `main()`, before any
DOS activity:

```c
struct Process *myproc = (struct Process *)FindTask(NULL);
myproc->pr_WindowPtr = (APTR)-1L;
```

**IMPORTANT — what CRASHES the system (do not attempt again):**
- Writing `pr_WindowPtr` to ANY process other than our own → hard lockup / black screen
- `Forbid()` + task list walk + write to foreign process → same hard lockup
- `LockDosList` is NOT available in this NDK's inline headers

---

### Fix 4 — ListBrowser `#` column showing "0" for all rows

**Symptom:** The `#` column displayed "0" on every row instead of 1, 2, 3…

**Root cause — `LBNCA_Integer` tag semantics:**
`LBNCA_Integer` takes a `LONG *` pointer, NOT an integer value. Without
`LBNCA_CopyInteger=TRUE`, the listbrowser stores the pointer and dereferences it at
render time — passing a value directly causes it to read from low memory
(exception vectors), producing garbage (8, 2048, 524297…).
`LBNCA_CopyInteger=TRUE` proved unreliable too — reads stale stack values at scroll.
**Do not use `LBNCA_Integer`.**

**Root cause — `Strncpy` overflow into adjacent struct field:**
AmigaOS `Strncpy(dest, src, n)` writes up to n+1 bytes (including null terminator).
`file->Name[32+1]` is immediately followed by `file->Number[5+1]` in the struct.
`Strncpy(file->Name, ..., 32)` could overwrite `file->Number[0]`, corrupting it.

**Fix:** Use a static array of compiled-in string literals for the `#` column. These
pointers are valid for the entire program lifetime so no `LBNCA_CopyText` is needed:

```c
static const char * const g_nums[64] = {
    "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",
    "9",  "10", "11", "12", "13", "14", "15", "16",
    /* ... up to "64" */
};
```

Also moved `sprintf(file->Number, "%d", f+1)` to AFTER `Strncpy` in `scsi.c` so the
overflow (if any) happens before Number is written.

---

### Fix 5 — Wrong CD selected for positions 10 and above

**Symptom:** Selecting disc #10 unmounted the disc; #12 mounted disc #3's image.

**Root cause A — rogue `WMHI_GADGETDOWN` handler:**
A `WMHI_GADGETDOWN` case was added that called `Toolbox_Set_Next_CD` immediately on
any gadget click. At `GADGETDOWN` time, `LISTBROWSER_SelectedNode` still holds the
*previously* selected node, not the one just clicked. This sent the wrong disc index,
especially when the list had scrolled. The original CDChanger had no `WMHI_GADGETDOWN`
handler. **Removed.**

**Root cause B — alphabetical sort breaking device index mapping:**
The device returns CD images in write-timestamp (directory) order and `c[0]`
(`file->Index`) is the sequential 0-based position in that order. After an alphabetical
`qsort`, the display position no longer matched `file->Index`: display #12 sent
`file->Index = 2` (device position 2 = 3rd disc in timestamp order) → mounted disc 3.

**Fix:** Remove the `qsort`. Display ISOs in the order the device returns them.
`file->Index` is then always correct for `SET_NEXT_CD`.

**Note:** `BlueSCSIToolbox LISTCDS` and `SETCD` sort alphabetically internally, which
means their display numbers do NOT match CDChanger's display numbers (or the device
order). This is a pre-existing issue in BlueSCSIToolbox that is not ours to fix.

---

### Fix 6 — Window opening at bottom of Workbench

**Symptom:** Window opened wherever the mouse cursor happened to be (bottom of screen
if mouse was there).

**Fix:** `WINDOW_Position, WPOS_TOPLEFT` instead of `WPOS_CENTERMOUSE`.

---

### Fix 7 — CDChanger window too large

**Symptom:** CDChanger window opened at 400×350 instead of original 250×250.

**Fix:** `WA_Width, 250` and `WA_Height, 250`.

---

### Fix 8 — SDTransfer binary 61 KB (vs 22 KB for others)

**Symptom:** SDTransfer was ~3× larger than CDChanger and BlueSCSIToolbox.

**Cause:** A single `snprintf()` call pulled in newlib's full floating-point printf
machinery even though no float format specifiers were used:
- `__svfprintf_r` — 9,460 bytes
- `__dtoa_r` — 4,880 bytes
- Locale/wide-char tables (`___loadlocale`, `___utf8_mbtowc`, etc.) — ~14 KB total

`sprintf()` uses the lighter integer-only `__svfiprintf_r` (4,366 bytes) when no float
formats are present.

**Fix:** Replace `snprintf(buf, size, ...)` with `sprintf(buf, ...)`. Verify the buffer
is large enough manually. Result: 61 KB → 22 KB.

**Rule:** Never use `snprintf`/`printf` if `sprintf`/`Printf` will do — on this
toolchain `snprintf` always links the full float printf engine.

---

### Fix 9 — SDTransfer Workbench startup didn't read tooltypes reliably

**Cause:** Used `(struct WBStartup *)argv` (SAS/C convention) but GCC toolchain passes
the WBStartup pointer in `argv[0]`. Also missing `CurrentDir(wbarg->wa_Lock)` before
`GetDiskObject`, causing icon file not found in some directory contexts.

**Fix:** Match CDChanger's pattern:

```c
struct WBStartup *WBenchMsg = (struct WBStartup *)argv[0];  // GCC toolchain
/* ... */
BPTR oldlock = CurrentDir(wbarg->wa_Lock);
dobj = GetDiskObject(wbarg->wa_Name);
CurrentDir(oldlock);
```

---

### Fix 10 — CLI upload (`SEND`) never worked: wrong SCSI direction + length

**Symptom:** `BlueSCSIToolbox ... SEND=file` did not actually write the file to the
shared folder (upstream 1.3; same bug in the bundled reference copy).

**Root cause — data direction:** `Toolbox_PutFileByName` issued `SEND_FILE_PREP`,
`SEND_FILE_10` and `SEND_FILE_END` with `SCSIF_READ | SCSIF_AUTOSENSE` — copied from
the read commands. These are **DATA-OUT** (host→device). In `<devices/scsidisk.h>`,
`SCSIF_WRITE = 0` (direction out), `SCSIF_READ = 1` (direction in); so `SCSIF_READ`
set up an inbound data phase for an outbound command.

**Root cause — transfer length:** every call passed `scsi_Length = MAX_DATA_LEN`
(4096) regardless of the real byte count. On a write the adapter clocks out exactly
`scsi_Length` bytes, so the firmware's data phase never matched.

**Firmware semantics** (from BlueSCSI `BlueSCSI_Toolbox.cpp`, verified upstream):
- `SEND_FILE_PREP` (0xD3): DATA-OUT = **exactly 33 bytes** (`32+1`) = filename.
- `SEND_FILE_10` (0xD4): DATA-OUT = the block's byte count. Legacy mode (`CDB[6]=0`):
  count in `CDB[1..2]`, 512-byte block index in `CDB[3..5]` (big-endian). Block mode
  (`CDB[6]>0`, needs `CAP_LARGE_SEND`): `CDB[6]×512` bytes.
- `SEND_FILE_END` (0xD5): no DATA-OUT (length 0).

**Fix:** direction `SCSIF_WRITE | SCSIF_AUTOSENSE`; length = 33 for PREP, the chunk
size (≤512, legacy mode) for SEND_FILE_10, 0 for END. **Still needs hardware testing.**

**Reusable primitives:** the same corrected protocol is now in `scsi.c` as
`Toolbox_Send_Prep` / `Toolbox_Send_Block` / `Toolbox_Send_End` (+ `Toolbox_Send_File`
wrapper), UI-agnostic (return 0 / SCSI `io_Error`), for the forthcoming `SHARED:`
filesystem handler. Multi-block `GET_FILE` reads (CDB[6]=block count, needs
`CAP_LARGE_TRANSFERS` + a larger buffer) are **deferred** to the handler's read path.

---

## SCSIToolbox-Amiga fork (branch `scsitoolbox-amiga`)

This repo is a GPL-3.0-or-later fork of Paul Hill's *BlueSCSI-toolbox-Amiga* by
**Laine Jones** ([lainejones](https://github.com/lainejones/)), adding a mountable
`SHARED:` handler. Upstream:
<https://github.com/paulroberthill/BlueSCSI-toolbox-Amiga>. See `README.md`,
`ATTRIBUTION.md`, and `LICENSE-HEADER.txt`. New files use the GPLv3 header with
© 2026 Laine Jones; modified files keep Paul Hill's © and add a `Modified … by Laine
Jones (lainejones)` line.

**Build note (D: NDK offline):** the Makefile points `INCLUDES` at `D:\Amiga\Include_H`
(NDK 47.x, the OS 3.2.3 reference). When D: is not mounted, the CLI + `scsi.o` compile
and link cleanly against amiga-gcc's bundled headers instead:
`-IC:\amiga-gcc\m68k-amigaos\ndk-include`. Prefer the D: NDK for official/release builds.

---

## Phase 2 — `SHARED:` handler (design notes, in progress)

Goal: an AmigaDOS **packet handler** (`sharedfs.c`) that mounts the shared folder as a
read/write volume. Decisions: raw `dos.library` packet loop (no filesysbox dependency);
**flat folder first** (subdirs via SET_WORKING_DIR later); reuse `scsi.c` for the SCSI
protocol.

**Action codes (verified from this NDK's `dos/dosextens.h` + wiki):**
`ACTION_STARTUP`=0, `ACTION_DIE`=5, `ACTION_LOCATE_OBJECT`=8, `ACTION_FREE_LOCK`=15,
`ACTION_COPY_DIR`=19, `ACTION_PARENT`=29, `ACTION_EXAMINE_OBJECT`=23,
`ACTION_EXAMINE_NEXT`=24, `ACTION_READ`=82, `ACTION_WRITE`=87, `ACTION_FINDINPUT`=1005,
`ACTION_FINDOUTPUT`=1006, `ACTION_END`=1007, `ACTION_SEEK`=1008, `ACTION_IS_FILESYSTEM`=1027,
`ACTION_INFO`=26, `ACTION_CURRENT_VOLUME`=7.

**Structure (verified):** handler runs as a process; its `pr_MsgPort` receives DOS
packets (a `Message` whose `mn_Node.ln_Name` points to the `DosPacket`). First packet is
the startup (`ACTION_STARTUP`). `FileSysStartupMsg` gives `fssm_Unit`, `fssm_Device`
(BSTR), `fssm_Environ`. The handler sets the `DeviceNode`'s `dn_Task` to its port, creates
a `DLT_VOLUME` DosList node, replies to the startup packet, then loops on packets until
`ACTION_DIE`.

**Startup packet — RESOLVED (verified against fat95-3.23 `src/fat95.s` ~line 1082):**
```
FindTask(NULL) -> proc;  port = &proc->pr_MsgPort
WaitPort(port); msg = GetMsg(port)
pkt  = (struct DosPacket *)msg->mn_Node.ln_Name      ; ln_Name -> DosPacket
dn   = (struct DeviceNode *)BADDR(pkt->dp_Arg3)      ; dp_Arg3 = BPTR DeviceNode
fssm = (struct FileSysStartupMsg *)BADDR(dn->dn_Startup)
unit = fssm->fssm_Unit;  device = BSTR fssm->fssm_Device;  env = BADDR(fssm->fssm_Environ)
... open underlying device ...
dn->dn_Task = port                                   ; become the handler
reply: pkt->dp_Res1 = DOSTRUE (-1) [or 0 + dp_Res2=error on failure]
then loop: WaitPort/GetMsg, dispatch pkt->dp_Type, until ACTION_DIE.
```
Reply idiom: save `dp_Port`, set `dp_Port = &proc->pr_MsgPort`, `msg->ln_Name = pkt`,
`PutMsg(savedPort, msg)`.

**C-handler build (no CLI startup) — IMPLEMENTED:** fat95 is asm with its own entry. Our
C handler builds **`-nostartfiles`** (the normal amiga-gcc/libnix startup would wait for a
WBStartup msg and misread the ACTION_STARTUP packet — classic hang).

**GOTCHA (cost a rebuild to find):** an Amiga hunk executable starts at **offset 0 of the
first code hunk — there is no entry symbol**. amiga-gcc emits **string literals (and switch
jump-tables) into `.text`**, so they can land at offset 0 and get "executed". Putting the C
entry first in the source is NOT enough (the rodata still precedes it). Solution: a 2-line
asm stub `hstart.s` (`_start: jmp _handlerMain`) **linked FIRST** → offset 0 is the jump.
Verify with `m68k-amigaos-objdump -d -j .text sharedfs` (offset 0 must be the `jmp`).

amiga-gcc defaults to absolute addressing (no `-fbaserel`), so no a4 setup; LoadSeg zeroes
BSS so globals start at 0. `sharedfs.c` defines `SysBase` (`*(APTR)4`), opens `dos.library`
+ `utility.library` (scsi.c uses utility Str*), and provides a no-op `MessageBox` (scsi.c
references it; do NOT link `common.c` → no Intuition). `scsi.c` was made libc-free (dropped
its one `sprintf`/`<stdio.h>`, added `numToStr`) so `scsi.o` links `-nostartfiles`.

Build: `make sharedfs` → `gcc -nostartfiles hstart.o sharedfs.o scsi.o -o sharedfs -s`.
Mount: `src/SHARED.mountlist` (FileSystem=L:sharedfs, Device/Unit → FSSM). Skeleton
answers ACTION_STARTUP/IS_FILESYSTEM/DIE only; objects/files are Phases 3–4. UNTESTED on HW.

**Capability ceiling (firmware):** list/read/create only — **no delete, no rename**;
writes commit on close (no in-place edit); 32-char names; synthetic dates/protection.

---

## ListBrowser Tag Reference (AmigaOS 3.2.3 ReAction)

Compile with `-DNO_INLINE_STDARG` to force `AllocListBrowserNodeA` (TagItem array)
instead of the varargs macro `AllocListBrowserNode`.

| Tag | Notes |
|---|---|
| `LBNA_Column, N` | Opens column N context for subsequent LBNCA_ tags |
| `LBNCA_Text, (ULONG)ptr` | Pointer to string. Without CopyText, must stay valid for program lifetime. |
| `LBNCA_CopyText, TRUE` | Copies string into node's internal buffer. Safe for stack/temp strings. |
| `LBNCA_MaxChars, N` | Buffer size for CopyText. |
| `LBNCA_Integer, (ULONG)longptr` | Takes a `LONG *`, NOT a value. Dereferences at render time. Unreliable — avoid. |
| `LBNCA_CopyInteger, TRUE` | Supposed to copy at node creation, but proved unreliable. Avoid. |
| `LBNCA_Justification, LCJ_LEFT` | Left-align text in column. |
| `LBNA_UserData, (ULONG)value` | Store arbitrary value per node. Place inside a `LBNA_Column` context block. |
| `LBNA_Generation, 2` | Required for proper node versioning. |

**Retrieving UserData:**
```c
ULONG userdata = 0;
struct TagItem gtags[] = {
    {LBNA_Column,   0},
    {LBNA_UserData, (ULONG)&userdata},
    {TAG_DONE,      0}
};
GetListBrowserNodeAttrsA(node, gtags);
```

**Retrieving Text pointer:**
```c
ULONG textptr = 0;
struct TagItem gtags[] = {
    {LBNA_Column,  0},
    {LBNCA_Text,   (ULONG)&textptr},
    {TAG_DONE,     0}
};
GetListBrowserNodeAttrsA(node, gtags);
char *text = (char *)textptr;
```

---

## BlueSCSI SCSI Toolbox Protocol

All commands are 10-byte vendor-specific CDBs. Data buffer: 4096 bytes (`MAX_DATA_LEN`).

| Command | CDB[0] | Notes |
|---|---|---|
| `LIST_FILES` | `0xD0` | Returns file entries from Shared/ folder |
| `GET_FILE` | `0xD1` | CDB[1]=index, CDB[2-5]=page offset. Returns 4096-byte pages. (CDB[6]=block count for multi-block when `CAP_LARGE_TRANSFERS`.) |
| `COUNT_FILES` | `0xD2` | Returns 1 byte: file count |
| `SEND_FILE_PREP` | `0xD3` | **DATA-OUT 33 bytes** = filename (32+NUL). Creates the file. |
| `SEND_FILE_10` | `0xD4` | **DATA-OUT**. Legacy (CDB[6]=0): count in CDB[1..2], 512-byte block index in CDB[3..5]. Block mode (CDB[6]>0, `CAP_LARGE_SEND`): CDB[6]×512 bytes. |
| `SEND_FILE_END` | `0xD5` | **DATA-OUT 0**. Closes the file. |
| `LIST_CDS` | `0xD7` | Returns CD image entries in write-timestamp order |
| `SET_NEXT_CD` | `0xD8` | CDB[1]=index. Selects next CD image to mount. |
| `METADATA` | `0xD9` | CDB[1]=subcommand: 0x00=list devices, 0x01=get capabilities, **0x02=SET_WORKING_DIR** (path DATA-OUT, ≤64B — for subdir descent), 0x03=get working dir |
| `COUNT_CDS` | `0xDA` | Returns 1 byte: CD count |

**Write commands are DATA-OUT (host→device): direction `SCSIF_WRITE`, `scsi_Length` =
the exact byte count. No DELETE or RENAME command exists in the protocol.** See Fix 10.

**Entry format** (40 bytes each):
- `[0]` — device-internal index (send this to `SET_NEXT_CD`)
- `[1]` — type: 1=file, 0=directory
- `[2..33]` — filename (32 bytes, null-terminated)
- `[36..39]` — file size (big-endian ULONG)

**CD ordering:** Device returns CDs in write-timestamp (directory) order, NOT
alphabetical. The index in `c[0]` is the sequential position in that order.
Do NOT sort the list — sorting breaks the `c[0]` → `SET_NEXT_CD` mapping.

---

## Current Source File State

### `src/scsi.c`
- `static int scsi_deviceOpened = 0` guards `CloseDevice`
- `extern int filecount` exported via `toolbox.h`
- `sprintf(file->Number, ...)` written AFTER `Strncpy` to avoid overflow corruption
- No sorting in `Toolbox_List_Files` — caller must not sort either
- **Upload primitives** `Toolbox_Send_Prep/Block/End` + `Toolbox_Send_File` (Fix 10),
  `SCSIF_WRITE`, exact lengths; return 0 / SCSI `io_Error`. For the `SHARED:` handler.
  *Untested on hardware.*
- **libc-free**: dropped `<stdio.h>`/`sprintf` (added `numToStr`) so `scsi.o` links into
  the `-nostartfiles` handler.
- `Toolbox_Get_Bytes(index, offset, buf, len)`: paged GET_FILE into RAM, for the handler's
  ACTION_READ. Plus a leak-guard in `Toolbox_List_Files` (frees the prior list).

### `src/sharedfs.c` + `src/hstart.s` (the SHARED: handler — Phase 3, read path)
- `hstart.s`: `_start` asm stub (offset 0) → `jmp _handlerMain`. MUST link first.
- `sharedfs.c`: startup (verified vs fat95), packet loop, no-op `MessageBox`, own
  `SysBase/DOSBase/UtilityBase`. **Read path done:** LOCATE_OBJECT/FREE_LOCK/COPY_DIR/
  PARENT, EXAMINE_OBJECT/EXAMINE_NEXT, FINDINPUT/READ/SEEK/END, INFO/DISK_INFO,
  CURRENT_VOLUME, IS_FILESYSTEM, DIE. **Write path (FINDOUTPUT/WRITE) + unsupported ops
  = Phase 4 stub** → `ERROR_ACTION_NOT_KNOWN`. *Untested on hardware.*
- Flat-folder model: only dir = root; files are leaves. Lock = `struct MyLock` (FileLock
  first + isFile/index/size/name/exNext). Open file = `struct MyFH` in `fh_Arg1`.
- **fib detail (from fat95):** `fib_FileName`/`fib_Comment` are **BCPL strings** (length
  byte + chars), `fib_DirEntryType` = 1 (root) / -3 (file). Got this wrong = garbage names.
- READ uses `Toolbox_Get_Bytes` (paged GET_FILE into RAM). scsi.c got a leak-guard so the
  handler can re-`Toolbox_List_Files` per directory scan.

### `src/BlueSCSIToolbox.c` (standalone CLI — does NOT use scsi.c)
- Self-contained: own `DoScsiCmd`, toolbox fns, `FileEntry`. Built from only its own `.o`.
- `Toolbox_PutFileByName` (the `SEND` verb) **fixed** per Fix 10. *Untested on hardware.*
- `$VER` bumped to 1.4.

### `src/CDChanger.c`
- `static const char * const g_nums[64]` for # column (compiled-in literals)
- `static int g_numcount` tracks display position
- No `WMHI_GADGETDOWN` handler — disc changes only on explicit Select button press
- No `qsort` — ISOs displayed in device (timestamp) order
- `WINDOW_Position, WPOS_TOPLEFT` — opens at top-left
- `WA_Width, 250` / `WA_Height, 250`

### `src/SDTransfer.c`
- Uses `sprintf` not `snprintf` — avoids 38 KB of float printf bloat
- `WINDOW_Position, WPOS_TOPLEFT`
- Workbench startup uses `argv[0]` for WBStartup pointer + `CurrentDir` before `GetDiskObject`

### `src/toolbox.h`
- `extern int filecount` declaration
- `struct FileEntry`: `Name[32+1]`, `Number[5+1]` — note adjacency, Strncpy can overflow

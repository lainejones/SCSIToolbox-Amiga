# Attribution & Provenance — SCSIToolbox-Amiga

This file records the provenance of the code in this repository so that GPL-3.0
attribution obligations (and the BlueSCSI/ZuluSCSI projects' attribution norms) are met.

## This repository

**SCSIToolbox-Amiga** is a fork of
[**BlueSCSI-toolbox-Amiga** v1.3](https://github.com/paulroberthill/BlueSCSI-toolbox-Amiga)
by **Paul Hill**, extended by **Laine Jones** (<https://github.com/lainejones/>) with a
mountable shared-folder filesystem handler and an upload path.

- License: **GPL-3.0-or-later** (unchanged from upstream).
- Upstream copyright © 2024 Paul Hill — preserved in all original files.
- Modifications © 2026 Laine Jones — marked in-file per GPLv3 §5.

### What this fork changes / adds

| File | Status | Notes |
|------|--------|-------|
| `src/scsi.c`            | modified | `SEND_FILE_*` upload primitives; `SET/GET_WORKING_DIR` primitives; multi-page `GET_FILE` reads |
| `src/toolbox.h`         | modified | declarations, capability flags, working-dir constants |
| `src/BlueSCSIToolbox.c` | modified | fix the `SEND` upload path; `SETDIR/GETDIR/RESETDIR`, `INFO`, `LISTDEVICES` |
| `src/SDTransfer.c`      | modified | subfolder navigation (double-click, to SD root), persistent listbrowser node text |
| `src/sharedfs.c`        | **new**  | AmigaDOS filesystem handler for the shared folder (`SHARED:`), hierarchical with working-dir firmware |
| `src/hstart.s`          | **new**  | handler entry stub (hunk offset 0 → handlerMain) |
| `src/SHARED.mountlist`, `src/SHARED-GVP.mountlist` | **new** | mountlists for `SHARED:` |
| `src/Makefile`          | modified | handler target; removed broken standalone-strip step |

Each modified file gains a notice of the form:

```
Modified <YYYY-MM-DD> by Laine Jones (lainejones): <summary of change>
```

added **below** Paul Hill's existing copyright header (never replacing it). Each new file
carries the GPLv3 header from `LICENSE-HEADER.txt` with copyright © 2026 Laine Jones.

## Device-side lineage (the firmware these tools talk to)

These Amiga tools speak the **BlueSCSI/ZuluSCSI Toolbox** vendor SCSI command set. That
firmware has its own open-source lineage (all GPL):

- **SCSI2SD** — Michael McMaster — foundational SCSI emulation firmware.
- **ZuluSCSI** — Rabbit Hole Computing — derived from SCSI2SD V6.
- **BlueSCSI v2** — Eric Helgeson — forked from ZuluSCSI (carries SCSI2SD V6).
- *(BlueSCSI v1 — separate root: Akira Hattori's ArdSCSino-stm32.)*

The Toolbox **protocol** (vendor command opcodes and wire formats) is published in the
[BlueSCSI Toolbox Developer Docs](https://bluescsi.com/docs/Toolbox-Developer-Docs); a
protocol interface is not a copyrightable expression, so implementing it is unrestricted.
What is reused under GPL here is Paul Hill's specific Amiga client *code*.

## Trademarks

**BlueSCSI** and **ZuluSCSI** are names/logos of their respective projects (Eric Helgeson;
Rabbit Hole Computing). They are referenced here only to indicate hardware compatibility.
No affiliation or endorsement is implied.

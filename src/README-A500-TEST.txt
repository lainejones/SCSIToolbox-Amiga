SCSIToolbox-Amiga - A500 + GVP A530 + BlueSCSI (fw v2026.04.27+) test checklist
===============================================================================
Goal: verify the NEW working-directory (subfolder) support - the one thing the
A1200/ZuluSCSI could not test. Device name below assumes gvpscsi.device; find
the BlueSCSI's SCSI ID first (try INFO with UNIT=0..6 until one answers).

Getting the files onto the A500: put this whole folder into the SD card's
"shared" folder from the PC, then on the Amiga use your existing toolbox to
pull them - or copy them onto a mountable partition/floppy directly.

Throughout:  <DEV> = DEVICE=gvpscsi.device UNIT=<id>

1) INFO - the go/no-go check
     BlueSCSIToolbox <DEV> INFO
   EXPECT: "Capabilities: 0x07" (or anything with bit 0x04) and
           "Working directory supported" + "Working directory: /shared"
   If capabilities is 0x03 the firmware is too old - stop here.

2) GETDIR
     BlueSCSIToolbox <DEV> GETDIR
   EXPECT: "Working directory: /shared" (or your configured shared dir)

3) Make a subfolder on the SD card (from the PC, or it may already have one),
   e.g. /shared/testdir with a couple of files in it. Then:
     BlueSCSIToolbox <DEV> SETDIR=/shared/testdir DIR
   EXPECT: lists ONLY testdir's contents. Directories in a listing show "Dir".

4) Persistence + reset
     BlueSCSIToolbox <DEV> DIR           -> still testdir (override persists!)
     BlueSCSIToolbox <DEV> RESETDIR DIR  -> back to /shared contents
   (SETDIR="" also resets. A bare "SETDIR=" does NOT - ReadArgs eats the
   next word as its value.)

5) SEND into a subfolder
     BlueSCSIToolbox <DEV> SETDIR=/shared/testdir SEND=ram:somefile
   EXPECT: "...bytes sent"; file appears in testdir (check DIR / the PC).
     BlueSCSIToolbox <DEV> SETDIR=

6) SDTransfer subfolder browsing (needs Workbench)
     SDTransfer DEVICE=gvpscsi.device UNIT=<id>
   EXPECT: directories listed with a trailing "/". Select one and click
   "Open / Download" -> descends into it; a "/" row appears at the top ->
   takes you back up. Selecting a file + the button downloads as before.
   On exit the device is reset to the default dir.

7) SHARED: with real subdirectories
     Copy sharedfs L:
     Mount SHARED: from SHARED-GVP.mountlist   (set Unit first!)
   EXPECT: SHARED disk icon on Workbench. Then:
     Dir SHARED:            -> shows files AND drawers
     Dir SHARED:testdir     -> subfolder contents
     Type SHARED:testdir/<somefile>
     Copy ram:x SHARED:testdir/x  -> lands in the subfolder (check from PC)
     Copy SHARED:testdir/x ram:y  -> reads back identical
   Deep paths, "/" parent syntax, and Workbench drawer double-click should
   all behave like a normal (read-mostly) volume. No delete/rename/makedir -
   those correctly refuse.

Known/expected:
 - Path limit: 63 chars total for the absolute SD path (firmware cap).
 - Don't run SDTransfer while SHARED: is mounted on the same unit - the
   working dir is one global on the device and they will fight.
 - INFO also dumps a LISTDEVICES section that may print many "Hard disk (0)"
   lines on some adapters - cosmetic, pipe to a file and Search if needed.

Report back what steps 1-7 show and I'll fix anything that misbehaves.

/*
 * ejectfix.h - making an eject stick (SCSIToolbox-Amiga)
 *
 * BlueSCSI and ZuluSCSI put a CD image back after an eject unless their ini
 * says otherwise (ReinsertAfterEject, ReinsertCDOnInquiry; BlueSCSI's
 * StartEjected keeps the drive empty after power-on too). These helpers find
 * out whether an eject held and write the settings that make it hold.
 *
 * Copyright (C) 2026 Laine Jones. GPL-3.0-or-later, see LICENSE.
 */
#ifndef EJECTFIX_H
#define EJECTFIX_H

/* TEST UNIT READY: 1 = a disc is in, 0 = no disc, -1 = could not tell. */
int ef_medium_present(void);

/* Call straight after a successful eject. Polls the drive for about 1.5 s
   (the firmware reinserts when it is polled): 1 = the eject held (empty),
   0 = the firmware put a disc back, -1 = could not tell. */
int ef_eject_held(void);

/* The ini lines that keep an eject on SCSI ID 'id', as text for a requester:
   "[SCSI2]\nReinsertAfterEject=0\nReinsertCDOnInquiry=0\n", plus
   "StartEjected=1" when startEmpty (BlueSCSI only: ZuluSCSI has no such
   key). Returns 'buf'. */
char *ef_ini_lines(char *buf, int buflen, int id, int startEmpty);

/* Rewrite an ini so section [SCSI<id>] holds the keep-ejected settings;
   every other line stays as it was (line endings too). Returns the new
   length, or -1 if 'outmax' is too small. Pure C, also built on the host
   for tests. */
long ef_patch_ini(const char *in, long inlen, int id, int startEmpty, char *out, long outmax);

/* CDChanger only (ejectini.c): on a BlueSCSI with working-directory
   firmware, rewrite bluescsi.ini in the SD card's root with ef_patch_ini,
   keeping the first original as bluescsi.ini.bak. 0 = done; otherwise an
   error text is in 'msg'. */
int ef_fix_bluescsi_ini(int id, int startEmpty, char *msg, int msglen);

#endif

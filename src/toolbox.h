/**
 * Copyright (C) 2024 Paul Hill
 *
 * Modified 2026-06-08 by Laine Jones (lainejones): part of SCSIToolbox-Amiga.
 * Declared the shared-folder upload primitives implemented in scsi.c.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. 
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/
#ifndef TOOLBOX_H
#define TOOLBOX_H

#include <exec/types.h>

#define BLUESCSI_TOOLBOX_COUNT_FILES 0xD2
#define BLUESCSI_TOOLBOX_LIST_FILES 0xD0
#define BLUESCSI_TOOLBOX_GET_FILE 0xD1
#define BLUESCSI_TOOLBOX_SEND_FILE_PREP 0xD3
#define BLUESCSI_TOOLBOX_SEND_FILE_10 0xD4
#define BLUESCSI_TOOLBOX_SEND_FILE_END 0xD5
#define BLUESCSI_TOOLBOX_TOGGLE_DEBUG 0xD6
#define BLUESCSI_TOOLBOX_LIST_CDS 0xD7
#define BLUESCSI_TOOLBOX_SET_NEXT_CD 0xD8
#define BLUESCSI_TOOLBOX_LIST_DEVICES 0xD9
#define BLUESCSI_TOOLBOX_METADATA 0xD9
#define BLUESCSI_TOOLBOX_COUNT_CDS 0xDA

// 0xD9 Metadata subcommands (CDB[1])
#define BLUESCSI_TOOLBOX_SUBCMD_LIST_DEVICES 0x00
#define BLUESCSI_TOOLBOX_SUBCMD_GET_CAPABILITIES 0x01
#define BLUESCSI_TOOLBOX_SUBCMD_SET_WORKING_DIR 0x02
#define BLUESCSI_TOOLBOX_SUBCMD_GET_WORKING_DIR 0x03

// Capability flags
#define BLUESCSI_TOOLBOX_CAP_LARGE_TRANSFERS 0x01
#define BLUESCSI_TOOLBOX_CAP_LARGE_SEND 0x02
#define BLUESCSI_TOOLBOX_CAP_SET_WORKING_DIR 0x04

/* SET_WORKING_DIR data phase is capped at 64 bytes by the firmware (path + NUL),
   so the longest settable absolute SD path is 63 chars. */
#define TOOLBOX_MAX_WD_PATH 64

// from BlueSCSI_Toolbox.cpp
#define MAX_MAC_PATH 32
#define ENTRY_SIZE 40

#define SCSI_CMD_INQ 0x12

// #define TESTMODE 1
#define MAXPATH 1024

#define BLUESCSI_FILE 1
#define BLUESCSI_DIR 0

// scsi.c
extern int filecount;
struct FileEntry *Toolbox_List_Files(int cdrom);
void Toolbox_Set_Next_CD(UBYTE index);
void scsi_cleanup(void);
ULONG Toolbox_Download(char *source, char *destination, void (*callback)(int));
/* Shared-folder upload primitives (return 0 on success, SCSI io_Error on fail) */
LONG Toolbox_Send_Prep(const char *remotename);
LONG Toolbox_Send_Block(ULONG block, const UBYTE *data, int len);
LONG Toolbox_Send_End(void);
LONG Toolbox_Send_File(const char *remotename, const char *source, void (*callback)(LONG));
LONG Toolbox_Get_Bytes(int index, ULONG offset, UBYTE *buf, ULONG len);
/* Working directory (needs CAP_SET_WORKING_DIR; firmware v2026.04.27+) */
LONG Toolbox_Set_Working_Dir(const char *path);
LONG Toolbox_Get_Working_Dir(char *buf, int buflen);
int scsi_setup(char *scsi_dev, int scsi_unit);
int Toolbox_GetCapabilities(void);
extern int scsi_isBlueSCSI, scsi_isZuluSCSI;
extern UBYTE scsi_apiVersion, scsi_capabilities;

// toolbox.c
void MessageBox(char *title, char *body);

struct FileEntry
{
   int Index;
   ULONG Size;
   int Type;
   char Name[32 + 1];
   char Number[5 + 1];
};

/* The current shared-folder / CD list, allocated by Toolbox_List_Files (scsi.c).
   Used by the SHARED: handler's EXAMINE_NEXT. */
extern struct FileEntry *files;

#endif


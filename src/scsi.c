/*
 * Copyright (C) 2024 Paul Hill
 *
 * Modified 2026-06-08 by Laine Jones (lainejones): part of SCSIToolbox-Amiga.
 * Added the shared-folder upload primitives (Toolbox_Send_Prep/Block/End and
 * the Toolbox_Send_File wrapper) for use by the forthcoming SHARED: AmigaDOS
 * filesystem handler. UI-agnostic: errors are returned, not shown.
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
 */
#include <stdio.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <devices/scsidisk.h>
#include "toolbox.h"

#define SENSE_LEN 252
#define MAX_DATA_LEN 4096

struct MsgPort *scsiPort;
struct IOStdReq *io_ptr;
struct SCSICmd *scsi_cmd;
UBYTE *scsi_sense;
UBYTE *scsi_data;
/* GCC: scsi_deviceOpened guards CloseDevice so we never call it on a
   device that was never successfully opened (bug in original VBCC source) */
static int scsi_deviceOpened = 0;

int scsi_isCD;
int scsi_isBlueSCSI;
int scsi_isZuluSCSI;
int scsi_isRemovable;
UBYTE scsi_apiVersion;
UBYTE scsi_capabilities;

struct FileEntry *files = NULL;
int filecount = 0;

int Toolbox_InitDevice(void);
int Toolbox_GetCapabilities(void);
static int Toolbox_InquiryHasName(const char *name);

static ULONG Toolbox_ParseEntrySize(const UBYTE *entry)
{
   return ((ULONG)entry[36] << 24)
      | ((ULONG)entry[37] << 16)
      | ((ULONG)entry[38] << 8)
      | (ULONG)entry[39];
}

/* Setup the SCSI device */
int scsi_setup(char *scsi_dev, int scsi_unit)
{
   if ((scsiPort = CreateMsgPort()) == NULL)
   {
      return -1;
   }
   if ((io_ptr = (struct IOStdReq *)CreateIORequest(scsiPort, sizeof(struct IOStdReq))) == NULL)
   {
      return -1;
   }
   if (OpenDevice(scsi_dev, scsi_unit, (struct IORequest *)io_ptr, 0) != 0)
   {
      MessageBox("scsi_setup", "Error opening SCSI device");
      return -1;
   }
   scsi_deviceOpened = 1;
   if ((scsi_cmd = (struct SCSICmd *)AllocMem(sizeof(struct SCSICmd), MEMF_CLEAR)) == NULL)
   {
      return -1;
   }
   if ((scsi_sense = (UBYTE *)AllocMem(SENSE_LEN, MEMF_CLEAR)) == NULL)
   {
      return -1;
   }
   if ((scsi_data = (UBYTE *)AllocMem(MAX_DATA_LEN, MEMF_CLEAR)) == NULL)
   {
      return -1;
   }

   // Init the device and check some flags
   if (Toolbox_InitDevice() < 0)
   {
      MessageBox("scsi_setup", "Error sending inquiry to device\n");
      return -1;
   }

   if (Toolbox_GetCapabilities() != 0 && !scsi_isBlueSCSI && !scsi_isZuluSCSI)
   {
      MessageBox("scsi_setup", "Toolbox API not available on this device\n");
      return -1;
   }

   return 0;
}

void scsi_cleanup()
{
   if (files) FreeMem(files, sizeof(struct FileEntry) * (filecount + 1));
   if (scsi_data) FreeMem(scsi_data, MAX_DATA_LEN);
   if (scsi_sense) FreeMem(scsi_sense, SENSE_LEN);
   if (scsi_cmd) FreeMem(scsi_cmd, sizeof(struct SCSICmd));

   if (io_ptr)
   {
      if (scsi_deviceOpened)
      {
         CloseDevice((struct IORequest *)io_ptr);
         scsi_deviceOpened = 0;
      }
      DeleteIORequest(io_ptr);
   }
   if (scsiPort) DeleteMsgPort(scsiPort);
}

/* Send a SCSI command */
int DoScsiCmd(UBYTE *data, int datasize, UBYTE *cmd, int cmdsize, UBYTE flags)
{
   io_ptr->io_Length = sizeof(struct SCSICmd);
   io_ptr->io_Data = scsi_cmd;
   io_ptr->io_Command = HD_SCSICMD;
   scsi_cmd->scsi_Data = (UWORD *)data;
   scsi_cmd->scsi_Length = datasize;
   scsi_cmd->scsi_SenseActual = 0;
   scsi_cmd->scsi_SenseLength = SENSE_LEN;
   scsi_cmd->scsi_SenseData = scsi_sense;
   scsi_cmd->scsi_Command = cmd;
   scsi_cmd->scsi_CmdLength = cmdsize;
   scsi_cmd->scsi_Flags = flags;
   DoIO((struct IORequest *)io_ptr);
   return (io_ptr->io_Error);
}

/* Send a SCSI inquiry command to the device to gather some info */
int Toolbox_InitDevice(void)
{
   int err = 0;
#ifdef TESTMODE
   scsi_isCD = 1;
   scsi_isBlueSCSI = 1;
   scsi_isZuluSCSI = 0;
   scsi_isRemovable = 1;
#else
   UBYTE command[] = {SCSI_CMD_INQ, 0, 0, 0, 252, 0};

   if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_DATA_LEN,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
   {
      return -1;
   }

   if (scsi_cmd->scsi_Actual)
   {
      scsi_isCD = (scsi_data[0] & 0x1F) == 0x05;
      scsi_isRemovable = (scsi_data[1] & 0x80) ? 1 : 0;
      scsi_isBlueSCSI = Strnicmp("BlueSCSI", &scsi_data[8], 8) == 0;
      scsi_isZuluSCSI = Strnicmp("ZuluSCSI", &scsi_data[8], 8) == 0;
      if (!scsi_isBlueSCSI && !scsi_isZuluSCSI)
      {
         scsi_isBlueSCSI = Toolbox_InquiryHasName("BlueSCSI");
         scsi_isZuluSCSI = Toolbox_InquiryHasName("ZuluSCSI");
      }
   }
#endif
   return err;
}

static int Toolbox_InquiryHasName(const char *name)
{
   int i;

   if (scsi_cmd->scsi_Actual < 36 + 8)
   {
      return 0;
   }

   for (i = 36; i <= (int)scsi_cmd->scsi_Actual - 8; i++)
   {
      if (Strnicmp((STRPTR)name, (STRPTR)&scsi_data[i], 8) == 0)
      {
         return 1;
      }
   }

   return 0;
}

/* Query firmware API version and capability flags */
int Toolbox_GetCapabilities(void)
{
   UBYTE command[] = {BLUESCSI_TOOLBOX_METADATA, BLUESCSI_TOOLBOX_SUBCMD_GET_CAPABILITIES, 0, 0, 0, 0, 0, 0, 8, 0};
   int err;

   scsi_apiVersion = 0;
   scsi_capabilities = 0;

   if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_DATA_LEN,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
   {
      return -1;
   }

   if (scsi_cmd->scsi_Actual >= 2)
   {
      scsi_apiVersion = scsi_data[0];
      scsi_capabilities = scsi_data[1];
   }
   return 0;
}

/* Execute BLUESCSI_TOOLBOX_COUNT_CDS / BLUESCSI_TOOLBOX_COUNT_FILES */
int Toolbox_Count_Files(int cdrom)
{
   UBYTE command[] = {BLUESCSI_TOOLBOX_COUNT_FILES, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   int err;
   int count = 0;
   if (cdrom)
   {
      command[0] = BLUESCSI_TOOLBOX_COUNT_CDS;
   }

   if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_DATA_LEN,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
   {
      MessageBox("Toolbox_Count_Files", "SCSI error\n");
      return -1;
   }

   if (scsi_cmd->scsi_Actual)
   {
      count = (int)scsi_data[0];
   }
   return count;
}

#ifdef TESTMODE
STRPTR TestData[] =
{
   "MacOS8_0.iso",
   "Debian.iso",
   "Mac OS 7.6.1.iso",
   "AmigaOS3.1.iso",
   "AmigaOS3.2.iso",
   "Star Wars - Dark Forces (1994)(LucasArts)[!][CDD6287].iso",
   "NetBSD-9.3-amiga.iso"
};
#endif

/* Execute BLUESCSI_TOOLBOX_LIST_CDS and create the files array */
struct FileEntry *Toolbox_List_Files(int cdrom)
{
#ifdef TESTMODE
   filecount = sizeof(TestData) / sizeof(char*);
   files = (struct FileEntry *)AllocMem(sizeof(struct FileEntry) * (filecount + 1), MEMF_CLEAR);

   struct FileEntry *file = files;
   for (int f = 0; f < filecount; f++)
   {
      file->Index = f;
      file->Type = BLUESCSI_FILE;
      file->Size = 4096*20000;
      sprintf(file->Number, "%d", f+1);
      Strncpy(file->Name, TestData[f], 32);
      file++;
   }
   file->Type = -1;  // EOF
#else

   if (cdrom)
   {
      if (!scsi_isRemovable)
      {
         MessageBox("Toolbox_List_Files", "Not a removable device!\n");
         return NULL;
      }
   }

   // Get the file count
   filecount = Toolbox_Count_Files(cdrom);
   if (filecount > 0)
   {
      struct FileEntry *file;
      UBYTE command[] = {BLUESCSI_TOOLBOX_LIST_FILES, 0, 0, 0, 0, 0, 0, 0, 0, 0};
      int err;
      file = files = (struct FileEntry *)AllocMem(sizeof(struct FileEntry) * (filecount + 1), MEMF_CLEAR);
      if (cdrom)
      {
         command[0] = BLUESCSI_TOOLBOX_LIST_CDS;
      }

      if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_DATA_LEN,
                           (UBYTE *)&command, sizeof(command),
                           (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
      {
         FreeMem(files, sizeof(struct FileEntry) * (filecount + 1));
         files = NULL;
         MessageBox("Toolbox_Count_Files", "SCSI error\n");
         return NULL;
      }

      if (scsi_cmd->scsi_Actual)
      {
         int f;
         for (f = 0; f < filecount; f++)
         {
            UBYTE *c = &scsi_data[ENTRY_SIZE * f];
            file->Index = c[0];
            file->Type = c[1];    // 0=dir 1=file

            Strncpy(file->Name, (char *)&c[2], MAX_MAC_PATH);
            file->Name[MAX_MAC_PATH] = '\0';
            /* Write Number AFTER Strncpy in case it overflows one byte into Number[0] */
            sprintf(file->Number, "%d", f+1);

            file->Size = Toolbox_ParseEntrySize(c);
            file++;
         }
         file->Type = -1;  // EOF
      }
   }
#endif
   return files;
}

/* Select a CD image */
void Toolbox_Set_Next_CD(UBYTE index)
{
   UBYTE command[] = {BLUESCSI_TOOLBOX_SET_NEXT_CD, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   int err;
   command[1] = (UBYTE) index;
   if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_DATA_LEN,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
   {
      UBYTE errmsg[80];
      sprintf(errmsg, "SCSI error %d\n", err);
      MessageBox("Toolbox_Set_Next_CD", errmsg);
   }
}

/* Download a file from the SD card */
ULONG Toolbox_Download(char *source, char *destination, void (*callback)(int))
{
   ULONG result = 0;
   if (files)
   {
      struct FileEntry *file = files;
      ULONG count = 0;
      int index = -1;
      int i;
      for (i = 0; i < filecount; i++)
      {
         if (Stricmp(file->Name, source) == 0)
         {
            index = file->Index;
            break;
         }
         file++;
      }

      if (index >= 0)
      {
         int offset = 0; // offset in 4096 size pages
         ULONG size = file->Size;
         BPTR fh;
         UBYTE command[] = {BLUESCSI_TOOLBOX_GET_FILE, 0, 0, 0, 0, 0, 0, 0, 0, 0};
         command[1] = index;

         fh = Open(destination, MODE_NEWFILE);
         if (!fh)
         {
            SetIoErr(ERROR_OBJECT_NOT_FOUND);
            PrintFault(IoErr(), destination);
            return 0;
         }

         while (count < size)
         {
            int err;
            command[2] = (offset & 0xFF000000) >> 24;
            command[3] = (offset & 0x00FF0000) >> 16;
            command[4] = (offset & 0x0000FF00) >> 8;
            command[5] = (offset & 0x000000FF);

            if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_DATA_LEN,
                                 (UBYTE *)&command, sizeof(command),
                                 (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
            {
               MessageBox("Toolbox_Download", "SCSI error");
               break;
            }

#ifdef TESTMODE
            {
               ULONG remaining = size - count;
               ULONG chunk = remaining > 4096 ? 4096 : remaining;

               count += chunk;
               offset++;
            }
#else
            if (scsi_cmd->scsi_Actual)
            {
               ULONG remaining = size - count;
               ULONG chunk = scsi_cmd->scsi_Actual;

               if (chunk > remaining)
               {
                  chunk = remaining;
               }

               if (chunk == 0)
               {
                  break;
               }

               count += chunk;
               offset++;
               if (Write(fh, scsi_data, chunk) != (LONG)chunk)
               {
                  MessageBox("Toolbox_Download", "Write error");
                  break;
               }
            }
            else
            {
               break;
            }
#endif

            if (callback && (count == size || (offset % 16 == 0)))
            {
               // Update progress every 64k
               int pc = size ? (int)(count / (size / 100 + 1)) : 100;
               if (pc > 100)
               {
                  pc = 100;
               }
               callback(pc);
            }
         }
         Close(fh);
         result = count;
      }
      else
      {
         SetIoErr(ERROR_OBJECT_NOT_FOUND);
         result = 0;
      }
   }

   return result;
}

/* ----------------------------------------------------------------------------
 * Shared-folder upload (write) primitives.
 *
 * These mirror the corrected SEND_FILE protocol (see BlueSCSIToolbox.c):
 *   PREP  (0xD3): DATA OUT = 33 bytes (filename, 32 + NUL).
 *   BLOCK (0xD4): DATA OUT = 'len' bytes (1..512). Legacy mode (CDB[6]=0):
 *                 byte count in CDB[1..2]; 512-byte block index in CDB[3..5].
 *   END   (0xD5): no DATA OUT.
 * All are DATA OUT (host->device): direction SCSIF_WRITE, transfer length must
 * equal the bytes the firmware reads.
 *
 * UI-agnostic: each returns 0 on success or the SCSI io_Error on failure, so
 * the SHARED: handler (which has no UI) can map the result to a DOS error.
 * -------------------------------------------------------------------------- */

/* Begin an upload named 'remotename' (leaf name, <=32 chars) in the shared dir. */
LONG Toolbox_Send_Prep(const char *remotename)
{
   UBYTE command[10] = {0};
   int i;

   command[0] = BLUESCSI_TOOLBOX_SEND_FILE_PREP;
   for (i = 0; i < MAX_MAC_PATH && remotename[i]; i++)
      scsi_data[i] = (UBYTE)remotename[i];
   for (; i < MAX_MAC_PATH + 1; i++)
      scsi_data[i] = 0;

   return DoScsiCmd((UBYTE *)scsi_data, MAX_MAC_PATH + 1,
                    (UBYTE *)&command, sizeof(command),
                    (SCSIF_WRITE | SCSIF_AUTOSENSE));
}

/* Send one 512-byte block (1..512 bytes) at 512-byte block index 'block'.
 * 'data' may be scsi_data itself (no copy) or any caller buffer. */
LONG Toolbox_Send_Block(ULONG block, const UBYTE *data, int len)
{
   UBYTE command[10] = {0};

   if (len < 1 || len > 512)
      return -1;
   if (data != scsi_data)
      CopyMem((APTR)data, scsi_data, len);

   command[0] = BLUESCSI_TOOLBOX_SEND_FILE_10;
   command[1] = (UBYTE)((len >> 8) & 0xFF);   /* byte count hi */
   command[2] = (UBYTE)(len & 0xFF);          /* byte count lo */
   command[3] = (UBYTE)((block >> 16) & 0xFF);
   command[4] = (UBYTE)((block >> 8) & 0xFF);
   command[5] = (UBYTE)(block & 0xFF);
   command[6] = 0;                            /* legacy byte-count mode */

   return DoScsiCmd((UBYTE *)scsi_data, len,
                    (UBYTE *)&command, sizeof(command),
                    (SCSIF_WRITE | SCSIF_AUTOSENSE));
}

/* Finish the current upload (close the file on the SD card). */
LONG Toolbox_Send_End(void)
{
   UBYTE command[10] = {0};

   command[0] = BLUESCSI_TOOLBOX_SEND_FILE_END;
   return DoScsiCmd((UBYTE *)scsi_data, 0,
                    (UBYTE *)&command, sizeof(command),
                    (SCSIF_WRITE | SCSIF_AUTOSENSE));
}

/* Whole-file convenience wrapper: upload local 'source' as 'remotename'.
 * Returns bytes sent, or -1 on error (IoErr set for DOS-side failures).
 * 'callback' (optional) is called with the running byte total. */
LONG Toolbox_Send_File(const char *remotename, const char *source, void (*callback)(LONG))
{
   BPTR fh;
   ULONG block = 0;
   ULONG total = 0;

   /* Open the local source FIRST: don't PREP (which creates the file on the SD
    * card) until we know the source is readable. */
   fh = Open((STRPTR)source, MODE_OLDFILE);
   if (!fh)
   {
      SetIoErr(ERROR_OBJECT_NOT_FOUND);
      return -1;
   }

   if (Toolbox_Send_Prep(remotename) != 0)
   {
      Close(fh);
      return -1;
   }

   for (;;)
   {
      LONG len = Read(fh, scsi_data, 512);
      if (len < 0)
      {
         Close(fh);
         return -1;
      }
      if (len == 0)
         break;

      if (Toolbox_Send_Block(block, scsi_data, (int)len) != 0)
      {
         Close(fh);
         return -1;
      }
      total += (ULONG)len;
      block++;
      if (callback)
         callback((LONG)total);
   }
   Close(fh);

   if (Toolbox_Send_End() != 0)
      return -1;

   return (LONG)total;
}

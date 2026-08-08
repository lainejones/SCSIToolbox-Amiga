/*
 * Copyright (C) 2024 Paul Hill
 *
 * Modified 2026-06-08 by Laine Jones (lainejones): part of SCSIToolbox-Amiga.
 * Fixed the SEND (upload) path in Toolbox_PutFileByName — corrected SCSI data
 * direction (SCSIF_WRITE, not SCSIF_READ) and per-command transfer lengths so
 * SEND_FILE_PREP/10/END actually work against BlueSCSI/ZuluSCSI firmware.
 * Modified 2026-08-06: added SETDIR/GETDIR (working-directory support, firmware
 * v2026.04.27+ metadata subcommands 0x02/0x03).
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
#include <exec/execbase.h>
#include <clib/alib_protos.h>
#include <clib/exec_protos.h>
#include <devices/scsidisk.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/utility.h>
#include <stdlib.h>
#include <string.h>

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

#define SCSI_CMD_INQ 0x12

// from BlueSCSI_Toolbox.cpp
#define MAX_MAC_PATH 32
#define ENTRY_SIZE 40

__attribute__((used)) static const char ver[] = "$VER: BlueSCSIToolbox 1.7 (8.8.2026)";

/* Last-CD state shared with CDChanger (see SaveLastCD there) */
#define LASTCD_ENV     "ENV:CDChanger.lastcd"
#define LASTCD_ENVARC  "ENVARC:CDChanger.lastcd"
#define EJECTED_MARKER "<EJECTED>"

int Toolbox_List_Files(int cdrom);
int Toolbox_List_Devices(void);
int Toolbox_GetCapabilities(void);
int Toolbox_Count_Files(int cdrom);
ULONG Toolbox_GetFileByName(char *destination, char *source);
int Toolbox_PutFileByName(char *destination, char *source);
int Toolbox_List_CDs(void);
int Toolbox_SetWorkingDir(char *path);
int Toolbox_GetWorkingDir(char *buf, int buflen);
int Toolbox_Eject(void);
void Toolbox_RestoreCD(void);
void SaveLastCD(char *name);
int ReadLastCD(char *buf, int buflen);
void Toolbox_Show_files(void);
void Toolbox_Next_CD(int index);
void Toolbox_Debug(int debugon);

void dump(char *msg, UBYTE *d, int len);
void DiskChange(void);
void bstrcpy(char *dest,UBYTE *src);
int DoScsiCmd(UBYTE *data, int datasize, UBYTE *cmd, int cmdsize, UBYTE flags);
int BlueSCSI_InitDevice(void);
static int Toolbox_InquiryHasName(const char *name);
static ULONG Toolbox_ParseEntrySize(const UBYTE *entry);
static void Toolbox_FormatSize(char *buffer, int length, ULONG size);

struct IOStdReq *io_ptr;
struct MsgPort *mp_ptr;
struct SCSICmd *scsi_cmd;
UBYTE *scsi_sense;
UBYTE *scsi_data = NULL;
struct Library *UtilityBase = NULL;

LONG scsi_removable;
LONG scsi_isCD;
LONG scsi_isBlueSCSI;
LONG scsi_isZuluSCSI;
UBYTE scsi_apiVersion;
UBYTE scsi_capabilities;

UBYTE scsi_dev[1024];
LONG scsi_id = 0;

#define SENSE_LEN 252
#define MAX_DATA_LEN 4096

struct FileEntry
{
   int Index;
   ULONG Size;
   int Type;
   char Name[32 + 1];
   char Number[5 + 1];
};

struct FileEntry *files = NULL;
int filecount = 0;

// ReadArgs template
char *template = "DEVICE/K,UNIT/K/N,DIR=LIST/S,SEND/K,RECEIVE/K,LISTDEVICES/S,LISTCDS/S,SETCD/K/N,SETDEBUG/K/N,INFO/S,SETDIR/K,GETDIR/S,RESETDIR/S,EJECT/S,RESTORECD/S";

enum ToolboxCommand
{
   TOOLBOX_NONE,
   TOOLBOX_DIR,
   TOOLBOX_SEND,
   TOOLBOX_RECEIVE,
   TOOLBOX_LISTDEVICES,
   TOOLBOX_LISTCDS,
   TOOLBOX_SETCD,
   TOOLBOX_SETDEBUG,
   TOOLBOX_INFO,
   TOOLBOX_EJECT,
   TOOLBOX_RESTORECD
};

enum ToolboxParams
{
   DEVICE,
   UNIT,
   DIR,
   SEND,
   RECEIVE,
   LISTDEVICES,
   LISTCDS,
   SETCD,
   SETDEBUG,
   INFO,
   SETDIR,
   GETDIR,
   RESETDIR,
   EJECT,
   RESTORECD
};

int main(int argc, char* argv[])
{
   struct RDArgs *rd;
   enum ToolboxCommand toolboxCommand = TOOLBOX_NONE;
   int capabilities_ok;
   char filename[256];
   char progname[256];
   char workdir[256];
   LONG params[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   LONG nextcd;
   LONG debugon;
   int setdir = 0;
   int getdir = 0;
   int resetdir = 0;

      { const char * volatile keepver = ver; (void)keepver; } /* keep $VER through --gc-sections */
GetProgramName(progname, sizeof(progname));

   if ((UtilityBase = OpenLibrary("utility.library", 37L)) == NULL)
   {
      goto exit;
   }

   rd = ReadArgs(template, params, NULL);
   if (rd)
   {
      if (params[DEVICE])
      {
         strncpy(scsi_dev, (char *)params[DEVICE], sizeof(scsi_dev));
      }
      if (params[UNIT])
      {
         scsi_id = (*((LONG *)params[UNIT]));
      }
      if (params[DIR])
      {
         toolboxCommand = TOOLBOX_DIR;
      }
      if (params[SEND])
      {
         toolboxCommand = TOOLBOX_SEND;
         strncpy(filename, (char *)params[SEND], 256);
      }
      if (params[RECEIVE])
      {
         toolboxCommand = TOOLBOX_RECEIVE;
         strncpy(filename, (char *)params[RECEIVE], 256);
      }
      if (params[LISTDEVICES])
      {
         toolboxCommand = TOOLBOX_LISTDEVICES;
      }
      if (params[LISTCDS])
      {
         toolboxCommand = TOOLBOX_LISTCDS;
      }
      if (params[SETCD])
      {
         toolboxCommand = TOOLBOX_SETCD;
         nextcd = (*((LONG *)params[SETCD]));
      }
      if (params[SETDEBUG])
      {
         toolboxCommand = TOOLBOX_SETDEBUG;
         debugon = (*((LONG *)params[SETDEBUG]));
      }
      if (params[INFO])
      {
         toolboxCommand = TOOLBOX_INFO;
      }
      if (params[SETDIR])
      {
         /* Not a command of its own: applied before whatever else runs, and
            the override persists on the device until reset or power-off.
            SETDIR="" resets to the firmware default directory. */
         setdir = 1;
         strncpy(workdir, (char *)params[SETDIR], sizeof(workdir));
         workdir[sizeof(workdir) - 1] = '\0';
      }
      if (params[GETDIR])
      {
         getdir = 1;
      }
      if (params[RESETDIR])
      {
         /* Unambiguous reset: SETDIR="" also works, but a bare "SETDIR="
            eats the next argument as its value (ReadArgs semantics). */
         resetdir = 1;
      }
      if (params[EJECT])
      {
         toolboxCommand = TOOLBOX_EJECT;
      }
      if (params[RESTORECD])
      {
         toolboxCommand = TOOLBOX_RESTORECD;
      }
      FreeArgs(rd);
   }
   else
   {
      SetIoErr(ERROR_REQUIRED_ARG_MISSING);
      PrintFault(IoErr(), progname);
      return 5;
   }

   if (toolboxCommand == TOOLBOX_NONE && !setdir && !getdir && !resetdir)
   {
      SetIoErr(ERROR_REQUIRED_ARG_MISSING);
      PrintFault(IoErr(), progname);
      return 5;
   }

   if ((mp_ptr = (struct MsgPort *)CreateMsgPort()) == NULL)
   {
      PutStr("CreatePort failed!\n");
      goto exit;
   }
   if ((io_ptr = (struct IOStdReq *)CreateIORequest(mp_ptr, sizeof(struct IOStdReq))) == NULL)
   {
      PutStr("CreateIORequest failed!\n");
      goto exit;
   }
   if (OpenDevice(scsi_dev, scsi_id, (struct IORequest *)io_ptr, 0) != 0)
   {
      Printf("Error %ld opening SCSI device %s unit %ld\n", io_ptr->io_Error, (ULONG)scsi_dev, scsi_id);
      goto exit;
   }
   if ((scsi_cmd = (struct SCSICmd *)AllocMem(sizeof(struct SCSICmd), MEMF_CLEAR)) == NULL)
   {
      PutStr("AllocMem scsi_cmd failed\n");
      goto exit;
   }
   if ((scsi_sense = (UBYTE *)AllocMem(SENSE_LEN, MEMF_CLEAR)) == NULL)
   {
      PutStr("AllocMem scsi_sense failed\n");
      goto exit;
   }
   scsi_data = (UBYTE *)AllocMem(MAX_DATA_LEN, MEMF_CLEAR);
   if (scsi_data == NULL)
   {
      PutStr("AllocMem scsi_data failed\n");
      goto exit;
   }

   // Init the device and read some flags
   if (BlueSCSI_InitDevice())
   {
      PutStr("Error sending inquiry to device\n");
      goto exit;
   }
   capabilities_ok = Toolbox_GetCapabilities() == 0;
   if (!capabilities_ok && !scsi_isBlueSCSI && !scsi_isZuluSCSI)
   {
      PutStr("Toolbox API not available on this device\n");
      goto exit;
   }

   if (setdir || resetdir)
   {
      if (!(scsi_capabilities & BLUESCSI_TOOLBOX_CAP_SET_WORKING_DIR))
      {
         PutStr("Working directory not supported by this firmware (needs v2026.04.27+)\n");
         goto exit;
      }
      if (Toolbox_SetWorkingDir(setdir ? workdir : (char *)"") != 0)
      {
         goto exit;
      }
   }
   if (getdir)
   {
      char curdir[256];
      if (Toolbox_GetWorkingDir(curdir, sizeof(curdir)) == 0)
      {
         Printf("Working directory: %s\n", (ULONG)curdir);
      }
   }

   switch (toolboxCommand)
   {
   case TOOLBOX_DIR:
      Toolbox_List_Files(0);
      Toolbox_Show_files();
      break;
   case TOOLBOX_LISTCDS:
      if (scsi_removable || scsi_isCD)
      {
         Toolbox_List_Files(1);
         Toolbox_Show_files();
      }
      else
      {
         PutStr("Not a CDROM\n");
      }
      break;
   case TOOLBOX_SEND:
      Toolbox_PutFileByName(FilePart(filename), filename);
      break;
   case TOOLBOX_RECEIVE:
      Toolbox_List_Files(0);
      Toolbox_GetFileByName(filename, FilePart(filename));
      break;
   case TOOLBOX_LISTDEVICES:
      Toolbox_List_Devices();
      break;
   case TOOLBOX_SETCD:
      if (scsi_removable || scsi_isCD)
      {
         Toolbox_List_Files(1);
         if (nextcd<=0 || nextcd>(filecount))
         {
            SetIoErr(ERROR_BAD_NUMBER);
            PrintFault(IoErr(), NULL);
         }
         else
         {
            Toolbox_Next_CD(files[nextcd - 1].Index);
            DiskChange();
            SaveLastCD(files[nextcd - 1].Name);
         }
      }
      else
      {
         PutStr("Not a CDROM\n");
      }
      break;
   case TOOLBOX_EJECT:
      if (scsi_removable || scsi_isCD)
      {
         if (Toolbox_Eject() == 0)
         {
            SaveLastCD((char *)EJECTED_MARKER);
            DiskChange();
            PutStr("Ejected\n");
         }
      }
      else
      {
         PutStr("Not a CDROM\n");
      }
      break;
   case TOOLBOX_RESTORECD:
      if (scsi_removable || scsi_isCD)
      {
         Toolbox_RestoreCD();
      }
      break;
   case TOOLBOX_SETDEBUG:
      Toolbox_Debug(debugon);
      break;
   case TOOLBOX_INFO:
      Printf("Toolbox API version: %ld\n", (LONG)scsi_apiVersion);
      Printf("Capabilities: 0x%02lx\n", (LONG)scsi_capabilities);
      if (scsi_capabilities & BLUESCSI_TOOLBOX_CAP_LARGE_TRANSFERS)
         PutStr("  Large transfers supported\n");
      if (scsi_capabilities & BLUESCSI_TOOLBOX_CAP_LARGE_SEND)
         PutStr("  Large send supported\n");
      if (scsi_capabilities & BLUESCSI_TOOLBOX_CAP_SET_WORKING_DIR)
      {
         char curdir[256];
         PutStr("  Working directory supported\n");
         if (Toolbox_GetWorkingDir(curdir, sizeof(curdir)) == 0)
            Printf("  Working directory: %s\n", (ULONG)curdir);
      }
      Toolbox_List_Devices();
      break;
   }

exit:

   if (UtilityBase) CloseLibrary(UtilityBase);
   if (files) FreeMem(files, sizeof(struct FileEntry) * filecount);
   if (scsi_data) FreeMem(scsi_data, MAX_DATA_LEN);
   if (scsi_sense) FreeMem(scsi_sense, SENSE_LEN);
   if (scsi_cmd) FreeMem(scsi_cmd, sizeof(struct SCSICmd));
   if (io_ptr)
   {
      CloseDevice((struct IORequest *)io_ptr);
      DeleteIORequest(io_ptr);
   }
   if (mp_ptr) DeleteMsgPort(mp_ptr);
   return 0;
}

/* Send a diskchange command to the filesystem that is connected to this device/unit */
void DiskChange(void)
{
   char drive[256];
   char device[256];
   int found = 0;

   struct DosList *dl;
   dl = LockDosList(LDF_DEVICES | LDF_READ);

   while ((dl = NextDosEntry(dl,LDF_DEVICES)))
   {
      struct FileSysStartupMsg *fssm = BADDR(dl->dol_misc.dol_handler.dol_Startup);
      if (TypeOfMem(fssm) && (APTR)fssm > (APTR)1000)
      {
         if (fssm->fssm_Unit == scsi_id)
         {
            bstrcpy(device, BADDR(fssm->fssm_Device));
            if (Stricmp(scsi_dev, device) == 0)
            {
               bstrcpy(drive, BADDR(dl->dol_Name));
               found = 1;
               break;
            }
         }
      }
   }

   UnLockDosList(LDF_DEVICES | LDF_READ);

   if (found)
   {
      strncat(drive, ":", sizeof(drive) - strlen(drive) - 1);
      Inhibit(drive, DOSTRUE);
      Inhibit(drive, DOSFALSE);
   }
}

/* Send a SCSI inquiry command to the device to gather some info */
int BlueSCSI_InitDevice(void)
{
   UBYTE command[] = {SCSI_CMD_INQ, 0, 0, 0, 252, 0};
   int err;

   if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_DATA_LEN,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
   {
      Printf("SCSI error %ld\n", err);
      return -1;
   }

   if (scsi_cmd->scsi_Actual)
   {
      scsi_removable = (scsi_data[1] & 0x80) ? 1 : 0;
      scsi_isCD = (scsi_data[0] & 0x1F) == 0x05;
      scsi_isBlueSCSI = Strnicmp("BlueSCSI", &scsi_data[8], 8) == 0;
      scsi_isZuluSCSI = Strnicmp("ZuluSCSI", &scsi_data[8], 8) == 0;
      if (!scsi_isBlueSCSI && !scsi_isZuluSCSI)
      {
         scsi_isBlueSCSI = Toolbox_InquiryHasName("BlueSCSI");
         scsi_isZuluSCSI = Toolbox_InquiryHasName("ZuluSCSI");
      }
   }
   return 0;
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

static ULONG Toolbox_ParseEntrySize(const UBYTE *entry)
{
   return ((ULONG)entry[36] << 24)
      | ((ULONG)entry[37] << 16)
      | ((ULONG)entry[38] << 8)
      | (ULONG)entry[39];
}

static void Toolbox_FormatSize(char *buffer, int length, ULONG size)
{
   char temp[32];
   int pos = sizeof(temp) - 1;

   temp[pos] = '\0';
   do
   {
      temp[--pos] = '0' + (size % 10);
      size /= 10;
   } while (size != 0 && pos > 0);

   strncpy(buffer, &temp[pos], length);
   buffer[length - 1] = '\0';
}

/* Copy a BCPL string to a C string */
void bstrcpy(char *dest, UBYTE *src)
{
   int len = *src++;
   strncpy(dest, src, len + 1);
   dest[len] = 0;
}

/* Show all the files array */
void Toolbox_Show_files(void)
{
   struct FileEntry *file = files;
   int i;
   for (i = 0; i < filecount; i++)
   {
      char size_text[32];

      Printf("%2ld: %-32s", i+1, (ULONG)file->Name);
      if (file->Type == 1)
      {
         Toolbox_FormatSize(size_text, sizeof(size_text), file->Size);
         Printf("%10s\n", (ULONG)size_text);
      }
      else
         PutStr("       Dir\n");
      file++;
   }
}

/* Select a CD image by its toolbox index. */
void Toolbox_Next_CD(int index)
{
   UBYTE command[] = {BLUESCSI_TOOLBOX_SET_NEXT_CD, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   int err;
   command[1] = index;
   if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_DATA_LEN,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
   {
      Printf("SCSI error %ld\n", err);
      return;
   }
}

/* Count the number of files/CD rom images */
int Toolbox_Count_Files(int cdrom)
{
   UBYTE command[] = {BLUESCSI_TOOLBOX_COUNT_FILES, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   int err;
   int count = 0;

   if (cdrom)
   {
      command[0] = BLUESCSI_TOOLBOX_COUNT_CDS;
   }

   // Causes an Unknown MsgID error if sent to a HD!?
   if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_DATA_LEN,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
   {
      Printf("SCSI error %ld\n", err);
      return -1;
   }

   if (scsi_cmd->scsi_Actual)
   {
      count = (int)scsi_data[0];
      if (count > 0)
      {
         filecount = count;
         files = (struct FileEntry *)AllocMem(sizeof(struct FileEntry) * count, MEMF_CLEAR);
      }
   }
   return count;
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

/* Set the device's working directory (absolute SD path, e.g. "/shared/sub").
 * "" resets to the firmware default. All file/CD ops then resolve against it,
 * and it persists on the device until reset or power-off. Firmware caps the
 * path at 63 chars (64-byte data phase). */
int Toolbox_SetWorkingDir(char *path)
{
   UBYTE command[10] = {0};
   int len = strlen(path);
   int err;

   if (len > 63)
   {
      PutStr("Path too long (max 63 characters)\n");
      return -1;
   }
   memcpy(scsi_data, path, len);
   scsi_data[len] = 0;      /* single NUL = reset to default */

   command[0] = BLUESCSI_TOOLBOX_METADATA;
   command[1] = BLUESCSI_TOOLBOX_SUBCMD_SET_WORKING_DIR;
   command[8] = (UBYTE)(len + 1);

   if ((err = DoScsiCmd((UBYTE *)scsi_data, len + 1,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_WRITE | SCSIF_AUTOSENSE))) != 0)
   {
      Printf("SCSI error %ld setting working directory\n", err);
      return -1;
   }
   return 0;
}

/* Read the current effective working directory into buf. */
int Toolbox_GetWorkingDir(char *buf, int buflen)
{
   UBYTE command[10] = {0};
   int err;
   int i;

   command[0] = BLUESCSI_TOOLBOX_METADATA;
   command[1] = BLUESCSI_TOOLBOX_SUBCMD_GET_WORKING_DIR;
   command[8] = 255;        /* firmware pads the data phase to this length */

   if ((err = DoScsiCmd((UBYTE *)scsi_data, 255,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
   {
      Printf("SCSI error %ld reading working directory\n", err);
      buf[0] = '\0';
      return -1;
   }

   for (i = 0; i < buflen - 1 && i < 255 && scsi_data[i]; i++)
      buf[i] = (char)scsi_data[i];
   buf[i] = '\0';
   return 0;
}

/* Eject the medium: standard SCSI START STOP UNIT with LoEj set. On
 * BlueSCSI the tray stays empty only with ReinsertAfterEject=0 in
 * bluescsi.ini (otherwise the firmware auto-inserts the next image). */
int Toolbox_Eject(void)
{
   UBYTE command[] = {0x1B, 0, 0, 0, 0x02, 0};
   int err;

   if ((err = DoScsiCmd((UBYTE *)scsi_data, 0,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_WRITE | SCSIF_AUTOSENSE))) != 0)
   {
      Printf("SCSI error %ld ejecting\n", err);
   }
   return err;
}

/* Remember the mounted image (or the ejected marker) in ENV: + ENVARC:.
 * Shared state with CDChanger's Eject/Select buttons. */
void SaveLastCD(char *name)
{
   static const char * const paths[] = {LASTCD_ENV, LASTCD_ENVARC};
   int i;

   for (i = 0; i < 2; i++)
   {
      BPTR fh = Open((STRPTR)paths[i], MODE_NEWFILE);
      if (fh)
      {
         Write(fh, name, strlen(name));
         Close(fh);
      }
   }
}

int ReadLastCD(char *buf, int buflen)
{
   BPTR fh = Open((STRPTR)LASTCD_ENV, MODE_OLDFILE);
   LONG got;

   if (!fh)
      fh = Open((STRPTR)LASTCD_ENVARC, MODE_OLDFILE);
   if (!fh)
      return 0;
   got = Read(fh, buf, buflen - 1);
   Close(fh);
   if (got <= 0)
      return 0;
   buf[got] = '\0';
   while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r' || buf[got - 1] == ' '))
      buf[--got] = '\0';
   return got > 0;
}

/* Boot-time restore (User-Startup): bring back the last mounted CD by
 * name, or keep the tray empty if it was ejected. Silent when there is
 * no saved state. */
void Toolbox_RestoreCD(void)
{
   char last[80];
   int i;

   if (!ReadLastCD(last, sizeof(last)))
      return;

   if (Stricmp((STRPTR)last, (STRPTR)EJECTED_MARKER) == 0)
   {
      Toolbox_Eject();                    /* ensure the tray is empty */
      DiskChange();
      return;
   }

   Toolbox_List_Files(1);
   for (i = 0; i < filecount; i++)
   {
      if (Stricmp((STRPTR)files[i].Name, (STRPTR)last) == 0)
      {
         Toolbox_Next_CD(files[i].Index);
         DiskChange();
         Printf("Restored CD: %s\n", (ULONG)last);
         return;
      }
   }
   Printf("Saved CD '%s' not found on the device\n", (ULONG)last);
}

static const char *deviceTypeName(UBYTE type)
{
   switch (type)
   {
   case 0: return "Hard disk";
   case 1: return "Removable disk";
   case 2: return "CD-ROM";
   case 3: return "Floppy";
   case 4: return "Magneto-optical";
   case 5: return "Tape";
   case 6: return "Network";
   case 7: return "ZIP drive";
   case 0xFF: return "Not enabled";
   default: return "Unknown";
   }
}

/* List active SCSI devices via metadata subcommand */
int Toolbox_List_Devices(void)
{
   UBYTE command[] = {BLUESCSI_TOOLBOX_METADATA, BLUESCSI_TOOLBOX_SUBCMD_LIST_DEVICES, 0, 0, 0, 0, 0, 0, 8, 0};
   int err;
   int i;

   if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_DATA_LEN,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
   {
      Printf("SCSI error %ld\n", err);
      return -1;
   }

   if (scsi_cmd->scsi_Actual)
   {
      PutStr("SCSI Devices:\n");
      for (i = 0; i < (int)scsi_cmd->scsi_Actual; i++)
      {
         if (scsi_data[i] != 0xFF)
         {
            Printf("  ID %ld: %s (%ld)\n", (LONG)i, (ULONG)deviceTypeName(scsi_data[i]), (LONG)scsi_data[i]);
         }
      }
   }

   return 0;
}

/* Used for sorting dirs/files */
int FileCompare(const void *s1, const void *s2)
{
   struct FileEntry *e1 = (struct FileEntry *)s1;
   struct FileEntry *e2 = (struct FileEntry *)s2;
   if (e1->Type == e2->Type)
      return Stricmp(e1->Name, e2->Name);
   else
      return e1->Type - e2->Type;
}

/* List the files in the shared folder / CD images */
int Toolbox_List_Files(int cdrom)
{
   UBYTE command[] = {BLUESCSI_TOOLBOX_LIST_FILES, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   int err;

   // Update the file count
   Toolbox_Count_Files(cdrom);

   if (cdrom)
   {
      command[0] = BLUESCSI_TOOLBOX_LIST_CDS;
   }

   if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_DATA_LEN,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
   {
      Printf("SCSI error %ld\n", err);
      return -1;
   }

   if (scsi_cmd->scsi_Actual)
   {
      struct FileEntry *file = files;
      int f;
      for (f = 0; f < filecount; f++)
      {
         UBYTE *c = &scsi_data[ENTRY_SIZE * f];
         file->Index = c[0];
         file->Type = c[1]; // 1=file 0=dir
         strncpy(file->Name, (char *)&c[2], MAX_MAC_PATH);
         file->Name[MAX_MAC_PATH] = '\0';
         file->Size = Toolbox_ParseEntrySize(c);
         file++;
      }

      qsort(files, filecount, sizeof(struct FileEntry), FileCompare);
   }
   return 0;
}

/* Copy a file from the shared folder to a destination */
ULONG Toolbox_GetFileByName(char *destination, char *source)
{
   ULONG count = 0;
   int index = -1;
   struct FileEntry *file = files;
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
      char size_text[32];
      UBYTE command[] = {BLUESCSI_TOOLBOX_GET_FILE, 0, 0, 0, 0, 0, 0, 0, 0, 0};
      BPTR fh;
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
            Printf("SCSI error %ld\n", err);
            break;
         }

         if (scsi_cmd->scsi_Actual)
         {
            ULONG remaining = size - count;
            ULONG chunk = scsi_cmd->scsi_Actual;

            if (chunk > remaining)
            {
               chunk = (ULONG)remaining;
            }

            if (chunk == 0)
            {
               break;
            }

            count += chunk;
            offset++;
            if (Write(fh, scsi_data, chunk) != (LONG)chunk)
            {
               Printf("Write error\n");
               break;
            }
         }
         else
         {
            break;
         }
      }
      Close(fh);
      Toolbox_FormatSize(size_text, sizeof(size_text), count);
      Printf("%s. %s bytes received\n", (ULONG)destination, (ULONG)size_text);
   }
   else
   {
      SetIoErr(ERROR_OBJECT_NOT_FOUND);
      PrintFault(IoErr(), destination);
   }
   
   return count;
}

/* Write a file to the shared folder.
 *
 * Protocol (per BlueSCSI firmware BlueSCSI_Toolbox.cpp):
 *   SEND_FILE_PREP (0xD3): DATA OUT = exactly 33 bytes (32 + NUL) = the
 *                          destination filename.
 *   SEND_FILE_10   (0xD4): DATA OUT = the byte count for this block. Legacy
 *                          mode (CDB[6]=0): count in CDB[1..2]; 512-byte block
 *                          offset in CDB[3..5], big-endian. We send <=512/cmd.
 *   SEND_FILE_END  (0xD5): no DATA OUT.
 *
 * All three are DATA-OUT (host->device): direction must be SCSIF_WRITE and the
 * SCSI transfer length must match the bytes the firmware reads. Upstream 1.3
 * used SCSIF_READ and scsi_Length=MAX_DATA_LEN for all of these, so uploads
 * never worked; corrected here.
 */
int Toolbox_PutFileByName(char *destination, char *source)
{
   UBYTE command[10] = {0};
   int err;
   int i;
   BPTR fh;
   ULONG offset;          /* in 512-byte blocks */
   ULONG total = 0;
   char size_text[32];

   /* Open the local source FIRST: don't PREP (which creates the file on the SD
    * card) until we know we can read what we're meant to upload. */
   fh = Open(source, MODE_OLDFILE);
   if (!fh)
   {
      SetIoErr(ERROR_OBJECT_NOT_FOUND);
      PrintFault(IoErr(), source);
      return 0;
   }

   /* --- SEND_FILE_PREP: 33-byte filename, DATA OUT --- */
   command[0] = BLUESCSI_TOOLBOX_SEND_FILE_PREP;
   for (i = 0; i < MAX_MAC_PATH && destination[i]; i++)
      scsi_data[i] = (UBYTE)destination[i];
   for (; i < MAX_MAC_PATH + 1; i++)
      scsi_data[i] = 0;

   if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_MAC_PATH + 1,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_WRITE | SCSIF_AUTOSENSE))) != 0)
   {
      Printf("SCSI error %ld preparing '%s'\n", err, (ULONG)destination);
      Close(fh);
      return 0;
   }

   /* --- SEND_FILE_10: stream up to 512 bytes per 512-byte block --- */
   offset = 0;
   while (1)
   {
      LONG len = Read(fh, scsi_data, 512);
      if (len < 0)
      {
         PrintFault(IoErr(), source);
         Close(fh);
         return 0;
      }
      if (len == 0)
         break;

      command[0] = BLUESCSI_TOOLBOX_SEND_FILE_10;
      command[1] = (UBYTE)((len >> 8) & 0xFF);   /* byte count hi */
      command[2] = (UBYTE)(len & 0xFF);          /* byte count lo */
      command[3] = (UBYTE)((offset >> 16) & 0xFF);
      command[4] = (UBYTE)((offset >> 8) & 0xFF);
      command[5] = (UBYTE)(offset & 0xFF);
      command[6] = 0;                            /* legacy byte-count mode */
      if ((err = DoScsiCmd((UBYTE *)scsi_data, (int)len,
                           (UBYTE *)&command, sizeof(command),
                           (SCSIF_WRITE | SCSIF_AUTOSENSE))) != 0)
      {
         Printf("SCSI error %ld sending '%s'\n", err, (ULONG)destination);
         Close(fh);
         return 0;
      }

      total += (ULONG)len;
      offset++;
   }
   Close(fh);

   /* --- SEND_FILE_END: no data phase --- */
   command[0] = BLUESCSI_TOOLBOX_SEND_FILE_END;
   command[1] = 0; command[2] = 0; command[3] = 0;
   command[4] = 0; command[5] = 0; command[6] = 0;
   if ((err = DoScsiCmd((UBYTE *)scsi_data, 0,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_WRITE | SCSIF_AUTOSENSE))) != 0)
   {
      Printf("SCSI error %ld finishing '%s'\n", err, (ULONG)destination);
      return 0;
   }

   Toolbox_FormatSize(size_text, sizeof(size_text), total);
   Printf("%s. %s bytes sent\n", (ULONG)destination, (ULONG)size_text);
   return (int)total;
}

/* Enable/Disable debug logging on the device */
void Toolbox_Debug(int debugon)
{
   int err;
   UBYTE command[] = {BLUESCSI_TOOLBOX_TOGGLE_DEBUG, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   // CDB[1]=0 means SET debug level, CDB[2]=value
   command[1] = 0;
   command[2] = debugon;

   if ((err = DoScsiCmd((UBYTE *)scsi_data, MAX_DATA_LEN,
                        (UBYTE *)&command, sizeof(command),
                        (SCSIF_READ | SCSIF_AUTOSENSE))) != 0)
   {
      Printf("SCSI error %ld\n", err);
   }
   else
   {
      Printf("Debug set to %ld\n", (LONG)debugon);
   }
}

/* Send a SCSI command */
int DoScsiCmd(UBYTE *data, int datasize, UBYTE *cmd, int cmdsize, UBYTE flags)
{
   io_ptr->io_Length = sizeof(struct SCSICmd);
   io_ptr->io_Data = scsi_cmd;
   io_ptr->io_Command = HD_SCSICMD;

//Printf("io_ptr->io_Flags=%lx\n", io_ptr->io_Flags);
//   io_ptr->io_Flags = 0;

   scsi_cmd->scsi_Data = (UWORD *)data;
   scsi_cmd->scsi_Length = datasize;
   scsi_cmd->scsi_SenseActual = 0;
   scsi_cmd->scsi_SenseLength = SENSE_LEN;
   scsi_cmd->scsi_SenseData = scsi_sense;
   scsi_cmd->scsi_Command = cmd;
   scsi_cmd->scsi_CmdLength = cmdsize;
   scsi_cmd->scsi_Flags = flags;
#if DEBUG
   dump("\nCalling DoIO", cmd, cmdsize);
#endif
   DoIO((struct IORequest *)io_ptr);

#if DEBUG
   Printf("io_Error=%lx\n", io_ptr->io_Error);
#endif
   return (io_ptr->io_Error);
}

#if DEBUG
void dump(char *msg, UBYTE *d, int len)
{
   Printf("%s (%lu) ", msg, (ULONG)len);
   for (int i = 0; i < len; i++)
   {
      if (i % 16 == 0)
         Printf("\n%04lx   ", (ULONG)i);
      Printf("%02lx ", d[i]);
   }
   Printf("\n");
}
#endif

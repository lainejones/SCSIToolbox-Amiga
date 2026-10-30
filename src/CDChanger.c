/*
 * Copyright (C) 2024 Paul Hill
 *
 * Modified 2026-08-07 by Laine Jones (lainejones): part of SCSIToolbox-Amiga.
 * Added an Eject button (SCSI START STOP UNIT) and last-CD persistence:
 * the selected image name (or an ejected marker) is written to
 * ENV(ARC):CDChanger.lastcd so "BlueSCSIToolbox RESTORECD" in User-Startup
 * can restore it after a reboot. Pair with StartEjected=1 and
 * ReinsertAfterEject=0 in bluescsi.ini for empty-tray-by-default behaviour.
 * Modified 2026-10-28: GadTools user interface (gtui.c) instead of ReAction,
 * so it runs on AmigaOS 2.04/3.0/3.1 without ClassAct/ReAction installed.
 *
 * This program is free software: you can redistribute and/or modify
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
#include <string.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <clib/alib_protos.h>
#include "toolbox.h"
#include "gtui.h"

__attribute__((used)) static const char ver[] = "$VER: CDChanger 1.9.1 (30.10.2026)";

/* Marker stored instead of an image name when the tray was ejected */
#define EJECTED_MARKER "<EJECTED>"

/* = NULL: strong definitions, so the startup code does not try to open these */
struct Library *UtilityBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;

static char scsi_dev[256];
static LONG scsi_unit = -1;

static char *appname = "CD Changer";

/* One list row: "<n>  <image name>". The images are shown in the order the
   device lists them (do NOT sort: index is the device's position). */
#define MAX_NODES 128
struct CdNode
{
   struct Node node;
   UBYTE index;                 /* for SET_NEXT_CD */
   char name[MAX_MAC_PATH + 1]; /* for CDChanger.lastcd */
   char text[MAX_MAC_PATH + 8];
};
static struct CdNode gNodes[MAX_NODES];
static struct List gList;

static struct GtUI ui;

extern UWORD bluescsi_logo_data[];
extern UWORD zuluscsi_logo_data[];

static void DiskChange(void);
static void SaveLastCD(const char *name);

int main(int argc, char **argv)
{
   struct FileEntry *files;
   char info[80];
   int count = 0;

   /* Suppress DOS volume requesters from our own process before any DOS activity. */
   ((struct Process *)FindTask(NULL))->pr_WindowPtr = (APTR)-1L;

   { const char * volatile keepver = ver; (void)keepver; } /* keep $VER through --gc-sections */

   if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37)))
   {
      PutStr("Needs AmigaOS 2.04 or newer\n");
      goto exit;
   }
   if (!(UtilityBase = OpenLibrary("utility.library", 37)))
   {
      MessageBox(appname, "Could not open utility.library");
      goto exit;
   }

   if (!gtui_GetArgs(argc, argv, appname, scsi_dev, sizeof(scsi_dev), &scsi_unit))
      goto exit;

   if (scsi_setup(scsi_dev, scsi_unit) != 0)
   {
      char msg[96];
      sprintf(msg, "scsi_setup failed!\nDevice: %.40s\nUnit: %ld", scsi_dev, scsi_unit);
      MessageBox(appname, msg);
      goto exit;
   }

   NewList(&gList);
   files = Toolbox_List_Files(1);
   if (files)
   {
      struct FileEntry *file;

      for (file = files; file->Type >= 0 && count < MAX_NODES; file++)
      {
         struct CdNode *n;

         if (file->Type != BLUESCSI_FILE)
            continue;
         n = &gNodes[count++];
         n->index = (UBYTE)file->Index;
         strcpy(n->name, file->Name);
         sprintf(n->text, "%-3ld %s", (LONG)count, file->Name);
         n->node.ln_Name = n->text;
         AddTail(&gList, &n->node);
      }
   }
   else if (argc > 0)
   {
      Printf("no files\n");
   }

   sprintf(info, "%.40s, unit %ld", scsi_dev, scsi_unit);

   ui.title = appname;
   ui.info = info;
   ui.logoData = scsi_isZuluSCSI ? zuluscsi_logo_data : bluescsi_logo_data;
   ui.labels = &gList;
   ui.nButtons = 2;
   ui.buttonText[0] = "_Select";
   ui.buttonText[1] = "_Eject";
   ui.hasStatus = FALSE;

   if (!gtui_Open(&ui, 250, 250))
   {
      MessageBox(appname, "Could not open window");
      goto exit;
   }

   for (;;)
   {
      LONG arg = 0;
      int ev = gtui_Wait(&ui, &arg);

      if (ev == GTUI_QUIT)
         break;
      if (ev != GTUI_BUTTON)
         continue;                               /* a click only selects */

      if (arg == 0)                              /* Select */
      {
         struct CdNode *n = (struct CdNode *)gtui_NodeAt(&gList, ui.selected);
         if (n)
         {
            Toolbox_Set_Next_CD(n->index);
            DiskChange();
            SaveLastCD(n->name);
         }
      }
      else                                       /* Eject */
      {
         if (Toolbox_Eject() == 0)
         {
            DiskChange();
            SaveLastCD(EJECTED_MARKER);
         }
         else
         {
            MessageBox(appname, "Eject failed (SCSI error)");
         }
      }
   }

exit:
   gtui_Close(&ui);
   scsi_cleanup();
   if (UtilityBase) CloseLibrary(UtilityBase);
   if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
   return 0;
}

/* Copy a BCPL string to a C string */
static void bstrcpy(char *dest, UBYTE *src)
{
   int len = *src++;
   strncpy(dest, (char *)src, len + 1);
   dest[len] = 0;
}

/* Make the filesystem mounted on this device/unit notice the new disc */
static void DiskChange(void)
{
#ifndef TESTMODE
   char drive[256];
   char device[256];
   int found = 0;
   struct DosList *dl;

   dl = LockDosList(LDF_DEVICES | LDF_READ);
   while ((dl = NextDosEntry(dl, LDF_DEVICES)))
   {
      struct FileSysStartupMsg *fssm = BADDR(dl->dol_misc.dol_handler.dol_Startup);
      if (TypeOfMem(fssm) && (APTR)fssm > (APTR)1000)
      {
         if ((LONG)fssm->fssm_Unit == scsi_unit)
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
#endif
}

/* Remember the mounted image (or the ejected marker) in ENV: + ENVARC:
   so BlueSCSIToolbox RESTORECD can bring it back after a reboot. */
static void SaveLastCD(const char *name)
{
   static const char * const paths[] = {"ENV:CDChanger.lastcd",
                                        "ENVARC:CDChanger.lastcd"};
   int i;

   for (i = 0; i < 2; i++)
   {
      BPTR fh = Open((STRPTR)paths[i], MODE_NEWFILE);
      if (fh)
      {
         Write(fh, (APTR)name, strlen(name));
         Close(fh);
      }
   }
}

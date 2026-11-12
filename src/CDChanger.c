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
 * Modified 2026-10-30: right-mouse-button menus (Project / CD / Settings),
 * Refresh list, a Device settings page kept in ENV:CDChanger.prefs, and
 * Snapshot / Unsnapshot window.
 *
 * Settings precedence (lowest first):
 *   1. the icon's Tool Types DEVICE= and UNIT= (Workbench start)
 *   2. ENV:CDChanger.prefs, else ENVARC:CDChanger.prefs - plain KEY=value
 *      lines: DEVICE, UNIT, WINDOW=x,y,w,h. DEVICE/UNIT are only in the file
 *      once they were set in Settings > Device; setting them back to the
 *      Tool Types values (the Defaults button) takes them out again.
 *   3. DEVICE= and UNIT= on the command line (Shell start) beat the file.
 * Save writes ENV: and ENVARC: (kept), Use writes ENV: only (until reboot).
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
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <clib/alib_protos.h>
#include "toolbox.h"
#include "gtui.h"
#include "amigui.h"

#define VERSION   "1.9.3"
#define PREFSFILE "CDChanger.prefs"

__attribute__((used)) static const char ver[] = "$VER: CDChanger " VERSION " (12.11.2026)";

/* Marker stored instead of an image name when the tray was ejected */
#define EJECTED_MARKER "<EJECTED>"

/* = NULL: strong definitions, so the startup code does not try to open these */
struct Library *UtilityBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;

static char scsi_dev[256];
static LONG scsi_unit = -1;
static BOOL gDevOK = FALSE;             /* a device is open (scsi_setup worked) */
static BOOL gFromCLI = FALSE;

/* DEVICE/UNIT from the Tool Types or the command line: the Defaults of
   Settings > Device */
static char gArgDev[256];
static LONG gArgUnit = -1;

/* The settings (see the precedence at the top) */
static struct
{
   char dev[256];                       /* "" = not in the file */
   LONG unit;                           /* -1 = not in the file */
   WORD wx, wy, ww, wh;                 /* snapshot; ww 0 = none */
} gP;

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
static char gInfo[80], gInfoAlt[96];

extern UWORD bluescsi_logo_data[];
extern UWORD zuluscsi_logo_data[];

/* ---- menus ---------------------------------------------------------------- */

enum
{
   MN_ABOUT = 1, MN_QUIT,
   MN_SETCD, MN_EJECT, MN_REFRESH,
   MN_SET_DEVICE, MN_SNAPSHOT, MN_UNSNAPSHOT
};

static struct NewMenu menuDef[] =
{
   {NM_TITLE, (STRPTR)"Project",           NULL, 0, 0, NULL},
   {NM_ITEM,  (STRPTR)"About...",          NULL, 0, 0, (APTR)MN_ABOUT},
   {NM_ITEM,  NM_BARLABEL,                 NULL, 0, 0, NULL},
   {NM_ITEM,  (STRPTR)"Quit",      (STRPTR)"Q",  0, 0, (APTR)MN_QUIT},
   {NM_TITLE, (STRPTR)"CD",                NULL, 0, 0, NULL},
   {NM_ITEM,  (STRPTR)"Set CD",    (STRPTR)"S",  0, 0, (APTR)MN_SETCD},
   {NM_ITEM,  (STRPTR)"Eject",     (STRPTR)"E",  0, 0, (APTR)MN_EJECT},
   {NM_ITEM,  NM_BARLABEL,                 NULL, 0, 0, NULL},
   {NM_ITEM,  (STRPTR)"Refresh list", (STRPTR)"R", 0, 0, (APTR)MN_REFRESH},
   {NM_TITLE, (STRPTR)"Settings",          NULL, 0, 0, NULL},
   {NM_ITEM,  (STRPTR)"Device...",         NULL, 0, 0, (APTR)MN_SET_DEVICE},
   {NM_ITEM,  NM_BARLABEL,                 NULL, 0, 0, NULL},
   {NM_ITEM,  (STRPTR)"Snapshot window",   NULL, 0, 0, (APTR)MN_SNAPSHOT},
   {NM_ITEM,  (STRPTR)"Unsnapshot window", NULL, 0, 0, (APTR)MN_UNSNAPSHOT},
   {NM_END,   NULL,                        NULL, 0, 0, NULL}
};

static void DiskChange(void);
static void SaveLastCD(const char *name);
static void BuildList(void);
static void ShowList(void);
static void SetHeader(void);
static void SetCD(void);
static void Eject(void);
static void PrefsLine(const char *key, char *val);
static BOOL PrefsWrite(BOOL keep);
static void DevicePage(void);

/* Started from Workbench, or by Run, a program gets the default 4 KB stack
   (the icon says 4096): too little for the ASL file requester and
   EasyRequest on top of the 1 KB path buffers - SDTransfer overran it and
   damaged the RAM disk's memory on an A1200. So it runs on its own 32 KB,
   through StackSwap(), as WHDUpdate does; across the swap only globals are
   used. */
#define NEED_STACK 32768UL
static struct StackSwapStruct swap;
static APTR swapMem;
static int swapRc, g_argc;
static char **g_argv;

static int cdc_main(int argc, char **argv);

int main(int argc, char **argv)
{
   struct Task *me = FindTask(NULL);

   if ((ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower >= NEED_STACK - 4096)
      return cdc_main(argc, argv);
   if (!(swapMem = AllocVec(NEED_STACK, MEMF_ANY)))
   {
      PutStr("CDChanger: not enough memory for its stack\n");
      return RETURN_FAIL;
   }
   g_argc = argc;
   g_argv = argv;
   swap.stk_Lower = swapMem;
   swap.stk_Upper = (ULONG)swapMem + NEED_STACK;
   swap.stk_Pointer = (APTR)swap.stk_Upper;
   StackSwap(&swap);
   swapRc = cdc_main(g_argc, g_argv);
   StackSwap(&swap);
   FreeVec(swapMem);
   return swapRc;
}

static int cdc_main(int argc, char **argv)
{
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

   /* Tool Types / command line, then the settings file (see the top) */
   if (!gtui_GetArgs(argc, argv, gArgDev, sizeof(gArgDev), &gArgUnit, &gFromCLI))
      goto exit;
   gP.unit = -1;
   gtui_PrefsRead(PREFSFILE, PrefsLine);
   strcpy(scsi_dev, gArgDev);
   scsi_unit = gArgUnit;
   if (gP.dev[0] && !(gFromCLI && gArgDev[0]))
      strcpy(scsi_dev, gP.dev);
   if (gP.unit >= 0 && !(gFromCLI && gArgUnit >= 0))
      scsi_unit = gP.unit;
   if (!gtui_CheckDevice(appname, PREFSFILE, gFromCLI, scsi_dev, scsi_unit))
      goto exit;

   if (scsi_setup(scsi_dev, scsi_unit) != 0)
   {
      char msg[96];
      sprintf(msg, "scsi_setup failed!\nDevice: %.40s\nUnit: %ld", scsi_dev, scsi_unit);
      MessageBox(appname, msg);
      goto exit;
   }
   gDevOK = TRUE;

   BuildList();

   ui.title = appname;
   ui.labels = &gList;
   ui.nButtons = 2;
   ui.buttonText[0] = "_Select";
   ui.buttonText[1] = "_Eject";
   ui.hasStatus = FALSE;
   ui.menuDef = menuDef;
   ui.snapX = gP.wx;
   ui.snapY = gP.wy;
   ui.snapW = gP.ww;
   ui.snapH = gP.wh;
   SetHeader();

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
      if (ev == GTUI_BUTTON)                     /* a list click only selects */
      {
         if (arg == 0)
            SetCD();
         else
            Eject();
      }
      else if (ev == GTUI_MENU)
      {
         if (arg == MN_QUIT)
            break;
         switch (arg)
         {
            case MN_ABOUT:
               amg_about(ui.win, "CDChanger", VERSION,
                         "Switches the CD image a BlueSCSI or ZuluSCSI emulates.\n"
                         "Part of SCSIToolbox-Amiga. Licence: GPL-3.0-or-later.");
               break;
            case MN_SETCD:      SetCD(); break;
            case MN_EJECT:      Eject(); break;
            case MN_REFRESH:    ShowList(); break;
            case MN_SET_DEVICE: DevicePage(); break;
            case MN_SNAPSHOT:   /* this place and size from now on (saved) */
               gP.wx = ui.win->LeftEdge;
               gP.wy = ui.win->TopEdge;
               gP.ww = ui.win->Width;
               gP.wh = ui.win->Height;
               if (!PrefsWrite(TRUE))
                  MessageBox(appname, "Could not write ENV:" PREFSFILE);
               break;
            case MN_UNSNAPSHOT: /* the default place and size at the next start */
               gP.ww = 0;
               if (!PrefsWrite(TRUE))
                  MessageBox(appname, "Could not write ENV:" PREFSFILE);
               break;
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

/* The logo and the line under it for the open device */
static void SetHeader(void)
{
   const char *kind = scsi_isZuluSCSI ? "ZuluSCSI" : "BlueSCSI";

   if (gDevOK)
   {
      sprintf(gInfo, "%.40s, unit %ld", scsi_dev, scsi_unit);
      sprintf(gInfoAlt, "%s: %.40s, unit %ld", kind, scsi_dev, scsi_unit);
   }
   else
   {
      strcpy(gInfo, "No device - see Settings");
      strcpy(gInfoAlt, gInfo);
   }
   gtui_SetHeader(&ui, scsi_isZuluSCSI ? zuluscsi_logo_data : bluescsi_logo_data,
                  gInfo, gInfoAlt);
}

/* The device's CD images, in its own order. The list must be detached from
   the gadget (or the window not open yet) while this runs. */
static void BuildList(void)
{
   struct FileEntry *files;
   int count = 0;

   NewList(&gList);
   if (!gDevOK)
      return;
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
   else if (gFromCLI && !ui.win)
   {
      Printf("no files\n");
   }
}

/* Re-read the images into the window */
static void ShowList(void)
{
   gtui_DetachList(&ui);
   BuildList();
   gtui_AttachList(&ui, &gList, -1);
}

static void SetCD(void)
{
   struct CdNode *n = (struct CdNode *)gtui_NodeAt(&gList, ui.selected);

   if (!n || !gDevOK)
   {
      DisplayBeep(ui.scr);
      return;
   }
   Toolbox_Set_Next_CD(n->index);
   DiskChange();
   SaveLastCD(n->name);
}

static void Eject(void)
{
   if (!gDevOK)
   {
      DisplayBeep(ui.scr);
      return;
   }
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

/* ---- settings ------------------------------------------------------------- */

static void PrefsLine(const char *key, char *val)
{
   LONG n;

   if (!strcmp(key, "DEVICE"))
   {
      strncpy(gP.dev, val, sizeof(gP.dev) - 1);
      gP.dev[sizeof(gP.dev) - 1] = '\0';
   }
   else if (!strcmp(key, "UNIT"))
   {
      if (StrToLong((STRPTR)val, &n) > 0 && n >= 0)
         gP.unit = n;
   }
   else if (!strcmp(key, "WINDOW"))
   {
      if (!gtui_ParseBox(val, &gP.wx, &gP.wy, &gP.ww, &gP.wh))
         gP.ww = 0;
   }
}

static BOOL PrefsWrite(BOOL keep)
{
   static char buf[400];
   char *p = buf;

   buf[0] = '\0';
   if (gP.dev[0])
   {
      sprintf(p, "DEVICE=%s\n", gP.dev);
      p += strlen(p);
   }
   if (gP.unit >= 0)
   {
      sprintf(p, "UNIT=%ld\n", gP.unit);
      p += strlen(p);
   }
   if (gP.ww > 0)
   {
      sprintf(p, "WINDOW=%ld,%ld,%ld,%ld\n",
                   (LONG)gP.wx, (LONG)gP.wy, (LONG)gP.ww, (LONG)gP.wh);
      p += strlen(p);
   }
   return (BOOL)(amg_env_write(PREFSFILE, buf, (ULONG)(p - buf), keep) != 0);
}

/* Close the device and open dev/unit instead; back to the old one if that
   fails. */
static void SwitchDevice(const char *dev, LONG unit)
{
   char oldDev[256];
   LONG oldUnit = scsi_unit;

   if (gDevOK && !strcmp(dev, scsi_dev) && unit == scsi_unit)
      return;
   strcpy(oldDev, scsi_dev);

   gtui_Busy(&ui, TRUE);
   scsi_cleanup();
   strcpy(scsi_dev, dev);
   scsi_unit = unit;
   gDevOK = (BOOL)(scsi_setup(scsi_dev, scsi_unit) == 0);
   if (!gDevOK)
   {
      scsi_cleanup();
      MessageBox(appname, "Could not use that device: back to the previous one.");
      strcpy(scsi_dev, oldDev);
      scsi_unit = oldUnit;
      gDevOK = (BOOL)(oldDev[0] && scsi_setup(scsi_dev, scsi_unit) == 0);
      if (!gDevOK)
         scsi_cleanup();
   }
   SetHeader();
   ShowList();
   gtui_Busy(&ui, FALSE);
}

static const struct GtField devFields[] =
{
   {"Device", GTF_STR, 255},
   {"Unit",   GTF_INT, 3}
};

static void DevicePage(void)
{
   struct GtVals v, def;
   int res;

   memset(&v, 0, sizeof(v));
   memset(&def, 0, sizeof(def));
   strcpy(v.s[0], scsi_dev);
   v.n[1] = scsi_unit;
   strcpy(def.s[0], gArgDev);
   def.n[1] = gArgUnit >= 0 ? gArgUnit : 0;

   res = gtui_Page(&ui, "CDChanger: Device", devFields, 2,
                   "Defaults = the icon's Tool Types", &v, &def);
   if (res == AMG_CANCEL)
      return;
   if (!v.s[0][0] || v.n[1] < 0)
   {
      MessageBox(appname, "Give a device name and a unit number.");
      return;
   }

   /* The same as the Tool Types: leave them in charge (not in the file) */
   if (!strcmp(v.s[0], gArgDev) && v.n[1] == gArgUnit)
   {
      gP.dev[0] = '\0';
      gP.unit = -1;
   }
   else
   {
      strncpy(gP.dev, v.s[0], sizeof(gP.dev) - 1);
      gP.dev[sizeof(gP.dev) - 1] = '\0';
      gP.unit = v.n[1];
   }
   if (!PrefsWrite(res == AMG_SAVE))
      MessageBox(appname, "Could not write ENV:" PREFSFILE);
   SwitchDevice(v.s[0], v.n[1]);
}

/*
 * Copyright (C) 2024 Paul Hill
 *
 * Modified 2026-08-06 by Laine Jones (lainejones): part of SCSIToolbox-Amiga.
 * Added SD-card subdirectory navigation (firmware v2026.04.27+ working-dir
 * support): directories are listed with a trailing "/", the "/" entry goes up,
 * and the Open/Download button descends into a selected directory.
 * Modified 2026-10-28: GadTools user interface (gtui.c) instead of ReAction,
 * so it runs on AmigaOS 2.04/3.0/3.1 without ClassAct/ReAction installed.
 * Modified 2026-10-30: right-mouse-button menus (Project / Transfer /
 * Settings), Upload, Refresh, Parent folder, settings pages (Device, Drawers)
 * kept in ENV:SDTransfer.prefs, and Snapshot / Unsnapshot window.
 *
 * Settings precedence (lowest first):
 *   1. built-in defaults (no device, the program's drawer, remember drawers)
 *   2. the icon's Tool Types DEVICE= and UNIT= (Workbench start)
 *   3. ENV:SDTransfer.prefs, else ENVARC:SDTransfer.prefs - plain KEY=value
 *      lines: DEVICE, UNIT, DOWNLOADDIR, UPLOADDIR, REMEMBERDIRS (YES/NO),
 *      WINDOW=x,y,w,h. DEVICE/UNIT are only in the file once they were set
 *      in Settings > Device; setting them back to the Tool Types values
 *      (the Defaults button) takes them out again.
 *   4. DEVICE= and UNIT= on the command line (Shell start) beat the file.
 * Save writes ENV: and ENVARC: (kept), Use writes ENV: only (until reboot).
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
#include <string.h>
#include <exec/memory.h>
#include <libraries/asl.h>
#include <proto/exec.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <proto/utility.h>
#include <clib/alib_protos.h>
#include "toolbox.h"
#include "gtui.h"
#include "amigui.h"

#define VERSION   "1.9.3"
#define PREFSFILE "SDTransfer.prefs"

__attribute__((used)) static const char ver[] = "$VER: SDTransfer " VERSION " (12.11.2026)";

/* Kinds of list row */
#define NODE_DIR    0
#define NODE_FILE   1
#define NODE_PARENT 2

/* = NULL: strong definitions, so the startup code does not try to open these */
struct Library *UtilityBase = NULL, *AslBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;

static char scsi_dev[256];
static LONG scsi_unit = -1;
static BOOL gDevOK = FALSE;             /* a device is open (scsi_setup worked) */

/* DEVICE/UNIT from the Tool Types or the command line: the Defaults of
   Settings > Device */
static char gArgDev[256];
static LONG gArgUnit = -1;

/* The settings (see the precedence at the top) */
static struct
{
   char dev[256];                       /* "" = not in the file */
   LONG unit;                           /* -1 = not in the file */
   char downDir[256], upDir[256];       /* "" = the program's drawer */
   BOOL remember;                       /* the requesters' last drawers */
   WORD wx, wy, ww, wh;                 /* snapshot; ww 0 = none */
} gP;

/* Working-directory navigation state (firmware CAP_SET_WORKING_DIR).
   gCurPath = absolute SD path currently browsed (starts at the device
   default, e.g. "/shared"); ascending is allowed all the way to "/",
   the SD card root. */
static int gHaveDirs = 0;
static char gCurPath[80];

static char *appname = "SD Transfer";

/* One list row. ln_Name points at text, which lives as long as the row. */
#define MAX_NODES 128
struct SdNode
{
   struct Node node;
   UBYTE kind;
   char text[40];
};
static struct SdNode gNodes[MAX_NODES];
static int gNodeCount = 0;
static struct List gList;

static struct GtUI ui;
static char gInfo[80], gInfoAlt[96];

extern UWORD bluescsi_logo_data[];
extern UWORD zuluscsi_logo_data[];

/* ---- menus ---------------------------------------------------------------- */

enum
{
   MN_ABOUT = 1, MN_QUIT,
   MN_OPEN, MN_UPLOAD, MN_PARENT, MN_REFRESH,
   MN_SET_DEVICE, MN_SET_DRAWERS, MN_SNAPSHOT, MN_UNSNAPSHOT
};
#define MENU_TRANSFER 1
#define ITEM_PARENT   2

static struct NewMenu menuDef[] =
{
   {NM_TITLE, (STRPTR)"Project",           NULL, 0, 0, NULL},
   {NM_ITEM,  (STRPTR)"About...",          NULL, 0, 0, (APTR)MN_ABOUT},
   {NM_ITEM,  NM_BARLABEL,                 NULL, 0, 0, NULL},
   {NM_ITEM,  (STRPTR)"Quit",      (STRPTR)"Q",  0, 0, (APTR)MN_QUIT},
   {NM_TITLE, (STRPTR)"Transfer",          NULL, 0, 0, NULL},
   {NM_ITEM,  (STRPTR)"Open / Download...",(STRPTR)"O", 0, 0, (APTR)MN_OPEN},
   {NM_ITEM,  (STRPTR)"Upload...",         (STRPTR)"U", 0, 0, (APTR)MN_UPLOAD},
   {NM_ITEM,  (STRPTR)"Parent folder",     (STRPTR)"P", 0, 0, (APTR)MN_PARENT},
   {NM_ITEM,  NM_BARLABEL,                 NULL, 0, 0, NULL},
   {NM_ITEM,  (STRPTR)"Refresh",           (STRPTR)"R", 0, 0, (APTR)MN_REFRESH},
   {NM_TITLE, (STRPTR)"Settings",          NULL, 0, 0, NULL},
   {NM_ITEM,  (STRPTR)"Device...",         NULL, 0, 0, (APTR)MN_SET_DEVICE},
   {NM_ITEM,  (STRPTR)"Drawers...",        NULL, 0, 0, (APTR)MN_SET_DRAWERS},
   {NM_ITEM,  NM_BARLABEL,                 NULL, 0, 0, NULL},
   {NM_ITEM,  (STRPTR)"Snapshot window",   NULL, 0, 0, (APTR)MN_SNAPSHOT},
   {NM_ITEM,  (STRPTR)"Unsnapshot window", NULL, 0, 0, (APTR)MN_UNSNAPSHOT},
   {NM_END,   NULL,                        NULL, 0, 0, NULL}
};

static BOOL RefreshFileList(void);
static void OpenOrDownload(LONG row);
static void Upload(void);
static void ParentFolder(void);
static void ShowList(void);
static void SetParentMenu(void);
static void SetHeader(void);
static void InitDirs(void);
static void PrefsDefaults(void);
static void PrefsLine(const char *key, char *val);
static BOOL PrefsWrite(BOOL keep);
static void DevicePage(void);
static void DrawersPage(void);
static void SettingsResult(int res, BOOL ok);

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

static int sdt_main(int argc, char **argv);

int main(int argc, char **argv)
{
   struct Task *me = FindTask(NULL);

   if ((ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower >= NEED_STACK - 4096)
      return sdt_main(argc, argv);
   if (!(swapMem = AllocVec(NEED_STACK, MEMF_ANY)))
   {
      PutStr("SDTransfer: not enough memory for its stack\n");
      return RETURN_FAIL;
   }
   g_argc = argc;
   g_argv = argv;
   swap.stk_Lower = swapMem;
   swap.stk_Upper = (ULONG)swapMem + NEED_STACK;
   swap.stk_Pointer = (APTR)swap.stk_Upper;
   StackSwap(&swap);
   swapRc = sdt_main(g_argc, g_argv);
   StackSwap(&swap);
   FreeVec(swapMem);
   return swapRc;
}

static int sdt_main(int argc, char **argv)
{
   BOOL fromCLI = FALSE;

   /* GCC startup does not set pr_WindowPtr = -1 like SAS/C does: suppress
      DOS volume requesters ourselves before any DOS activity. */
   ((struct Process *)FindTask(NULL))->pr_WindowPtr = (APTR)-1L;

   { const char * volatile keepver = ver; (void)keepver; } /* keep $VER through --gc-sections */

   if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37)))
   {
      PutStr("Needs AmigaOS 2.04 or newer\n");
      goto exit;
   }
   if (!(UtilityBase = OpenLibrary("utility.library", 37)) ||
       !(AslBase = OpenLibrary("asl.library", 37)))
   {
      MessageBox(appname, "Could not open utility.library or asl.library");
      goto exit;
   }

   /* Tool Types / command line, then the settings file (see the top) */
   if (!gtui_GetArgs(argc, argv, gArgDev, sizeof(gArgDev), &gArgUnit, &fromCLI))
      goto exit;
   PrefsDefaults();
   gtui_PrefsRead(PREFSFILE, PrefsLine);
   strcpy(scsi_dev, gArgDev);
   scsi_unit = gArgUnit;
   if (gP.dev[0] && !(fromCLI && gArgDev[0]))
      strcpy(scsi_dev, gP.dev);
   if (gP.unit >= 0 && !(fromCLI && gArgUnit >= 0))
      scsi_unit = gP.unit;
   if (!gtui_CheckDevice(appname, PREFSFILE, fromCLI, scsi_dev, scsi_unit))
      goto exit;

   if (scsi_setup(scsi_dev, scsi_unit) != 0)
      goto exit;
   gDevOK = TRUE;
   InitDirs();

   if (!RefreshFileList())
      goto exit;

   ui.title = appname;
   ui.labels = &gList;
   ui.nButtons = 1;
   ui.buttonText[0] = "_Open / Download";
   ui.hasStatus = TRUE;
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
   SetParentMenu();

   for (;;)
   {
      LONG arg = 0;
      int ev = gtui_Wait(&ui, &arg);

      if (ev == GTUI_QUIT)
         break;
      if (ev == GTUI_BUTTON)
         OpenOrDownload(ui.selected);
      else if (ev == GTUI_OPEN)                  /* double-click = the button */
         OpenOrDownload(arg);
      else if (ev == GTUI_MENU)
      {
         if (arg == MN_QUIT)
            break;
         switch (arg)
         {
            case MN_ABOUT:
               amg_about(ui.win, "SDTransfer", VERSION,
                         "Copies files to and from a BlueSCSI or ZuluSCSI SD card.\n"
                         "Part of SCSIToolbox-Amiga. Licence: GPL-3.0-or-later.");
               break;
            case MN_OPEN:       OpenOrDownload(ui.selected); break;
            case MN_UPLOAD:     Upload(); break;
            case MN_PARENT:     ParentFolder(); break;
            case MN_REFRESH:    ShowList(); break;
            case MN_SET_DEVICE: DevicePage(); break;
            case MN_SET_DRAWERS: DrawersPage(); break;
            case MN_SNAPSHOT:   /* this place and size from now on (saved) */
               gP.wx = ui.win->LeftEdge;
               gP.wy = ui.win->TopEdge;
               gP.ww = ui.win->Width;
               gP.wh = ui.win->Height;
               gtui_SetStatus(&ui, PrefsWrite(TRUE) ? "Window place and size saved"
                                                    : "Could not write ENV:" PREFSFILE, -1);
               break;
            case MN_UNSNAPSHOT: /* the default place and size at the next start */
               gP.ww = 0;
               gtui_SetStatus(&ui, PrefsWrite(TRUE) ? "Default window place from next start"
                                                    : "Could not write ENV:" PREFSFILE, -1);
               break;
         }
      }
   }

exit:
   gtui_Close(&ui);
   if (gDevOK && gHaveDirs)
      Toolbox_Set_Working_Dir("");               /* leave the device at its default dir */
   scsi_cleanup();
   if (AslBase) CloseLibrary(AslBase);
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

/* Does the device do working directories? Then start where it is. */
static void InitDirs(void)
{
   gHaveDirs = 0;
   gCurPath[0] = '\0';
   if (gDevOK && (scsi_capabilities & BLUESCSI_TOOLBOX_CAP_SET_WORKING_DIR))
   {
      if (Toolbox_Get_Working_Dir(gCurPath, sizeof(gCurPath)) == 0 && gCurPath[0])
         gHaveDirs = 1;
   }
}

/* Parent folder: only with working-dir firmware and below the SD root */
static void SetParentMenu(void)
{
   gtui_MenuEnable(&ui, MENU_TRANSFER, ITEM_PARENT,
                   (BOOL)(gHaveDirs && strcmp(gCurPath, "/") != 0));
}

/* Toolbox_Download progress callback */
static void progress(int pc)
{
   gtui_SetStatus(&ui, NULL, pc);
}

static void format_size(char *buffer, int length, ULONG size)
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

/* Add a row. kind: NODE_DIR (shown with a trailing "/"), NODE_FILE, or
   NODE_PARENT (shown as "/"). */
static BOOL AddNode(UBYTE kind, const char *name)
{
   struct SdNode *n;

   if (gNodeCount >= MAX_NODES)
      return FALSE;
   n = &gNodes[gNodeCount++];
   strncpy(n->text, name, sizeof(n->text) - 2);
   n->text[sizeof(n->text) - 2] = '\0';
   if (kind == NODE_DIR)
      strcat(n->text, "/");
   n->kind = kind;
   n->node.ln_Name = n->text;
   AddTail(&gList, &n->node);
   return TRUE;
}

/* Rebuild the list from the device's current working directory. Returns FALSE
   only on SCSI failure (an empty directory is fine). The list must be detached
   from the gadget (or the window not open yet) while this runs. */
static BOOL RefreshFileList(void)
{
   struct FileEntry *file;

   NewList(&gList);
   gNodeCount = 0;

   if (!gDevOK)
      return TRUE;

   file = Toolbox_List_Files(0);
   if (!file && filecount < 0)
      return FALSE;

   if (gHaveDirs && strcmp(gCurPath, "/") != 0)
      AddNode(NODE_PARENT, "/");

   if (file)
   {
      for (; file->Type >= 0; file++)
      {
         if (file->Type == BLUESCSI_FILE)
            AddNode(NODE_FILE, file->Name);
         else if (gHaveDirs && file->Type == BLUESCSI_DIR)
            AddNode(NODE_DIR, file->Name);
      }
   }
   return TRUE;
}

/* Re-read the current folder into the window */
static void ShowList(void)
{
   static char msg[100];

   gtui_DetachList(&ui);
   RefreshFileList();
   gtui_AttachList(&ui, &gList, -1);
   SetParentMenu();
   if (!gDevOK)
      gtui_SetStatus(&ui, "No device", -1);
   else if (gHaveDirs)
   {
      sprintf(msg, "Dir: %.80s", gCurPath);
      gtui_SetStatus(&ui, msg, -1);
   }
   else
      gtui_SetStatus(&ui, "", -1);
}

/* Point the device's working directory at gCurPath. */
static int ApplyWorkingDir(void)
{
   if (strlen(gCurPath) > TOOLBOX_MAX_WD_PATH - 1)
      return -1;
   return (int)Toolbox_Set_Working_Dir(gCurPath);
}

/* Descend into subdirectory 'name' (display form; trailing '/' stripped). */
static int DirDescend(const char *name)
{
   char leaf[40];
   int len;
   int oldlen = strlen(gCurPath);

   strncpy(leaf, name, sizeof(leaf) - 1);
   leaf[sizeof(leaf) - 1] = '\0';
   len = strlen(leaf);
   if (len > 0 && leaf[len - 1] == '/')
      leaf[len - 1] = '\0';
   if (leaf[0] == '\0')
      return -1;

   if (oldlen + 1 + strlen(leaf) >= TOOLBOX_MAX_WD_PATH)
      return -1;
   if (gCurPath[oldlen - 1] != '/')
      strcat(gCurPath, "/");
   strcat(gCurPath, leaf);

   if (ApplyWorkingDir() != 0)
   {
      gCurPath[oldlen] = '\0';                    /* revert */
      ApplyWorkingDir();
      return -1;
   }
   return 0;
}

/* Go up one directory level; the SD card root "/" is the floor. */
static int DirAscend(void)
{
   char old[sizeof(gCurPath)];
   char *p;

   if (strcmp(gCurPath, "/") == 0)
      return -1;
   strcpy(old, gCurPath);
   p = strrchr(gCurPath, '/');
   if (p == gCurPath)
      gCurPath[1] = '\0';                         /* "/shared" -> "/" */
   else
      *p = '\0';
   if (ApplyWorkingDir() != 0)
   {
      strcpy(gCurPath, old);                      /* revert */
      ApplyWorkingDir();
      return -1;
   }
   return 0;
}

/* An ASL file requester over the window. drawer is the initial drawer and,
   with "remember drawers" on, gets the one the user ended in (kept in ENV:
   only, like Use). out = the full path. TRUE if confirmed. */
static BOOL AskFile(char *out, const char *title, char *drawer, const char *file,
                    BOOL save)
{
   struct FileRequester *fr;
   BOOL ok = FALSE;

   if ((fr = (struct FileRequester *)
             AllocAslRequestTags(ASL_FileRequest,
                                 ASLFR_Window,        (ULONG)ui.win,
                                 ASLFR_SleepWindow,   TRUE,
                                 ASLFR_TitleText,     (ULONG)title,
                                 ASLFR_InitialFile,   (ULONG)file,
                                 ASLFR_InitialDrawer, (ULONG)drawer,
                                 ASLFR_DoSaveMode,    (ULONG)save,
                                 TAG_DONE)))
   {
      if (AslRequest(fr, NULL) && fr->rf_File && fr->rf_File[0])
      {
         strncpy(out, (char *)fr->rf_Dir, MAXPATH - 1);
         out[MAXPATH - 1] = '\0';
         AddPart(out, fr->rf_File, MAXPATH);
         ok = TRUE;
         if (gP.remember && strcmp(drawer, (char *)fr->rf_Dir) != 0)
         {
            strncpy(drawer, (char *)fr->rf_Dir, sizeof(gP.downDir) - 1);
            drawer[sizeof(gP.downDir) - 1] = '\0';
            PrefsWrite(FALSE);
         }
      }
      FreeAslRequest(fr);
   }
   return ok;
}

static void ParentFolder(void)
{
   if (gDevOK && gHaveDirs && DirAscend() == 0)
      ShowList();
}

/* Act on a row: descend into a dir, ascend on the "/" row, download a file.
   Shared by the button, the menu and double-click. */
static void OpenOrDownload(LONG row)
{
   static char msg[MAXPATH + 48];
   struct SdNode *n = (struct SdNode *)gtui_NodeAt(&gList, row);

   if (!n)
   {
      gtui_SetStatus(&ui, "Select a file or folder first", -1);
      return;
   }

   if (n->kind != NODE_FILE)
   {
      int navres;

      gtui_DetachList(&ui);
      navres = (n->kind == NODE_PARENT) ? DirAscend() : DirDescend(n->text);
      if (navres == 0)
         RefreshFileList();                       /* n is invalid from here */
      gtui_AttachList(&ui, &gList, -1);
      SetParentMenu();
      sprintf(msg, "Dir: %s", gCurPath);
      gtui_SetStatus(&ui, msg, -1);
   }
   else
   {
      char source[sizeof(n->text)];
      char destination[MAXPATH];

      strcpy(source, n->text);

      /* Only download if the requester was confirmed */
      if (AskFile(destination, "Save As", gP.downDir, source, TRUE))
      {
         ULONG bytes;

         gtui_Busy(&ui, TRUE);
         gtui_SetStatus(&ui, NULL, 0);
         bytes = Toolbox_Download(source, destination, progress);
         gtui_Busy(&ui, FALSE);
         if (bytes > 0)
         {
            char size_text[32];

            format_size(size_text, sizeof(size_text), bytes);
            sprintf(msg, "%s bytes saved to %.900s", size_text, destination);
            gtui_SetStatus(&ui, msg, -1);
         }
         else
            gtui_SetStatus(&ui, NULL, -1);        /* back to the previous text */
      }
   }
}

/* Toolbox_Send_File progress: bytes so far of gUpSize */
static ULONG gUpSize;

static void upProgress(LONG sent)
{
   if (gUpSize)
      gtui_SetStatus(&ui, NULL, (LONG)(((ULONG)sent / 256) * 100 / (gUpSize / 256 + 1)));
}

/* Copy a local file into the folder shown, under its own name. The firmware
   takes names of up to 32 characters. An existing name is replaced after
   asking; ZuluSCSI truncates the old file from firmware v2026.10.30 on
   (before that a shorter upload kept the old tail, and the firmware version
   cannot be read over SCSI), so its question says so. */
static void Upload(void)
{
   static char msg[MAXPATH + 48];
   char path[MAXPATH], name[MAX_MAC_PATH + 1];
   struct FileInfoBlock *fib;
   char *leaf;
   BPTR lock;
   LONG sent;
   int i;

   if (!gDevOK)
      return;
   if (!AskFile(path, "Upload to the SD card", gP.upDir, "", FALSE))
      return;
   leaf = (char *)FilePart((STRPTR)path);
   if (strlen(leaf) > MAX_MAC_PATH)
   {
      MessageBox(appname, "The name is too long for the SD card\n"
                          "(32 characters at most). Rename it first.");
      return;
   }
   strcpy(name, leaf);

   /* the current listing is still in 'files' (scsi.c) */
   for (i = 0; files && i < filecount; i++)
   {
      if (Stricmp((STRPTR)files[i].Name, (STRPTR)name) != 0)
         continue;
      if (files[i].Type != BLUESCSI_FILE)
      {
         MessageBox(appname, "A folder of that name is already there.");
         return;
      }
      {
         struct EasyStruct es = {sizeof(struct EasyStruct), 0, NULL,
                                 NULL, NULL};
         es.es_Title = (UBYTE *)appname;
         es.es_TextFormat = (UBYTE *)(scsi_isZuluSCSI
            ? "'%s' is already on the SD card.\nReplace it?\n\n"
              "(ZuluSCSI needs firmware v2026.10.30 or\nlater to replace a file correctly.)"
            : "'%s' is already on the SD card.\nReplace it?");
         es.es_GadgetFormat = (UBYTE *)"Replace|Cancel";
         if (EasyRequest(ui.win, &es, NULL, (ULONG)name) != 1)
            return;
      }
      break;
   }

   gUpSize = 0;
   if ((lock = Lock((STRPTR)path, ACCESS_READ)))
   {
      if ((fib = AllocDosObject(DOS_FIB, NULL)))
      {
         if (Examine(lock, fib))
         {
            if (fib->fib_DirEntryType > 0)
            {
               FreeDosObject(DOS_FIB, fib);
               UnLock(lock);
               MessageBox(appname, "That is a drawer: pick a file.");
               return;
            }
            gUpSize = (ULONG)fib->fib_Size;
         }
         FreeDosObject(DOS_FIB, fib);
      }
      UnLock(lock);
   }

   gtui_Busy(&ui, TRUE);
   gtui_SetStatus(&ui, NULL, 0);
   sent = Toolbox_Send_File(name, path, upProgress);
   gtui_Busy(&ui, FALSE);

   ShowList();
   if (sent < 0)
      sprintf(msg, "Upload of %s failed", name);
   else
   {
      char size_text[32];

      format_size(size_text, sizeof(size_text), (ULONG)sent);
      sprintf(msg, "%s bytes sent as %s", size_text, name);
   }
   gtui_SetStatus(&ui, msg, -1);
}

/* ---- settings ------------------------------------------------------------- */

static void PrefsDefaults(void)
{
   memset(&gP, 0, sizeof(gP));
   gP.unit = -1;
   gP.remember = TRUE;
}

static void copyVal(char *dst, const char *src, int size)
{
   strncpy(dst, src, size - 1);
   dst[size - 1] = '\0';
}

static void PrefsLine(const char *key, char *val)
{
   LONG n;

   if (!strcmp(key, "DEVICE"))
      copyVal(gP.dev, val, sizeof(gP.dev));
   else if (!strcmp(key, "UNIT"))
   {
      if (StrToLong((STRPTR)val, &n) > 0 && n >= 0)
         gP.unit = n;
   }
   else if (!strcmp(key, "DOWNLOADDIR"))
      copyVal(gP.downDir, val, sizeof(gP.downDir));
   else if (!strcmp(key, "UPLOADDIR"))
      copyVal(gP.upDir, val, sizeof(gP.upDir));
   else if (!strcmp(key, "REMEMBERDIRS"))
      gP.remember = (BOOL)(val[0] != 'N' && val[0] != 'n' && val[0] != '0');
   else if (!strcmp(key, "WINDOW"))
   {
      if (!gtui_ParseBox(val, &gP.wx, &gP.wy, &gP.ww, &gP.wh))
         gP.ww = 0;
   }
}

static BOOL PrefsWrite(BOOL keep)
{
   static char buf[1200];
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
   sprintf(p, "DOWNLOADDIR=%s\nUPLOADDIR=%s\nREMEMBERDIRS=%s\n",
                gP.downDir, gP.upDir, gP.remember ? "YES" : "NO");
   p += strlen(p);
   if (gP.ww > 0)
   {
      sprintf(p, "WINDOW=%ld,%ld,%ld,%ld\n",
                   (LONG)gP.wx, (LONG)gP.wy, (LONG)gP.ww, (LONG)gP.wh);
      p += strlen(p);
   }
   return (BOOL)(amg_env_write(PREFSFILE, buf, (ULONG)(p - buf), keep) != 0);
}

static void SettingsResult(int res, BOOL ok)
{
   if (!ok)
      gtui_SetStatus(&ui, "Could not write ENV:" PREFSFILE, -1);
   else
      gtui_SetStatus(&ui, res == AMG_SAVE ? "Settings saved"
                                          : "Settings in use until reboot", -1);
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
   if (gDevOK && gHaveDirs)
      Toolbox_Set_Working_Dir("");
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
   InitDirs();
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
   BOOL ok;

   memset(&v, 0, sizeof(v));
   memset(&def, 0, sizeof(def));
   copyVal(v.s[0], scsi_dev, GTF_STRLEN);
   v.n[1] = scsi_unit;
   copyVal(def.s[0], gArgDev, GTF_STRLEN);
   def.n[1] = gArgUnit >= 0 ? gArgUnit : 0;

   res = gtui_Page(&ui, "SDTransfer: Device", devFields, 2,
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
      copyVal(gP.dev, v.s[0], sizeof(gP.dev));
      gP.unit = v.n[1];
   }
   ok = PrefsWrite(res == AMG_SAVE);
   SwitchDevice(v.s[0], v.n[1]);
   SettingsResult(res, ok);
}

static const struct GtField dirFields[] =
{
   {"Download to", GTF_STR, 255},
   {"Upload from", GTF_STR, 255},
   {"Remember the last drawers", GTF_CHECK, 0}
};

static void DrawersPage(void)
{
   struct GtVals v, def;
   int res;

   memset(&v, 0, sizeof(v));
   memset(&def, 0, sizeof(def));
   copyVal(v.s[0], gP.downDir, GTF_STRLEN);
   copyVal(v.s[1], gP.upDir, GTF_STRLEN);
   v.n[2] = gP.remember;
   def.n[2] = TRUE;

   res = gtui_Page(&ui, "SDTransfer: Drawers", dirFields, 3,
                   "Empty = the program's drawer", &v, &def);
   if (res == AMG_CANCEL)
      return;
   copyVal(gP.downDir, v.s[0], sizeof(gP.downDir));
   copyVal(gP.upDir, v.s[1], sizeof(gP.upDir));
   gP.remember = (BOOL)(v.n[2] != 0);
   SettingsResult(res, PrefsWrite(res == AMG_SAVE));
}

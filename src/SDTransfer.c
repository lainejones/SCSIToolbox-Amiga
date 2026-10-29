/*
 * Copyright (C) 2024 Paul Hill
 *
 * Modified 2026-08-06 by Laine Jones (lainejones): part of SCSIToolbox-Amiga.
 * Added SD-card subdirectory navigation (firmware v2026.04.27+ working-dir
 * support): directories are listed with a trailing "/", the "/" entry goes up,
 * and the Open/Download button descends into a selected directory.
 * Modified 2026-10-28: GadTools user interface (gtui.c) instead of ReAction,
 * so it runs on AmigaOS 2.04/3.0/3.1 without ClassAct/ReAction installed.
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
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <clib/alib_protos.h>
#include "toolbox.h"
#include "gtui.h"

__attribute__((used)) static const char ver[] = "$VER: SDTransfer 1.9 (28.10.2026)";

/* Kinds of list row */
#define NODE_DIR    0
#define NODE_FILE   1
#define NODE_PARENT 2

/* = NULL: strong definitions, so the startup code does not try to open these */
struct Library *UtilityBase = NULL, *AslBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;

static char scsi_dev[256];
static LONG scsi_unit = -1;

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

extern UWORD bluescsi_logo_data[];
extern UWORD zuluscsi_logo_data[];

static BOOL RefreshFileList(void);
static void OpenOrDownload(LONG row);

int main(int argc, char **argv)
{
   char info[80];

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

   if (!gtui_GetArgs(argc, argv, appname, scsi_dev, sizeof(scsi_dev), &scsi_unit))
      goto exit;

   if (scsi_setup(scsi_dev, scsi_unit) != 0)
      goto exit;

   if (scsi_capabilities & BLUESCSI_TOOLBOX_CAP_SET_WORKING_DIR)
   {
      if (Toolbox_Get_Working_Dir(gCurPath, sizeof(gCurPath)) == 0 && gCurPath[0])
         gHaveDirs = 1;
   }

   NewList(&gList);
   if (!RefreshFileList())
      goto exit;

   sprintf(info, "%.40s, unit %ld", scsi_dev, scsi_unit);

   ui.title = appname;
   ui.info = info;
   ui.logoData = scsi_isZuluSCSI ? zuluscsi_logo_data : bluescsi_logo_data;
   ui.labels = &gList;
   ui.nButtons = 1;
   ui.buttonText[0] = "_Open / Download";
   ui.hasStatus = TRUE;

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
      if (ev == GTUI_BUTTON)
         OpenOrDownload(ui.selected);
      else if (ev == GTUI_OPEN)                  /* double-click = the button */
         OpenOrDownload(arg);
   }

exit:
   gtui_Close(&ui);
   if (gHaveDirs)
      Toolbox_Set_Working_Dir("");               /* leave the device at its default dir */
   scsi_cleanup();
   if (AslBase) CloseLibrary(AslBase);
   if (UtilityBase) CloseLibrary(UtilityBase);
   if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
   return 0;
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

/* Ask for a destination. Returns TRUE if the user confirmed, FALSE if
   cancelled (name untouched). */
static BOOL getfilename(char *name, char *title)
{
   struct FileRequester *fr;
   BOOL ok = FALSE;
   char dirname[MAXPATH];
   char *filename;

   if (strlen(name) > 0)
   {
      strncpy(dirname, name, MAXPATH);
      dirname[MAXPATH - 1] = '\0';
      filename = (STRPTR)PathPart(dirname);
      filename[0] = '\0';
      filename = (STRPTR)FilePart(name);
   }
   else
   {
      dirname[0] = '\0';
      filename = dirname;
   }

   if ((fr = (struct FileRequester *)
             AllocAslRequestTags(ASL_FileRequest,
                                 ASLFR_Window,        (ULONG)ui.win,
                                 ASLFR_SleepWindow,   TRUE,
                                 ASLFR_TitleText,     (ULONG)title,
                                 ASLFR_InitialFile,   (ULONG)filename,
                                 ASLFR_InitialDrawer, (ULONG)dirname,
                                 ASLFR_DoSaveMode,    TRUE,
                                 TAG_DONE)))
   {
      if (AslRequest(fr, NULL))
      {
         strcpy(name, fr->rf_Dir);
         AddPart(name, fr->rf_File, MAXPATH);
         ok = TRUE;
      }
      FreeAslRequest(fr);
   }
   return ok;
}

/* Act on a row: descend into a dir, ascend on the "/" row, download a file.
   Shared by the button and double-click. */
static void OpenOrDownload(LONG row)
{
   static char msg[MAXPATH + 48];
   struct SdNode *n = (struct SdNode *)gtui_NodeAt(&gList, row);

   if (!n)
      return;

   if (n->kind != NODE_FILE)
   {
      int navres;

      gtui_DetachList(&ui);
      navres = (n->kind == NODE_PARENT) ? DirAscend() : DirDescend(n->text);
      if (navres == 0)
         RefreshFileList();                       /* n is invalid from here */
      gtui_AttachList(&ui, &gList, -1);
      sprintf(msg, "Dir: %s", gCurPath);
      gtui_SetStatus(&ui, msg, -1);
   }
   else
   {
      char source[sizeof(n->text)];
      char destination[MAXPATH];

      strcpy(source, n->text);
      strcpy(destination, source);

      /* Only download if the requester was confirmed */
      if (getfilename(destination, "Save As"))
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

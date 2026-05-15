/*
 * Copyright (C) 2024 Paul Hill
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
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <proto/layout.h>
#include <proto/label.h>
#include <proto/listbrowser.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/window.h>
#include <proto/expansion.h>
#include <clib/alib_protos.h>
#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/listbrowser.h>
#include <gadgets/palette.h>
#include <images/label.h>
#include <reaction/reaction_macros.h>
#include <devices/scsidisk.h>
#include <graphics/gfx.h>
#include <workbench/startup.h>
#include "toolbox.h"

static const char ver[] = "$VER: CDChanger 1.2 (18.5.2024)";

void GetVolumeName(void);
void DiskChange(void);
void FreeListBrowserNodes(void);
BOOL AddListBrowserNode(ULONG index, STRPTR number, STRPTR text);
void bstrcpy(char *dest, UBYTE *src);

struct Library *WindowBase = NULL, *LayoutBase = NULL, *LabelBase = NULL, *ListBrowserBase = NULL;
struct Library *UtilityBase = NULL, *IconBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct MsgPort *AppPort;

UBYTE scsi_dev[1024];
LONG scsi_unit = -1;

char selectedDrive[256];
static char* appname = "CD Changer";

enum Gadgets
{
   GID_NEXT,
   GID_LISTBROWSER,
};

struct List gb_List;
static const char * const g_nums[64] = {
    "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",
    "9",  "10", "11", "12", "13", "14", "15", "16",
    "17", "18", "19", "20", "21", "22", "23", "24",
    "25", "26", "27", "28", "29", "30", "31", "32",
    "33", "34", "35", "36", "37", "38", "39", "40",
    "41", "42", "43", "44", "45", "46", "47", "48",
    "49", "50", "51", "52", "53", "54", "55", "56",
    "57", "58", "59", "60", "61", "62", "63", "64"
};
static int  g_numcount = 0;
struct ColumnInfo gb_ListbrowserColumn[] =
{
   {  30, "#", 0 },
   { 170, "ISO Image", 0 },
   {  -1, (STRPTR)~0, (ULONG)-1 }
};

extern UWORD bluescsi_logo_data[];
extern UWORD zuluscsi_logo_data[];
#define LOGO_WIDTH  192
#define LOGO_HEIGHT 71
#define LOGO_DATA_SIZE (RASSIZE(LOGO_WIDTH, LOGO_HEIGHT))
UWORD *chip_logo_data = NULL;
struct Image logo_image =
{
    0, 0,
    LOGO_WIDTH, LOGO_HEIGHT, 1,
    NULL,
    0x0001, 0x0000,
    NULL
};

struct Node *selectedNode;
struct Window *mainWindow;

char dos_drive[256];
int started_from_workbench = 0;
static char debug_msg[512];

void ToUpperCase(char *str)
{
   while (*str)
   {
      if (*str >= 'a' && *str <= 'z')
         *str = *str - 'a' + 'A';
      str++;
   }
}

void StripColon(char *str)
{
   int len = strlen(str);
   if (len > 0 && str[len - 1] == ':')
   {
      str[len - 1] = '\0';
   }
}

int main(int argc, char **argv)
{
   struct RDArgs *rd = NULL;
   LONG params[2];
   struct FileEntry *files;
   char drive[260];
   APTR windowObj;
   Object *listBrowser;

   /* Suppress DOS volume requesters from our own process before any DOS activity. */
   {
      struct Process *myproc = (struct Process *)FindTask(NULL);
      myproc->pr_WindowPtr = (APTR)-1L;
   }

   params[0] = 0;
   params[1] = 0;

   if ((UtilityBase = OpenLibrary("utility.library", 37L)) == NULL)
   {
      PutStr("Could not open utility.library\n");
      goto exit;
   }

   if ((IntuitionBase = (struct IntuitionBase *) OpenLibrary("intuition.library", 33L)) == NULL)
   {
      PutStr("Could not open intuition.library\n");
      goto exit;
   }

   if ((WindowBase = OpenLibrary("window.class", 44)) == NULL)
   {
      PutStr("Could not open window.class\n");
      goto exit;
   }

   if ((ListBrowserBase = OpenLibrary("gadgets/listbrowser.gadget", 44)) == NULL)
   {
      PutStr("Could not open listbrowser.gadget\n");
      goto exit;
   }

   if ((LayoutBase = OpenLibrary("gadgets/layout.gadget", 44)) == NULL)
   {
      PutStr("Could not open layout.gadget\n");
      goto exit;
   }

   if ((LabelBase = OpenLibrary("images/label.image", 44)) == NULL)
   {
      PutStr("Could not open label.image\n");
      goto exit;
   }

   if ((AppPort = CreateMsgPort()) == NULL)
   {
      goto exit;
   }

   IconBase = OpenLibrary("icon.library", 33L);

   if (argc > 0)
   {
      /* CLI mode */
      started_from_workbench = 0;

      rd = ReadArgs("DEVICE/K,UNIT/K/N", params, NULL);
      if (rd)
      {
         if (params[0])
            strncpy((char *)scsi_dev, (char *)params[0], sizeof(scsi_dev));
         if (params[1])
            scsi_unit = ((ULONG *)params[1])[0];
         FreeArgs(rd);
      }
   }
   else
   {
      /* Workbench mode: argc==0, argv[0] holds the WBStartup message pointer
         (SAS/C convention used by this toolchain, not the libnix argv-direct style) */
      started_from_workbench = 1;

      if (IconBase != NULL && argv != NULL && argv[0] != NULL)
      {
         LONG i;
         struct WBStartup *WBenchMsg = (struct WBStartup *)argv[0];
         struct WBArg *wbarg;

         if (WBenchMsg->sm_ArgList != NULL)
         {
            wbarg = WBenchMsg->sm_ArgList;
            for (i = 0; i < WBenchMsg->sm_NumArgs; i++, wbarg++)
            {
               struct DiskObject *dobj;
               STRPTR s;

               if (wbarg->wa_Name != NULL && wbarg->wa_Name[0] != '\0')
               {
                  BPTR oldlock = CurrentDir(wbarg->wa_Lock);
                  dobj = GetDiskObject(wbarg->wa_Name);
                  CurrentDir(oldlock);

                  if (dobj)
                  {
                     STRPTR *toolarray = (STRPTR *)dobj->do_ToolTypes;
                     s = (STRPTR)FindToolType(toolarray, "DEVICE");
                     if (s != NULL)
                        strncpy((char *)scsi_dev, (char *)s, sizeof(scsi_dev));
                     s = (STRPTR)FindToolType(toolarray, "UNIT");
                     if (s != NULL)
                        StrToLong(s, &scsi_unit);
                     FreeDiskObject(dobj);
                     break;
                  }
               }
            }
         }
      }
   }

   scsi_dev[sizeof(scsi_dev)-1] = '\0';

   if (scsi_dev[0] == '\0' || scsi_unit < 0)
   {
      char msg[256];
      if (started_from_workbench)
         sprintf(msg, "Missing SCSI device or unit\nDEVICE=%s\nUNIT=%ld",
                 scsi_dev[0] ? (char *)scsi_dev : "(empty)", scsi_unit);
      else
         sprintf(msg, "Missing required arguments\nDEVICE=%s\nUNIT=%ld",
                 scsi_dev[0] ? (char *)scsi_dev : "(empty)", scsi_unit);
      MessageBox(appname, msg);
      goto exit;
   }

   dos_drive[0] = '\0';
   {
      char tmpdev[256];
      struct DosList *dl;
      dl = LockDosList(LDF_DEVICES | LDF_READ);
      while ((dl = NextDosEntry(dl, LDF_DEVICES)))
      {
         struct FileSysStartupMsg *fssm = BADDR(dl->dol_misc.dol_handler.dol_Startup);
         if (TypeOfMem(fssm) && (APTR)fssm > (APTR)1000)
         {
            if ((LONG)fssm->fssm_Unit == scsi_unit)
            {
               bstrcpy(tmpdev, BADDR(fssm->fssm_Device));
               if (Stricmp(scsi_dev, tmpdev) == 0)
               {
                  bstrcpy(dos_drive, BADDR(dl->dol_Name));
                  strncat(dos_drive, ":", sizeof(dos_drive) - strlen(dos_drive) - 1);
                  break;
               }
            }
         }
      }
      UnLockDosList(LDF_DEVICES | LDF_READ);
   }

   if (scsi_setup(scsi_dev, scsi_unit) != 0)
   {
      sprintf(debug_msg, "scsi_setup failed!\nDevice: %s\nUnit: %ld", (char *)scsi_dev, scsi_unit);
      MessageBox(appname, debug_msg);
      goto exit;
   }

   chip_logo_data = (UWORD *)AllocVec(LOGO_DATA_SIZE, MEMF_CHIP);
   if (chip_logo_data)
   {
      CopyMem(scsi_isZuluSCSI ? zuluscsi_logo_data : bluescsi_logo_data,
              chip_logo_data, LOGO_DATA_SIZE);
      logo_image.ImageData = chip_logo_data;
   }

   NewList(&gb_List);
   GetVolumeName();

   files = Toolbox_List_Files(1);
   if (files)
   {
      struct FileEntry *file = files;

      g_numcount = 0;
      while (file->Type >= 0)
      {
         if (file->Type == 1 && g_numcount < 64)
         {
            AddListBrowserNode(file->Index, (STRPTR)g_nums[g_numcount], file->Name);
            g_numcount++;
         }
         file++;
      }
   }
   else
   {
      if (started_from_workbench == 0)
      {
         Printf("no files\n");
      }
   }

   sprintf(debug_msg, "Device:%s Unit:%ld", (char *)scsi_dev, scsi_unit);

   windowObj = WindowObject,
      WA_Title, appname,
      WA_Activate, TRUE,
      WA_DepthGadget, TRUE,
      WA_DragBar, TRUE,
      WA_CloseGadget, TRUE,
      WA_SizeGadget, TRUE,
      WA_Width, 250,
      WA_Height, 250,
      WINDOW_IconifyGadget, TRUE,
      WINDOW_IconTitle, appname,
      WINDOW_AppPort, AppPort,
      WINDOW_Position, WPOS_TOPLEFT,
      WINDOW_ParentGroup, VLayoutObject,
         LAYOUT_SpaceOuter, TRUE,
         LAYOUT_DeferLayout, TRUE,

         LAYOUT_AddChild, VLayoutObject,
            LAYOUT_VertAlignment, LALIGN_CENTER,
            LAYOUT_HorizAlignment, LALIGN_CENTER,
            LAYOUT_BevelStyle, BVS_THIN,
            LAYOUT_AddChild,
               ButtonObject,
               GA_Image, &logo_image,
               GA_ReadOnly, TRUE,
               BUTTON_BevelStyle, BVS_NONE,
            End,
            CHILD_MaxWidth, 200,
         End,
         CHILD_MaxHeight, 81,

         LAYOUT_AddImage,
            LabelObject,
               LABEL_Text, debug_msg,
            End,

         LAYOUT_AddChild,
            listBrowser = ListBrowserObject,
               GA_ID, GID_LISTBROWSER,
               GA_RelVerify, TRUE,
               LISTBROWSER_ColumnInfo, &gb_ListbrowserColumn,
               LISTBROWSER_ColumnTitles, TRUE,
               LISTBROWSER_Labels, &gb_List,
               LISTBROWSER_VertSeparators, TRUE,
               LISTBROWSER_Spacing, 1,
               LISTBROWSER_ShowSelected, TRUE,
               LISTBROWSER_AutoFit, TRUE,
               LISTBROWSER_Editable, FALSE,
               LISTBROWSER_MinVisible, 8,
            End,

         LAYOUT_AddChild,
            ButtonObject,
               GA_ID, GID_NEXT,
               GA_RelVerify, TRUE,
               GA_Text, "_Select",
            End,
            CHILD_MinHeight, 20,
            CHILD_MaxHeight, 20,

      EndGroup,
   EndWindow;

   mainWindow = (struct Window *)RA_OpenWindow(windowObj);
   if (mainWindow)
   {
      ULONG done = FALSE;
      ULONG result;
      ULONG code;
      ULONG signal;
      struct Node *node;
      UBYTE app = (1L << AppPort->mp_SigBit);

      selectedNode = NULL;

      GetAttr(WINDOW_SigMask, windowObj, &signal);

      while (!done)
      {
         Wait(signal | SIGBREAKF_CTRL_C | app);
         while ((result = RA_HandleInput(windowObj, &code)) != WMHI_LASTMSG)
         {
            switch (result & WMHI_CLASSMASK)
            {
               case WMHI_RAWKEY:
                  if ((result & WMHI_KEYMASK) == RAWKEY_ESC)
                  {
                     done = TRUE;
                  }
                  break;
               case WMHI_CLOSEWINDOW:
                  done = TRUE;
                  break;
               case WMHI_GADGETUP:
                  switch (result & WMHI_GADGETMASK)
                  {
                     case GID_LISTBROWSER:
                        GetAttr(LISTBROWSER_SelectedNode, listBrowser, (ULONG*) &selectedNode);
                        break;
                     case GID_NEXT:
                        if (selectedNode)
                        {
                           ULONG userdata = 0;
                           struct TagItem gtags[] = {
                              {LBNA_Column,   0},
                              {LBNA_UserData, (ULONG)&userdata},
                              {TAG_DONE,      0}
                           };
                           GetListBrowserNodeAttrsA(selectedNode, gtags);
                           Toolbox_Set_Next_CD((UBYTE) userdata);
                           DiskChange();
                        }
                        break;
                  }
                  break;
               case WMHI_ICONIFY:
                  RA_Iconify(windowObj);
                  mainWindow = NULL;
                  break;
               case WMHI_UNICONIFY:
                  mainWindow = (struct Window *)RA_OpenWindow(windowObj);
                  if (mainWindow)
                  {
                     GetAttr(WINDOW_SigMask, windowObj, &signal);
                  }
                  else
                  {
                     done = TRUE;
                  }
                  break;
               default:
                  break;
            }
         }
      }

      DisposeObject(windowObj);
      FreeListBrowserNodes();
   }
   else
   {
      PutStr("Could not open window\n");
   }

exit:
   if (chip_logo_data) FreeVec(chip_logo_data);
   scsi_cleanup();
   if (AppPort) DeleteMsgPort(AppPort);
   if (IconBase) CloseLibrary(IconBase);
   if (LabelBase) CloseLibrary(LabelBase);
   if (LayoutBase) CloseLibrary(LayoutBase);
   if (ListBrowserBase) CloseLibrary(ListBrowserBase);
   if (WindowBase) CloseLibrary(WindowBase);
   if (UtilityBase) CloseLibrary(UtilityBase);
   if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
}



void DiskChange()
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
         if (fssm->fssm_Unit == scsi_unit)
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

BOOL AddListBrowserNode(ULONG index, STRPTR number, STRPTR text)
{
   struct Node *node;
   struct TagItem tags[] = {
       {LBNA_Generation,    2},
       {LBNA_Column,        0},
       {LBNCA_Text,         (ULONG)number},   /* pointer into g_numstrs[], lives forever */
       {LBNCA_Justification,LCJ_LEFT},
       {LBNA_UserData,      (ULONG)index},
       {LBNA_Column,        1},
       {LBNCA_Text,         (ULONG)text},
       {LBNCA_Justification,LCJ_LEFT},
       {LBNCA_CopyText,     TRUE},
       {LBNCA_MaxChars,     100},
       {LBNA_UserData,      (ULONG)index},
       {TAG_END,            0}
   };

   if ((node = AllocListBrowserNodeA(2, tags)))
   {
      AddTail(&gb_List, node);
   }
   return (BOOL)(node ? TRUE : FALSE);
}

void FreeListBrowserNodes()
{
   struct Node *node, *nextnode;
   node = gb_List.lh_Head;

   while ((nextnode = node->ln_Succ))
   {
      Remove(node);
      FreeListBrowserNode(node);
      node = nextnode;
   }
}

void bstrcpy(char *dest, UBYTE *src)
{
   int len = *src++;
   strncpy(dest, src, len + 1);
   dest[len] = 0;
}

void GetVolumeName()
{
   char device[256];
   struct MsgPort *dol_Task = NULL;
   struct DosList *dl;

   dl = LockDosList(LDF_DEVICES | LDF_READ);
   while ((dl = NextDosEntry(dl, LDF_DEVICES)))
   {
      struct FileSysStartupMsg *fssm = BADDR(dl->dol_misc.dol_handler.dol_Startup);
      if (TypeOfMem(fssm) && (APTR)fssm > (APTR)1000)
      {
         if (fssm->fssm_Unit == scsi_unit)
         {
            bstrcpy(device, BADDR(fssm->fssm_Device));
            if (Stricmp(scsi_dev, device) == 0)
            {
               dol_Task = dl->dol_Task;
               break;
            }
         }
      }
   }
   UnLockDosList(LDF_DEVICES | LDF_READ);

   if (dol_Task)
   {
      dl = LockDosList(LDF_VOLUMES | LDF_READ);
      while ((dl = NextDosEntry(dl, LDF_VOLUMES)))
      {
         if (dl->dol_Task == dol_Task)
         {
            bstrcpy(selectedDrive, BADDR(dl->dol_Name));
            break;
         }
      }
      UnLockDosList(LDF_VOLUMES | LDF_READ);
   }
}

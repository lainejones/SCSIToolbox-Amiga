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
#include <stdlib.h>
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
BOOL AddListBrowserNode(ULONG num, STRPTR number, STRPTR text);
void bstrcpy(char *dest, UBYTE *src);
void InhibitMountedVolumes(void);
void CleanupVirtualVolumes(void);

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

char *mounted_volumes[64];
int mounted_volume_count = 0;
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
      started_from_workbench = 0;

      for (int i = 0; i < argc; i++)
      {
         char *arg = argv[i];
         if (arg && arg[0] == '-' && arg[1] != 0)
         {
            arg += 2;
         }

         char *eq = strchr(arg, '=');
         if (eq)
         {
            char key[64] = {0};
            size_t key_len = eq - arg;
            if (key_len >= sizeof(key))
               key_len = sizeof(key) - 1;
            strncpy(key, arg, key_len);
            key[key_len] = 0;

            ToUpperCase(key);
            if (strcmp(key, "DEVICE") == 0)
            {
               strncpy((char *)scsi_dev, eq + 1, sizeof(scsi_dev) - 1);
            }
            else if (strcmp(key, "UNIT") == 0)
            {
               scsi_unit = atol(eq + 1);
            }
         }
      }

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
      started_from_workbench = 1;

      if (IconBase != NULL)
      {
         LONG i;
         struct WBStartup *WBenchMsg = (struct WBStartup *)argv;
         struct WBArg *wbarg;

         wbarg = WBenchMsg->sm_ArgList;
         for(i = 0; i < WBenchMsg->sm_NumArgs; i++, wbarg++)
         {
            struct DiskObject *dobj;
            STRPTR s;

            if((*wbarg->wa_Name) && (dobj = GetDiskObject(wbarg->wa_Name)))
            {
               STRPTR *toolarray = (STRPTR *)dobj->do_ToolTypes;
               s = (STRPTR)FindToolType(toolarray, "DEVICE");
               if(s != NULL)
               {
                  strncpy((char *)scsi_dev, (char *)s, sizeof(scsi_dev));
               }
               s = (STRPTR)FindToolType(toolarray, "UNIT");
               if(s != NULL)
               {
                  StrToLong(s, &scsi_unit);
               }
               FreeDiskObject(dobj);
               break;
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

   InhibitMountedVolumes();

   files = Toolbox_List_Files(1);
   if (files)
   {
      struct FileEntry *file = files;
      while (file->Type >= 0)
      {
         if (file->Type == 1)
         {
            AddListBrowserNode(file->Index, file->Number, file->Name);
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

   InhibitMountedVolumes();

   sprintf(debug_msg, "Device:%s Unit:%ld", (char *)scsi_dev, scsi_unit);

   windowObj = WindowObject,
      WA_Title, appname,
      WA_Activate, TRUE,
      WA_DepthGadget, TRUE,
      WA_DragBar, TRUE,
      WA_CloseGadget, TRUE,
      WA_SizeGadget, TRUE,
      WA_Width, 400,
      WA_Height, 350,
      WINDOW_IconifyGadget, TRUE,
      WINDOW_IconTitle, appname,
      WINDOW_AppPort, AppPort,
      WINDOW_Position, WPOS_CENTERMOUSE,
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
               case WMHI_GADGETDOWN:
                  {
                     GetAttr(LISTBROWSER_SelectedNode, listBrowser, (ULONG*) &node);
                     if (node)
                     {
                        ULONG userdata = 0;
                        struct TagItem gtags[] = {
                           {LBNA_Column,   0},
                           {LBNA_UserData, (ULONG)&userdata},
                           {TAG_DONE,      0}
                        };
                        GetListBrowserNodeAttrsA(node, gtags);
                        Toolbox_Set_Next_CD((UBYTE) userdata);
                        DiskChange();
                     }
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
   CleanupVirtualVolumes();
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

void InhibitMountedVolumes()
{
   struct DosList *dl;
   char drive[256];
   /* Collect names under the lock, then inhibit after releasing it.
      Calling Inhibit() while holding the DosList lock deadlocks the system:
      the handler needs a DosList write lock to respond, which cannot be
      granted while we hold the read lock. */
   char *candidates[64];
   int num_candidates = 0;
   int i;

   dl = LockDosList(LDF_VOLUMES | LDF_READ);
   while ((dl = NextDosEntry(dl, LDF_VOLUMES)))
   {
      bstrcpy(drive, BADDR(dl->dol_Name));
      if (drive[0] == 'C' && drive[1] == 'D')
      {
         if (num_candidates < 64)
         {
            candidates[num_candidates] = AllocVec(strlen(drive) + 1, MEMF_PUBLIC);
            if (candidates[num_candidates])
            {
               strcpy(candidates[num_candidates], drive);
               num_candidates++;
            }
         }
      }
   }
   UnLockDosList(LDF_VOLUMES | LDF_READ);

   /* DosList lock is now released — safe to call Inhibit */
   for (i = 0; i < num_candidates; i++)
   {
      if (mounted_volume_count < 63)
      {
         mounted_volumes[mounted_volume_count] = candidates[i];
         if (!Inhibit(candidates[i], DOSTRUE))
         {
            FreeVec(candidates[i]);
            mounted_volumes[mounted_volume_count] = NULL;
         }
         else
         {
            mounted_volume_count++;
         }
      }
      else
      {
         FreeVec(candidates[i]);
      }
   }
}

void CleanupVirtualVolumes()
{
   int i;
   for (i = 0; i < mounted_volume_count; i++)
   {
      if (mounted_volumes[i] && mounted_volumes[i][0])
      {
         Inhibit(mounted_volumes[i], DOSFALSE);
         FreeVec(mounted_volumes[i]);
      }
   }
   mounted_volume_count = 0;
   memset(mounted_volumes, 0, sizeof(mounted_volumes));
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
      strncat(drive, ":", 256);
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
       {LBNCA_Text,         (ULONG)number},
       {LBNCA_Justification,LCJ_LEFT},
       {LBNCA_CopyText,     TRUE},
       {LBNCA_MaxChars,     10},
       {LBNA_UserData,      (ULONG)index},
       {LBNA_Column,        1},
       {LBNCA_Text,         (ULONG)(STRPTR)text},
       {LBNCA_Justification,LCJ_LEFT},
       {LBNCA_CopyText,     TRUE},
       {LBNCA_MaxChars,     100},
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

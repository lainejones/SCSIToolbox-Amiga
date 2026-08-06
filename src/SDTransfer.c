/*
 * Copyright (C) 2024 Paul Hill
 *
 * Modified 2026-08-06 by Laine Jones (lainejones): part of SCSIToolbox-Amiga.
 * Added SD-card subdirectory navigation (firmware v2026.04.27+ working-dir
 * support): directories are listed with a trailing "/", the "/" entry goes up,
 * and the Open/Download button descends into a selected directory.
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
#include <stdlib.h>
#include <proto/asl.h>
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
#include <proto/fuelgauge.h>
#include <images/label.h>
#include <reaction/reaction_macros.h>
#include <devices/scsidisk.h>
#include <graphics/gfx.h>
#include <workbench/startup.h>
#include "toolbox.h"

static const char ver[] = "$VER: SDTransfer 1.3 (6.8.2026)";

/* Node kinds stored in LBNA_UserData */
#define NODE_DIR    0
#define NODE_FILE   1
#define NODE_PARENT 2

void FreeListBrowserNodes(void);
BOOL AddListBrowserNode(ULONG kind, STRPTR filename);
BOOL RefreshFileList(void);
int DirDescend(const char *name);
int DirAscend(void);
void progress(int pc);
void getfilename(char *name, char *title);
void format_size(char *buffer, int length, ULONG size);
void bstrcpy(char *dest, UBYTE *src);

/* GCC: = NULL makes these STRONG symbols so libstubs.a won't auto-open them */
struct Library *WindowBase = NULL, *LayoutBase = NULL, *LabelBase = NULL, *ListBrowserBase = NULL;
struct Library *UtilityBase = NULL, *FuelGaugeBase = NULL, *IconBase = NULL, *AslBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct MsgPort *AppPort;

UBYTE scsi_dev[1024];
LONG scsi_unit = -1;

/* Working-directory navigation state (firmware CAP_SET_WORKING_DIR).
   gBaseDir = the device default (e.g. "/shared"); gRelPath = where we are
   below it ("" = top). */
static int gHaveDirs = 0;
static char gBaseDir[80];
static char gRelPath[64];

static char *readArgsTemplate = "DEVICE/K,UNIT/K/N";
static char* appname = "SD Transfer";
static char fuelGaugeText[MAXPATH + 48];

enum ToolboxParams
{
   DEVICE,
   UNIT,
};

enum Gadgets
{
   GID_DOWNLOAD,
   GID_LISTBROWSER,
   GID_FUELGAUGE,
};

struct List gb_List;
struct ColumnInfo gb_ListbrowserColumn[] =
{
   { 170, "Image", 0 },
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
    0, 0,             // LeftEdge, TopEdge
    LOGO_WIDTH, LOGO_HEIGHT, 1,  // Width, Height, Depth
    NULL,        // ImageData
    0x0001, 0x0000,   // PlanePick, PlaneOnOff
    NULL              // NextImage
};

struct Window *mainWindow;
Object *fuelGauge;

int main(int argc, char **argv)
{
   struct RDArgs *rd;
   LONG params[] = {0, 0};
   char scsi_msg[50];
   APTR windowObj;
   Object *listBrowser;

   /* SAS/C startup sets pr_WindowPtr = -1 automatically to suppress DOS
      volume requesters. GCC startup does not, so we do it explicitly. */
   {
      struct Process *myproc = (struct Process *)FindTask(NULL);
      myproc->pr_WindowPtr = (APTR)-1L;
   }

   if ((IntuitionBase = (struct IntuitionBase *) OpenLibrary("intuition.library", 33L)) == NULL)
   {
      PutStr("Could not open intuition.library\n");
      goto exit;
   }
   if ((WindowBase = OpenLibrary("window.class", 0)) == NULL)
   {
      MessageBox(appname, "Could not open window.class\n");
      goto exit;
   }
   if ((UtilityBase = OpenLibrary("utility.library", 37L)) == NULL)
   {
      MessageBox(appname, "Could not open utility.library\n");
      goto exit;
   }
   if ((AslBase = OpenLibrary("asl.library", 44L)) == NULL)
   {
      MessageBox(appname, "Could not open asl.library\n");
      goto exit;
   }
   if ((ListBrowserBase = OpenLibrary("gadgets/listbrowser.gadget", 44)) == NULL)
   {
      MessageBox(appname, "Could not open listbrowser.gadget");
      goto exit;
   }
   if ((LayoutBase = OpenLibrary("gadgets/layout.gadget", 44)) == NULL)
   {
      MessageBox(appname, "Could not open layout.gadget");
      goto exit;
   }
   if ((LabelBase = OpenLibrary("images/label.image", 44)) == NULL)
   {
      MessageBox(appname, "Could not open label.image");
      goto exit;
   }
   if ((FuelGaugeBase = OpenLibrary("gadgets/fuelgauge.gadget", 44)) == NULL)
   {
      MessageBox(appname, "Could not open fuelgauge.gadget");
      goto exit;
   }
   if ((AppPort = CreateMsgPort()) == NULL)
   {
      goto exit;
   }

   if (argc==0)
   {
      // Started from Workbench. Read tooltypes
      IconBase = OpenLibrary("icon.library", 33L);
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

      if (scsi_dev[0] == '\0' || scsi_unit < 0)
      {
         MessageBox(appname, "Missing SCSI device or unit");
         goto exit;
      }
   }
   else
   {
      // Started from CLI
      rd = ReadArgs(readArgsTemplate, params, NULL);
      if (rd)
      {
         if (params[DEVICE])
         {
            strncpy(scsi_dev, (UBYTE *)params[DEVICE], sizeof(scsi_dev));
         }
         if (params[UNIT])
         {
            scsi_unit = (*((ULONG *)params[UNIT]));
         }
      }
      else
      {
         SetIoErr(ERROR_REQUIRED_ARG_MISSING);
         PrintFault(IoErr(), argv[0]);
         goto exit;
      }
      FreeArgs(rd);

      if (scsi_dev[0] == '\0' || scsi_unit < 0)
      {
         SetIoErr(ERROR_REQUIRED_ARG_MISSING);
         PrintFault(IoErr(), argv[0]);
         goto exit;
      }
   }

   if (scsi_setup(scsi_dev, scsi_unit) != 0)
   {
      goto exit;
   }

   if (scsi_capabilities & BLUESCSI_TOOLBOX_CAP_SET_WORKING_DIR)
   {
      if (Toolbox_Get_Working_Dir(gBaseDir, sizeof(gBaseDir)) == 0 && gBaseDir[0])
         gHaveDirs = 1;
   }

   chip_logo_data = (UWORD *)AllocVec(LOGO_DATA_SIZE, MEMF_CHIP);
   if (chip_logo_data) {
      CopyMem(scsi_isZuluSCSI ? zuluscsi_logo_data : bluescsi_logo_data,
              chip_logo_data, LOGO_DATA_SIZE);
      logo_image.ImageData = chip_logo_data;
   }

   NewList(&gb_List);

   // Read the SD Card Files
   if (!RefreshFileList())
   {
      goto exit;
   }

   sprintf(scsi_msg, "Device:%s Unit:%ld", scsi_dev, scsi_unit);

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

         // BlueSCSI Logo
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

         // Current SCSI device
         LAYOUT_AddImage,
            LabelObject,
               LABEL_Text, scsi_msg,
            End,

         // ListBrowser with the SD files
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
               LISTBROWSER_MinVisible, 5,
            End,

         // Download button
         LAYOUT_AddChild,
            ButtonObject,
               GA_ID, GID_DOWNLOAD,
               GA_RelVerify, TRUE,
               GA_Text, "_Open / Download",
            End,
            CHILD_MinHeight, 20,
            CHILD_MaxHeight, 20,

         // Progress
         LAYOUT_AddChild,
            fuelGauge = FuelGaugeObject,
               GA_ID, GID_FUELGAUGE,
               GA_RelVerify, TRUE,
               GA_Text, "",
               FUELGAUGE_Min, 0,
               FUELGAUGE_Max, 100,
               FUELGAUGE_Level, 0,
               FUELGAUGE_Percent, FALSE,
               FUELGAUGE_FillPen, FILLPEN,
               FUELGAUGE_Ticks, 0,
            End,
            CHILD_MaxHeight, 20,

      EndGroup,
   EndWindow;

   mainWindow = (struct Window *)RA_OpenWindow(windowObj);
   if (mainWindow)
   {
      ULONG wait;
      ULONG done = FALSE;
      ULONG result;
      ULONG code;
      ULONG signal;
      struct Node *node;
      UBYTE app = (1L << AppPort->mp_SigBit);

      /* Obtain the window wait signal mask */
      GetAttr(WINDOW_SigMask, windowObj, &signal);
      wait = Wait(signal | SIGBREAKF_CTRL_C | app);

      while (!done)
      {
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
                        //Printf("WMHI_GADGETUP GID_LISTBROWSER\n");
                        break;
                     case GID_DOWNLOAD:
                        GetAttr(LISTBROWSER_SelectedNode, listBrowser, (ULONG*) &node);
                        if (node)
                        {
                           // Get the filename and the entry kind
                           ULONG userdata;
                           ULONG kind = NODE_FILE;
                           {
                              struct TagItem gtags[] = {
                                  {LBNA_Column,  0},
                                  {LBNCA_Text,   (ULONG)&userdata},
                                  {LBNA_UserData,(ULONG)&kind},
                                  {TAG_DONE,     0}
                              };
                              GetListBrowserNodeAttrsA(node, gtags);
                           }
                           if (userdata && kind != NODE_FILE)
                           {
                              // Directory navigation: rebuild the list
                              int navres;
                              SetGadgetAttrs((struct Gadget *)listBrowser, mainWindow, NULL,
                                             LISTBROWSER_Labels, ~0, TAG_END);
                              if (kind == NODE_PARENT)
                                 navres = DirAscend();
                              else
                                 navres = DirDescend((char *)userdata);
                              if (navres == 0)
                                 RefreshFileList();
                              SetGadgetAttrs((struct Gadget *)listBrowser, mainWindow, NULL,
                                             LISTBROWSER_Labels, &gb_List, TAG_END);
                              sprintf(fuelGaugeText, "Dir: /%s", gRelPath);
                              SetGadgetAttrs((struct Gadget *)fuelGauge, mainWindow, NULL,
                                             GA_Text, fuelGaugeText,
                                             FUELGAUGE_Percent, FALSE,
                                             TAG_END);
                           }
                           else if (userdata)
                           {
                              char *source = (char *) userdata;
                              char destination[MAXPATH];

                              // Set the fuelGauge to %
                              SetGadgetAttrs((struct Gadget *)fuelGauge, mainWindow, NULL, FUELGAUGE_Percent, TRUE, FUELGAUGE_Level, 0, TAG_END);

                              strncpy(destination, source, MAXPATH);

                              getfilename(destination, "Save As");
                              if (destination)
                              {
                                 ULONG bytes = Toolbox_Download(source, destination, progress);
                                 if (bytes > 0)
                                 {
                                    char size_text[32];

                                    format_size(size_text, sizeof(size_text), bytes);
                                    sprintf(fuelGaugeText, "%s bytes saved to %s", size_text, destination);
                                    SetGadgetAttrs((struct Gadget *)fuelGauge, mainWindow, NULL,
                                                   GA_Text, fuelGaugeText,
                                                   FUELGAUGE_Percent, FALSE,
                                                   TAG_END);
                                 }
                              }
                           }
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
                  //printf("%lx\n", result & WMHI_CLASSMASK);
                  break;
            }
         }
      }

      DisposeObject( windowObj );

      FreeListBrowserNodes();
   }
   else
   {
      MessageBox(appname, "Could not open window\n");
   }

exit:
   if (gHaveDirs && gRelPath[0])
      Toolbox_Set_Working_Dir("");   /* leave the device at its default dir */
   if (chip_logo_data) FreeVec(chip_logo_data);
   scsi_cleanup();

   if (AppPort) DeleteMsgPort(AppPort);
   if (IconBase) CloseLibrary(IconBase);
   if (FuelGaugeBase) CloseLibrary(FuelGaugeBase);
   if (LabelBase) CloseLibrary(LabelBase);
   if (LayoutBase) CloseLibrary(LayoutBase);
   if (ListBrowserBase) CloseLibrary(ListBrowserBase);
   if (WindowBase) CloseLibrary(WindowBase);
   if (AslBase) CloseLibrary(AslBase);
   if (UtilityBase) CloseLibrary(UtilityBase);
   if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
}

/* Update progress */
void progress(int pc)
{
   SetGadgetAttrs((struct Gadget *)fuelGauge, mainWindow, NULL, FUELGAUGE_Level, pc, TAG_END);
}

void format_size(char *buffer, int length, ULONG size)
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

/* Add an entry to the browser. kind: NODE_DIR (shown with a trailing "/"),
   NODE_FILE, or NODE_PARENT (shown as "/"). */
BOOL AddListBrowserNode(ULONG kind, STRPTR filename)
{
  struct Node *node;
  char display[40];
  struct TagItem tags[] = {
      {LBNA_Generation,    2},
      {LBNA_Column,        0},
      {LBNCA_Text,         (ULONG)display},
      {LBNCA_Justification,LCJ_LEFT},
      {LBNCA_CopyText,     TRUE},
      {LBNCA_MaxChars,     100},
      {LBNA_UserData,      kind},
      {TAG_END,            0}
  };
  strncpy(display, filename, sizeof(display) - 2);
  display[sizeof(display) - 2] = '\0';
  if (kind == NODE_DIR)
     strcat(display, "/");
  if((node = AllocListBrowserNodeA(2, tags)))
  {
    AddTail(&gb_List,node);
  }
  return (BOOL)( node ? TRUE : FALSE );
}

/* Rebuild gb_List from the device's current working directory. Returns FALSE
   only on SCSI failure (an empty directory is fine). The listbrowser must be
   detached (or not yet created) when this is called. */
BOOL RefreshFileList(void)
{
   struct FileEntry *file;

   FreeListBrowserNodes();

   file = Toolbox_List_Files(0);
   if (!file && filecount < 0)
      return FALSE;

   if (gHaveDirs && gRelPath[0])
      AddListBrowserNode(NODE_PARENT, "/");

   if (file)
   {
      while (file->Type >= 0)
      {
         if (file->Type == BLUESCSI_FILE)
            AddListBrowserNode(NODE_FILE, file->Name);
         else if (gHaveDirs && file->Type == BLUESCSI_DIR)
            AddListBrowserNode(NODE_DIR, file->Name);
         file++;
      }
   }
   return TRUE;
}

/* Point the device's working directory at gBaseDir/gRelPath. */
static int ApplyWorkingDir(void)
{
   char abs[160];

   strcpy(abs, gBaseDir);
   if (gRelPath[0])
   {
      strcat(abs, "/");
      strcat(abs, gRelPath);
   }
   if (strlen(abs) > TOOLBOX_MAX_WD_PATH - 1)
      return -1;
   return (int)Toolbox_Set_Working_Dir(abs);
}

/* Descend into subdirectory 'name' (display form; trailing '/' stripped). */
int DirDescend(const char *name)
{
   char leaf[40];
   int len;
   int oldlen = strlen(gRelPath);

   strncpy(leaf, name, sizeof(leaf) - 1);
   leaf[sizeof(leaf) - 1] = '\0';
   len = strlen(leaf);
   if (len > 0 && leaf[len - 1] == '/')
      leaf[len - 1] = '\0';
   if (leaf[0] == '\0')
      return -1;

   if (oldlen + 1 + strlen(leaf) >= sizeof(gRelPath))
      return -1;
   if (oldlen)
      strcat(gRelPath, "/");
   strcat(gRelPath, leaf);

   if (ApplyWorkingDir() != 0)
   {
      gRelPath[oldlen] = '\0';                    /* revert */
      ApplyWorkingDir();
      return -1;
   }
   return 0;
}

/* Go up one directory level. */
int DirAscend(void)
{
   char *p;

   if (gRelPath[0] == '\0')
      return -1;
   p = strrchr(gRelPath, '/');
   if (p)
      *p = '\0';
   else
      gRelPath[0] = '\0';
   return ApplyWorkingDir();
}

/* Free the browser nodes */
void FreeListBrowserNodes(void)
{
  struct Node *node, *nextnode;
  node = gb_List.lh_Head;

  while((nextnode = node->ln_Succ))
  {
    Remove(node);
    FreeListBrowserNode(node);
    node = nextnode;
  }
}

/* Copy a BCPL string to a C string */
void bstrcpy(char *dest, UBYTE *src)
{
   int len = *src++;
   strncpy(dest, src, len + 1);
   dest[len] = 0;
}

/*
** getfilename()
** - ask the user for a file name using the ASL file requester
*/
void getfilename(char *name, char *title)
{
   struct FileRequester *fr;
   char dirname[MAXPATH];
   char *filename;

   if (strlen(name) > 0)
   {
      strncpy(dirname, name, MAXPATH);
      dirname[MAXPATH - 1] = '\0';
      filename = (STRPTR) PathPart(dirname);
      filename[0] = '\0';
      filename = (STRPTR) FilePart(name);
   } else
   {
      dirname[0] = '\0';
      filename = dirname;
   }

   if (fr = (struct FileRequester *)
            AllocAslRequestTags(ASL_FileRequest,
                                ASLFR_TitleText,     (ULONG) title,
                                ASLFR_InitialFile,   (ULONG) filename,
                                ASLFR_InitialDrawer, (ULONG) dirname,
                                TAG_DONE))
   {
      if (AslRequest(fr, NULL))
      {
         strcpy(name, fr->rf_Dir);
         AddPart(name, fr->rf_File, MAXPATH);
      }
      FreeAslRequest(fr);
   }
}

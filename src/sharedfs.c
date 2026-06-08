/*
 * sharedfs.c - part of SCSIToolbox-Amiga
 *
 * Copyright (C) 2026 Laine Jones <https://github.com/lainejones/>
 *
 * SCSIToolbox-Amiga is a fork of BlueSCSI-toolbox-Amiga (C) 2024 Paul Hill.
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
 *
 * ---------------------------------------------------------------------------
 * SHARED: filesystem handler - mounts the BlueSCSI/ZuluSCSI Toolbox shared
 * folder as a read/write AmigaDOS volume, so native tools (Copy, List, DOpus,
 * Workbench) can use it instead of SDTransfer's one-file download.
 *
 * PHASE 2 SKELETON: startup + packet loop + enough to mount. Object/file
 * packets (LOCATE, EXAMINE, FINDINPUT, READ, FINDOUTPUT, WRITE, END, INFO) are
 * stubbed -> ERROR_ACTION_NOT_KNOWN and land in Phases 3-4.
 *
 * Startup convention verified against fat95-3.23 (src/fat95.s ~line 1082):
 *   pkt = msg->ln_Name; dn = BADDR(pkt->dp_Arg3);
 *   fssm = BADDR(dn->dn_Startup); unit = fssm->fssm_Unit; dev = fssm->fssm_Device
 *   dn->dn_Task = &proc->pr_MsgPort; reply dp_Res1 = DOSTRUE.
 *
 * BUILD: -nostartfiles. The asm stub hstart.s provides _start at offset 0 (the
 * hunk entry) and jumps here to handlerMain - needed because amiga-gcc emits
 * string literals / switch jump-tables into .text, which could otherwise sit at
 * offset 0 and be "executed". The normal libnix startup would also wait for a
 * WBStartup msg and misread the ACTION_STARTUP packet. amiga-gcc defaults to
 * absolute addressing (no a4 setup); LoadSeg zeroes BSS so globals start at 0.
 * Links hstart.o (first) + scsi.o (libc-free) but NOT common.o (no Intuition).
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "toolbox.h"

/* With -nostartfiles we provide the library bases ourselves. */
struct ExecBase *SysBase = NULL;
struct DosLibrary *DOSBase = NULL;
struct Library *UtilityBase = NULL;   /* scsi.c uses utility.library Str* fns */

static struct Process *gProc = NULL;

static void replyPkt(struct DosPacket *pkt, LONG res1, LONG res2);
static void bstrToC(BSTR bstr, char *buf, int max);

/* Handler main, reached from the hstart.s _start stub (see header). */
LONG handlerMain(void)
{
   struct DosPacket *pkt;
   struct Message *msg;
   struct DeviceNode *dn;
   struct FileSysStartupMsg *fssm;
   char devname[64];
   LONG unit = 0;
   int running = 1;

   SysBase = *(struct ExecBase **)4L;
   gProc = (struct Process *)FindTask(NULL);

   /* Receive the startup packet (DOS sends it to our process port). */
   WaitPort(&gProc->pr_MsgPort);
   msg = GetMsg(&gProc->pr_MsgPort);
   pkt = (struct DosPacket *)msg->mn_Node.ln_Name;

   DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37L);
   UtilityBase = OpenLibrary("utility.library", 37L);
   if (!DOSBase || !UtilityBase)
   {
      replyPkt(pkt, DOSFALSE, ERROR_INVALID_RESIDENT_LIBRARY);
      goto cleanup;
   }

   /* dp_Arg3 -> DeviceNode -> dn_Startup -> FileSysStartupMsg (unit + device). */
   dn = (struct DeviceNode *)BADDR(pkt->dp_Arg3);
   fssm = (struct FileSysStartupMsg *)BADDR(dn->dn_Startup);
   unit = fssm->fssm_Unit;
   bstrToC(fssm->fssm_Device, devname, sizeof(devname));

   /* Open the BlueSCSI/ZuluSCSI device through the shared toolbox SCSI layer. */
   if (scsi_setup(devname, (int)unit) != 0)
   {
      replyPkt(pkt, DOSFALSE, ERROR_DEVICE_NOT_MOUNTED);
      goto cleanup;
   }

   /* Become the handler for this node and acknowledge the startup. */
   dn->dn_Task = &gProc->pr_MsgPort;
   replyPkt(pkt, DOSTRUE, 0);

   /* Packet loop. */
   while (running)
   {
      WaitPort(&gProc->pr_MsgPort);
      while ((msg = GetMsg(&gProc->pr_MsgPort)) != NULL)
      {
         pkt = (struct DosPacket *)msg->mn_Node.ln_Name;

         switch (pkt->dp_Type)
         {
         case ACTION_IS_FILESYSTEM:
            replyPkt(pkt, DOSTRUE, 0);
            break;

         case ACTION_DIE:
            replyPkt(pkt, DOSTRUE, 0);
            running = 0;
            break;

         /* ---- Phase 3 (read path): ----
          * ACTION_LOCATE_OBJECT, ACTION_FREE_LOCK, ACTION_COPY_DIR,
          * ACTION_PARENT, ACTION_EXAMINE_OBJECT, ACTION_EXAMINE_NEXT,
          * ACTION_FINDINPUT, ACTION_READ, ACTION_SEEK, ACTION_END,
          * ACTION_INFO, ACTION_CURRENT_VOLUME
          * ---- Phase 4 (write path): ----
          * ACTION_FINDOUTPUT, ACTION_WRITE
          * ---- Permanently unsupported (no firmware command): ----
          * ACTION_DELETE_OBJECT, ACTION_RENAME_OBJECT, ACTION_SET_PROTECT,
          * ACTION_SET_COMMENT, ACTION_SET_DATE, ACTION_CREATE_DIR
          */
         default:
            replyPkt(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
            break;
         }
      }
   }

   scsi_cleanup();

cleanup:
   if (UtilityBase)
      CloseLibrary(UtilityBase);
   if (DOSBase)
      CloseLibrary((struct Library *)DOSBase);
   return 0;
}

/* scsi.c calls MessageBox on errors (normally common.c -> Intuition EasyRequest).
 * The handler has no UI, so swallow it here and avoid pulling in Intuition. */
void MessageBox(char *title, char *body)
{
   (void)title;
   (void)body;
}

/* Reply a DOS packet to its sender (standard idiom). */
static void replyPkt(struct DosPacket *pkt, LONG res1, LONG res2)
{
   struct MsgPort *port = pkt->dp_Port;
   struct Message *msg = pkt->dp_Link;

   pkt->dp_Res1 = res1;
   pkt->dp_Res2 = res2;
   pkt->dp_Port = &gProc->pr_MsgPort;
   msg->mn_Node.ln_Name = (char *)pkt;
   PutMsg(port, msg);
}

/* Copy a BCPL string (length-prefixed) to a NUL-terminated C string. */
static void bstrToC(BSTR bstr, char *buf, int max)
{
   UBYTE *bp = (UBYTE *)BADDR(bstr);
   int len = bp ? bp[0] : 0;
   int i;

   if (len > max - 1)
      len = max - 1;
   for (i = 0; i < len; i++)
      buf[i] = (char)bp[1 + i];
   buf[i] = '\0';
}

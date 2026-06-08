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
 * folder as an AmigaDOS volume so native tools (Copy, List, DOpus, Workbench)
 * can use it instead of SDTransfer's one-file download.
 *
 * PHASE 3: read path. Implements LOCATE/FREE_LOCK/COPY_DIR/PARENT, EXAMINE_
 * OBJECT/NEXT, FINDINPUT/READ/SEEK/END, INFO, CURRENT_VOLUME. The write path
 * (FINDOUTPUT/WRITE) and the firmware-unsupported ops (DELETE/RENAME/...) are
 * still stubbed -> Phase 4.
 *
 * Flat folder model (the shared dir has no subdirs over this protocol): the
 * only directory is the root; everything else is a file in it.
 *
 * Startup convention + FileInfoBlock/lock details verified against fat95-3.23
 * (src/fat95.s): fib_FileName is a BCPL string (length byte + chars);
 * fib_DirEntryType = 1 (root) / -3 (file).
 *
 * BUILD: -nostartfiles. hstart.s provides _start at hunk offset 0 and jumps to
 * handlerMain. Links scsi.o (libc-free) but NOT common.o (no Intuition).
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
#include <proto/utility.h>
#include "toolbox.h"

#define ID_SHARED_DISK 0x53484152UL   /* 'SHAR' */

/* With -nostartfiles we provide the library bases ourselves. */
struct ExecBase *SysBase = NULL;
struct DosLibrary *DOSBase = NULL;
struct Library *UtilityBase = NULL;   /* scsi.c (and we) use utility Str* fns */

static struct Process *gProc = NULL;
static BPTR gVolBptr = 0;             /* our DeviceNode, used as the volume */
static LONG gUnit = 0;

/* A lock = a FileLock plus our per-object bookkeeping (fl_ must be first). */
struct MyLock
{
   struct FileLock fl;
   LONG  isFile;      /* 0 = root dir, 1 = file */
   LONG  index;       /* toolbox file index (file) */
   ULONG fsize;       /* file size in bytes (file) */
   LONG  exNext;      /* EXAMINE_NEXT cursor (root dir) */
   char  name[40];    /* file leaf name (file) */
};

/* An open file. Read mode uses index/fsize/pos; write mode streams 512-byte
   blocks to the toolbox upload protocol (PREP done at open, END at close). */
struct MyFH
{
   LONG  isWrite;     /* 0 = read, 1 = write */
   /* read */
   LONG  index;
   ULONG fsize;
   ULONG pos;
   /* write */
   ULONG wblock;      /* next 512-byte block index */
   ULONG wbuflen;     /* bytes currently buffered in wbuf */
   UBYTE wbuf[512];
};

/* ---- small libc-free string helpers ------------------------------------ */

static int cLen(const char *s)
{
   int n = 0;
   while (s[n])
      n++;
   return n;
}

static void cCopy(char *d, const char *s, int max)
{
   int i = 0;
   while (s[i] && i < max - 1)
   {
      d[i] = s[i];
      i++;
   }
   d[i] = '\0';
}

/* Write a BCPL string (length byte + chars) for fib_FileName / fib_Comment. */
static void putBStr(UBYTE *dest, const char *src, int maxchars)
{
   int n = cLen(src);
   int i;
   if (n > maxchars)
      n = maxchars;
   dest[0] = (UBYTE)n;
   for (i = 0; i < n; i++)
      dest[1 + i] = (UBYTE)src[i];
}

/* Convert a BSTR (BPTR) name to a C string leaf: strip any "VOLUME:" prefix
   and a leading '/'. An empty result means the volume root. */
static void leafFromBStr(BPTR bname, char *out, int max)
{
   UBYTE *bp = (UBYTE *)BADDR(bname);
   int len = bp ? bp[0] : 0;
   int start = 0;
   int i, j;

   for (i = 0; i < len; i++)        /* skip past a colon, if any */
   {
      if (bp[1 + i] == ':')
         start = i + 1;
   }
   if (start < len && bp[1 + start] == '/')
      start++;

   for (i = start, j = 0; i < len && j < max - 1; i++, j++)
      out[j] = (char)bp[1 + i];
   out[j] = '\0';
}

/* ---- locks ------------------------------------------------------------- */

static BPTR makeLock(LONG isFile, LONG index, ULONG fsize, const char *name)
{
   struct MyLock *lk = AllocVec(sizeof(struct MyLock), MEMF_CLEAR | MEMF_PUBLIC);
   if (!lk)
      return 0;
   lk->fl.fl_Key = index;
   lk->fl.fl_Access = ACCESS_READ;
   lk->fl.fl_Task = &gProc->pr_MsgPort;
   lk->fl.fl_Volume = gVolBptr;
   lk->isFile = isFile;
   lk->index = index;
   lk->fsize = fsize;
   lk->exNext = 0;
   if (isFile && name)
      cCopy(lk->name, name, sizeof(lk->name));
   return MKBADDR(lk);
}

static void freeLock(BPTR lock)
{
   struct MyLock *lk = (struct MyLock *)BADDR(lock);
   if (lk)
      FreeVec(lk);
}

/* Look up a file by name (case-insensitive). Re-lists the shared folder.
   Returns 1 and fills idx and size on success, 0 if not found. */
static int findFile(const char *name, LONG *idx, ULONG *size)
{
   struct FileEntry *list = Toolbox_List_Files(0);
   int i;
   if (!list)
      return 0;
   for (i = 0; i < filecount; i++)
   {
      if (list[i].Type == BLUESCSI_FILE && Stricmp((STRPTR)list[i].Name, (STRPTR)name) == 0)
      {
         *idx = list[i].Index;
         *size = list[i].Size;
         return 1;
      }
   }
   return 0;
}

/* ---- packet handlers --------------------------------------------------- */

static void doLocateObject(struct DosPacket *pkt)
{
   char leaf[40];
   LONG idx;
   ULONG size;

   leafFromBStr(pkt->dp_Arg2, leaf, sizeof(leaf));

   if (leaf[0] == '\0')                          /* root */
   {
      BPTR lk = makeLock(0, 0, 0, NULL);
      if (lk)
         { pkt->dp_Res1 = (LONG)lk; pkt->dp_Res2 = 0; }
      else
         { pkt->dp_Res1 = 0; pkt->dp_Res2 = ERROR_NO_FREE_STORE; }
      return;
   }

   if (findFile(leaf, &idx, &size))
   {
      BPTR lk = makeLock(1, idx, size, leaf);
      if (lk)
         { pkt->dp_Res1 = (LONG)lk; pkt->dp_Res2 = 0; }
      else
         { pkt->dp_Res1 = 0; pkt->dp_Res2 = ERROR_NO_FREE_STORE; }
   }
   else
   {
      pkt->dp_Res1 = 0;
      pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
   }
}

static void doCopyDir(struct DosPacket *pkt)
{
   struct MyLock *src = (struct MyLock *)BADDR((BPTR)pkt->dp_Arg1);
   BPTR lk;
   if (!src) { pkt->dp_Res1 = 0; return; }       /* dup of root (0) -> root */
   lk = makeLock(src->isFile, src->index, src->fsize, src->name);
   pkt->dp_Res1 = (LONG)lk;
   pkt->dp_Res2 = lk ? 0 : ERROR_NO_FREE_STORE;
}

static void doParent(struct DosPacket *pkt)
{
   struct MyLock *lk = (struct MyLock *)BADDR((BPTR)pkt->dp_Arg1);
   if (lk && lk->isFile)                          /* file -> root lock */
      pkt->dp_Res1 = (LONG)makeLock(0, 0, 0, NULL);
   else                                           /* root -> no parent */
      pkt->dp_Res1 = 0;
   pkt->dp_Res2 = 0;
}

static void fillDirFib(struct FileInfoBlock *fib, const char *name)
{
   fib->fib_DiskKey = 0;
   fib->fib_DirEntryType = 1;                     /* root dir */
   fib->fib_EntryType = 1;
   putBStr((UBYTE *)fib->fib_FileName, name, 30);
   fib->fib_Protection = 0;
   fib->fib_Size = 0;
   fib->fib_NumBlocks = 0;
   fib->fib_Comment[0] = 0;
}

static void fillFileFib(struct FileInfoBlock *fib, const char *name, ULONG size)
{
   fib->fib_DiskKey = 0;
   fib->fib_DirEntryType = -3;                    /* file */
   fib->fib_EntryType = -3;
   putBStr((UBYTE *)fib->fib_FileName, name, 106);
   fib->fib_Protection = 0;
   fib->fib_Size = (LONG)size;
   fib->fib_NumBlocks = (LONG)((size + 511) / 512);
   fib->fib_Comment[0] = 0;
}

static void doExamineObject(struct DosPacket *pkt)
{
   struct MyLock *lk = (struct MyLock *)BADDR((BPTR)pkt->dp_Arg1);
   struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR((BPTR)pkt->dp_Arg2);

   if (lk && lk->isFile)
   {
      fillFileFib(fib, lk->name, lk->fsize);
   }
   else                                           /* root dir */
   {
      fillDirFib(fib, "SHARED");
      if (lk)
         lk->exNext = 0;                          /* reset directory scan */
      Toolbox_List_Files(0);                      /* refresh list for ExNext */
   }
   pkt->dp_Res1 = DOSTRUE;
   pkt->dp_Res2 = 0;
}

static void doExamineNext(struct DosPacket *pkt)
{
   struct MyLock *lk = (struct MyLock *)BADDR((BPTR)pkt->dp_Arg1);
   struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR((BPTR)pkt->dp_Arg2);
   struct FileEntry *list = files;                /* cached by ExamineObject */
   LONG pos;

   if (!lk || lk->isFile || !list)
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
      return;
   }

   pos = lk->exNext;
   while (pos < filecount && list[pos].Type != BLUESCSI_FILE)
      pos++;                                       /* skip non-file entries */

   if (pos >= filecount)
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
      return;
   }

   fillFileFib(fib, list[pos].Name, list[pos].Size);
   lk->exNext = pos + 1;
   pkt->dp_Res1 = DOSTRUE;
   pkt->dp_Res2 = 0;
}

static void doFindInput(struct DosPacket *pkt)
{
   struct FileHandle *fh = (struct FileHandle *)BADDR((BPTR)pkt->dp_Arg1);
   char leaf[40];
   LONG idx;
   ULONG size;
   struct MyFH *mfh;

   leafFromBStr(pkt->dp_Arg3, leaf, sizeof(leaf));
   if (leaf[0] == '\0' || !findFile(leaf, &idx, &size))
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
      return;
   }

   mfh = AllocVec(sizeof(struct MyFH), MEMF_CLEAR | MEMF_PUBLIC);
   if (!mfh)
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_NO_FREE_STORE;
      return;
   }
   mfh->index = idx;
   mfh->fsize = size;
   mfh->pos = 0;
   fh->fh_Arg1 = (LONG)mfh;
   pkt->dp_Res1 = DOSTRUE;
   pkt->dp_Res2 = 0;
}

static void doRead(struct DosPacket *pkt)
{
   struct MyFH *mfh = (struct MyFH *)pkt->dp_Arg1;
   UBYTE *buf = (UBYTE *)pkt->dp_Arg2;
   ULONG len = (ULONG)pkt->dp_Arg3;
   LONG got;

   if (!mfh)
   {
      pkt->dp_Res1 = -1;
      pkt->dp_Res2 = ERROR_INVALID_LOCK;
      return;
   }
   if (mfh->pos >= mfh->fsize || len == 0)
   {
      pkt->dp_Res1 = 0;                            /* EOF */
      return;
   }
   if (len > mfh->fsize - mfh->pos)
      len = mfh->fsize - mfh->pos;

   got = Toolbox_Get_Bytes((int)mfh->index, mfh->pos, buf, len);
   if (got < 0)
   {
      pkt->dp_Res1 = -1;
      pkt->dp_Res2 = ERROR_SEEK_ERROR;
      return;
   }
   mfh->pos += (ULONG)got;
   pkt->dp_Res1 = got;
   pkt->dp_Res2 = 0;
}

static void doSeek(struct DosPacket *pkt)
{
   struct MyFH *mfh = (struct MyFH *)pkt->dp_Arg1;
   LONG offset = pkt->dp_Arg2;
   LONG mode = pkt->dp_Arg3;
   LONG oldpos;
   LONG newpos;

   if (!mfh || mfh->isWrite)            /* upload is sequential; no seek */
   {
      pkt->dp_Res1 = -1;
      pkt->dp_Res2 = ERROR_SEEK_ERROR;
      return;
   }
   oldpos = (LONG)mfh->pos;

   if (mode == OFFSET_BEGINNING)
      newpos = offset;
   else if (mode == OFFSET_END)
      newpos = (LONG)mfh->fsize + offset;
   else                                            /* OFFSET_CURRENT */
      newpos = oldpos + offset;

   if (newpos < 0 || newpos > (LONG)mfh->fsize)
   {
      pkt->dp_Res1 = -1;
      pkt->dp_Res2 = ERROR_SEEK_ERROR;
      return;
   }
   mfh->pos = (ULONG)newpos;
   pkt->dp_Res1 = oldpos;
   pkt->dp_Res2 = 0;
}

static void doEnd(struct DosPacket *pkt)
{
   struct MyFH *mfh = (struct MyFH *)pkt->dp_Arg1;
   if (mfh)
   {
      if (mfh->isWrite)                            /* finish the upload */
      {
         if (mfh->wbuflen > 0)
            Toolbox_Send_Block(mfh->wblock, mfh->wbuf, (int)mfh->wbuflen);
         Toolbox_Send_End();
      }
      FreeVec(mfh);
   }
   pkt->dp_Res1 = DOSTRUE;
   pkt->dp_Res2 = 0;
}

static void doFindOutput(struct DosPacket *pkt)
{
   struct FileHandle *fh = (struct FileHandle *)BADDR((BPTR)pkt->dp_Arg1);
   char leaf[40];
   struct MyFH *mfh;

   leafFromBStr(pkt->dp_Arg3, leaf, sizeof(leaf));
   if (leaf[0] == '\0')                            /* can't create the root */
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
      return;
   }

   if (Toolbox_Send_Prep(leaf) != 0)              /* opens/creates on the SD card */
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_OBJECT_IN_USE;
      return;
   }

   mfh = AllocVec(sizeof(struct MyFH), MEMF_CLEAR | MEMF_PUBLIC);
   if (!mfh)
   {
      Toolbox_Send_End();                          /* close the prepared file */
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_NO_FREE_STORE;
      return;
   }
   mfh->isWrite = 1;
   fh->fh_Arg1 = (LONG)mfh;
   pkt->dp_Res1 = DOSTRUE;
   pkt->dp_Res2 = 0;
}

static void doWrite(struct DosPacket *pkt)
{
   struct MyFH *mfh = (struct MyFH *)pkt->dp_Arg1;
   UBYTE *src = (UBYTE *)pkt->dp_Arg2;
   ULONG len = (ULONG)pkt->dp_Arg3;
   ULONG done = 0;

   if (!mfh || !mfh->isWrite)
   {
      pkt->dp_Res1 = -1;
      pkt->dp_Res2 = ERROR_INVALID_LOCK;
      return;
   }

   while (done < len)                              /* fill 512-byte blocks, flush each */
   {
      ULONG space = 512 - mfh->wbuflen;
      ULONG n = len - done;
      if (n > space)
         n = space;
      CopyMem(src + done, mfh->wbuf + mfh->wbuflen, n);
      mfh->wbuflen += n;
      done += n;
      if (mfh->wbuflen == 512)
      {
         if (Toolbox_Send_Block(mfh->wblock, mfh->wbuf, 512) != 0)
         {
            pkt->dp_Res1 = -1;
            pkt->dp_Res2 = ERROR_DISK_FULL;
            return;
         }
         mfh->wblock++;
         mfh->wbuflen = 0;
      }
   }
   pkt->dp_Res1 = (LONG)len;
   pkt->dp_Res2 = 0;
}

static void doInfo(struct DosPacket *pkt, struct InfoData *id)
{
   id->id_NumSoftErrors = 0;
   id->id_UnitNumber = gUnit;
   id->id_DiskState = ID_VALIDATED;
   id->id_NumBlocks = 0x7FFFFFFF / 512;
   id->id_NumBlocksUsed = 0;
   id->id_BytesPerBlock = 512;
   id->id_DiskType = ID_SHARED_DISK;
   id->id_VolumeNode = gVolBptr;
   id->id_InUse = 0;
   pkt->dp_Res1 = DOSTRUE;
   pkt->dp_Res2 = 0;
}

/* ---- entry / loop ------------------------------------------------------ */

LONG handlerMain(void)
{
   struct DosPacket *pkt;
   struct Message *msg;
   struct DeviceNode *dn;
   struct FileSysStartupMsg *fssm;
   char devname[64];
   int running = 1;

   SysBase = *(struct ExecBase **)4L;
   gProc = (struct Process *)FindTask(NULL);

   WaitPort(&gProc->pr_MsgPort);
   msg = GetMsg(&gProc->pr_MsgPort);
   pkt = (struct DosPacket *)msg->mn_Node.ln_Name;

   DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37L);
   UtilityBase = OpenLibrary("utility.library", 37L);
   if (!DOSBase || !UtilityBase)
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_INVALID_RESIDENT_LIBRARY;
      goto reply_and_exit;
   }

   gVolBptr = (BPTR)pkt->dp_Arg3;                  /* DeviceNode = our volume */
   dn = (struct DeviceNode *)BADDR((BPTR)pkt->dp_Arg3);
   fssm = (struct FileSysStartupMsg *)BADDR(dn->dn_Startup);
   gUnit = fssm->fssm_Unit;
   leafFromBStr(fssm->fssm_Device, devname, sizeof(devname));
   /* leafFromBStr would strip a ':' - device names have none, so this is just
      a plain BSTR->C copy here. */

   if (scsi_setup(devname, (int)gUnit) != 0)
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_DEVICE_NOT_MOUNTED;
      goto reply_and_exit;
   }

   dn->dn_Task = &gProc->pr_MsgPort;
   pkt->dp_Res1 = DOSTRUE;
   pkt->dp_Res2 = 0;
   {
      struct MsgPort *rp = pkt->dp_Port;
      pkt->dp_Port = &gProc->pr_MsgPort;
      msg->mn_Node.ln_Name = (char *)pkt;
      PutMsg(rp, msg);
   }

   while (running)
   {
      WaitPort(&gProc->pr_MsgPort);
      while ((msg = GetMsg(&gProc->pr_MsgPort)) != NULL)
      {
         struct MsgPort *rp;
         pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
         pkt->dp_Res1 = DOSFALSE;
         pkt->dp_Res2 = 0;

         switch (pkt->dp_Type)
         {
         case ACTION_IS_FILESYSTEM:  pkt->dp_Res1 = DOSTRUE; break;
         case ACTION_CURRENT_VOLUME: pkt->dp_Res1 = (LONG)gVolBptr; break;
         case ACTION_LOCATE_OBJECT:  doLocateObject(pkt); break;
         case ACTION_FREE_LOCK:      freeLock((BPTR)pkt->dp_Arg1); pkt->dp_Res1 = DOSTRUE; break;
         case ACTION_COPY_DIR:       doCopyDir(pkt); break;
         case ACTION_PARENT:         doParent(pkt); break;
         case ACTION_EXAMINE_OBJECT: doExamineObject(pkt); break;
         case ACTION_EXAMINE_NEXT:   doExamineNext(pkt); break;
         case ACTION_FINDINPUT:      doFindInput(pkt); break;
         case ACTION_FINDOUTPUT:     doFindOutput(pkt); break;
         case ACTION_READ:           doRead(pkt); break;
         case ACTION_WRITE:          doWrite(pkt); break;
         case ACTION_SEEK:           doSeek(pkt); break;
         case ACTION_END:            doEnd(pkt); break;
         case ACTION_INFO:           doInfo(pkt, (struct InfoData *)BADDR((BPTR)pkt->dp_Arg2)); break;
         case ACTION_DISK_INFO:      doInfo(pkt, (struct InfoData *)BADDR((BPTR)pkt->dp_Arg1)); break;

         case ACTION_DIE:
            pkt->dp_Res1 = DOSTRUE;
            running = 0;
            break;

         /* Firmware has no delete/rename/metadata ops -> fail tools gracefully. */
         case ACTION_DELETE_OBJECT:
            pkt->dp_Res2 = ERROR_DELETE_PROTECTED;
            break;
         case ACTION_RENAME_OBJECT:
         case ACTION_SET_PROTECT:
         case ACTION_SET_COMMENT:
         case ACTION_SET_DATE:
            pkt->dp_Res2 = ERROR_WRITE_PROTECTED;
            break;

         default:
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
            break;
         }

         rp = pkt->dp_Port;
         pkt->dp_Port = &gProc->pr_MsgPort;
         msg->mn_Node.ln_Name = (char *)pkt;
         PutMsg(rp, msg);
      }
   }

   scsi_cleanup();

reply_and_exit:
   if (running == 0)
   {
      /* normal ACTION_DIE path already replied each packet */
   }
   else
   {
      /* startup failure: reply the startup packet we still hold */
      struct MsgPort *rp = pkt->dp_Port;
      pkt->dp_Port = &gProc->pr_MsgPort;
      msg->mn_Node.ln_Name = (char *)pkt;
      PutMsg(rp, msg);
   }
   if (UtilityBase)
      CloseLibrary(UtilityBase);
   if (DOSBase)
      CloseLibrary((struct Library *)DOSBase);
   return 0;
}

/* scsi.c calls MessageBox on errors (normally common.c -> Intuition). The
 * handler has no UI, so swallow it and avoid pulling in Intuition. */
void MessageBox(char *title, char *body)
{
   (void)title;
   (void)body;
}

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
 * PHASE 6 (firmware v2026.04.27+): subdirectories via the working-directory
 * metadata subcommands (SET/GET_WORKING_DIR, CAP_SET_WORKING_DIR). Locks carry
 * a path relative to the share root; every directory-scoped operation first
 * points the device's (global, persistent) working dir at gBaseDir/rel. On
 * older firmware everything degrades to the previous flat model. The firmware
 * caps SET_WORKING_DIR at 63 chars, which bounds base-dir + tree depth.
 * NOTE: the working dir is device-global - don't run SDTransfer (or another
 * navigating client) on the same unit while SHARED: is mounted.
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
static BPTR gVolBptr = 0;             /* BPTR to our volume DosList node */
static struct DosList *gVol = NULL;   /* DLT_VOLUME node (Workbench disk icon) */
static LONG gUnit = 0;

/* Working-directory state (CAP_SET_WORKING_DIR firmware). gBaseDir is the
   device default (e.g. "/shared"); relative paths hang below it. gCurAbs
   mirrors what the device's working dir was last set to, so navTo can skip
   redundant SET commands. gListedDir says which relative dir the global
   'files' array currently describes. */
static int gHaveDirs = 0;
static char gBaseDir[80];
static char gCurAbs[80];
static char gListedDir[64];
static int gListedValid = 0;

/* A lock = a FileLock plus our per-object bookkeeping (fl_ must be first). */
struct MyLock
{
   struct FileLock fl;
   LONG  isFile;      /* 0 = directory, 1 = file */
   LONG  index;       /* toolbox file index within its dir (file) */
   ULONG fsize;       /* file size in bytes (file) */
   LONG  exNext;      /* EXAMINE_NEXT cursor (dir) */
   char  dir[64];     /* rel path: the dir itself (dir) / parent dir (file) */
   char  name[40];    /* leaf name ("" = root) */
};

/* An open file. Read mode uses index/fsize/pos; write mode streams 512-byte
   blocks to the toolbox upload protocol (PREP done at open, END at close). */
struct MyFH
{
   LONG  isWrite;     /* 0 = read, 1 = write */
   char  dir[64];     /* rel dir the file lives in (working dir for reads) */
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

/* ---- relative-path helpers --------------------------------------------- */

/* Strip the last component of a rel path in place ("a/b" -> "a", "a" -> ""). */
static void parentOf(char *dir)
{
   int i = cLen(dir);
   while (i > 0 && dir[i - 1] != '/')
      i--;
   if (i > 0)
      i--;                                        /* also drop the '/' */
   dir[i] = '\0';
}

/* Last component of a rel path ("a/b" -> "b", "" -> ""). */
static const char *leafOf(const char *dir)
{
   int i = cLen(dir);
   while (i > 0 && dir[i - 1] != '/')
      i--;
   return dir + i;
}

/* Append a component to a rel path ("a" + "b" -> "a/b"). 0 or DOS error. */
static LONG catAppend(char *dir, const char *comp, int max)
{
   int l = cLen(dir);
   if (l + (l ? 1 : 0) + cLen(comp) >= max)
      return ERROR_INVALID_COMPONENT_NAME;
   if (l)
      dir[l++] = '/';
   cCopy(dir + l, comp, max - l);
   return 0;
}

/* ---- device working-directory navigation ------------------------------- */

/* Point the device's working directory at gBaseDir/rel. Without the working-
   dir capability only the root ("") exists. Returns 0 or a DOS error. */
static LONG navTo(const char *rel)
{
   char abs[160];

   if (!gHaveDirs)
      return rel[0] ? ERROR_DIR_NOT_FOUND : 0;

   cCopy(abs, gBaseDir, sizeof(abs));
   if (rel[0])
   {
      int l = cLen(abs);
      if (l + 1 + cLen(rel) >= (int)sizeof(abs))
         return ERROR_INVALID_COMPONENT_NAME;
      abs[l] = '/';
      cCopy(abs + l + 1, rel, sizeof(abs) - l - 1);
   }
   if (cLen(abs) > TOOLBOX_MAX_WD_PATH - 1)       /* firmware 63-char limit */
      return ERROR_INVALID_COMPONENT_NAME;

   if (Stricmp((STRPTR)abs, (STRPTR)gCurAbs) == 0)
      return 0;
   if (Toolbox_Set_Working_Dir(abs) != 0)
      return ERROR_DIR_NOT_FOUND;
   cCopy(gCurAbs, abs, sizeof(gCurAbs));
   gListedValid = 0;
   return 0;
}

/* Make the global 'files' array describe relative dir 'rel' (cached). */
static LONG listDir(const char *rel)
{
   LONG err;

   if (gListedValid && Stricmp((STRPTR)gListedDir, (STRPTR)rel) == 0)
      return 0;
   gListedValid = 0;
   if ((err = navTo(rel)) != 0)
      return err;
   Toolbox_List_Files(0);
   if (filecount < 0)
      return ERROR_NOT_A_DOS_DISK;
   cCopy(gListedDir, rel, sizeof(gListedDir));
   gListedValid = 1;
   return 0;
}

/* Find 'leaf' in relative dir 'dirRel' (case-insensitive). Returns 1 found
   (type/idx/size filled), 0 not found, or a negated DOS error. On flat
   (no-capability) firmware, directory entries are invisible as before. */
static LONG findEntry(const char *dirRel, const char *leaf,
                      LONG *type, LONG *idx, ULONG *size)
{
   LONG err = listDir(dirRel);
   int i;

   if (err)
      return -err;
   for (i = 0; i < filecount; i++)
   {
      if (!gHaveDirs && files[i].Type != BLUESCSI_FILE)
         continue;
      if (Stricmp((STRPTR)files[i].Name, (STRPTR)leaf) == 0)
      {
         *type = files[i].Type;
         *idx = files[i].Index;
         *size = files[i].Size;
         return 1;
      }
   }
   return 0;
}

/* Resolve an AmigaDOS path (BSTR 'bname', relative to lock 'base') into a
   validated parent dir 'outDir' (rel to the share root, all intermediate
   components checked to exist) and an unvalidated final component 'outLeaf'
   ("" = the directory itself). AmigaDOS semantics: ':' resets to the root,
   each leading (or doubled) '/' goes up one level. 0 or DOS error. */
static LONG resolvePath(struct MyLock *base, BPTR bname,
                        char *outDir, char *outLeaf)
{
   UBYTE *bp = (UBYTE *)BADDR(bname);
   int len = bp ? bp[0] : 0;
   int i = 0, j;

   if (base)
      cCopy(outDir, base->dir, 64);
   else
      outDir[0] = '\0';
   outLeaf[0] = '\0';

   for (j = 0; j < len; j++)
   {
      if (bp[1 + j] == ':')
      {
         i = j + 1;
         outDir[0] = '\0';
      }
   }

   while (i < len && bp[1 + i] == '/')            /* leading '/' = parent */
   {
      if (outDir[0] == '\0')
         return ERROR_OBJECT_NOT_FOUND;           /* above the root */
      parentOf(outDir);
      i++;
   }

   while (i < len)
   {
      char comp[40];
      int cl = 0;
      int more;

      while (i < len && bp[1 + i] != '/')
      {
         if (cl < (int)sizeof(comp) - 1)
            comp[cl++] = (char)bp[1 + i];
         i++;
      }
      comp[cl] = '\0';
      more = (i < len);
      if (more)
         i++;                                     /* skip the '/' */

      if (!more)
      {
         cCopy(outLeaf, comp, 40);
         break;
      }

      if (cl == 0)                                /* "a//b": extra '/' = parent */
      {
         if (outDir[0] == '\0')
            return ERROR_OBJECT_NOT_FOUND;
         parentOf(outDir);
      }
      else
      {
         LONG t, idx;
         ULONG sz;
         LONG r = findEntry(outDir, comp, &t, &idx, &sz);
         if (r < 0)
            return -r;
         if (r == 0 || t != BLUESCSI_DIR)
            return ERROR_DIR_NOT_FOUND;
         if (catAppend(outDir, comp, 64) != 0)
            return ERROR_INVALID_COMPONENT_NAME;
      }
   }
   return 0;
}

/* ---- locks ------------------------------------------------------------- */

static BPTR makeLock(LONG isFile, LONG index, ULONG fsize,
                     const char *dir, const char *name)
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
   if (dir)
      cCopy(lk->dir, dir, sizeof(lk->dir));
   if (name)
      cCopy(lk->name, name, sizeof(lk->name));
   return MKBADDR(lk);
}

static void freeLock(BPTR lock)
{
   struct MyLock *lk = (struct MyLock *)BADDR(lock);
   if (lk)
      FreeVec(lk);
}

/* ---- packet handlers --------------------------------------------------- */

static void doLocateObject(struct DosPacket *pkt)
{
   struct MyLock *base = (struct MyLock *)BADDR((BPTR)pkt->dp_Arg1);
   char dir[64];
   char leaf[40];
   LONG err, t, idx;
   ULONG size;
   BPTR lk;

   if ((err = resolvePath(base, pkt->dp_Arg2, dir, leaf)) != 0)
   {
      pkt->dp_Res1 = 0;
      pkt->dp_Res2 = err;
      return;
   }

   if (leaf[0] == '\0')                          /* the directory itself */
   {
      lk = makeLock(0, 0, 0, dir, leafOf(dir));
      pkt->dp_Res1 = (LONG)lk;
      pkt->dp_Res2 = lk ? 0 : ERROR_NO_FREE_STORE;
      return;
   }

   {
      LONG r = findEntry(dir, leaf, &t, &idx, &size);
      if (r < 0)
         { pkt->dp_Res1 = 0; pkt->dp_Res2 = -r; return; }
      if (r == 0)
         { pkt->dp_Res1 = 0; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }
   }

   if (t == BLUESCSI_DIR)
   {
      char full[64];
      cCopy(full, dir, sizeof(full));
      if (catAppend(full, leaf, sizeof(full)) != 0)
         { pkt->dp_Res1 = 0; pkt->dp_Res2 = ERROR_INVALID_COMPONENT_NAME; return; }
      lk = makeLock(0, 0, 0, full, leaf);
   }
   else
   {
      lk = makeLock(1, idx, size, dir, leaf);
   }
   pkt->dp_Res1 = (LONG)lk;
   pkt->dp_Res2 = lk ? 0 : ERROR_NO_FREE_STORE;
}

static void doCopyDir(struct DosPacket *pkt)
{
   struct MyLock *src = (struct MyLock *)BADDR((BPTR)pkt->dp_Arg1);
   BPTR lk;
   if (!src) { pkt->dp_Res1 = 0; return; }       /* dup of root (0) -> root */
   lk = makeLock(src->isFile, src->index, src->fsize, src->dir, src->name);
   pkt->dp_Res1 = (LONG)lk;
   pkt->dp_Res2 = lk ? 0 : ERROR_NO_FREE_STORE;
}

static void doParent(struct DosPacket *pkt)
{
   struct MyLock *lk = (struct MyLock *)BADDR((BPTR)pkt->dp_Arg1);
   BPTR par = 0;

   pkt->dp_Res2 = 0;
   if (lk && lk->isFile)                          /* file -> its dir */
   {
      par = makeLock(0, 0, 0, lk->dir, leafOf(lk->dir));
      if (!par)
         pkt->dp_Res2 = ERROR_NO_FREE_STORE;
   }
   else if (lk && lk->dir[0])                     /* subdir -> parent dir */
   {
      char pdir[64];
      cCopy(pdir, lk->dir, sizeof(pdir));
      parentOf(pdir);
      par = makeLock(0, 0, 0, pdir, leafOf(pdir));
      if (!par)
         pkt->dp_Res2 = ERROR_NO_FREE_STORE;
   }
   /* root (or zero lock) -> no parent */
   pkt->dp_Res1 = (LONG)par;
}

static void fillDirFib(struct FileInfoBlock *fib, const char *name, LONG entrytype)
{
   fib->fib_DiskKey = 0;
   fib->fib_DirEntryType = entrytype;             /* 1 = root, 2 = user dir */
   fib->fib_EntryType = entrytype;
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
   else                                           /* a directory */
   {
      const char *nm = (lk && lk->name[0]) ? lk->name : "SHARED";
      fillDirFib(fib, nm, (lk && lk->dir[0]) ? 2 : 1);
      if (lk)
         lk->exNext = 0;                          /* reset directory scan */
      listDir(lk ? lk->dir : "");                 /* prime list for ExNext */
   }
   pkt->dp_Res1 = DOSTRUE;
   pkt->dp_Res2 = 0;
}

static void doExamineNext(struct DosPacket *pkt)
{
   struct MyLock *lk = (struct MyLock *)BADDR((BPTR)pkt->dp_Arg1);
   struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR((BPTR)pkt->dp_Arg2);
   LONG pos;

   if (!lk || lk->isFile || listDir(lk->dir) != 0)
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
      return;
   }

   pos = lk->exNext;
   while (pos < filecount &&
          !(files[pos].Type == BLUESCSI_FILE ||
            (gHaveDirs && files[pos].Type == BLUESCSI_DIR)))
      pos++;                                       /* skip unusable entries */

   if (pos >= filecount)
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES;
      return;
   }

   if (files[pos].Type == BLUESCSI_FILE)
      fillFileFib(fib, files[pos].Name, files[pos].Size);
   else
      fillDirFib(fib, files[pos].Name, 2);
   lk->exNext = pos + 1;
   pkt->dp_Res1 = DOSTRUE;
   pkt->dp_Res2 = 0;
}

static void doFindInput(struct DosPacket *pkt)
{
   struct FileHandle *fh = (struct FileHandle *)BADDR((BPTR)pkt->dp_Arg1);
   struct MyLock *base = (struct MyLock *)BADDR((BPTR)pkt->dp_Arg2);
   char dir[64];
   char leaf[40];
   LONG err, t, idx;
   ULONG size;
   struct MyFH *mfh;
   LONG r;

   if ((err = resolvePath(base, pkt->dp_Arg3, dir, leaf)) != 0)
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = err;
      return;
   }
   if (leaf[0] == '\0')
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
      return;
   }
   r = findEntry(dir, leaf, &t, &idx, &size);
   if (r <= 0)
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = (r < 0) ? -r : ERROR_OBJECT_NOT_FOUND;
      return;
   }
   if (t != BLUESCSI_FILE)
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
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
   cCopy(mfh->dir, dir, sizeof(mfh->dir));
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

   if (navTo(mfh->dir) != 0)                       /* GET_FILE indexes are per-dir */
   {
      pkt->dp_Res1 = -1;
      pkt->dp_Res2 = ERROR_DIR_NOT_FOUND;
      return;
   }
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
         gListedValid = 0;                         /* new file appears now */
      }
      FreeVec(mfh);
   }
   pkt->dp_Res1 = DOSTRUE;
   pkt->dp_Res2 = 0;
}

static void doFindOutput(struct DosPacket *pkt)
{
   struct FileHandle *fh = (struct FileHandle *)BADDR((BPTR)pkt->dp_Arg1);
   struct MyLock *base = (struct MyLock *)BADDR((BPTR)pkt->dp_Arg2);
   char dir[64];
   char leaf[40];
   LONG err, t, idx;
   ULONG size;
   struct MyFH *mfh;
   LONG r;

   if ((err = resolvePath(base, pkt->dp_Arg3, dir, leaf)) != 0)
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = err;
      return;
   }
   if (leaf[0] == '\0')                            /* can't create the root */
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
      return;
   }
   r = findEntry(dir, leaf, &t, &idx, &size);      /* also navigates to 'dir' */
   if (r < 0)
   {
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = -r;
      return;
   }
   if (r == 1 && t != BLUESCSI_FILE)               /* name taken by a subdir */
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
   gListedValid = 0;                               /* dir listing now stale */

   mfh = AllocVec(sizeof(struct MyFH), MEMF_CLEAR | MEMF_PUBLIC);
   if (!mfh)
   {
      Toolbox_Send_End();                          /* close the prepared file */
      pkt->dp_Res1 = DOSFALSE;
      pkt->dp_Res2 = ERROR_NO_FREE_STORE;
      return;
   }
   mfh->isWrite = 1;
   cCopy(mfh->dir, dir, sizeof(mfh->dir));
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

   /* Subdirectory support (firmware v2026.04.27+): learn the device's default
      shared dir so relative paths can be composed below it. */
   if (scsi_capabilities & BLUESCSI_TOOLBOX_CAP_SET_WORKING_DIR)
   {
      if (Toolbox_Get_Working_Dir(gBaseDir, sizeof(gBaseDir)) == 0 && gBaseDir[0])
      {
         gHaveDirs = 1;
         cCopy(gCurAbs, gBaseDir, sizeof(gCurAbs));
      }
   }

   /* Create a DLT_VOLUME node so Workbench shows SHARED: as a disk and locks
      have a real volume to point at. Falls back to the device node if it fails. */
   gVol = MakeDosEntry("SHARED", DLT_VOLUME);
   if (gVol)
   {
      gVol->dol_Task = &gProc->pr_MsgPort;
      DateStamp(&gVol->dol_misc.dol_volume.dol_VolumeDate);
      gVol->dol_misc.dol_volume.dol_DiskType = ID_SHARED_DISK;
      AddDosEntry(gVol);
      gVolBptr = MKBADDR(gVol);
   }
   else
   {
      gVolBptr = (BPTR)pkt->dp_Arg3;
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

         /* Firmware has no delete/rename/mkdir ops -> fail gracefully. */
         case ACTION_DELETE_OBJECT:
            pkt->dp_Res2 = ERROR_DELETE_PROTECTED;
            break;
         case ACTION_RENAME_OBJECT:
         case ACTION_CREATE_DIR:
            pkt->dp_Res2 = ERROR_WRITE_PROTECTED;
            break;

         /* No protection/comment/date storage either, but Copy clones these
            after writing and treats a failure as the whole copy failing
            ("file is write protected", verified on hardware 2026-08-06) -
            so accept and discard them like other metadata-less filesystems. */
         case ACTION_SET_PROTECT:
         case ACTION_SET_COMMENT:
         case ACTION_SET_DATE:
            pkt->dp_Res1 = DOSTRUE;
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

   if (gVol)                            /* remove the volume node on unmount */
   {
      LockDosList(LDF_VOLUMES | LDF_WRITE);
      RemDosEntry(gVol);
      UnLockDosList(LDF_VOLUMES | LDF_WRITE);
      FreeDosEntry(gVol);
      gVol = NULL;
   }
   if (gHaveDirs)
      Toolbox_Set_Working_Dir("");     /* leave the device at its default dir */
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

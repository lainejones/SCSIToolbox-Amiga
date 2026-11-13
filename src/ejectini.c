/*
 * ejectini.c - CDChanger: write the keep-ejected settings into bluescsi.ini
 * (SCSIToolbox-Amiga). See ejectfix.h.
 *
 * The ini also holds things like the Wi-Fi password, so it is only ever held
 * in memory here: never shown, never written to the Amiga's disks.
 *
 * Copyright (C) 2026 Laine Jones. GPL-3.0-or-later, see LICENSE.
 */
#include <string.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include "toolbox.h"
#include "ejectfix.h"

#define INI_NAME "bluescsi.ini"
#define BAK_NAME "bluescsi.ini.bak"
#define INI_MAX  16384

static int ieq(const char *a, const char *b)
{
   while (*a && *b)
   {
      char x = *a++, y = *b++;
      if (x >= 'A' && x <= 'Z') x += 32;
      if (y >= 'A' && y <= 'Z') y += 32;
      if (x != y) return 0;
   }
   return *a == *b;
}

static void setmsg(char *msg, int msglen, const char *s)
{
   int i;
   for (i = 0; i < msglen - 1 && s[i]; i++)
      msg[i] = s[i];
   if (msglen > 0)
      msg[i] = '\0';
}

/* Index of 'name' in the freshly listed folder, or -1 (size in *size). */
static int find_file(const char *name, ULONG *size)
{
   int i;
   if (!Toolbox_List_Files(0))
      return -1;
   for (i = 0; i < filecount; i++)
      if (files[i].Type == BLUESCSI_FILE && ieq(files[i].Name, name))
      {
         if (size) *size = files[i].Size;
         return files[i].Index;
      }
   return -1;
}

static LONG send_mem(const char *name, const UBYTE *buf, long len)
{
   long off = 0;
   if (Toolbox_Send_Prep(name) != 0)
      return -1;
   while (off < len)
   {
      int n = (len - off > 512) ? 512 : (int)(len - off);
      if (Toolbox_Send_Block(buf + off, n) != 0)
         return -1;
      off += n;
   }
   return Toolbox_Send_End() == 0 ? 0 : -1;
}

/* Read back 'name' and compare it with buf[0..len). 0 = identical. */
static int verify(const char *name, const UBYTE *buf, long len, UBYTE *tmp)
{
   ULONG size = 0;
   int idx = find_file(name, &size);
   if (idx < 0 || (long)size != len)
      return -1;
   Toolbox_Drop_Cache();
   if (Toolbox_Get_Bytes(idx, 0, tmp, (ULONG)len) != len)
      return -1;
   return memcmp(tmp, buf, (size_t)len) == 0 ? 0 : -1;
}

int ef_fix_bluescsi_ini(int id, int startEmpty, char *msg, int msglen)
{
   char oldwd[TOOLBOX_MAX_WD_PATH];
   UBYTE *in = NULL, *out = NULL, *chk = NULL;
   ULONG size = 0;
   long outlen;
   int idx, rc = -1;

   if (!scsi_isBlueSCSI || !(scsi_capabilities & BLUESCSI_TOOLBOX_CAP_SET_WORKING_DIR))
   {
      setmsg(msg, msglen, "This firmware can't reach bluescsi.ini from the Amiga.");
      return -1;
   }
   if (Toolbox_Get_Working_Dir(oldwd, sizeof(oldwd)) != 0)
      oldwd[0] = '\0';
   if (Toolbox_Set_Working_Dir("/") != 0)
   {
      setmsg(msg, msglen, "Could not open the SD card's top folder.");
      return -1;
   }

   in = AllocVec(INI_MAX, MEMF_ANY);
   out = AllocVec(INI_MAX + 512, MEMF_ANY);
   chk = AllocVec(INI_MAX + 512, MEMF_ANY);
   if (!in || !out || !chk)
   {
      setmsg(msg, msglen, "Not enough memory.");
      goto done;
   }

   idx = find_file(INI_NAME, &size);
   if (idx >= 0)
   {
      if (size > INI_MAX)
      {
         setmsg(msg, msglen, "bluescsi.ini is larger than 16 KB; edit it on a PC.");
         goto done;
      }
      Toolbox_Drop_Cache();
      if (Toolbox_Get_Bytes(idx, 0, in, size) != (LONG)size)
      {
         setmsg(msg, msglen, "Could not read bluescsi.ini.");
         goto done;
      }
      /* Keep the first original only: an older .bak is the real one. An
         upload over an existing file may not shorten it, so never rewrite
         a .bak that is there. */
      if (find_file(BAK_NAME, NULL) < 0)
      {
         if (send_mem(BAK_NAME, in, (long)size) != 0 || verify(BAK_NAME, in, (long)size, chk) != 0)
         {
            setmsg(msg, msglen, "Could not save bluescsi.ini.bak, so bluescsi.ini was left alone.");
            goto done;
         }
      }
   }
   else
      size = 0;                       /* no ini yet: write a new one */

   outlen = ef_patch_ini((const char *)in, (long)size, id, startEmpty, (char *)out, INI_MAX + 500);
   if (outlen < 0)
   {
      setmsg(msg, msglen, "bluescsi.ini is too large to change here.");
      goto done;
   }
   /* Some firmware writes over an existing file without shortening it, so
      never make the file shorter: pad with a comment line to the old size. */
   if (outlen < (long)size)
   {
      long pad = (long)size - outlen;
      long i;
      int crlf = (outlen > 1 && out[outlen - 2] == '\r');
      long body = pad - (crlf ? 2 : 1);
      if (body < 1)                    /* too little room for ";"; pad with newlines */
      {
         for (i = 0; i < pad; i++) out[outlen++] = '\n';
      }
      else
      {
         out[outlen++] = ';';
         for (i = 1; i < body; i++) out[outlen++] = ' ';
         if (crlf) out[outlen++] = '\r';
         out[outlen++] = '\n';
      }
   }

   if (send_mem(INI_NAME, out, outlen) != 0 || verify(INI_NAME, out, outlen, chk) != 0)
   {
      setmsg(msg, msglen, "Writing bluescsi.ini did not check out. The original is in bluescsi.ini.bak.");
      goto done;
   }
   setmsg(msg, msglen, "");
   rc = 0;

done:
   if (chk) FreeVec(chk);
   if (out) FreeVec(out);
   if (in) FreeVec(in);
   Toolbox_Set_Working_Dir(oldwd);   /* "" = the firmware's default folder */
   Toolbox_Drop_Cache();
   return rc;
}

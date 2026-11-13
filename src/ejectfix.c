/*
 * ejectfix.c - making an eject stick (SCSIToolbox-Amiga)
 * Shared by BlueSCSIToolbox (CLI) and CDChanger. See ejectfix.h.
 *
 * Copyright (C) 2026 Laine Jones. GPL-3.0-or-later, see LICENSE.
 */
#include "ejectfix.h"

#ifndef EF_HOST_TEST
#include <proto/exec.h>
#include <proto/dos.h>
#include <devices/scsidisk.h>

/* Provided by scsi.c (CDChanger) and by BlueSCSIToolbox.c (CLI) alike */
extern struct SCSICmd *scsi_cmd;
extern UBYTE *scsi_sense;
extern UBYTE *scsi_data;
int DoScsiCmd(UBYTE *data, int datasize, UBYTE *cmd, int cmdsize, UBYTE flags);

int ef_medium_present(void)
{
   UBYTE tur[6] = {0, 0, 0, 0, 0, 0};
   int tries;

   for (tries = 0; tries < 3; tries++)
   {
      int err = DoScsiCmd(scsi_data, 0, tur, 6, (UBYTE)(SCSIF_READ | SCSIF_AUTOSENSE));
      UBYTE key, asc;

      if (err == 0 && scsi_cmd->scsi_Status == 0)
         return 1;
      if (scsi_cmd->scsi_SenseActual < 13)
         return -1;                        /* a CHECK CONDITION without sense data */
      key = scsi_sense[2] & 0x0F;
      asc = scsi_sense[12];
      if (key == 0x02 && asc == 0x3A)      /* NOT READY, MEDIUM NOT PRESENT */
         return 0;
      if (key != 0x06)                     /* anything but UNIT ATTENTION: unknown */
         return -1;
      /* UNIT ATTENTION (medium changed, reset): ask again */
   }
   return -1;
}

int ef_eject_held(void)
{
   int i, r = -1;

   /* Both firmwares reinsert on the next status poll rather than at once, so
      poll a few times, as the CD file system would, and take the last answer. */
   for (i = 0; i < 3; i++)
   {
      Delay(25);
      r = ef_medium_present();
   }
   if (r < 0)
      return -1;
   return r == 0;
}
#endif /* EF_HOST_TEST */

static const char *const efKeys[3] = {"ReinsertAfterEject", "ReinsertCDOnInquiry", "StartEjected"};
static const char *const efVals[3] = {"0", "0", "1"};
#define EF_NKEYS(se) ((se) ? 3 : 2)         /* StartEjected only when asked for (BlueSCSI only) */

static int ef_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ef_nieq(const char *a, const char *b, long n)
{
   long i;
   for (i = 0; i < n; i++)
      if (ef_lower((unsigned char)a[i]) != ef_lower((unsigned char)b[i]))
         return 0;
   return 1;
}

static long ef_len(const char *s) { long n = 0; while (s[n]) n++; return n; }

static char *ef_put(char *p, const char *s)
{
   while (*s)
      *p++ = *s++;
   return p;
}

static char *ef_putnum(char *p, int n)
{
   char t[12];
   int i = 0;
   if (n == 0) t[i++] = '0';
   while (n > 0) { t[i++] = (char)('0' + n % 10); n /= 10; }
   while (i > 0) *p++ = t[--i];
   return p;
}

char *ef_ini_lines(char *buf, int buflen, int id, int startEmpty)
{
   char tmp[160];
   char *p = tmp;
   int k, i;

   p = ef_put(p, "[SCSI");
   p = ef_putnum(p, id);
   p = ef_put(p, "]\n");
   for (k = 0; k < EF_NKEYS(startEmpty); k++)
   {
      p = ef_put(p, efKeys[k]);
      p = ef_put(p, "=");
      p = ef_put(p, efVals[k]);
      p = ef_put(p, "\n");
   }
   *p = '\0';
   for (i = 0; i < buflen - 1 && tmp[i]; i++)
      buf[i] = tmp[i];
   if (buflen > 0)
      buf[i] = '\0';
   return buf;
}

/* Is this line the header of section [SCSI<id>]? */
static int ef_is_section(const char *l, long n, int id, int *isHeader)
{
   char want[16];
   char *w;
   long i = 0, wl;

   while (i < n && (l[i] == ' ' || l[i] == '\t'))
      i++;
   *isHeader = (i < n && l[i] == '[');
   if (!*isHeader)
      return 0;
   w = ef_put(want, "[SCSI");
   w = ef_putnum(w, id);
   w = ef_put(w, "]");
   *w = '\0';
   wl = ef_len(want);
   return (n - i >= wl) && ef_nieq(l + i, want, wl);
}

/* Is this line a setting of one of our keys (not a comment)? */
static int ef_is_our_key(const char *l, long n, int startEmpty)
{
   long i = 0, e;
   int k;

   while (i < n && (l[i] == ' ' || l[i] == '\t'))
      i++;
   for (e = i; e < n && l[e] != '=' && l[e] != ' ' && l[e] != '\t'; e++)
      ;
   for (k = 0; k < EF_NKEYS(startEmpty); k++)  /* only the keys we write */
   {
      long kl = ef_len(efKeys[k]);
      if (e - i == kl && ef_nieq(l + i, efKeys[k], kl))
         return 1;
   }
   return 0;
}

long ef_patch_ini(const char *in, long inlen, int id, int startEmpty, char *out, long outmax)
{
   const char *nl = "\n";
   char *o = out;
   char *const end = out + outmax;
   long pos = 0, i;
   int inTarget = 0, done = 0, k;

   for (i = 0; i + 1 < inlen; i++)
      if (in[i] == '\r' && in[i + 1] == '\n') { nl = "\r\n"; break; }

   if (inlen + 64 + 40 * 3 > outmax)
      return -1;

   while (pos < inlen)
   {
      long s = pos, n, full;
      int header;

      while (pos < inlen && in[pos] != '\n')
         pos++;
      n = pos - s;                     /* line without its '\n' */
      if (pos < inlen)
         pos++;                        /* the '\n' */
      full = pos - s;
      if (n > 0 && in[s + n - 1] == '\r')
         n--;

      if (ef_is_section(in + s, n, id, &header))
      {
         for (k = 0; k < full; k++) *o++ = in[s + k];
         if (in[s + full - 1] != '\n')   /* header was the last line, no newline */
            o = ef_put(o, nl);
         for (k = 0; k < EF_NKEYS(startEmpty); k++)
         {
            o = ef_put(o, efKeys[k]);
            o = ef_put(o, "=");
            o = ef_put(o, efVals[k]);
            o = ef_put(o, nl);
         }
         inTarget = 1;
         done = 1;
         continue;
      }
      if (header)
         inTarget = 0;
      if (inTarget && ef_is_our_key(in + s, n, startEmpty))
         continue;                     /* replaced by ours above */
      for (k = 0; k < full; k++) *o++ = in[s + k];
   }

   if (!done)
   {
      if (inlen > 0 && in[inlen - 1] != '\n')
         o = ef_put(o, nl);
      if (inlen > 0)
         o = ef_put(o, nl);            /* a blank line before the new section */
      o = ef_put(o, "[SCSI");
      o = ef_putnum(o, id);
      o = ef_put(o, "]");
      o = ef_put(o, nl);
      for (k = 0; k < EF_NKEYS(startEmpty); k++)
      {
         o = ef_put(o, efKeys[k]);
         o = ef_put(o, "=");
         o = ef_put(o, efVals[k]);
         o = ef_put(o, nl);
      }
   }
   if (o > end)
      return -1;
   return (long)(o - out);
}

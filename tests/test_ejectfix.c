/* Host test for ef_patch_ini (ejectfix.c). Build:
   gcc -DEF_HOST_TEST -I tests -I src -o /tmp/tef tests/test_ejectfix.c src/ejectfix.c */
#include <stdio.h>
#include <string.h>
#include "ejectfix.h"

static int fails = 0;

static void check(const char *name, const char *in, int id, int zulu, const char *want)
{
   char out[2048];
   long n = ef_patch_ini(in, (long)strlen(in), id, zulu, out, sizeof(out) - 1);
   if (n < 0) { printf("FAIL %s: returned %ld\n", name, n); fails++; return; }
   out[n] = 0;
   if (strcmp(out, want) != 0)
   {
      printf("FAIL %s\n--- got ---\n%s\n--- want ---\n%s\n", name, out, want);
      fails++;
   }
   else
      printf("ok   %s\n", name);
}

int main(void)
{
   check("empty file", "", 2, 1,
         "[SCSI2]\nReinsertAfterEject=0\nReinsertCDOnInquiry=0\nStartEjected=1\n");

   check("append section, keep others",
         "[SCSI]\nWiFiSSID=x\nWiFiPassword=secret\n", 3, 1,
         "[SCSI]\nWiFiSSID=x\nWiFiPassword=secret\n\n[SCSI3]\nReinsertAfterEject=0\nReinsertCDOnInquiry=0\nStartEjected=1\n");

   check("append, no trailing newline", "[SCSI]\nDebug=0", 1, 0,
         "[SCSI]\nDebug=0\n\n[SCSI1]\nReinsertAfterEject=0\nReinsertCDOnInquiry=0\n");

   check("existing section, replace keys, keep others",
         "[SCSI]\nDebug=1\n[scsi2]\nType=2\nreinsertafterEject = 1\nStartEjected=0\n; StartEjected=0 comment\n[SCSI3]\nStartEjected=0\n", 2, 1,
         "[SCSI]\nDebug=1\n[scsi2]\nReinsertAfterEject=0\nReinsertCDOnInquiry=0\nStartEjected=1\nType=2\n; StartEjected=0 comment\n[SCSI3]\nStartEjected=0\n");

   check("CRLF preserved",
         "[SCSI]\r\nDebug=0\r\n[SCSI2]\r\nReinsertCDOnInquiry=1\r\n", 2, 0,
         "[SCSI]\r\nDebug=0\r\n[SCSI2]\r\nReinsertAfterEject=0\r\nReinsertCDOnInquiry=0\r\n");

   check("header last line without newline", "[SCSI]\n[SCSI4]", 4, 0,
         "[SCSI]\n[SCSI4]\nReinsertAfterEject=0\nReinsertCDOnInquiry=0\n");

   check("SCSI20 is not SCSI2", "[SCSI20]\nA=1\n", 2, 0,
         "[SCSI20]\nA=1\n\n[SCSI2]\nReinsertAfterEject=0\nReinsertCDOnInquiry=0\n");

   check("idempotent",
         "[SCSI2]\nReinsertAfterEject=0\nReinsertCDOnInquiry=0\nStartEjected=1\n", 2, 1,
         "[SCSI2]\nReinsertAfterEject=0\nReinsertCDOnInquiry=0\nStartEjected=1\n");

   {
      char out[50];
      long n = ef_patch_ini("[SCSI]\n", 7, 2, 1, out, sizeof(out));
      if (n != -1) { printf("FAIL small buffer: %ld\n", n); fails++; } else printf("ok   small buffer\n");
   }
   {
      char b[200];
      ef_ini_lines(b, sizeof(b), 5, 1);
      if (strcmp(b, "[SCSI5]\nReinsertAfterEject=0\nReinsertCDOnInquiry=0\nStartEjected=1\n"))
      { printf("FAIL ini_lines: %s\n", b); fails++; } else printf("ok   ini_lines\n");
   }
   check("no start-empty keeps a StartEjected that is there",
         "[SCSI2]\nStartEjected=1\nReinsertAfterEject=1\n", 2, 0,
         "[SCSI2]\nReinsertAfterEject=0\nReinsertCDOnInquiry=0\nStartEjected=1\n");
   printf(fails ? "%d FAILED\n" : "all passed\n", fails);
   return fails != 0;
}

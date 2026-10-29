/*
 * Copyright (C) 2026 Laine Jones (lainejones)
 *
 * Part of SCSIToolbox-Amiga. Shared GadTools user interface for CDChanger and
 * SDTransfer (see gtui.h). Layout, top to bottom: logo in a recessed box, an
 * info line, a list that takes the spare height, a row of buttons, and an
 * optional status line that doubles as a progress bar. The window can be
 * resized; the gadgets are rebuilt to fit.
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
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/icon.h>
#include "gtui.h"

/* = NULL: strong definitions, so the startup code does not try to open these */
struct Library *GadToolsBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Library *IconBase = NULL;
extern struct IntuitionBase *IntuitionBase;

void MessageBox(char *title, char *body);   /* common.c */

#define GAP      4
#define LIST_ID  100

static WORD fontHeight(struct GtUI *ui)
{
   return (WORD)ui->win->RPort->TxHeight;
}

/* Interior heights of the fixed parts */
static WORD buttonHeight(struct GtUI *ui) { return fontHeight(ui) + 6; }
static WORD statusHeight(struct GtUI *ui) { return fontHeight(ui) + 6; }
static WORD logoBoxHeight(void)           { return GTUI_LOGO_H + 4; }

static void computeLimits(struct GtUI *ui)
{
   struct Window *w = ui->win;
   WORD fh = fontHeight(ui);
   WORD listMin = 4 * (fh + 1) + 8;          /* a few rows (+ V37's selection line) */

   ui->minW = w->BorderLeft + w->BorderRight + 2 * GAP + GTUI_LOGO_W + 4;
   ui->minH = w->BorderTop + GAP + logoBoxHeight() + GAP + fh + GAP + listMin
              + GAP + buttonHeight(ui)
              + (ui->hasStatus ? GAP + statusHeight(ui) : 0)
              + GAP + w->BorderBottom;
}

static void drawStatus(struct GtUI *ui)
{
   struct RastPort *rp = ui->win->RPort;
   UWORD *pens = ui->dri->dri_Pens;
   WORD x1, y1, x2, y2, fill = 0;
   char pct[8];
   const char *text = ui->status;
   struct TextExtent te;
   ULONG len, fit;

   if (!ui->hasStatus || ui->statW <= 0)
      return;

   DrawBevelBox(rp, ui->statX, ui->statY, ui->statW, ui->statH,
                GT_VisualInfo, (ULONG)ui->vi, GTBB_Recessed, TRUE, TAG_DONE);

   x1 = ui->statX + 2;
   y1 = ui->statY + 1;
   x2 = ui->statX + ui->statW - 3;
   y2 = ui->statY + ui->statH - 2;

   if (ui->level >= 0)
   {
      fill = (WORD)(((LONG)(x2 - x1 + 1) * ui->level) / 100);
      sprintf(pct, "%ld%%", ui->level);
      text = pct;
   }
   if (fill > 0)
   {
      SetAPen(rp, pens[FILLPEN]);
      RectFill(rp, x1, y1, x1 + fill - 1, y2);
   }
   if (x1 + fill <= x2)
   {
      SetAPen(rp, pens[BACKGROUNDPEN]);
      RectFill(rp, x1 + fill, y1, x2, y2);
   }

   len = strlen(text);
   if (len)
   {
      fit = TextFit(rp, (STRPTR)text, len, &te, NULL, 1, x2 - x1 - 3, y2 - y1 + 1);
      if (fit)
      {
         WORD tw = TextLength(rp, (STRPTR)text, fit);
         WORD tx = (ui->level >= 0) ? x1 + (x2 - x1 + 1 - tw) / 2 : x1 + 2;
         SetAPen(rp, pens[TEXTPEN]);
         SetDrMd(rp, JAM1);
         Move(rp, tx, y1 + (y2 - y1 + 1 - rp->TxHeight) / 2 + rp->TxBaseline);
         Text(rp, (STRPTR)text, fit);
      }
   }
}

static void drawDecor(struct GtUI *ui)
{
   struct RastPort *rp = ui->win->RPort;
   UWORD *pens = ui->dri->dri_Pens;

   DrawBevelBox(rp, ui->logoX, ui->logoY, ui->logoW, ui->logoH,
                GT_VisualInfo, (ULONG)ui->vi, GTBB_Recessed, TRUE, TAG_DONE);
   if (ui->logoChip)
   {
      WORD x = ui->logoX + (ui->logoW - GTUI_LOGO_W) / 2;
      DrawImage(rp, &ui->logo, x, ui->logoY + 2);
   }

   if (ui->info && ui->info[0])
   {
      struct TextExtent te;
      ULONG len = strlen(ui->info);
      ULONG fit = TextFit(rp, (STRPTR)ui->info, len, &te, NULL, 1,
                          ui->logoW, rp->TxHeight);
      SetAPen(rp, pens[TEXTPEN]);
      SetBPen(rp, pens[BACKGROUNDPEN]);
      SetDrMd(rp, JAM2);
      Move(rp, ui->logoX, ui->infoY + rp->TxBaseline);
      Text(rp, (STRPTR)ui->info, fit);
   }
   drawStatus(ui);
}

/* Create the gadgets for the window's current size and draw everything. */
static BOOL build(struct GtUI *ui)
{
   struct Window *w = ui->win;
   struct NewGadget ng;
   struct Gadget *g;
   WORD fh = fontHeight(ui);
   WORD left = w->BorderLeft + GAP;
   WORD width = w->Width - w->BorderLeft - w->BorderRight - 2 * GAP;
   WORD bottom = w->Height - w->BorderBottom - GAP;  /* first row below the content */
   WORD listTop, buttonTop, bw;
   int i;

   ui->logoX = left;
   ui->logoY = w->BorderTop + GAP;
   ui->logoW = width;
   ui->logoH = logoBoxHeight();
   ui->infoY = ui->logoY + ui->logoH + GAP;
   listTop = ui->infoY + fh + GAP;

   if (ui->hasStatus)
   {
      ui->statX = left;
      ui->statW = width;
      ui->statH = statusHeight(ui);
      ui->statY = bottom - ui->statH;
      bottom = ui->statY - GAP;
   }
   buttonTop = bottom - buttonHeight(ui);

   g = CreateContext(&ui->glist);

   memset(&ng, 0, sizeof(ng));
   ng.ng_TextAttr = ui->scr->Font;
   ng.ng_VisualInfo = ui->vi;
   ng.ng_LeftEdge = left;
   ng.ng_TopEdge = listTop;
   ng.ng_Width = width;
   ng.ng_Height = buttonTop - GAP - listTop;
   ng.ng_GadgetID = LIST_ID;
   ui->lv = g = CreateGadget(LISTVIEW_KIND, g, &ng,
                             GTLV_Labels, (ULONG)ui->labels,
                             GTLV_ShowSelected, 0,
                             GTLV_Selected, ui->selected >= 0 ? (ULONG)ui->selected : ~0UL,
                             GTLV_Top, ui->selected >= 0 ? (ULONG)ui->selected : 0,
                             TAG_DONE);

   bw = (width - GAP * (ui->nButtons - 1)) / (ui->nButtons > 0 ? ui->nButtons : 1);
   for (i = 0; i < ui->nButtons; i++)
   {
      ng.ng_LeftEdge = left + i * (bw + GAP);
      ng.ng_TopEdge = buttonTop;
      ng.ng_Width = (i == ui->nButtons - 1) ? left + width - ng.ng_LeftEdge : bw;
      ng.ng_Height = buttonHeight(ui);
      ng.ng_GadgetText = (UBYTE *)ui->buttonText[i];
      ng.ng_Flags = PLACETEXT_IN;
      ng.ng_GadgetID = i;
      g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);
   }

   if (!g)
   {
      FreeGadgets(ui->glist);
      ui->glist = NULL;
      ui->lv = NULL;
      return FALSE;
   }

   AddGList(w, ui->glist, (UWORD)~0, -1, NULL);
   RefreshGList(ui->glist, w, NULL, -1);
   GT_RefreshWindow(w, NULL);
   drawDecor(ui);
   return TRUE;
}

/* After a resize: throw the gadgets away, clear the inside, build again. */
static void relayout(struct GtUI *ui)
{
   struct Window *w = ui->win;

   if (ui->glist)
   {
      RemoveGList(w, ui->glist, -1);
      FreeGadgets(ui->glist);
      ui->glist = NULL;
      ui->lv = NULL;
   }
   SetAPen(w->RPort, ui->dri->dri_Pens[BACKGROUNDPEN]);
   RectFill(w->RPort, w->BorderLeft, w->BorderTop,
            w->Width - w->BorderRight - 1, w->Height - w->BorderBottom - 1);
   RefreshWindowFrame(w);
   build(ui);
}

BOOL gtui_Open(struct GtUI *ui, WORD width, WORD height)
{
   struct Screen *scr;
   WORD left, top;
   int i;

   ui->win = NULL;
   ui->glist = ui->lv = NULL;
   ui->selected = ui->lastRow = -1;
   ui->level = -1;
   ui->status[0] = '\0';

   if (!(GadToolsBase = OpenLibrary("gadtools.library", 37)) ||
       !(GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37)))
      return FALSE;
   if (!(ui->scr = scr = LockPubScreen(NULL)))
      return FALSE;
   if (!(ui->vi = GetVisualInfo(scr, TAG_DONE)) ||
       !(ui->dri = GetScreenDrawInfo(scr)))
      return FALSE;

   /* The logo must be in chip RAM for the blitter */
   if (ui->logoData &&
       (ui->logoChip = AllocVec(RASSIZE(GTUI_LOGO_W, GTUI_LOGO_H), MEMF_CHIP)))
   {
      CopyMem((APTR)ui->logoData, ui->logoChip, RASSIZE(GTUI_LOGO_W, GTUI_LOGO_H));
      ui->logo.LeftEdge = ui->logo.TopEdge = 0;
      ui->logo.Width = GTUI_LOGO_W;
      ui->logo.Height = GTUI_LOGO_H;
      ui->logo.Depth = 1;
      ui->logo.ImageData = ui->logoChip;
      ui->logo.PlanePick = 0x01;
      ui->logo.PlaneOnOff = 0x00;
      ui->logo.NextImage = NULL;
   }

   /* Keyboard shortcuts: the letter after '_' in each button */
   for (i = 0; i < ui->nButtons; i++)
   {
      char *u = strchr(ui->buttonText[i], '_');
      ui->keys[i] = (u && u[1]) ? (UBYTE)(u[1] | 0x20) : 0;
   }

   /* Centred under the mouse, kept on the screen */
   if (width > scr->Width) width = scr->Width;
   if (height > scr->Height) height = scr->Height;
   left = scr->MouseX - width / 2;
   top = scr->MouseY - height / 2;
   if (left + width > scr->Width) left = scr->Width - width;
   if (top + height > scr->Height) top = scr->Height - height;
   if (left < 0) left = 0;
   if (top < 0) top = 0;

   ui->win = OpenWindowTags(NULL,
                            WA_Left, left, WA_Top, top,
                            WA_Width, width, WA_Height, height,
                            WA_Title, (ULONG)ui->title,
                            WA_PubScreen, (ULONG)scr,
                            WA_Activate, TRUE,
                            WA_DragBar, TRUE,
                            WA_DepthGadget, TRUE,
                            WA_CloseGadget, TRUE,
                            WA_SizeGadget, TRUE,
                            WA_SizeBBottom, TRUE,
                            WA_SmartRefresh, TRUE,
                            WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_NEWSIZE |
                                      IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY |
                                      LISTVIEWIDCMP | BUTTONIDCMP,
                            TAG_DONE);
   if (!ui->win)
      return FALSE;

   computeLimits(ui);
   WindowLimits(ui->win, ui->minW, ui->minH, (UWORD)~0, (UWORD)~0);
   if (ui->win->Width < ui->minW || ui->win->Height < ui->minH)
   {
      /* Bigger font than expected: grow; the NEWSIZE that follows builds */
      ChangeWindowBox(ui->win, ui->win->LeftEdge, ui->win->TopEdge,
                      ui->win->Width < ui->minW ? ui->minW : ui->win->Width,
                      ui->win->Height < ui->minH ? ui->minH : ui->win->Height);
      return TRUE;
   }
   return build(ui);
}

void gtui_Close(struct GtUI *ui)
{
   if (ui->win)
   {
      CloseWindow(ui->win);
      ui->win = NULL;
   }
   if (ui->glist)
   {
      FreeGadgets(ui->glist);           /* after the window is gone */
      ui->glist = NULL;
   }
   if (ui->logoChip)
   {
      FreeVec(ui->logoChip);
      ui->logoChip = NULL;
   }
   if (ui->dri)
   {
      FreeScreenDrawInfo(ui->scr, ui->dri);
      ui->dri = NULL;
   }
   if (ui->vi)
   {
      FreeVisualInfo(ui->vi);
      ui->vi = NULL;
   }
   if (ui->scr)
   {
      UnlockPubScreen(NULL, ui->scr);
      ui->scr = NULL;
   }
   if (GfxBase)
   {
      CloseLibrary((struct Library *)GfxBase);
      GfxBase = NULL;
   }
   if (GadToolsBase)
   {
      CloseLibrary(GadToolsBase);
      GadToolsBase = NULL;
   }
}

int gtui_Wait(struct GtUI *ui, LONG *arg)
{
   struct Window *w = ui->win;
   struct IntuiMessage *m;

   for (;;)
   {
      while ((m = GT_GetIMsg(w->UserPort)))
      {
         ULONG cls = m->Class;
         UWORD code = m->Code;
         ULONG secs = m->Seconds, micros = m->Micros;
         UWORD id = 0xffff;
         int i;

         if (cls == IDCMP_GADGETUP && m->IAddress)
            id = ((struct Gadget *)m->IAddress)->GadgetID;
         GT_ReplyIMsg(m);

         switch (cls)
         {
            case IDCMP_CLOSEWINDOW:
               return GTUI_QUIT;

            case IDCMP_NEWSIZE:
               relayout(ui);
               break;

            case IDCMP_REFRESHWINDOW:
               GT_BeginRefresh(w);
               drawDecor(ui);
               GT_EndRefresh(w, TRUE);
               break;

            case IDCMP_VANILLAKEY:
               if (code == 27)
                  return GTUI_QUIT;
               for (i = 0; i < ui->nButtons; i++)
               {
                  if (ui->keys[i] && (UBYTE)(code | 0x20) == ui->keys[i])
                  {
                     *arg = i;
                     return GTUI_BUTTON;
                  }
               }
               break;

            case IDCMP_GADGETUP:
               if (id == LIST_ID)
               {
                  BOOL dbl = ((LONG)code == ui->lastRow &&
                              DoubleClick(ui->lastSecs, ui->lastMicros, secs, micros));
                  ui->selected = code;
                  ui->lastRow = dbl ? -1 : (LONG)code;   /* a third click starts over */
                  ui->lastSecs = secs;
                  ui->lastMicros = micros;
                  *arg = code;
                  return dbl ? GTUI_OPEN : GTUI_SELECT;
               }
               if (id < ui->nButtons)
               {
                  *arg = id;
                  return GTUI_BUTTON;
               }
               break;
         }
      }

      if (Wait((1L << w->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
         return GTUI_QUIT;
   }
}

void gtui_DetachList(struct GtUI *ui)
{
   if (ui->lv)
      GT_SetGadgetAttrs(ui->lv, ui->win, NULL, GTLV_Labels, ~0UL, TAG_DONE);
}

void gtui_AttachList(struct GtUI *ui, struct List *labels, LONG selected)
{
   ui->labels = labels;
   ui->selected = selected;
   ui->lastRow = -1;
   if (ui->lv)
      GT_SetGadgetAttrs(ui->lv, ui->win, NULL,
                        GTLV_Labels, (ULONG)labels,
                        GTLV_Selected, selected >= 0 ? (ULONG)selected : ~0UL,
                        GTLV_Top, selected >= 0 ? (ULONG)selected : 0,
                        TAG_DONE);
}

struct Node *gtui_NodeAt(struct List *list, LONG row)
{
   struct Node *n;

   if (row < 0)
      return NULL;
   for (n = list->lh_Head; n->ln_Succ; n = n->ln_Succ)
      if (row-- == 0)
         return n;
   return NULL;
}

void gtui_SetStatus(struct GtUI *ui, const char *text, LONG level)
{
   if (text)
   {
      strncpy(ui->status, text, GTUI_STATUSLEN - 1);
      ui->status[GTUI_STATUSLEN - 1] = '\0';
   }
   ui->level = level > 100 ? 100 : level;
   if (ui->win && ui->glist)
      drawStatus(ui);
}

void gtui_Busy(struct GtUI *ui, BOOL busy)
{
   /* The standard busy pointer exists from V39 (OS 3.0); 2.x just keeps the arrow */
   if (ui->win && IntuitionBase->LibNode.lib_Version >= 39)
   {
      if (busy)
         SetWindowPointer(ui->win, WA_BusyPointer, TRUE, TAG_DONE);
      else
         SetWindowPointer(ui->win, TAG_DONE);
   }
}

BOOL gtui_GetArgs(int argc, char **argv, char *appname,
                  char *dev, int devsize, LONG *unit)
{
   dev[0] = '\0';
   *unit = -1;

   if (argc > 0)
   {
      /* Shell: DEVICE=<driver> UNIT=<n> */
      LONG params[2] = {0, 0};
      struct RDArgs *rd = ReadArgs("DEVICE/K,UNIT/K/N", params, NULL);

      if (!rd)
      {
         PrintFault(IoErr(), argv[0]);
         return FALSE;
      }
      if (params[0])
      {
         strncpy(dev, (char *)params[0], devsize - 1);
         dev[devsize - 1] = '\0';
      }
      if (params[1])
         *unit = *(LONG *)params[1];
      FreeArgs(rd);

      if (dev[0] == '\0' || *unit < 0)
      {
         PrintFault(ERROR_REQUIRED_ARG_MISSING, argv[0]);
         return FALSE;
      }
      return TRUE;
   }

   /* Workbench: argv[0] is the WBStartup message with this toolchain's startup */
   if (argv && argv[0] && (IconBase = OpenLibrary("icon.library", 37)))
   {
      struct WBStartup *wbmsg = (struct WBStartup *)argv[0];
      struct WBArg *wbarg = wbmsg->sm_ArgList;
      LONG i;

      for (i = 0; wbarg && i < wbmsg->sm_NumArgs; i++, wbarg++)
      {
         struct DiskObject *dobj;
         BPTR oldlock;

         if (!wbarg->wa_Name || !wbarg->wa_Name[0])
            continue;
         oldlock = CurrentDir(wbarg->wa_Lock);
         dobj = GetDiskObject(wbarg->wa_Name);
         CurrentDir(oldlock);
         if (dobj)
         {
            STRPTR s;
            if ((s = FindToolType((CONST_STRPTR *)dobj->do_ToolTypes, "DEVICE")))
            {
               strncpy(dev, (char *)s, devsize - 1);
               dev[devsize - 1] = '\0';
            }
            if ((s = FindToolType((CONST_STRPTR *)dobj->do_ToolTypes, "UNIT")))
               StrToLong(s, unit);
            FreeDiskObject(dobj);
            break;
         }
      }
      CloseLibrary(IconBase);
      IconBase = NULL;
   }

   if (dev[0] == '\0' || *unit < 0)
   {
      char msg[128];
      sprintf(msg, "Missing SCSI device or unit in the Tool Types\nDEVICE=%.40s\nUNIT=%ld",
              dev[0] ? dev : "(empty)", *unit);
      MessageBox(appname, msg);
      return FALSE;
   }
   return TRUE;
}

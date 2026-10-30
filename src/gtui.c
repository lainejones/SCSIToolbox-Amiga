/*
 * Copyright (C) 2026 Laine Jones (lainejones)
 *
 * Part of SCSIToolbox-Amiga. Shared GadTools user interface for CDChanger and
 * SDTransfer (see gtui.h). Layout, top to bottom: logo in a recessed box, an
 * info line, a list that takes the spare height, a row of buttons, and an
 * optional status line that doubles as a progress bar. The window can be
 * resized; the gadgets are rebuilt to fit.
 *
 * Window rules: the smallest layout fits a 640x200 NTSC screen with Topaz 8
 * (screen title bar and window borders included). The logo is only shown when
 * the window is tall enough to keep a useful list under it, so on a 200-line
 * screen it is left out and the info line names the device instead. When the
 * screen font is so big that even the smallest layout would not fit, the
 * window uses Topaz 8. The default size is clamped to the screen (below its
 * title bar) and to 600 pixels wide.
 *
 * 2026-10-30: menus (right mouse button), snapshot place/size, settings pages
 * (via amigui.c) and the ENV: settings file reader.
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
#include "amigui.h"

/* = NULL: strong definitions, so the startup code does not try to open these */
struct Library *GadToolsBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Library *IconBase = NULL;
extern struct IntuitionBase *IntuitionBase;

void MessageBox(char *title, char *body);   /* common.c */

#define GAP        4
#define LIST_ID    100
#define MAXDEFW    600      /* default width cap, whatever the screen */
#define BORDER_B   11       /* bottom border (size gadget) before the window exists */

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };

static WORD fontHeight(struct GtUI *ui)
{
   return (WORD)ui->font->tf_YSize;
}

/* Interior heights of the fixed parts */
static WORD buttonHeight(struct GtUI *ui) { return fontHeight(ui) + 6; }
static WORD statusHeight(struct GtUI *ui) { return fontHeight(ui) + 6; }
static WORD logoBoxHeight(void)           { return GTUI_LOGO_H + 4; }
static WORD listMinHeight(struct GtUI *ui){ return 4 * (fontHeight(ui) + 1) + 8; }
/* The logo is shown only when at least this much list height is left under it */
static WORD listLogoHeight(struct GtUI *ui){ return 6 * (fontHeight(ui) + 1) + 4; }

/* Interior height of everything but the list (and the logo) */
static WORD fixedHeight(struct GtUI *ui)
{
   return 4 * GAP + fontHeight(ui) + buttonHeight(ui)
          + (ui->hasStatus ? GAP + statusHeight(ui) : 0);
}

/* Interior width the buttons need, measured in rp's font */
static WORD buttonsWidth(struct GtUI *ui, struct RastPort *rp)
{
   WORD w = 0;
   int i;

   for (i = 0; i < ui->nButtons; i++)
   {
      char t[48];
      const char *s;
      int n = 0;

      for (s = ui->buttonText[i]; *s && n < (int)sizeof(t) - 1; s++)
         if (*s != '_')
            t[n++] = *s;
      w += TextLength(rp, (STRPTR)t, n) + 16 + (i ? GAP : 0);
   }
   return w;
}

static WORD innerMinWidth(struct GtUI *ui, struct RastPort *rp)
{
   WORD w = GTUI_LOGO_W + 4, b = buttonsWidth(ui, rp);
   return (WORD)(2 * GAP + (b > w ? b : w));
}

static void computeLimits(struct GtUI *ui)
{
   struct Window *w = ui->win;

   ui->minW = w->BorderLeft + w->BorderRight + innerMinWidth(ui, w->RPort);
   ui->minH = w->BorderTop + fixedHeight(ui) + listMinHeight(ui) + w->BorderBottom;
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
   char *info = ui->info;

   if (ui->showLogo)
   {
      DrawBevelBox(rp, ui->logoX, ui->logoY, ui->logoW, ui->logoH,
                   GT_VisualInfo, (ULONG)ui->vi, GTBB_Recessed, TRUE, TAG_DONE);
      if (ui->logoChip)
      {
         WORD x = ui->logoX + (ui->logoW - GTUI_LOGO_W) / 2;
         DrawImage(rp, &ui->logo, x, ui->logoY + 2);
      }
   }
   else if (ui->infoAlt)
      info = ui->infoAlt;

   if (info && info[0])
   {
      struct TextExtent te;
      ULONG len = strlen(info);
      ULONG fit = TextFit(rp, (STRPTR)info, len, &te, NULL, 1,
                          ui->logoW, rp->TxHeight);
      SetAPen(rp, pens[TEXTPEN]);
      SetBPen(rp, pens[BACKGROUNDPEN]);
      SetDrMd(rp, JAM2);
      Move(rp, ui->logoX, ui->infoY + rp->TxBaseline);
      Text(rp, (STRPTR)info, fit);
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
   WORD inner = w->Height - w->BorderTop - w->BorderBottom;
   WORD bottom = w->Height - w->BorderBottom - GAP;  /* first row below the content */
   WORD listTop, buttonTop, bw;
   int i;

   ui->showLogo = (inner - fixedHeight(ui) - logoBoxHeight() - GAP >= listLogoHeight(ui));

   ui->logoX = left;
   ui->logoY = w->BorderTop + GAP;
   ui->logoW = width;
   ui->logoH = ui->showLogo ? logoBoxHeight() : 0;
   ui->infoY = ui->logoY + (ui->showLogo ? ui->logoH + GAP : 0);
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
   ng.ng_TextAttr = ui->ta;
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

/* Would the smallest layout in this font fit the screen? */
static BOOL fontFits(struct GtUI *ui, struct Screen *scr)
{
   struct RastPort rp;
   WORD needW, needH;

   InitRastPort(&rp);
   SetFont(&rp, ui->font);
   needW = scr->WBorLeft + scr->WBorRight + innerMinWidth(ui, &rp);
   needH = scr->WBorTop + scr->Font->ta_YSize + 1
           + fixedHeight(ui) + listMinHeight(ui) + BORDER_B;
   return (BOOL)(needW <= scr->Width && needH <= scr->Height - scr->BarHeight - 1);
}

BOOL gtui_Open(struct GtUI *ui, WORD width, WORD height)
{
   struct Screen *scr;
   WORD left, top, usableTop, usableH;
   int i;

   ui->win = NULL;
   ui->glist = ui->lv = NULL;
   ui->menu = NULL;
   ui->menuNext = MENUNULL;
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

   /* The screen's font, unless even the smallest layout would not fit in it */
   ui->ta = scr->Font;
   if (!(ui->font = OpenFont(ui->ta)) || !fontFits(ui, scr))
   {
      if (ui->font)
         CloseFont(ui->font);
      ui->ta = &topaz8;
      if (!(ui->font = OpenFont(ui->ta)))
         return FALSE;
   }

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

   /* Below the screen's title bar when there is room for it */
   usableTop = scr->BarHeight + 1;
   usableH = scr->Height - usableTop;

   if (ui->snapW > 0 && ui->snapH > 0)
   {
      /* Settings > Snapshot window: that place and size, kept on the screen */
      left = ui->snapX;
      top = ui->snapY;
      width = ui->snapW;
      height = ui->snapH;
      if (width > scr->Width) width = scr->Width;
      if (height > scr->Height) height = scr->Height;
   }
   else
   {
      /* A classic window: at most 600 wide, centred under the mouse */
      if (width > MAXDEFW) width = MAXDEFW;
      if (width > scr->Width) width = scr->Width;
      if (height > usableH) height = usableH;
      left = scr->MouseX - width / 2;
      top = scr->MouseY - height / 2;
      if (top < usableTop) top = usableTop;
   }
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
                            WA_NewLookMenus, TRUE,       /* V39, ignored before */
                            WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_NEWSIZE |
                                      IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY |
                                      IDCMP_MENUPICK |
                                      LISTVIEWIDCMP | BUTTONIDCMP,
                            TAG_DONE);
   if (!ui->win)
      return FALSE;
   SetFont(ui->win->RPort, ui->font);

   /* The right-mouse-button menus; GTMN_NewLookMenus is V39, ignored on 2.x */
   if (ui->menuDef && (ui->menu = CreateMenus(ui->menuDef, TAG_DONE)))
   {
      if (LayoutMenus(ui->menu, ui->vi, GTMN_NewLookMenus, TRUE, TAG_DONE))
         SetMenuStrip(ui->win, ui->menu);
      else
      {
         FreeMenus(ui->menu);
         ui->menu = NULL;
      }
   }

   computeLimits(ui);
   WindowLimits(ui->win, ui->minW, ui->minH, (UWORD)~0, (UWORD)~0);
   if (ui->win->Width < ui->minW || ui->win->Height < ui->minH)
   {
      /* Bigger font than expected: grow; the NEWSIZE that follows builds */
      WORD nw = ui->win->Width < ui->minW ? ui->minW : ui->win->Width;
      WORD nh = ui->win->Height < ui->minH ? ui->minH : ui->win->Height;
      WORD nl = ui->win->LeftEdge, nt = ui->win->TopEdge;

      if (nl + nw > scr->Width) nl = scr->Width - nw;
      if (nt + nh > scr->Height) nt = scr->Height - nh;
      if (nl < 0) nl = 0;
      if (nt < 0) nt = 0;
      ChangeWindowBox(ui->win, nl, nt, nw, nh);
      return TRUE;
   }
   return build(ui);
}

void gtui_Close(struct GtUI *ui)
{
   if (ui->win)
   {
      if (ui->menu)
         ClearMenuStrip(ui->win);
      CloseWindow(ui->win);
      ui->win = NULL;
   }
   if (ui->menu)
   {
      FreeMenus(ui->menu);
      ui->menu = NULL;
   }
   if (ui->glist)
   {
      FreeGadgets(ui->glist);           /* after the window is gone */
      ui->glist = NULL;
   }
   if (ui->font)
   {
      CloseFont(ui->font);
      ui->font = NULL;
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

/* The next pick of the current menu selection, if any */
static BOOL nextMenuPick(struct GtUI *ui, LONG *arg)
{
   while (ui->menu && ui->menuNext != MENUNULL)
   {
      struct MenuItem *it = ItemAddress(ui->menu, ui->menuNext);

      if (!it)
         break;
      ui->menuNext = it->NextSelect;
      *arg = (LONG)GTMENUITEM_USERDATA(it);
      return TRUE;
   }
   ui->menuNext = MENUNULL;
   return FALSE;
}

int gtui_Wait(struct GtUI *ui, LONG *arg)
{
   struct Window *w = ui->win;
   struct IntuiMessage *m;

   for (;;)
   {
      if (nextMenuPick(ui, arg))
         return GTUI_MENU;

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

            case IDCMP_MENUPICK:
               ui->menuNext = code;
               if (nextMenuPick(ui, arg))
                  return GTUI_MENU;
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

void gtui_SetHeader(struct GtUI *ui, const UWORD *logoData, char *info, char *infoAlt)
{
   ui->info = info;
   ui->infoAlt = infoAlt;
   if (logoData && logoData != ui->logoData)
   {
      ui->logoData = logoData;
      if (ui->logoChip)
         CopyMem((APTR)logoData, ui->logoChip, RASSIZE(GTUI_LOGO_W, GTUI_LOGO_H));
   }
   if (ui->win)
      relayout(ui);
}

void gtui_MenuEnable(struct GtUI *ui, UWORD menu, UWORD item, BOOL on)
{
   if (!ui->win || !ui->menu)
      return;
   if (on)
      OnMenu(ui->win, FULLMENUNUM(menu, item, NOSUB));
   else
      OffMenu(ui->win, FULLMENUNUM(menu, item, NOSUB));
}

BOOL gtui_GetArgs(int argc, char **argv, char *dev, int devsize, LONG *unit,
                  BOOL *fromCLI)
{
   dev[0] = '\0';
   *unit = -1;
   *fromCLI = (argc > 0);

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
   return TRUE;
}

BOOL gtui_CheckDevice(char *appname, const char *prefsfile, BOOL fromCLI,
                      const char *dev, LONG unit)
{
   char msg[200];

   if (dev[0] && unit >= 0)
      return TRUE;
   if (fromCLI)
   {
      PrintFault(ERROR_REQUIRED_ARG_MISSING, (STRPTR)appname);
      Printf("Give DEVICE=<driver> UNIT=<n>, or set them in ENV:%s\n", (ULONG)prefsfile);
   }
   else
   {
      sprintf(msg, "Missing SCSI device or unit in the Tool Types\n"
                   "(or in ENV:%.30s)\nDEVICE=%.40s\nUNIT=%ld",
              prefsfile, dev[0] ? dev : "(empty)", unit);
      MessageBox(appname, msg);
   }
   return FALSE;
}

/* ---- settings ------------------------------------------------------------ */

BOOL gtui_PrefsRead(const char *file, void (*line)(const char *key, char *val))
{
   static char buf[1024];
   char *p;

   if (amg_env_read(file, buf, sizeof(buf)) < 0)
      return FALSE;
   for (p = buf; *p; )
   {
      char *e = p, *eq, *k;

      while (*e && *e != '\n')
         e++;
      if (*e)
         *e++ = '\0';
      while (*p == ' ' || *p == '\t')
         p++;
      if (*p && *p != '#' && *p != ';' && (eq = strchr(p, '=')))
      {
         char *v = eq + 1, *t;

         *eq = '\0';
         for (t = eq; t > p && (t[-1] == ' ' || t[-1] == '\t'); )
            *--t = '\0';
         for (k = p; *k; k++)
            if (*k >= 'a' && *k <= 'z')
               *k -= 'a' - 'A';
         while (*v == ' ' || *v == '\t')
            v++;
         t = v + strlen(v);
         while (t > v && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r'))
            *--t = '\0';
         line(p, v);
      }
      p = e;
   }
   return TRUE;
}

static BOOL parseNum(const char **s, WORD *out)
{
   const char *p = *s;
   LONG v = 0;
   BOOL neg = FALSE, any = FALSE;

   while (*p == ' ')
      p++;
   if (*p == '-')
   {
      neg = TRUE;
      p++;
   }
   while (*p >= '0' && *p <= '9')
   {
      v = v * 10 + (*p++ - '0');
      if (v > 32767)
         return FALSE;
      any = TRUE;
   }
   while (*p == ' ')
      p++;
   if (*p == ',')
      p++;
   *s = p;
   *out = (WORD)(neg ? -v : v);
   return any;
}

BOOL gtui_ParseBox(const char *val, WORD *x, WORD *y, WORD *w, WORD *h)
{
   const char *p = val;
   WORD v[4];
   int i;

   for (i = 0; i < 4; i++)
      if (!parseNum(&p, &v[i]))
         return FALSE;
   if (v[2] <= 0 || v[3] <= 0)
      return FALSE;
   *x = v[0];
   *y = v[1];
   *w = v[2];
   *h = v[3];
   return TRUE;
}

struct PageCtx
{
   const struct GtField *f;
   int nf;
   const struct GtVals *def;
};

static void setField(AmgPage *pg, const struct GtField *f, int i, const struct GtVals *v)
{
   switch (f[i].kind)
   {
      case GTF_STR:
         GT_SetGadgetAttrs(pg->g[i], pg->win, NULL, GTST_String, (ULONG)v->s[i], TAG_DONE);
         break;
      case GTF_INT:
         GT_SetGadgetAttrs(pg->g[i], pg->win, NULL, GTIN_Number, (ULONG)v->n[i], TAG_DONE);
         break;
      case GTF_CHECK:
         GT_SetGadgetAttrs(pg->g[i], pg->win, NULL, GTCB_Checked, (ULONG)v->n[i], TAG_DONE);
         break;
   }
}

static void pageGadget(AmgPage *pg, int id, UWORD code, void *ctx)
{
   struct PageCtx *pc = (struct PageCtx *)ctx;
   int i;

   (void)code;
   if (id == AMG_ID_DEFAULTS && pc->def)
      for (i = 0; i < pc->nf; i++)
         setField(pg, pc->f, i, pc->def);
}

/* One page, laid out from the window's font: with Topaz 8 a three-field page
   is about 400 x 110, so it fits a 640x200 screen. */
int gtui_Page(struct GtUI *ui, const char *title, const struct GtField *f, int nf,
              const char *note, struct GtVals *v, const struct GtVals *def)
{
   struct RastPort rp;
   struct PageCtx pc;
   struct NewGadget ng;
   struct Gadget *gad;
   struct Requester block;
   BOOL blocked;
   AmgPage pg;
   WORD fh, bh, rowH, labW = 0, strW, intW, w, h, x0, y, chkW = 0, zw;
   int i, res;

   if (nf > GTF_MAX)
      nf = GTF_MAX;
   InitRastPort(&rp);
   SetFont(&rp, ui->font);
   fh = ui->font->tf_YSize;
   bh = fh + 6;
   rowH = bh + 4;
   zw = TextLength(&rp, (STRPTR)"0", 1);

   for (i = 0; i < nf; i++)
   {
      WORD l = TextLength(&rp, (STRPTR)f[i].label, strlen(f[i].label));
      if (f[i].kind == GTF_CHECK)
      {
         if (l + 34 > chkW)
            chkW = l + 34;
      }
      else if (l + 8 > labW)
         labW = l + 8;
   }
   strW = zw * 28;
   intW = zw * 7 + 12;
   w = 8 + labW + (strW > chkW ? strW : chkW) + 8;
   if (note)
   {
      i = TextLength(&rp, (STRPTR)note, strlen(note)) + 16;
      if (w < i)
         w = i;
   }
   if (w < amg_min_width(ui->ta))
      w = amg_min_width(ui->ta);
   h = 8 + nf * rowH + (note ? fh + 8 : 0) + bh + 8;

   /* an empty requester keeps the main window still while the page is up */
   InitRequester(&block);
   blocked = Request(&block, ui->win);

   res = AMG_CANCEL;
   if (!amg_page_open(&pg, ui->scr, ui->vi, ui->ta, ui->win, title, w, h))
      goto done;
   w = pg.win->Width - pg.win->BorderLeft - pg.win->BorderRight;   /* may be clipped */

   gad = CreateContext(&pg.glist);
   x0 = pg.win->BorderLeft + 8;
   y = pg.win->BorderTop + 8;
   memset(&ng, 0, sizeof(ng));
   ng.ng_TextAttr = ui->ta;
   ng.ng_VisualInfo = ui->vi;
   for (i = 0; i < nf && gad; i++)
   {
      ng.ng_LeftEdge = x0 + labW;
      ng.ng_TopEdge = y;
      ng.ng_Height = bh;
      ng.ng_GadgetText = (UBYTE *)f[i].label;
      ng.ng_GadgetID = i + 1;
      ng.ng_Flags = PLACETEXT_LEFT;
      switch (f[i].kind)
      {
         case GTF_STR:
            ng.ng_Width = w - 16 - labW < strW ? w - 16 - labW : strW;
            gad = CreateGadget(STRING_KIND, gad, &ng,
                               GTST_String, (ULONG)v->s[i],
                               GTST_MaxChars, (ULONG)f[i].maxc, TAG_DONE);
            break;
         case GTF_INT:
            ng.ng_Width = intW;
            gad = CreateGadget(INTEGER_KIND, gad, &ng,
                               GTIN_Number, (ULONG)v->n[i],
                               GTIN_MaxChars, (ULONG)f[i].maxc, TAG_DONE);
            break;
         default:
            ng.ng_LeftEdge = x0;
            ng.ng_Width = 26;
            ng.ng_Height = fh + 3;
            ng.ng_TopEdge = y + (bh - (fh + 3)) / 2;
            ng.ng_Flags = PLACETEXT_RIGHT;
            gad = CreateGadget(CHECKBOX_KIND, gad, &ng,
                               GTCB_Checked, (ULONG)v->n[i], TAG_DONE);
            break;
      }
      pg.g[i] = gad;
      y += rowH;
   }
   if (gad && note)
   {
      ng.ng_LeftEdge = x0;
      ng.ng_TopEdge = y;
      ng.ng_Width = w - 16;
      ng.ng_Height = fh + 2;
      ng.ng_GadgetText = NULL;
      ng.ng_GadgetID = 0;
      ng.ng_Flags = 0;
      /* GTTX_Clipped (V39, ignored before): never draw past the gadget */
      gad = CreateGadget(TEXT_KIND, gad, &ng, GTTX_Text, (ULONG)note,
                         GTTX_Clipped, TRUE, TAG_DONE);
      y += fh + 8;
   }
   if (gad)
      gad = amg_buttons(&pg, gad, y - pg.win->BorderTop, w, def != NULL);
   if (!gad)
   {
      amg_page_close(&pg);
      goto done;
   }

   amg_page_show(&pg);
   pc.f = f;
   pc.nf = nf;
   pc.def = def;
   res = amg_page_run(&pg, pageGadget, &pc);
   if (res != AMG_CANCEL)
   {
      for (i = 0; i < nf; i++)
      {
         if (f[i].kind == GTF_STR)
         {
            strncpy(v->s[i], amg_string(pg.g[i]), GTF_STRLEN - 1);
            v->s[i][GTF_STRLEN - 1] = '\0';
         }
         else if (f[i].kind == GTF_INT)
         {
            LONG n;
            if (StrToLong((STRPTR)amg_string(pg.g[i]), &n) >= 0)
               v->n[i] = n;
         }
         else
            v->n[i] = amg_checked(pg.g[i]);
      }
   }
   amg_page_close(&pg);

done:
   if (blocked)
      EndRequest(&block, ui->win);
   return res;
}

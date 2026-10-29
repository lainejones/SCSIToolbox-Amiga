/*
 * Copyright (C) 2026 Laine Jones (lainejones)
 *
 * Part of SCSIToolbox-Amiga. Shared GadTools user interface for CDChanger and
 * SDTransfer, replacing ReAction so both run on AmigaOS 2.04/3.0/3.1 (only
 * intuition, graphics and gadtools V37 are needed).
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
#ifndef GTUI_H
#define GTUI_H

#include <exec/types.h>
#include <exec/lists.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>

#define GTUI_MAXBUTTONS 2
#define GTUI_STATUSLEN  160

/* The BlueSCSI/ZuluSCSI logo: one bitplane, 192 x 71 */
#define GTUI_LOGO_W 192
#define GTUI_LOGO_H 71

/* What gtui_Wait() reports */
enum
{
   GTUI_QUIT,       /* close gadget, Esc or Ctrl-C */
   GTUI_BUTTON,     /* *arg = button number (also its underscored key) */
   GTUI_SELECT,     /* *arg = list row clicked */
   GTUI_OPEN        /* *arg = list row double-clicked */
};

struct GtUI
{
   /* Filled in by the caller before gtui_Open() */
   char *title;
   char *info;                          /* line under the logo */
   const UWORD *logoData;               /* logo bitplane (any memory), or NULL */
   struct List *labels;                 /* list rows: ln_Name is the text */
   int nButtons;
   char *buttonText[GTUI_MAXBUTTONS];   /* "_Select" style, '_' marks the key */
   BOOL hasStatus;                      /* text / progress bar at the bottom */

   /* Current state, readable by the caller */
   struct Window *win;
   LONG selected;                       /* list row, -1 = none */

   /* Private */
   struct Screen *scr;
   APTR vi;
   struct DrawInfo *dri;
   struct Gadget *glist, *lv;
   struct Image logo;
   UWORD *logoChip;
   UBYTE keys[GTUI_MAXBUTTONS];
   LONG lastRow;
   ULONG lastSecs, lastMicros;
   char status[GTUI_STATUSLEN];
   LONG level;                          /* progress 0-100, -1 = text only */
   WORD logoX, logoY, logoW, logoH;
   WORD infoY;
   WORD statX, statY, statW, statH;
   WORD minW, minH;
};

BOOL gtui_Open(struct GtUI *ui, WORD width, WORD height);
void gtui_Close(struct GtUI *ui);
int  gtui_Wait(struct GtUI *ui, LONG *arg);

/* Detach the list before changing it, attach it again afterwards. */
void gtui_DetachList(struct GtUI *ui);
void gtui_AttachList(struct GtUI *ui, struct List *labels, LONG selected);
struct Node *gtui_NodeAt(struct List *list, LONG row);

/* text == NULL keeps the current text; level -1 = no bar, 0-100 = percent */
void gtui_SetStatus(struct GtUI *ui, const char *text, LONG level);
void gtui_Busy(struct GtUI *ui, BOOL busy);

/* DEVICE/UNIT from the command line or the icon's Tool Types.
   Reports the problem itself and returns FALSE if either is missing. */
BOOL gtui_GetArgs(int argc, char **argv, char *appname,
                  char *dev, int devsize, LONG *unit);

#endif

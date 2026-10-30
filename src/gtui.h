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
#include <libraries/gadtools.h>
#include <graphics/text.h>

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
   GTUI_OPEN,       /* *arg = list row double-clicked */
   GTUI_MENU        /* *arg = the menu item's user data (one per pick) */
};

struct GtUI
{
   /* Filled in by the caller before gtui_Open() */
   char *title;
   char *info;                          /* line under the logo */
   char *infoAlt;                       /* the same line when the logo is hidden
                                           (too little height), or NULL = info */
   const UWORD *logoData;               /* logo bitplane (any memory), or NULL */
   struct List *labels;                 /* list rows: ln_Name is the text */
   int nButtons;
   char *buttonText[GTUI_MAXBUTTONS];   /* "_Select" style, '_' marks the key */
   BOOL hasStatus;                      /* text / progress bar at the bottom */
   struct NewMenu *menuDef;             /* the menu strip, or NULL */
   WORD snapX, snapY, snapW, snapH;     /* snapshot place + size; snapW 0 = none */

   /* Current state, readable by the caller */
   struct Window *win;
   struct Screen *scr;
   APTR vi;
   struct TextAttr *ta;                 /* the font the window uses: the screen's,
                                           or Topaz 8 when that one is too big */
   LONG selected;                       /* list row, -1 = none */

   /* Private */
   struct DrawInfo *dri;
   struct Gadget *glist, *lv;
   struct Menu *menu;
   UWORD menuNext;                      /* the next pick of a multi-select */
   struct TextFont *font;
   struct Image logo;
   UWORD *logoChip;
   UBYTE keys[GTUI_MAXBUTTONS];
   LONG lastRow;
   ULONG lastSecs, lastMicros;
   char status[GTUI_STATUSLEN];
   LONG level;                          /* progress 0-100, -1 = text only */
   BOOL showLogo;
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

/* New logo (another device) and/or info lines: redraws the window. */
void gtui_SetHeader(struct GtUI *ui, const UWORD *logoData, char *info, char *infoAlt);

/* Enable or disable menu item 'item' of menu 'menu' (both from 0). */
void gtui_MenuEnable(struct GtUI *ui, UWORD menu, UWORD item, BOOL on);

/* DEVICE/UNIT from the command line (*fromCLI = TRUE) or the icon's Tool
   Types. Either may be missing (dev "" / unit -1): the caller merges the
   settings file and then checks. FALSE only when the command line is bad
   (the problem is reported). */
BOOL gtui_GetArgs(int argc, char **argv, char *dev, int devsize, LONG *unit,
                  BOOL *fromCLI);

/* TRUE if dev and unit are set; else reports what is missing (to the shell
   or in a requester) and returns FALSE. */
BOOL gtui_CheckDevice(char *appname, const char *prefsfile, BOOL fromCLI,
                      const char *dev, LONG unit);

/* ---- settings (gtuiprefs in gtui.c, built on amigui.c) ------------------- */

/* Read ENV:<file> (else ENVARC:<file>) and call line() for each KEY=value
   line (key upper-cased, value trimmed). FALSE if there is no file. */
BOOL gtui_PrefsRead(const char *file, void (*line)(const char *key, char *val));

/* Parse "x,y,w,h" (WINDOW=). TRUE if all four are there and w, h > 0. */
BOOL gtui_ParseBox(const char *val, WORD *x, WORD *y, WORD *w, WORD *h);

/* A settings page: labelled fields, a note line, Save / Use / [Defaults] /
   Cancel. Returns AMG_SAVE / AMG_USE / AMG_CANCEL; unless cancelled the
   edited values are in v. The main window is blocked while it is open. */
enum { GTF_STR, GTF_INT, GTF_CHECK };
#define GTF_MAX    3
#define GTF_STRLEN 256

struct GtField
{
   const char *label;
   int kind;
   int maxc;                            /* GTF_STR / GTF_INT: most characters */
};

struct GtVals
{
   char s[GTF_MAX][GTF_STRLEN];
   LONG n[GTF_MAX];
};

int gtui_Page(struct GtUI *ui, const char *title, const struct GtField *f, int nf,
              const char *note, struct GtVals *v, const struct GtVals *def);

#endif

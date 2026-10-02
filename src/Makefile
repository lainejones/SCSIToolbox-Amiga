OBJS	 = BlueSCSIToolbox.o CDChanger.o BlueSCSIlogo.o ZuluSCSIlogo.o scsi.o common.o SDTransfer.o sharedfs.o hstart.o

DISTDIR  = ../dist

ifeq ($(OS),Windows_NT)
CC       = m68k-amigaos-gcc
STRIP    = m68k-amigaos-strip
INCLUDES = "-ID:\Amiga\Include_H"
RM       = del
CP       = copy
MKDIR    = if not exist $(DISTDIR) mkdir $(DISTDIR)
PATHSEP  = \\
else
CC       = /opt/amiga/bin/m68k-amigaos-gcc
STRIP    = /opt/amiga/bin/m68k-amigaos-strip
INCLUDES = -I/opt/amiga/m68k-amigaos/ndk-include
RM       = rm
CP       = cp
MKDIR    = mkdir -p $(DISTDIR)
PATHSEP  = /
endif

TIMEVAL_DEFS = -D__USE_NEW_TIMEVAL__ -Dtimeval=TimeVal -Dtimerequest=TimeRequest -Dtv_sec=tv_secs -Dtv_usec=tv_micro

# CPU target. Override on the command line or use a shortcut target:
#   make              -> 68000 (default, runs on all Amigas)
#   make 68020        -> 68020/030 (A1200, A2000+030, A3000, A4000)
#   make 68040        -> 68040     (A4000/040, A1200+040)
#   make 68060        -> 68060     (A4000/060, A1200+060)
CPUFLAGS ?= -m68000
OPT      ?= -O2

CFLAGS   = $(INCLUDES) $(TIMEVAL_DEFS) -DNO_INLINE_STDARG $(OPT) -std=c99 $(CPUFLAGS) -ffunction-sections -fdata-sections
#CFLAGS  += -DDEBUG
LFLAGS   = -lamiga -Wl,--gc-sections -s
#LFLAGS  += -ldebug

.PHONY: all clean release release-all 68020 68040 68060

all: BlueSCSIToolbox CDChanger SDTransfer sharedfs

# SHARED: filesystem handler. Built -nostartfiles: the hstart.s stub provides the
# hunk entry (_start at offset 0) and jumps to handlerMain; links scsi.o (libc-free)
# but NOT common.o, so no Intuition is pulled in. See sharedfs.c / CLAUDE.md.
sharedfs: hstart.o sharedfs.o scsi.o
	$(CC) -nostartfiles hstart.o sharedfs.o scsi.o -o sharedfs -s

sharedfs.o: sharedfs.c toolbox.h
	$(CC) $(CFLAGS) -c sharedfs.c

hstart.o: hstart.s
	$(CC) $(INCLUDES) -c hstart.s

# NOTE: do NOT run $(STRIP) on the linked binaries - this toolchain's
# m68k-amigaos-strip corrupts hunk executables (jumps into rodata at startup).
# LFLAGS already links with -s, which strips correctly.
BlueSCSIToolbox: BlueSCSIToolbox.o
	$(CC) BlueSCSIToolbox.o -o BlueSCSIToolbox $(LFLAGS)

BlueSCSIToolbox.o: BlueSCSIToolbox.c
	$(CC) $(CFLAGS) -c BlueSCSIToolbox.c

CDChanger: CDChanger.o BlueSCSIlogo.o ZuluSCSIlogo.o scsi.o common.o
	$(CC) CDChanger.o -o CDChanger $(LFLAGS) BlueSCSIlogo.o ZuluSCSIlogo.o scsi.o common.o

CDChanger.o: CDChanger.c BlueSCSIlogo.c ZuluSCSIlogo.c toolbox.h
	$(CC) $(CFLAGS) -c CDChanger.c

SDTransfer: SDTransfer.o BlueSCSIlogo.o ZuluSCSIlogo.o scsi.o common.o
	$(CC) SDTransfer.o -o SDTransfer $(LFLAGS) BlueSCSIlogo.o ZuluSCSIlogo.o scsi.o common.o

SDTransfer.o: SDTransfer.c BlueSCSIlogo.c ZuluSCSIlogo.c toolbox.h
	$(CC) $(CFLAGS) -c SDTransfer.c

scsi.o: scsi.c toolbox.h
	$(CC) $(CFLAGS) -c scsi.c

common.o: common.c toolbox.h
	$(CC) $(CFLAGS) -c common.c

BlueSCSIlogo.o: BlueSCSIlogo.c
	$(CC) $(CFLAGS) -c BlueSCSIlogo.c

ZuluSCSIlogo.o: ZuluSCSIlogo.c
	$(CC) $(CFLAGS) -c ZuluSCSIlogo.c

68020:
	$(MAKE) clean
	$(MAKE) CPUFLAGS="-m68020" OPT="-O3"

68040:
	$(MAKE) clean
	$(MAKE) CPUFLAGS="-m68040" OPT="-O3"

68060:
	$(MAKE) clean
	$(MAKE) CPUFLAGS="-m68060" OPT="-O3"

clean:
	$(RM) -f $(OBJS) BlueSCSIToolbox CDChanger SDTransfer sharedfs

# ---- release packaging -----------------------------------------------------
# Needs a POSIX shell (WSL/Linux, or amiga-gcc's sh.exe on Windows - the same
# SHELL= the build uses). Produces a ready-to-archive drawer:
#   dist/SCSIToolbox.info            drawer icon
#   dist/SCSIToolbox/                BlueSCSIToolbox, CDChanger(.info),
#                                    SDTransfer(.info), sharedfs,
#                                    SHARED.mountlist, SHARED-GVP.mountlist,
#                                    LICENSE.txt, README.md
# release-all puts the programs in one drawer per CPU instead
# (dist/SCSIToolbox/68000/ ... 68060/, each with its drawer icon).
REL      = $(DISTDIR)/SCSIToolbox
PROGS    = BlueSCSIToolbox CDChanger CDChanger.info SDTransfer SDTransfer.info sharedfs
DOCS     = SHARED.mountlist SHARED-GVP.mountlist ../LICENSE.txt ../README.md

release: all
	rm -rf $(REL) $(REL).info
	mkdir -p $(REL)
	cp $(PROGS) $(DOCS) $(REL)/
	cp SCSIToolbox.info $(REL).info

# Build all CPU variants into dist/SCSIToolbox/<cpu>/ drawers
release-all:
	rm -rf $(REL) $(REL).info
	mkdir -p $(REL)
	for cpu in 68000 68020 68040 68060; do 	    opt=-O3; [ $$cpu = 68000 ] && opt=-O2; 	    $(MAKE) clean && $(MAKE) CPUFLAGS="-m$$cpu" OPT="$$opt" || exit 1; 	    mkdir -p $(REL)/$$cpu && cp $(PROGS) $(REL)/$$cpu/ && cp SCSIToolbox.info $(REL)/$$cpu.info || exit 1; 	done
	cp $(DOCS) $(REL)/
	cp SCSIToolbox.info $(REL).info

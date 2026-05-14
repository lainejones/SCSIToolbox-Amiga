OBJS	 = BlueSCSIToolbox.o CDChanger.o BlueSCSIlogo.o ZuluSCSIlogo.o scsi.o common.o SDTransfer.o

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

all: BlueSCSIToolbox CDChanger SDTransfer

BlueSCSIToolbox: BlueSCSIToolbox.o
	$(CC) BlueSCSIToolbox.o -o BlueSCSIToolbox $(LFLAGS)
	$(STRIP) BlueSCSIToolbox

BlueSCSIToolbox.o: BlueSCSIToolbox.c
	$(CC) $(CFLAGS) -c BlueSCSIToolbox.c

CDChanger: CDChanger.o BlueSCSIlogo.o ZuluSCSIlogo.o scsi.o common.o
	$(CC) CDChanger.o -o CDChanger $(LFLAGS) BlueSCSIlogo.o ZuluSCSIlogo.o scsi.o common.o
	$(STRIP) CDChanger

CDChanger.o: CDChanger.c BlueSCSIlogo.c ZuluSCSIlogo.c toolbox.h
	$(CC) $(CFLAGS) -c CDChanger.c

SDTransfer: SDTransfer.o BlueSCSIlogo.o ZuluSCSIlogo.o scsi.o common.o
	$(CC) SDTransfer.o -o SDTransfer $(LFLAGS) BlueSCSIlogo.o ZuluSCSIlogo.o scsi.o common.o
	$(STRIP) SDTransfer

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
	$(RM) -f $(OBJS) BlueSCSIToolbox CDChanger SDTransfer

release:
	$(MKDIR)
	$(RM) -f $(DISTDIR)$(PATHSEP)*
	$(CP) CDChanger $(DISTDIR)
	$(CP) CDChanger.info $(DISTDIR)
	$(CP) SDTransfer $(DISTDIR)
	$(CP) SDTransfer.info $(DISTDIR)
	$(CP) ..$(PATHSEP)LICENSE.txt $(DISTDIR)
	$(CP) BlueSCSIToolbox $(DISTDIR)

# Build all CPU variants into dist/<cpu>/ subdirectories
release-all:
ifeq ($(OS),Windows_NT)
	for %%C in (68000 68020 68040 68060) do (if not exist $(DISTDIR)\%%C mkdir $(DISTDIR)\%%C)
	$(MAKE) clean && $(MAKE) CPUFLAGS="-m68000" OPT="-O2" && $(CP) BlueSCSIToolbox CDChanger SDTransfer $(DISTDIR)\68000
	$(MAKE) clean && $(MAKE) CPUFLAGS="-m68020" OPT="-O3" && $(CP) BlueSCSIToolbox CDChanger SDTransfer $(DISTDIR)\68020
	$(MAKE) clean && $(MAKE) CPUFLAGS="-m68040" OPT="-O3" && $(CP) BlueSCSIToolbox CDChanger SDTransfer $(DISTDIR)\68040
	$(MAKE) clean && $(MAKE) CPUFLAGS="-m68060" OPT="-O3" && $(CP) BlueSCSIToolbox CDChanger SDTransfer $(DISTDIR)\68060
	$(CP) CDChanger.info SDTransfer.info ..$(PATHSEP)LICENSE.txt $(DISTDIR)
else
	for cpu in 68000 68020 68040 68060; do mkdir -p $(DISTDIR)/$$cpu; done
	$(MAKE) clean && $(MAKE) CPUFLAGS="-m68000" OPT="-O2" && $(CP) BlueSCSIToolbox CDChanger SDTransfer $(DISTDIR)/68000
	$(MAKE) clean && $(MAKE) CPUFLAGS="-m68020" OPT="-O3" && $(CP) BlueSCSIToolbox CDChanger SDTransfer $(DISTDIR)/68020
	$(MAKE) clean && $(MAKE) CPUFLAGS="-m68040" OPT="-O3" && $(CP) BlueSCSIToolbox CDChanger SDTransfer $(DISTDIR)/68040
	$(MAKE) clean && $(MAKE) CPUFLAGS="-m68060" OPT="-O3" && $(CP) BlueSCSIToolbox CDChanger SDTransfer $(DISTDIR)/68060
	$(CP) CDChanger.info SDTransfer.info ../LICENSE.txt $(DISTDIR)
endif


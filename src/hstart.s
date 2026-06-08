| hstart.s - entry stub for the SCSIToolbox-Amiga SHARED: handler.
|
| Copyright (C) 2026 Laine Jones <https://github.com/lainejones/>
| GPL-3.0-or-later (see LICENSE.txt).
|
| Amiga hunk executables begin at offset 0 of the FIRST code hunk - there is no
| entry symbol. amiga-gcc emits string literals (and switch jump-tables) into
| .text, which can otherwise occupy offset 0 and get "executed" -> crash. This
| stub is linked FIRST so _start sits at offset 0 and simply jumps to the C
| handler (handlerMain in sharedfs.c). See CLAUDE.md "C-handler build".

	.text
	.globl	_start
	.globl	_handlerMain
_start:
	jmp	_handlerMain

# The open SECRMAN (secrman_special), from the FMCB installer v0.987, built with
# maxsdk's IOP rules: make [OUT=<dir>] -> <dir>/secrman.irx
MODULE = secrman
SRCS = secrman.c MechaAuth.c CardAuth.c keyman.c
# DEX_SUPPORT: CEX and DEX consoles alike (ROMVER's type letter)
CFLAGS_MODULE = -DDEX_SUPPORT=1 -fno-strict-aliasing
include ../../../../libs/maxsdk/iop/Rules.mk

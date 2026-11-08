# mxboot: the PS2's memory card boot program, in two phases, and its installer.
#
#   make            build/apps/mxboot/:
#                     MXBOOT.XLF          phase 1 (kelf/): the system update, a KELF to
#                                         be bound to a card; starts MXOS/MXBOOT.ELF
#                     MXBOOT.ELF          phase 2 (boot/): the configs, the one-shot,
#                                         the ELF they name (for tests: run it from
#                                         mxlink with "mc1:" as argument)
#                     mxboot-install.elf  the installer (see README.md)
#                     SECRMAN.IMG         the IOPRP image with the open SECRMAN
#                                         (iop/secrman, clang and lld)
#   make programs   all of it but MXBOOT.XLF (no keys needed)
#   make clean
#
# MXBOOT.XLF needs the KELF keys: PS2KEYS=<file> (default: PS2KEYS.dat in the
# ps2max directory). They are never copied out of build/.

all:

PS2MAX  ?= $(abspath $(CURDIR)/../..)
OUT     ?= $(PS2MAX)/build/apps/mxboot
PS2MAX_PS2DEV  ?= $(abspath $(PS2MAX)/../ps2dev/install)
PS2MAX_IOP_SDK ?= $(PS2MAX_PS2DEV)/ps2sdk
PS2MAX_IRX     ?= $(PS2MAX_IOP_SDK)/iop/irx
PS2MAX_IOP_BIN ?= $(PS2MAX_PS2DEV)/iop/bin
KELFTOOL ?= $(PS2MAX)/build/tools/kelftool/kelftool
PS2KEYS  ?= $(PS2MAX)/PS2KEYS.dat

# libmx built for size, a section per function, as mxlink's
MAXSDK_OUT = $(OUT)/maxsdk
export EXTRA_CFLAGS = -Os -ffunction-sections -fdata-sections

include $(PS2MAX)/libs/maxsdk/mk/maxsdk.mk

# phase 2: in the top 256 KiB, so the ELF it starts may be anywhere below
# (mxlink at 0x88000, programs from 0x100000)
BOOT_LDFLAGS = --defsym=__mx_base=0x1FC0000 --defsym=__mx_top=0x2000000 \
               --defsym=__mx_stack_size=0x4000
# phase 1: 64 KiB below phase 2, its stack included; no maxsdk runtime, only
# maxsdk's link script
KELF_LDFLAGS = --defsym=__mx_base=0x1F80000 --defsym=__mx_top=0x1F90000 \
               --defsym=__mx_stack_size=0x1000

all: programs $(OUT)/MXBOOT.XLF
# what needs no keys
programs: $(OUT)/MXBOOT.ELF $(OUT)/mxboot-kelf.elf $(OUT)/mxboot-install.elf $(OUT)/SECRMAN.IMG \
          $(OUT)/iop/secrman.irx

$(OUT)/obj/%.o: %.c $(MX_HDRS) $(wildcard install/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/mxboot-full.elf: $(OUT)/obj/boot/mxboot.o $(MX_LIB) $(MX_CRT0)
	$(call link,$@,$(BOOT_LDFLAGS) $(MX_CRT0) $(OUT)/obj/boot/mxboot.o $(MX_LIB))

$(OUT)/MXBOOT.ELF: $(OUT)/mxboot-full.elf
	$(LLVM)/llvm-strip -o $@ $<

$(OUT)/obj/kelf/%.o: kelf/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Oz -c $< -o $@
$(OUT)/obj/kelf/start.o: kelf/start.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/mxboot-kelf-full.elf: $(OUT)/obj/kelf/start.o $(OUT)/obj/kelf/kelf.o
	$(call link,$@,$(KELF_LDFLAGS) $^)
# the ROM loads it by its program header: no section headers
$(OUT)/mxboot-kelf.elf: $(OUT)/mxboot-kelf-full.elf
	$(LLVM)/llvm-objcopy --strip-sections $< $@

# The keys file as kelftool wants it: with a [default] section (a file of
# bare KEY=value lines gets one). kelftool reads ./PS2KEYS.dat, so it runs
# in that directory.
KEYDIR = $(OUT)/keys
$(KEYDIR)/PS2KEYS.dat: $(PS2KEYS)
	@mkdir -p $(dir $@)
	@chmod 700 $(dir $@)
	@{ grep -q '^\[' $< || echo '[default]'; cat $<; } > $@.tmp && chmod 600 $@.tmp && mv $@.tmp $@

# dnasload: the memory card KELF header PS2BBL uses (retail PS2s; the PSX's
# whitelist too)
$(OUT)/MXBOOT.XLF: $(OUT)/mxboot-kelf.elf $(KEYDIR)/PS2KEYS.dat $(KELFTOOL)
	cd $(KEYDIR) && $(KELFTOOL) encrypt dnasload $< $@ > $(OUT)/kelftool.log || \
	  { cat $(OUT)/kelftool.log; rm -f $@; exit 1; }

$(KELFTOOL):
	$(MAKE) -C $(PS2MAX)/tools/kelftool dir_build=$(dir $@) CXXFLAGS="--std=c++17 -include cstdint"

# The IOPRP image with the open SECRMAN (secrman_special: it binds KELFs to a
# card), for the installer's IOP reboot: built here from iop/secrman (the FMCB
# installer's, v0.987). ps2sdk's prebuilt secrman.irx (from its refactored
# copy of the same code) loads there but fails every card authentication
# (the ROM's MCMAN: sceMcResFailResetAuth). SECRMAN_IRX=<irx> takes another.
SECRMAN_IRX ?= $(OUT)/iop/secrman.irx
$(OUT)/SECRMAN.IMG: $(SECRMAN_IRX) tools/mk-ioprp.py
	@mkdir -p $(dir $@)
	python3 tools/mk-ioprp.py $@ SECRMAN=$<

# iop/secrman, with maxsdk's IOP rules (the IOP gcc, ps2sdk's IOP build files)
$(OUT)/iop/secrman.irx: $(wildcard iop/secrman/src/* iop/secrman/include/*) iop/secrman/Makefile \
                        $(PS2MAX)/libs/maxsdk/iop/Rules.mk $(PS2MAX)/libs/maxsdk/iop/iop.mk
	$(MAKE) -C iop/secrman OUT=$(OUT)/iop EXTRA_CFLAGS=

# ---- the installer --------------------------------------------------------------

INSTALL_SRCS = install/install.c install/secr.c
INSTALL_OBJS = $(INSTALL_SRCS:%.c=$(OUT)/obj/%.o) $(OUT)/obj/install/blobs.o
# what it puts on the IOP after its reboot, and the image it reboots with
INSTALL_IRX = $(OUT)/SECRMAN.IMG $(PS2MAX_IRX)/secrsif.irx

$(OUT)/obj/install/blobs.o: install/blobs.S $(INSTALL_IRX)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DSECRMAN_IMG='"$(OUT)/SECRMAN.IMG"' -DSECRSIF='"$(PS2MAX_IRX)/secrsif.irx"' \
	  -c $< -o $@

$(OUT)/mxboot-install.elf: $(INSTALL_OBJS) $(MX_LIB) $(MX_CRT0)
	$(call link,$@,$(MX_CRT0) $(INSTALL_OBJS) $(MX_LIB))

clean:
	rm -rf $(OUT)

# not intermediate: a deleted one is made again
.SECONDARY: $(OUT)/iop/secrman.irx
.PHONY: all programs clean

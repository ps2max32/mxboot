# mxboot

A memory card boot program for the PS2, in the place FMCB or PS2BBL take:
the ROM starts it at power-on as the card's "system update", and it starts
one ELF from that card. Nothing else: no menu, no pad, no IOP modules of its
own. Built with LLVM (clang, ld.lld) only.

## Two phases

| Phase | File | Size | Does |
|---|---|---|---|
| 1 | the system update (`MXBOOT.XLF`, a KELF bound to the card), on every update path | 3.8 KB | starts phase 2 |
| 2 | `MXOS/MXBOOT.ELF`, a plain ELF, once per card | 39 KB | the configs, the one-shot, starts their ELF |

Phase 1 (`kelf/`) is written freestanding, without maxsdk's runtime, to stay
small: it is copied to 8 paths, and changing it means binding it to the card
again. It finds its card (the one in `argv[0]`, else mc0, then mc1), loads
`MXOS/MXBOOT.ELF` with the ROM's FILEIO and starts it with ExecPS2. The IOP
stays as the ROM had it. If phase 2 cannot be started, the screen turns red.

Phase 2 (`boot/`) is a maxsdk program, replaced like any file. It is not
covered by the one-shot below: a broken `MXBOOT.ELF` leaves the card without
a boot until it is written again, so `update` writes it and reads it back
before the KELF, and it is best tried as a plain ELF first (see Build).

## What phase 2 starts

Two config files on the card it booted from, each naming one ELF:

| File | Use |
|---|---|
| `MXOS/BOOTONCE.CFG` | once: removed, then its ELF is started |
| `MXOS/BOOT.CFG` | otherwise: the default |

The one-shot file is for trying a new ELF without risking the card: if it
hangs, reset or power the PS2 off and on, and it boots the default again.

A config's first line that is not empty and not a `#` comment is the ELF's
path, e.g. `mc?:/MXOS/MXLINK.ELF`; `mc?:` is the card the config is on. The
ELF gets that path as `argv[0]`. Its segments must lie between 0x82000 and
0x1FC0000 (phase 2's own place, the top 256 KiB): loaders such as mxlink at
0x88000 and programs from 0x100000 fit.

Before starting it, phase 2 unlocks the disc drive (boot certification with
the ROM version, as the ROM's browser does). It leaves the IOP as it is
unless the cards cannot be read then (it resets the IOP and loads the ROM's
memory card drivers).

If nothing can be started the screen turns one colour and stays:

| Colour | Meaning |
|---|---|
| red | phase 1: no `MXOS/MXBOOT.ELF` it could start; phase 2: no card can be read |
| yellow | no `MXOS/BOOT.CFG` |
| magenta | the ELF named is missing or not one mxboot loads |

## Models

`mxboot-install update` writes the system update for every PS2 with ROM
v1.10 or later, all regions: `BIEXEC-SYSTEM` (Japan), `BAEXEC-SYSTEM` (USA,
Asia), `BEEXEC-SYSTEM` (Europe), `BCEXEC-SYSTEM` (China), with the file
names the early ROMs look for (`osd120.elf`, `osd130.elf`) and `osdmain.elf`.
One card boots on any of them.

Not yet: the first Japanese models (SCPH-10000, SCPH-15000: ROM v1.00 and
v1.01) need a kernel patch before the update, and the PSX (DESR) a KELF of
its own type.

## Build

```
make            # build/tools/mxboot/
make programs   # all but MXBOOT.XLF: no keys needed
```

- `MXBOOT.XLF`: phase 1 as a KELF (kelftool's `dnasload` header), to be bound
  to a card by the installer; `mxboot-kelf.elf` is the same as a plain ELF.
  The KELF needs the KELF keys, which you have to provide yourself:
  `PS2KEYS=<file>`, default `PS2KEYS.dat` in the ps2max directory. A file of
  bare `KEY=value` lines is fine; the build gives kelftool a copy with a
  `[default]` section, in `build/` (mode 600).
- `MXBOOT.ELF`: phase 2. For tests, run it from mxlink with the card as
  argument (`mc1:`): it reads that card only (the one-shot config is
  removed as at a boot), checks the ELF and ends with a status instead of
  starting it or showing a colour (0 the ELF would start, 2 no card, 3 no
  config, 4 no ELF). With `exec` after the card it starts the ELF too: only
  from `mxlink-high.elf`, as an ELF for 0x88000 (mxlink) would overwrite the
  mxlink that runs it.
- `mxboot-kelf.elf` run from mxlink tries both phases together: it starts
  phase 2 from the card, which starts its config's ELF. With a one-shot
  naming a program at 0x100000 (not mxlink, whose code mxlink keeps
  read-only) the program's output comes back to the host.
- `mxboot-install.elf`: the installer.
- `SECRMAN.IMG`: the IOPRP image (`tools/mk-ioprp.py`) with the open
  SECRMAN, built into the installer. The SECRMAN is `iop/secrman`, the FMCB
  installer's (v0.987, AFL 2.0), built here by clang and ld.lld with
  maxsdk's IOP rules. ps2sdk's prebuilt `secrman.irx`, from its refactored
  copy of the same code, loads but fails every card authentication
  (`sceMcResFailResetAuth`); `SECRMAN_IRX=<irx>` takes another one.

Needs maxsdk, the feature LLVM, kelftool (`tools/kelftool`, built here),
ps2sdk's IOP headers and its prebuilt `secrsif.irx`
(`PS2MAX_IOP_SDK`, `PS2MAX_IRX`), and Python.

## Install

Run the installer from mxlink, with the files on the host:

```
ps2client execee host:mxboot-install.elf <command> ...
```

| Command | Does |
|---|---|
| `update mcN: host:MXBOOT.XLF host:MXBOOT.ELF [force]` | write phase 2, then bind phase 1 to the card and write it to every update path |
| `boot mcN: <ELF path>` | write `MXOS/BOOT.CFG` |
| `once mcN: <ELF path>` | write `MXOS/BOOTONCE.CFG` |
| `copy <from> <to>` | copy a file of any size (`host:`, `mc0:`, `mc1:`, `usb0:`, ...) in pieces, read back and compared |
| `copydir <from> <to>` | the same for every file of a directory |
| `rm <path>` | remove a file |
| `list mcN:` | the card's root, the update directories, `MXOS` and the configs |
| `remove mcN:` | remove the update files and directories |
| `format mcN: erase` | format the card: everything on it is gone |
| `bindtest mcN: host:MXBOOT.XLF` | `update` up to the binding, writing nothing |
| `mods mcN:` | the IOP's modules |
| `log mcN:` | what the last `update` did after its IOP reboot |

`update` leaves a card that has a system update already (FMCB, PS2BBL, an
older mxboot) as it is, unless `force` is given: then each file it replaces
is copied to `host:mxboot-backup-<dir>-<file>` first. A bound file only works
on the card it was bound to, so the backup can be put back on that card only.

For the binding, `update` reboots the IOP with the open SECRMAN (the ROM's
cannot bind), which ends the host connection: everything after that is told
by the exit status alone (0 done, 67 the card cannot be read, 70-72 the IOP
reboot or its modules failed, 73 the binding failed, 74 a write failed, 75 it
reads back differently; the full list is at the top of
`install/install.c`), and by `log`. mxlink shows the status, and resets the
IOP for the next program. Then check with `list`.

A card from scratch, for mxlink (its IOP modules and shell go to `MXOS/`
too, see its README):

```
mxboot-install.elf format mc0: erase
mxboot-install.elf copydir host:MXOS mc0:/MXOS      # mxlink, BOOT.CFG, its modules
mxboot-install.elf update mc0: host:MXBOOT.XLF host:MXBOOT.ELF
```

Write `MXOS/` before `update`: mxlink reads its IOP modules from the card
when it resets the IOP after `update`.

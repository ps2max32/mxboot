# secrman

The open SECRMAN (`secrman_special` 1.4): the IOP's security manager with
the functions that bind a KELF to a memory card (`SecrDownloadFile` and its
parts), which the ROM's SECRMAN does not have. The installer reboots the IOP
with it (`../../tools/mk-ioprp.py`).

From the FMCB installer v0.987, Copyright 2001-2004 ps2dev
(http://www.ps2dev.org), Academic Free License 2.0 (see ps2sdk's README and
LICENSE). ps2sdk's `iop/security/secrman` is a later, refactored copy of the
same code; its prebuilt module fails every card authentication, so this one
is built here.

Changed here: built with clang and ld.lld (maxsdk's `iop/Rules.mk`, see
`Makefile`); pointer increments written as assignments (newer compilers
refuse a cast as an lvalue); a cast for `SetSecrmanCallbacks`'s third
argument; the exports in `src/exports.ilb` (ld.lld writes the table, first in
`.text`, and makes the entries it leaves out only return, as `_funcret` did).

/* What the installer puts on the IOP: the IOPRP image with the open SECRMAN
   it reboots with, and SECRMAN's RPC server it loads after (the memory card
   drivers are the ROM's). The paths come from the Makefile. */

#define BLOB(name, path)       \
  .section .rodata.name, "a";  \
  .balign 64;                  \
  .globl name;                 \
  name:;                       \
  .incbin path;                \
  .globl name##_end;           \
  name##_end:

BLOB(secrman_img, SECRMAN_IMG)
BLOB(secrsif_irx, SECRSIF)

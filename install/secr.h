#ifndef MXBOOT_SECR_H
#define MXBOOT_SECR_H

#include <mx/types.h>

/* The client of ps2sdk's secrsif.irx (SECRMAN's RPC server), after ps2sdk's
   libsecr (ee/rpc/secr, Academic Free License 2.0). */

/* Bind the servers (after the IOP reboot that loaded SECRMAN and secrsif):
   0, < 0 if they do not answer. */
int secr_init(void);

/* Bind the KELF at kelf (in place) to the memory card in port (0: mc0, 1:
   mc1): MagicGate encrypts its header and keys for that card, as SECRMAN's
   SecrDownloadFile. 0, < 0 on failure. */
int secr_bind_kelf(int port, void *kelf);

#endif

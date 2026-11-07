/* SECRMAN's RPC client: what the installer needs of ps2sdk's libsecr
   (ee/rpc/secr/src/libsecr.c, Academic Free License 2.0; its structures are
   common/include/secrsif.h and libsecr-common.h). The calls send and receive
   a 4 KiB buffer, as libsecr does. */

#include <mx/cpu.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/string.h>

#include "secr.h"

#define SECR_DOWNLOAD_HEADER 0x80000A01u
#define SECR_DOWNLOAD_BLOCK 0x80000A02u
#define SECR_DOWNLOAD_GET_KBIT 0x80000A03u
#define SECR_DOWNLOAD_GET_KC 0x80000A04u
#define SECR_DOWNLOAD_GET_ICVPS2 0x80000A05u

struct bit_block {
  u32 size;
  u32 flags; /* 1 signed, 2 encrypted */
  u8 checksum[8];
};
struct bit_table {
  u32 headersize;
  u8 block_count, pad[3];
  struct bit_block blocks[63];
};
struct kelf_header {
  u8 user[16];
  u32 elf_size;
  u16 header_size;
  u16 unknown5;
  u16 flags; /* the layout of the header */
  u16 bit_count;
  u32 mg_zones;
};

/* the parameters of each server (secrsif.h) */
struct header_params {
  s32 port, slot;
  u8 buffer[0x400];
  struct bit_table bits;
  s32 size, result;
};
struct block_params {
  u8 buffer[0x400];
  s32 size, result;
};
struct key_params {
  s32 port, slot;
  u8 key[16];
  s32 result;
};
struct icv_params {
  u8 icvps2[8];
  s32 result;
};

#define RPC_SIZE 0x1000
static u8 send_buf[RPC_SIZE] MX_DMA_LINES;
static u8 recv_buf[RPC_SIZE] MX_DMA_LINES;
static struct mx_sif_client client[5];

int secr_init(void) {
  for (int i = 0; i < 5; i++)
    if (mx_sif_bind_tries(&client[i], SECR_DOWNLOAD_HEADER + (u32)i, 200) < 0 || !client[i].server)
      return -1;
  return 0;
}

/* call server i with what is in send_buf; the answer is read uncached */
static volatile u8 *call(int i) {
  if (mx_sif_call(&client[i], 1, send_buf, RPC_SIZE, (void *)MX_UNCACHED(recv_buf), RPC_SIZE) < 0)
    return 0;
  return (volatile u8 *)MX_UNCACHED(recv_buf);
}

static void copy_in(void *dst, const volatile u8 *src, u32 n) {
  u8 *d = dst;
  for (u32 i = 0; i < n; i++)
    d[i] = src[i];
}

/* where the header keeps Kbit (Kc after it): after the bit counts, an
   optional length-prefixed block and an optional 8 bytes */
static u32 key_offset(const struct kelf_header *h) {
  u32 off = 0x20;
  if (h->bit_count > 0)
    off += h->bit_count * sizeof(struct bit_block);
  if (h->flags & 1)
    off += ((const u8 *)h)[off] + 1;
  if ((h->flags & 0xF000) == 0)
    off += 8;
  return off;
}

int secr_bind_kelf(int port, void *kelf) {
  struct kelf_header *h = kelf;
  int mcport = 2 + port, slot = 0;

  struct header_params *hp = (struct header_params *)send_buf;
  hp->port = mcport;
  hp->slot = slot;
  memcpy(hp->buffer, kelf, sizeof hp->buffer);
  const volatile u8 *r = call(0);
  if (!r)
    return -1;
  const volatile struct header_params *hr = (const volatile struct header_params *)r;
  if (hr->result == 0)
    return -2; /* the header was not accepted */
  static struct bit_table bits;
  s32 bsize = hr->size;
  if (bsize <= 0 || bsize > (s32)sizeof bits)
    bsize = sizeof bits;
  copy_in(&bits, (const volatile u8 *)&hr->bits, (u32)bsize);

  /* the encrypted blocks go through the MechaCon too */
  u32 off = bits.headersize;
  for (u32 i = 0; i < bits.block_count && i < 63; i++) {
    if (bits.blocks[i].flags & 2) {
      struct block_params *bp = (struct block_params *)send_buf;
      memcpy(bp->buffer, (u8 *)kelf + off, sizeof bp->buffer);
      bp->size = (s32)bits.blocks[i].size;
      r = call(1);
      if (!r || ((const volatile struct block_params *)r)->result == 0)
        return -3;
    }
    off += bits.blocks[i].size;
  }

  u8 kbit[16], kc[16];
  for (int k = 0; k < 2; k++) {
    struct key_params *kp = (struct key_params *)send_buf;
    kp->port = mcport;
    kp->slot = slot;
    r = call(2 + k);
    const volatile struct key_params *kr = (const volatile struct key_params *)r;
    if (!r || kr->result == 0)
      return -4 - k;
    copy_in(k ? kc : kbit, kr->key, 16);
  }
  u32 ko = key_offset(h);
  memcpy((u8 *)kelf + ko, kbit, 16);
  memcpy((u8 *)kelf + ko + 16, kc, 16);

  if ((h->flags >> 1) & 1) { /* the header has an ICVPS2 */
    r = call(4);
    const volatile struct icv_params *ir = (const volatile struct icv_params *)r;
    if (!r || ir->result == 0)
      return -6;
    u8 icv[8];
    copy_in(icv, ir->icvps2, 8);
    memcpy((u8 *)kelf + h->header_size - 8, icv, 8);
  }
  return 0;
}

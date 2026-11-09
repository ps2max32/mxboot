/* mxboot's phase 1: the system update the ROM starts at power-on (the KELF,
   one copy per model's update path). It starts phase 2, MXBOOT/MXBOOT.ELF of
   the card it booted from (argv[0] names it; else mc0, then mc1), with
   ExecPS2, and does nothing else: phase 2 reads the configs and starts the
   ELF they name.

   Freestanding (no maxsdk runtime), to stay small: the BIOS's syscalls, the
   SIF command and RPC protocol as maxsdk's ee/src/base/sif.c speaks it, and
   of the ROM's FILEIO only open, read, lseek and close. The IOP is left as
   the ROM had it: it has just read this KELF from the card, so its memory
   card drivers and FILEIO are there. Waits sleep on a semaphore the SIF0
   interrupt signals.

   If phase 2 cannot be started, the screen turns red and the PS2 stays
   there. */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;

#define PHASE2 "MXBOOT/MXBOOT.ELF"

/* ---- syscalls -------------------------------------------------------------- */

#define SYSCALL(name, num, ret, ...)                                                    \
  ret name(__VA_ARGS__);                                                                \
  __asm__(".text\n.globl " #name "\n" #name ":\n\tli $3, " #num "\n\tsyscall\n\tjr $31\n\tnop\n")
SYSCALL(set_gs_crt, 2, void, int interlace, int mode, int field);
SYSCALL(exec_ps2, 7, void, void *entry, void *gp, int argc, char **argv);
SYSCALL(add_dmac_handler, 18, int, int ch, int (*fn)(int, void *, void *), int next, void *arg);
SYSCALL(enable_dmac, 22, int, int ch);
SYSCALL(sleep_thread, 50, int, void);
SYSCALL(create_sema, 64, int, void *param);
SYSCALL(isignal_sema, -67, int, int id);
SYSCALL(wait_sema, 68, int, int id);
SYSCALL(flush_cache, 100, void, int op);
SYSCALL(sif_set_dma, 119, u32, void *t, int n);
SYSCALL(sif_set_dchain, 120, void, void);
SYSCALL(isif_set_dchain, -120, void, void);
SYSCALL(sif_set_reg, 121, int, u32 reg, s32 value);
SYSCALL(sif_get_reg, 122, s32, u32 reg);

#define R32(a) (*(volatile u32 *)(a))
#define R64(a) (*(volatile unsigned long long *)(a))
#define UNCACHED(p) ((volatile void *)(((u32)(p) & 0x0FFFFFFFu) | 0x20000000u))
#define PHYS(p) ((u32)(p) & 0x0FFFFFFFu)
#define ALIGNED __attribute__((aligned(64)))

void *memset(void *d, int c, u32 n) {
  u8 *p = d;
  while (n--)
    *p++ = (u8)c;
  return d;
}
void *memcpy(void *d, const void *s, u32 n) {
  u8 *p = d;
  const u8 *q = s;
  while (n--)
    *p++ = *q++;
  return d;
}

/* hit invalidate of the data cache's lines over [p, p + n) (cache 0x1A,
   each between two SYNCs as the EE Core manual wants) */
static void dcache_invalidate(void *p, u32 n) {
  for (u32 a = (u32)p & ~63u; a < (u32)p + n; a += 64)
    __asm__ volatile("sync.l\n\tcache 0x1a, 0(%0)\n\tsync.l" : : "r"(a) : "memory");
}

/* ---- SIF ------------------------------------------------------------------ */

#define CMD_CHANGE_SADDR 0x80000000u
#define CMD_SET_SREG 0x80000001u
#define CMD_INIT_CMD 0x80000002u
#define CMD_RPC_END 0x80000008u
#define CMD_RPC_BIND 0x80000009u
#define CMD_RPC_CALL 0x8000000Au
#define SYSREG_SUBADDR 0x80000000u
#define SYSREG_RPCINIT 0x80000002u

struct client {
  volatile u32 done, server, buf;
};
static u8 rx_buf[128] ALIGNED; /* the IOP's packets, by SIF0 DMA */
static u8 pkt[64] ALIGNED;     /* ours: one at a time */
static u32 iop_buf;            /* where the IOP takes them */
static volatile s32 sreg0;
static int sema;

static int sif0_handler(int ch, void *arg, void *addr) {
  (void)ch, (void)arg, (void)addr;
  volatile u32 *rx = UNCACHED(rx_buf);
  u32 p[12];
  if (!(rx[0] & 0xFF))
    return 0;
  for (int i = 0; i < 12; i++)
    p[i] = rx[i];
  rx[0] = 0;
  __asm__ volatile("sync.l");
  isif_set_dchain();
  if (p[2] == CMD_CHANGE_SADDR)
    iop_buf = p[4];
  else if (p[2] == CMD_SET_SREG && p[4] == 0) {
    sreg0 = (s32)p[5];
    isignal_sema(sema);
  } else if (p[2] == CMD_RPC_END) {
    struct client *c = (struct client *)p[7];
    if (p[8] == CMD_RPC_BIND)
      c->server = p[9], c->buf = p[10];
    c->done = 1;
    isignal_sema(sema);
  }
  return 0;
}

/* a command packet of psize bytes (and dsize bytes of data from src to the
   IOP's dest before it) */
static void send_cmd(u32 cid, u32 psize, const void *src, u32 dest, u32 dsize) {
  volatile u32 *h = UNCACHED(pkt);
  struct {
    u32 src, dest;
    s32 size, attr;
  } t[2];
  int n = 0;
  h[0] = psize | (dsize << 8);
  h[1] = dsize ? dest : 0;
  h[2] = cid;
  if (dsize)
    t[n].src = PHYS(src), t[n].dest = dest, t[n].size = (s32)dsize, t[n].attr = 0, n++;
  t[n].src = (u32)UNCACHED(pkt), t[n].dest = iop_buf, t[n].size = (s32)psize;
  t[n].attr = 0x44; /* ERT | INT_O */
  n++;
  __asm__ volatile("sync.l");
  while (!sif_set_dma(t, n))
    ;
}

static int sif_init(void) {
  static const s32 sema_param[6] = {0, 0x7fff, 0, 0, 0, 0}; /* count, max */
  sema = create_sema((void *)sema_param);
  if (R32(0x1000E010) & (1u << 5)) /* D_STAT: a SIF0 end left over */
    R32(0x1000E010) = 1u << 5;
  if (!(R32(0x1000C000) & (1u << 8))) /* SIF0 not receiving */
    sif_set_dchain();
  add_dmac_handler(5, sif0_handler, 0, 0);
  enable_dmac(5);
  iop_buf = (u32)sif_get_reg(SYSREG_SUBADDR);
  if (!iop_buf || sema < 0)
    return -1; /* the IOP's SIF not set up: not after the ROM's EELOAD */
  volatile u32 *h = UNCACHED(pkt);
  h[4] = (u32)UNCACHED(rx_buf);
  send_cmd(CMD_CHANGE_SADDR, 32, 0, 0, 0);
  if (!sif_get_reg(SYSREG_RPCINIT)) {
    h[3] = 1;
    send_cmd(CMD_INIT_CMD, 16, 0, 0, 0);
    while (!sreg0)
      wait_sema(sema);
    sif_set_reg(SYSREG_RPCINIT, 1);
  }
  return 0;
}

static void rpc_send(struct client *c, u32 cid, u32 w8, const void *send, u32 ssize, void *recv,
                     u32 rsize) {
  volatile u32 *w = UNCACHED(pkt);
  for (int i = 3; i < 16; i++)
    w[i] = 0;
  w[4] = 5;
  w[5] = (u32)UNCACHED(pkt);
  w[6] = 1;
  w[7] = (u32)c;
  w[8] = w8;
  if (cid == CMD_RPC_CALL) {
    w[9] = ssize, w[10] = PHYS(recv), w[11] = rsize, w[12] = 1, w[13] = c->server;
    flush_cache(0);
  }
  c->done = 0;
  send_cmd(cid, 64, send, c->buf, cid == CMD_RPC_CALL ? ssize : 0);
  while (!c->done)
    wait_sema(sema);
}

/* ---- FILEIO ---------------------------------------------------------------- */

static struct client fio;
static union {
  struct {
    s32 flags;
    char path[256];
  } open;
  s32 w[4];
} arg ALIGNED;
static s32 res[4] ALIGNED;
static struct {
  u32 size1, size2, dest1, dest2;
  u8 buf1[16], buf2[16];
} rd ALIGNED;

static s32 fio_call(int fno, u32 size) {
  rpc_send(&fio, CMD_RPC_CALL, (u32)fno, &arg, size, res, 16);
  return ((volatile s32 *)UNCACHED(res))[0];
}

static int fio_open(const char *path) {
  arg.open.flags = 1; /* O_RDONLY */
  u32 i = 0;
  for (; path[i] && i < 255; i++)
    arg.open.path[i] = path[i];
  arg.open.path[i] = 0;
  return fio_call(0, sizeof arg.open);
}
static void fio_close(int fd) {
  arg.w[0] = fd;
  fio_call(1, 16);
}
static int fio_lseek(int fd, s32 off) {
  arg.w[0] = fd, arg.w[1] = off, arg.w[2] = 0;
  return fio_call(4, 16);
}
/* n bytes at the file position into d: the IOP DMAs the 16-byte aligned
   middle, and sends the bytes before and after it in rd */
static int fio_read(int fd, void *d, u32 n) {
  flush_cache(0); /* no dirty line (a neighbour's) is lost to the invalidate */
  dcache_invalidate(d, n);
  volatile __typeof__(rd) *r = UNCACHED(&rd);
  r->size1 = r->size2 = 0;
  arg.w[0] = fd, arg.w[1] = (s32)PHYS(d), arg.w[2] = (s32)n, arg.w[3] = (s32)PHYS(&rd);
  int got = fio_call(2, 16);
  dcache_invalidate(d, n);
  if (got > 0) {
    u8 *b = d;
    for (u32 i = 0; i < r->size1; i++)
      b[r->dest1 - PHYS(d) + i] = r->buf1[i];
    for (u32 i = 0; i < r->size2; i++)
      b[r->dest2 - PHYS(d) + i] = r->buf2[i];
  }
  return got;
}

/* ---- phase 2 ----------------------------------------------------------------- */

struct ehdr {
  u8 ident[16];
  u16 type, machine;
  u32 version, entry, phoff, shoff, flags;
  u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct phdr {
  u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
extern char __mx_base[], __mx_top[]; /* phase 1, its stack included (Makefile) */

static void run(const char *path) {
  static struct ehdr eh ALIGNED;
  static struct phdr ph[8] ALIGNED;
  int fd = fio_open(path);
  if (fd < 0)
    return;
  if (fio_read(fd, &eh, sizeof eh) != sizeof eh || eh.ident[0] != 0x7f || eh.ident[1] != 'E' ||
      eh.phentsize != sizeof(struct phdr) || eh.phnum > 8 || fio_lseek(fd, (s32)eh.phoff) < 0 ||
      fio_read(fd, ph, eh.phnum * sizeof(struct phdr)) != (int)(eh.phnum * sizeof(struct phdr)))
    goto out;
  for (u32 i = 0; i < eh.phnum; i++) /* all of it fits, or nothing is loaded */
    if (ph[i].type == 1 && (ph[i].vaddr < 0x82000 || ph[i].vaddr + ph[i].memsz > 0x2000000 ||
                            ph[i].filesz > ph[i].memsz ||
                            (ph[i].vaddr < (u32)__mx_top && ph[i].vaddr + ph[i].memsz > (u32)__mx_base)))
      goto out;
  for (u32 i = 0; i < eh.phnum; i++) {
    if (ph[i].type != 1)
      continue;
    u8 *d = (u8 *)ph[i].vaddr;
    if (ph[i].filesz && (fio_lseek(fd, (s32)ph[i].offset) < 0 ||
                         fio_read(fd, d, ph[i].filesz) != (int)ph[i].filesz))
      goto out;
    memset(d + ph[i].filesz, 0, ph[i].memsz - ph[i].filesz);
  }
  fio_close(fd);
  flush_cache(0);
  flush_cache(2);
  static char *argv[1];
  argv[0] = (char *)path;
  exec_ps2((void *)eh.entry, 0, 1, argv);
out:
  fio_close(fd);
}

/* the screen red (GS: reset, SetGsCrt, both read circuits off: BGCOLOR), PAL
   on a European console (ROMVER's region letter E), else NTSC */
static void fail(void) {
  static char rv[16] ALIGNED;
  int pal = 0, fd = fio.server ? fio_open("rom0:ROMVER") : -1;
  if (fd >= 0) {
    pal = fio_read(fd, rv, 16) > 4 && rv[4] == 'E';
    fio_close(fd);
  }
  R64(0x12001000) = 0x200; /* CSR: reset */
  set_gs_crt(1, pal ? 3 : 2, 1);
  R64(0x12000000) = (1u << 7) | (1u << 5) | (1u << 2); /* PMODE: SLBG, MMOD, CRTMD 1 */
  R64(0x120000E0) = 0x0000ff;                          /* BGCOLOR: red */
  for (;;)
    sleep_thread();
}

/* filled by SetupThread (start.S): argc, argv as ExecPS2 passed them */
struct {
  s32 argc;
  char *argv[16];
  char payload[256];
} args;

int main(int argc, char **argv) {
  if (sif_init() < 0)
    fail();
  for (int i = 0; i < 1000 && !fio.server; i++) /* until FILEIO answers a bind */
    rpc_send(&fio, CMD_RPC_BIND, 0x80000001u, 0, 0, 0, 0);
  if (!fio.server)
    fail();
  char first = argc > 0 && argv[0][0] == 'm' && argv[0][1] == 'c' && argv[0][2] == '1' ? '1' : '0';
  static char path[] = "mc0:/" PHASE2;
  /* a card's first access may fail while it is detected: a few rounds */
  for (int round = 0; round < 20; round++)
    for (int s = 0; s < 2; s++) {
      path[2] = s ? (char)('0' + '1' - first) : first;
      run(path);
    }
  fail();
  return 0;
}

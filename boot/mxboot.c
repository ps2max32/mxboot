/* mxboot phase 2 (MXOS/MXBOOT.ELF): started by phase 1 (kelf/, the
   "system update" the ROM starts at power-on, installed by mxboot-install).
   It starts one ELF of the card it booted from, named by a config file:

     MXOS/BOOTONCE.CFG  once: removed, then its ELF is started. If that ELF
                        hangs, the next boot starts the default.
     MXOS/BOOT.CFG      otherwise: the default.

   A config file's first line that is not empty and does not start with '#'
   is the path of the ELF; "mc?:" in it is the card the config is on. The ELF
   gets its path as argv[0].

   Nothing else: no pad, no menu. The IOP is left as the ROM had it (the
   program started resets it), unless the card cannot be read with it: then a
   reset and the ROM's memory card drivers. If no ELF can be started the
   screen shows a colour (see fail()) and the PS2 stays there.

   Linked high (MXBOOT_BASE, Makefile), so it loads ELFs anywhere in 0x82000
   up to its own start: mxlink (0x88000), programs at 0x100000.

   Run as a plain ELF (from mxlink, for tests) it takes the card from
   argv[1] ("mc0:" or "mc1:") and only checks the ELF, unless argv[2] is
   "exec" (see test below); as the system update it takes the card from
   argv[0], else it tries mc0 then mc1. */

#include <mx/mx.h>
#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/iop.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/video.h>
#include <mx/hw/regs.h>

#define CFG_DIR "MXOS/"
#define CFG_ONCE CFG_DIR "BOOTONCE.CFG"
#define CFG_BOOT CFG_DIR "BOOT.CFG"

#define LOAD_LOW 0x00082000u
extern char __text_start[]; /* our own start: the ELF must end below it */

/* The screen colours (BGCOLOR: 0xBBGGRR) of fail() */
#define FAIL_NO_CARD 0x0000ffu /* red: no card could be read */
#define FAIL_NO_CFG 0x00ffffu  /* yellow: no MXOS/BOOT.CFG on it */
#define FAIL_NO_ELF 0xff00ffu  /* magenta: its ELF is missing or not one we load */

/* Started with a card in argv[1] (a test from a loader): end with a status
   instead of staying, and with no "exec" in argv[2] check the ELF but do
   not load it: it would go over the loader that runs this (mxlink at
   0x88000). "exec" only from one that is not where the ELF goes
   (mxlink-high). */
static int test, test_exec;

static void fail(u32 colour, const char *why) {
  mx_printf("mxboot: %s\n", why);
  if (test)
    mx_exit(colour == FAIL_NO_CARD ? 2 : colour == FAIL_NO_CFG ? 3 : 4);
  if (mx_video_set(mx_video_default()) == 0) {
    /* both read circuits off: OUT1 is BGCOLOR (SLBG 1, MMOD 1, ALP 0);
       CRTMD 1 as the manual says */
    MX_R64(MX_GS_PMODE) = (1u << 7) | (1u << 5) | (1u << 2);
    MX_R64(MX_GS_BGCOLOR) = colour;
  }
  for (;;)
    mx_bios_sleep_thread();
}

/* ---- the card ------------------------------------------------------------ */

static char slots[2] = {'0', '1'};
static int nslots = 2;

/* "mcN:" + rest into out (room bytes) */
static void card_path(char *out, u32 room, char slot, const char *rest) {
  u32 n = 0;
  const char *pre = "mc0:/";
  for (; pre[n] && n < room - 1; n++)
    out[n] = pre[n];
  out[2] = slot;
  for (u32 i = 0; rest[i] && n < room - 1; i++)
    out[n++] = rest[i];
  out[n] = 0;
}

/* Read the file at path whole into buf (room bytes, 0-terminated): its size,
   < 0 if it is not there. */
static int read_small(const char *path, char *buf, u32 room) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return -1;
  int n = mx_fio_read(fd, buf, room - 1);
  mx_fio_close(fd);
  if (n < 0)
    return -1;
  buf[n] = 0;
  return n;
}

/* Can the IOP as it is read the cards? The first access takes a card a
   while: a few tries. A card's root always opens. */
static int cards_readable(void) {
  char root[8];
  for (int t = 0; t < 10; t++) {
    for (int s = 0; s < nslots; s++) {
      card_path(root, sizeof root, slots[s], "");
      int fd = mx_fio_dopen(root);
      if (fd >= 0) {
        mx_fio_dclose(fd);
        return 1;
      }
    }
    mx_sleep_us(100000);
  }
  return 0;
}

/* ---- config -------------------------------------------------------------- */

/* The ELF path of a config's text, "mc?:" made "mcN:"; 0 if it names none. */
static int cfg_elf(char *text, char slot, char *path, u32 room) {
  char *p = text;
  while (*p) {
    char *line = p;
    while (*p && *p != '\n')
      p++;
    char *end = p;
    if (*p)
      p++;
    while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
      end--;
    while (line < end && (*line == ' ' || *line == '\t'))
      line++;
    if (line == end || *line == '#')
      continue;
    u32 n = (u32)(end - line);
    if (n >= room)
      return 0;
    memcpy(path, line, n);
    path[n] = 0;
    if (n > 3 && path[0] == 'm' && path[1] == 'c' && path[2] == '?')
      path[2] = slot;
    return 1;
  }
  return 0;
}

/* ---- the ELF ------------------------------------------------------------- */

struct ehdr {
  u8 ident[16];
  u16 type, machine;
  u32 version, entry, phoff, shoff, flags;
  u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct phdr {
  u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
#define PT_LOAD 1
#define MAX_PHDRS 16

/* Load the ELF at path and start it with ExecPS2. Returns only when it is
   not there or not one to load (memory is changed only when every segment
   fits). A test without "exec" ends after the checks, with status 0. */
static void run_elf(const char *path) {
  static struct ehdr eh;
  static struct phdr ph[MAX_PHDRS];
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0) {
    mx_printf("mxboot: %s: not there\n", path);
    return;
  }
  const char *why = 0;
  u32 top = (u32)(uintptr_t)__text_start;
  if (mx_fio_read(fd, &eh, sizeof eh) != sizeof eh || eh.ident[0] != 0x7f ||
      eh.ident[1] != 'E' || eh.ident[2] != 'L' || eh.ident[3] != 'F' || eh.machine != 8)
    why = "not a MIPS ELF";
  else if (eh.phentsize != sizeof(struct phdr) || eh.phnum == 0 || eh.phnum > MAX_PHDRS ||
           mx_fio_lseek(fd, (s32)eh.phoff, MX_FIO_SEEK_SET) != (int)eh.phoff ||
           mx_fio_read(fd, ph, eh.phnum * sizeof(struct phdr)) != (int)(eh.phnum * sizeof(struct phdr)))
    why = "cannot read its program headers";
  for (u32 i = 0; !why && i < eh.phnum; i++) {
    const struct phdr *p = &ph[i];
    if (p->type != PT_LOAD || p->memsz == 0)
      continue;
    if (p->filesz > p->memsz || p->vaddr < LOAD_LOW || p->vaddr + p->memsz > top ||
        p->vaddr + p->memsz < p->vaddr)
      why = "a segment is outside 0x82000 up to mxboot";
  }
  if (!why && test && !test_exec) {
    mx_fio_close(fd);
    mx_printf("mxboot: would start %s, entry %08x (a test: not loaded)\n", path, eh.entry);
    mx_exit(0);
  }
  for (u32 i = 0; !why && i < eh.phnum; i++) {
    const struct phdr *p = &ph[i];
    if (p->type != PT_LOAD || p->memsz == 0)
      continue;
    u8 *d = (u8 *)(uintptr_t)p->vaddr;
    if (p->filesz && (mx_fio_lseek(fd, (s32)p->offset, MX_FIO_SEEK_SET) != (int)p->offset ||
                      mx_fio_read(fd, d, p->filesz) != (int)p->filesz))
      why = "cannot read a segment";
    else
      memset(d + p->filesz, 0, p->memsz - p->filesz);
  }
  mx_fio_close(fd);
  if (why) {
    mx_printf("mxboot: %s: %s\n", path, why);
    return;
  }
  mx_printf("mxboot: start %s\n", path);
  mx_debug_flush();
  mx_bios_flush_cache(0);
  mx_bios_flush_cache(2);
  static char argv0[128];
  for (u32 i = 0; i < sizeof argv0 - 1 && path[i]; i++)
    argv0[i] = path[i];
  char *argv[1] = {argv0};
  mx_sif_exit();
  mx_bios_exec_ps2((void *)(uintptr_t)eh.entry, 0, 1, argv);
}

/* ---- the drive ----------------------------------------------------------- */

/* Unlock the CD/DVD drive for discs (what the ROM's browser does before it
   boots anything): CDVDFSV's S-command 0x1E with ROMVER's version and
   region, e.g. "0160HC" = 1, 60, 'H', 'C'. Early models do not have it:
   the result does not matter. */
#define CDVD_SCMD_SID 0x80000593u
#define CDVD_BOOT_CERTIFY 0x1E
static void boot_certify(void) {
  static struct mx_sif_client c;
  static u8 send[16] MX_DMA_LINES;
  static s32 recv[4] MX_DMA_LINES;
  char rv[16];
  if (read_small("rom0:ROMVER", rv, sizeof rv) < 6 || mx_sif_bind_tries(&c, CDVD_SCMD_SID, 50) < 0 ||
      !c.server)
    return;
  send[0] = (u8)((rv[0] - '0') * 10 + (rv[1] - '0'));
  send[1] = (u8)((rv[2] - '0') * 10 + (rv[3] - '0'));
  send[2] = (u8)rv[4];
  send[3] = (u8)rv[5];
  mx_sif_call(&c, CDVD_BOOT_CERTIFY, send, 16, (void *)MX_UNCACHED(recv), 4);
}

/* ---- main ---------------------------------------------------------------- */

static int slot_of(const char *s) {
  return s && s[0] == 'm' && s[1] == 'c' && (s[2] == '0' || s[2] == '1') && s[3] == ':' ? s[2] : 0;
}

int main(int argc, char **argv) {
  int slot = argc > 1 ? slot_of(argv[1]) : 0;
  if (slot) {
    slots[0] = (char)slot; /* a test: that card only */
    nslots = 1;
    test = 1;
    test_exec = argc > 2 && !strcmp(argv[2], "exec");
  } else if (argc > 0 && (slot = slot_of(argv[0]))) {
    slots[0] = (char)slot;
    slots[1] = slot == '0' ? '1' : '0';
  }

  if (mx_sif_init() < 0 || !cards_readable()) {
    if (mx_iop_reset("") < 0 || mx_iop_load_module("rom0:SIO2MAN") < 0 ||
        mx_iop_load_module("rom0:MCMAN") < 0 || mx_iop_load_module("rom0:MCSERV") < 0 ||
        !cards_readable())
      fail(FAIL_NO_CARD, "no memory card can be read");
  }
  /* the ROM's FILEIO: remove goes on into mkdir without this */
  mx_iop_patch_fileio();
  boot_certify();

  static char text[512], path[256], cfg[64];
  int any_cfg = 0;
  for (int s = 0; s < nslots; s++) {
    card_path(cfg, sizeof cfg, slots[s], CFG_ONCE);
    if (read_small(cfg, text, sizeof text) >= 0) {
      any_cfg = 1;
      /* gone before its ELF runs, or it does not run: a hang must not
         come back at the next boot */
      int ok = mx_fio_remove(cfg) >= 0 && read_small(cfg, path, sizeof path) < 0;
      if (!ok)
        mx_printf("mxboot: %s: cannot remove it, not started\n", cfg);
      else if (cfg_elf(text, slots[s], path, sizeof path))
        run_elf(path);
      else
        mx_printf("mxboot: %s: names no ELF\n", cfg);
    }
    card_path(cfg, sizeof cfg, slots[s], CFG_BOOT);
    if (read_small(cfg, text, sizeof text) >= 0) {
      any_cfg = 1;
      if (cfg_elf(text, slots[s], path, sizeof path))
        run_elf(path);
      else
        mx_printf("mxboot: %s: names no ELF\n", cfg);
    }
  }
  fail(any_cfg ? FAIL_NO_ELF : FAIL_NO_CFG, any_cfg ? "no ELF could be started" : "no " CFG_BOOT);
  return 0;
}

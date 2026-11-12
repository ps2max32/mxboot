/* mxboot-install: puts mxboot on a memory card, from a loader with host:
   (mxlink). Commands (mcN: is mc0: or mc1:):

     update mcN: <MXBOOT.XLF> <MXBOOT.ELF> [force]
         Write phase 2 (MXBOOT.ELF) to MXBOOT/MXBOOT.ELF, then bind phase 1 (the
         KELF) to the card and write it as the system update of every model
         with ROM v1.10 or later, all regions (paths[] below). Existing
         update files are kept unless "force": then they are copied to
         host:mxboot-backup-<dir>-<file> first. The IOP is rebooted with the
         open SECRMAN for the binding, so the host is gone from then on: no
         more output, the exit status says how it went (mxlink prints it).
     boot mcN: <ELF path>     MXBOOT/BOOT.CFG names that ELF (the default)
     once mcN: <ELF path>     MXBOOT/BOOTONCE.CFG names it (the next boot only)
     copy <from> <to>         copy a file (e.g. host:MXKERNEL.ELF to
                              mc1:/MXOS/MXKERNEL.ELF), read back and compared
     copydir <from> <to>      the same for each file of a directory
     rm <path>                remove a file
     format mcN: erase        format the card: everything on it is gone
     bindtest mcN: <MXBOOT.XLF>  update up to the binding, writing nothing
     list mcN:                what of mxboot is on the card
     remove mcN:              remove the update files and their directories

   mxboot-install-mxos.elf is the same as an MXOS program (mx OS plan 2h: the
   kernel runs only those): all but update, bindtest and log, which reboot
   the IOP with SECRMAN or read a log at a fixed physical address: those
   are the standalone mxboot-install.elf's (from a legacy loader).

   Exit status: 0 done, 64 usage, 65 a file to read is not there or too big,
   66 the card has update files already (no "force"), 67 the card cannot be
   read, 68 the backup failed, 70 the IOP did not come back from its
   reboot, 71 an IOP module did not load, 72 SECRMAN did not answer, 73 the
   binding failed (the KELF is not one for memory cards, or the card is not
   a MagicGate one), 74 writing failed, 75 what was written reads back
   differently. */

#include <mx/mx.h>
#include <mx/iop.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/thread.h>

#ifndef MXBOOT_INSTALL_MXOS
#include "secr.h"

extern const u8 secrman_img[], secrman_img_end[], secrsif_irx[], secrsif_irx_end[];
#endif

/* The system update paths of the models with ROM v1.10 and later (the list
   of KELFBinder, israpps). The first models (SCPH-10000, SCPH-15000: ROM
   v1.00 and v1.01, BIEXEC-SYSTEM/osdsys.elf and osd110.elf) need a kernel
   patch first and are not covered. */
static const char *const paths[] = {
    "BIEXEC-SYSTEM/osd130.elf",  /* Japan, ROM v1.20 (SCPH-18000) */
    "BIEXEC-SYSTEM/osdmain.elf", /* Japan, later models */
    "BAEXEC-SYSTEM/osd120.elf",  /* USA, ROM v1.10 (SCPH-30001, B chassis) */
    "BAEXEC-SYSTEM/osd130.elf",  /* USA, ROM v1.20 (SCPH-30001, C chassis) */
    "BAEXEC-SYSTEM/osdmain.elf", /* USA and Asia, later models */
    "BEEXEC-SYSTEM/osd130.elf",  /* Europe, ROM v1.20 (SCPH-3000x, C chassis) */
    "BEEXEC-SYSTEM/osdmain.elf", /* Europe, later models */
    "BCEXEC-SYSTEM/osdmain.elf", /* China */
};
#define NPATHS (sizeof paths / sizeof paths[0])
static const char *const dirs[] = {"BIEXEC-SYSTEM", "BAEXEC-SYSTEM", "BEEXEC-SYSTEM",
                                   "BCEXEC-SYSTEM"};
#define NDIRS (sizeof dirs / sizeof dirs[0])

#define MAX_FILE (4u << 20)
static u8 buf[MAX_FILE] __attribute__((aligned(64)));
static u8 back[MAX_FILE] __attribute__((aligned(64)));

static int port; /* of the card: 0 or 1 */

/* After the IOP reboot there is no host: what happens goes into a log in EE
   memory above the programs' (the next run's "log" prints it), as pairs of
   a step and a value. */
#ifdef MXBOOT_INSTALL_MXOS
static void diag(u32 step, s32 value) {
  (void)step, (void)value;
}
#else
#define DIAG ((volatile u32 *)0x21F00000u) /* 0x1F00000 uncached (user mode: not kseg1) */
#define DIAG_MAGIC 0x4d58424cu            /* "MXBL" */
#define DIAG_MAX 120
static void diag(u32 step, s32 value) {
  if (DIAG[0] != DIAG_MAGIC) {
    DIAG[0] = DIAG_MAGIC;
    DIAG[1] = 0;
  }
  u32 n = DIAG[1];
  if (n < DIAG_MAX) {
    DIAG[2 + 2 * n] = step;
    DIAG[3 + 2 * n] = (u32)value;
    DIAG[1] = n + 1;
  }
}
#endif

static void cat(char *out, u32 room, const char *a, const char *b, const char *c) {
  u32 n = 0;
  for (const char *s = a; s && *s && n < room - 1; s++)
    out[n++] = *s;
  for (const char *s = b; s && *s && n < room - 1; s++)
    out[n++] = *s;
  for (const char *s = c; s && *s && n < room - 1; s++)
    out[n++] = *s;
  out[n] = 0;
}

/* "mcN:/" + rest */
static void card(char *out, u32 room, const char *rest) {
  char pre[6] = "mc0:/";
  pre[2] = (char)('0' + port);
  cat(out, room, pre, rest, 0);
}

static int read_file(const char *path, u8 *to, u32 room) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return -1;
  int size = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  int n = -1;
  if (size >= 0 && (u32)size <= room && mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET) == 0)
    n = mx_fio_read(fd, to, (u32)size);
  mx_fio_close(fd);
  return n == size ? size : -1;
}

static int write_file(const char *path, const u8 *from, u32 size) {
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0) {
    mx_printf("mxboot-install: %s: open: %d\n", path, fd);
    return -1;
  }
  int w = mx_fio_write(fd, from, size), c = mx_fio_close(fd);
  if (w != 0 || c < 0) {
    mx_printf("mxboot-install: %s: write: %d (%u bytes), close: %d\n", path, w, (unsigned)size, c);
    return -1;
  }
  return 0;
}

/* write, then read back and compare: 0, 74 or 75 */
static int put(const char *path, const u8 *from, u32 size) {
  if (write_file(path, from, size) < 0) {
    mx_printf("mxboot-install: %s: cannot write it\n", path);
    return 74;
  }
  if (read_file(path, back, sizeof back) != (int)size || memcmp(back, from, size) != 0) {
    mx_printf("mxboot-install: %s: reads back differently\n", path);
    return 75;
  }
  return 0;
}

static int exists(const char *path) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return 0;
  mx_fio_close(fd);
  return 1;
}

/* The card answers: its root opens (the first access takes a while). */
static int card_ready(void) {
  char root[8];
  card(root, sizeof root, "");
  int fd = -1;
  for (int t = 0; t < 30; t++) {
    fd = mx_fio_dopen(root);
    if (fd >= 0) {
      mx_fio_dclose(fd);
      diag(1, t);
      return 1;
    }
    mx_sleep_us(100000);
  }
  diag(2, fd);
  return 0;
}

#ifndef MXBOOT_INSTALL_MXOS
static int load(const u8 *start, const u8 *end, const char *name) {
  int res = 0;
  int id = mx_iop_load_module_buffer(start, (u32)(end - start), 0, 0, &res);
  diag(10, id);
  diag(11, res);
  if (id < 0 || (res & 3) == 1) /* NO_RESIDENT_END: it did not stay */
    return -1;
  (void)name;
  return 0;
}

/* ---- update -------------------------------------------------------------- */

static int copy(const char *from, const char *to);

static int update(const char *kelf_path, const char *elf_path, int force, int dry) {
  if (!card_ready()) {
    mx_printf("mxboot-install: mc%d: cannot be read\n", port);
    return 67;
  }
  char path[64], host[96];
  int found = 0;
  for (u32 i = 0; i < NPATHS; i++) {
    card(path, sizeof path, paths[i]);
    if (!exists(path))
      continue;
    found++;
    if (!force || dry) {
      mx_printf("mxboot-install: %s is there already\n", path);
      continue;
    }
    int n = read_file(path, back, sizeof back);
    char flat[40];
    cat(flat, sizeof flat, paths[i], 0, 0);
    for (char *c = flat; *c; c++)
      if (*c == '/')
        *c = '-';
    cat(host, sizeof host, "host:mxboot-backup-", flat, 0);
    if (n < 0 || write_file(host, back, (u32)n) < 0) {
      mx_printf("mxboot-install: %s: backup to %s failed\n", path, host);
      return 68;
    }
    mx_printf("mxboot-install: %s backed up to %s (%d bytes)\n", path, host, n);
  }
  if (found && !force && !dry) {
    mx_printf("mxboot-install: the card has a system update: not changed (add \"force\")\n");
    return 66;
  }
  /* phase 2 first, while the host is there: a card never gets phase 1
     without it */
  if (!dry) {
    char dir[16];
    card(dir, sizeof dir, "MXBOOT");
    mx_fio_mkdir(dir, 0777); /* there already: fine */
    card(path, sizeof path, "MXBOOT/MXBOOT.ELF");
    int r = copy(elf_path, path);
    if (r)
      return r;
  }
  int size = read_file(kelf_path, buf, sizeof buf);
  if (size < 0x80) {
    mx_printf("mxboot-install: %s: not there or not a KELF\n", kelf_path);
    return 65;
  }

  mx_printf("mxboot-install: %s (%d bytes) -> mc%d, %u paths. Rebooting the IOP with the open "
            "SECRMAN: no output from here on, the exit status tells the result\n",
            kelf_path, size, port, (unsigned)NPATHS);
  mx_debug_flush();

  DIAG[0] = 0; /* a new log */
  int rr = mx_iop_reboot_image(secrman_img, (u32)(secrman_img_end - secrman_img));
  diag(20, rr);
  if (rr < 0)
    return 70;
  struct mx_iop_module m;
  diag(21, mx_iop_find_module("secrman_special", &m));
  diag(22, m.version);
  diag(23, mx_iop_patch_fileio());
  /* the ROM's memory card drivers: they register with the ROM's IOMAN, which
     the ROM's FILEIO uses (ps2sdk's mcman registers with IOMANX's AddDrv,
     which FILEIO does not reach), and they link to the SECRMAN there is:
     ours */
  int s1 = mx_iop_load_module("rom0:SIO2MAN"), s2 = s1 < 0 ? -1 : mx_iop_load_module("rom0:MCMAN");
  diag(12, s1);
  diag(13, s2);
  if (s1 < 0 || s2 < 0 || load(secrsif_irx, secrsif_irx_end, "secrsif") < 0)
    return 71;
  if (secr_init() < 0)
    return 72;
  for (int p = 0; p < 2; p++) { /* both cards, for the log */
    int keep = port;
    port = p;
    diag(30 + (u32)p, card_ready());
    port = keep;
  }
  if (!card_ready())
    return 67;
  int b = secr_bind_kelf(port, buf);
  diag(40, b);
  if (b < 0)
    return 73;
  if (dry)
    return 0; /* bindtest: bound, nothing written */

  for (u32 i = 0; i < NDIRS; i++) {
    card(path, sizeof path, dirs[i]);
    mx_fio_mkdir(path, 0777); /* there already: fine */
  }
  for (u32 i = 0; i < NPATHS; i++) {
    card(path, sizeof path, paths[i]);
    int r = put(path, buf, (u32)size);
    if (r)
      return r;
  }
  return 0;
}

#endif /* !MXBOOT_INSTALL_MXOS */

/* ---- the others ---------------------------------------------------------- */

static int config(const char *name, const char *elf) {
  if (!card_ready())
    return 67;
  char path[64], dir[16];
  card(dir, sizeof dir, "MXBOOT");
  mx_fio_mkdir(dir, 0777); /* there already: fine */
  card(path, sizeof path, name);
  u32 n = (u32)strlen(elf);
  if (n + 2 > sizeof buf)
    return 64;
  memcpy(buf, elf, n);
  buf[n++] = '\n';
  int r = put(path, buf, n);
  if (!r)
    mx_printf("mxboot-install: %s: %s\n", path, elf);
  return r;
}

/* from to to in pieces of buf's size (any file size: a game's ELF from
   host: to usb0:), then both read again piece by piece and compared */
static int copy(const char *from, const char *to) {
  int in = mx_fio_open(from, MX_FIO_RDONLY);
  int size = in < 0 ? -1 : mx_fio_lseek(in, 0, MX_FIO_SEEK_END);
  if (size < 0 || mx_fio_lseek(in, 0, MX_FIO_SEEK_SET) != 0) {
    if (in >= 0)
      mx_fio_close(in);
    mx_printf("mxboot-install: %s: not there\n", from);
    return 65;
  }
  int out = mx_fio_open(to, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (out < 0) {
    mx_fio_close(in);
    mx_printf("mxboot-install: %s: open: %d\n", to, out);
    return 74;
  }
  int r = 0;
  for (u32 done = 0; done < (u32)size && !r;) {
    u32 n = (u32)size - done < sizeof buf ? (u32)size - done : sizeof buf;
    if (mx_fio_read(in, buf, n) != (int)n)
      r = 65;
    else if (mx_fio_write(out, buf, n) != 0)
      r = 74;
    done += n;
  }
  mx_fio_close(in);
  if (mx_fio_close(out) < 0 && !r)
    r = 74;
  if (r) {
    mx_printf("mxboot-install: %s -> %s: cannot %s it\n", from, to, r == 65 ? "read" : "write");
    return r;
  }
  /* read back */
  in = mx_fio_open(from, MX_FIO_RDONLY);
  out = mx_fio_open(to, MX_FIO_RDONLY);
  if (in < 0 || out < 0 || mx_fio_lseek(out, 0, MX_FIO_SEEK_END) != size ||
      mx_fio_lseek(out, 0, MX_FIO_SEEK_SET) != 0)
    r = 75;
  for (u32 done = 0; done < (u32)size && !r;) {
    u32 n = (u32)size - done < sizeof buf ? (u32)size - done : sizeof buf;
    if (mx_fio_read(in, buf, n) != (int)n || mx_fio_read(out, back, n) != (int)n ||
        memcmp(buf, back, n) != 0)
      r = 75;
    done += n;
  }
  if (in >= 0)
    mx_fio_close(in);
  if (out >= 0)
    mx_fio_close(out);
  if (r)
    mx_printf("mxboot-install: %s: reads back differently\n", to);
  else
    mx_printf("mxboot-install: %s -> %s (%d bytes)\n", from, to, size);
  return r;
}

/* every file of directory from (not its subdirectories) into to, which is
   made if it is not there */
static int copy_dir(const char *from, const char *to) {
  static char names[64][40];
  u32 sizes[64], n = 0;
  int fd = mx_fio_dopen(from);
  if (fd < 0) {
    mx_printf("mxboot-install: %s: not a directory\n", from);
    return 65;
  }
  char name[40];
  u32 size;
  while (n < 64 && mx_fio_dread(fd, name, sizeof name, &size) > 0)
    if (name[0] != '.') {
      memcpy(names[n], name, sizeof name);
      sizes[n++] = size;
    }
  mx_fio_dclose(fd);
  mx_fio_mkdir(to, 0777); /* there already: fine */
  for (u32 i = 0; i < n; i++) {
    char a[96], b[96];
    cat(a, sizeof a, from, "/", names[i]);
    cat(b, sizeof b, to, "/", names[i]);
    int r = copy(a, b);
    if (r)
      return r;
  }
  (void)sizes;
  mx_printf("mxboot-install: %u files\n", (unsigned)n);
  return 0;
}

static void list_dir(const char *rest) {
  char path[64], name[64];
  card(path, sizeof path, rest);
  int fd = mx_fio_dopen(path);
  if (fd < 0)
    return;
  mx_printf("%s:\n", path);
  u32 size;
  while (mx_fio_dread(fd, name, sizeof name, &size) > 0)
    if (name[0] != '.')
      mx_printf("  %-32s %u\n", name, (unsigned)size);
  mx_fio_dclose(fd);
}

static int list(void) {
  if (!card_ready())
    return 67;
  list_dir("");
  for (u32 i = 0; i < NDIRS; i++)
    list_dir(dirs[i]);
  list_dir("MXBOOT");
  static const char *const cfgs[] = {"MXBOOT/BOOT.CFG", "MXBOOT/BOOTONCE.CFG"};
  char path[64];
  for (int i = 0; i < 2; i++) {
    card(path, sizeof path, cfgs[i]);
    int n = read_file(path, buf, 256);
    if (n >= 0) {
      buf[n] = 0;
      mx_printf("%s: %s", path, (const char *)buf);
    }
  }
  return 0;
}

static int remove_update(void) {
  if (!card_ready())
    return 67;
  mx_iop_patch_fileio();
  char path[64];
  for (u32 i = 0; i < NPATHS; i++) {
    card(path, sizeof path, paths[i]);
    if (exists(path))
      mx_printf("mxboot-install: remove %s: %s\n", path, mx_fio_remove(path) < 0 ? "failed" : "done");
  }
  for (u32 i = 0; i < NDIRS; i++) {
    card(path, sizeof path, dirs[i]);
    int fd = mx_fio_dopen(path);
    if (fd < 0)
      continue;
    mx_fio_dclose(fd);
    mx_printf("mxboot-install: remove %s/: %s\n", path,
              mx_fio_rmdir(path) < 0 ? "failed (not empty?)" : "done");
  }
  return 0;
}

/* Format the card through MCSERV (RPC 0x80000400, as ps2sdk's libmc): its
   init tells the interface (XMCSERV's function numbers, or the old MCSERV's
   when the new init is refused with sceMcResDeniedPermit), then FORMAT with
   port and slot. Everything on the card is gone. */
#define MCSERV_SID 0x80000400u
static int format(void) {
  /* sent and answered by SIF DMA: core (an MXOS program's statics are VM) */
  static struct mx_sif_client mc MX_CORE_DATA;
  static s32 desc[12] __attribute__((aligned(64))) MX_CORE_DATA; /* mcDescParam_t */
  static s32 res[4] __attribute__((aligned(64))) MX_CORE_DATA;
  volatile s32 *r = MX_UNCACHED(res);
  if (mx_sif_bind_tries(&mc, MCSERV_SID, 100) < 0 || !mc.server) {
    mx_printf("mxboot-install: no MCSERV on the IOP\n");
    return 72;
  }
  memset(desc, 0, sizeof desc);
  desc[0] = -1; /* FLUSH of no file: the result is sceMcResDeniedPermit (-5) */
  mx_sif_call(&mc, 0x0A, desc, sizeof desc, (void *)r, 4);
  mx_sif_call(&mc, 0x7A, desc, sizeof desc, (void *)r, 4);
  r[0] = 0;
  mx_sif_call(&mc, 0xFE, desc, sizeof desc, (void *)r, 12); /* XMCSERV's INIT */
  int xmc = r[0] != -5;
  if (!xmc)
    mx_sif_call(&mc, 0x70, desc, sizeof desc, (void *)r, 12); /* MCSERV's */
  mx_printf("mxboot-install: MCSERV %s (versions %x, %x); formatting mc%d\n", xmc ? "new" : "old",
            r[1], r[2], port);
  mx_debug_flush();
  memset(desc, 0, sizeof desc);
  desc[1] = port;
  desc[2] = 0;
  r[0] = -1;
  mx_sif_call(&mc, xmc ? 0x10 : 0x77, desc, sizeof desc, (void *)r, 4);
  mx_printf("mxboot-install: format mc%d: %d\n", port, r[0]);
  return r[0] < 0 ? 74 : 0;
}

/* The IOP's modules (LOADCORE's list from 0x800, as mx_iop_find_module) */
static int mods(void) {
  struct mx_iop_module m;
  char name[40];
  u32 a = 0x800;
  for (int k = 0; a && k < 256; k++, a = m.next) {
    if (mx_iop_read(a, &m, sizeof m) < 0)
      return 67;
    name[0] = 0;
    if (m.name && mx_iop_read(m.name, name, sizeof name - 1) >= 0)
      name[sizeof name - 1] = 0;
    mx_printf("mod %2u %-28s %x.%02x text %06x\n", m.id, name, m.version >> 8, m.version & 0xff,
              (unsigned)m.text_start);
  }
  return 0;
}

static int slot_arg(const char *s) {
  if (!s || s[0] != 'm' || s[1] != 'c' || (s[2] != '0' && s[2] != '1') || s[3] != ':' || s[4])
    return -1;
  port = s[2] - '0';
  return 0;
}

static int usage(void) {
  mx_printf("usage: mxboot-install update mcN: <MXBOOT.XLF> <MXBOOT.ELF> [force] | boot mcN: <elf> | "
            "once mcN: <elf> | copy <from> <to> | copydir <from> <to> | rm <path> | format mcN: erase | list mcN: | remove mcN:\n");
  return 64;
}

#ifdef MXBOOT_INSTALL_MXOS
/* The memory card's drivers: the kernel's, on the PS2 (it loads them for
   its own files); where the IOP has none (the kernel's PCSX2 build, which
   keeps PCSX2's IOP), the ROM's. */
static void mc_drivers(void) {
  static struct mx_sif_client mc MX_CORE_DATA;
  if (mx_sif_bind_tries(&mc, 0x80000400u /* MCSERV */, 2) == 0 && mc.server)
    return;
  int a = mx_iop_load_module("rom0:SIO2MAN"), b = mx_iop_load_module("rom0:MCMAN");
  int c = mx_iop_load_module("rom0:MCSERV");
  mx_printf("mxboot-install: the ROM's card drivers: SIO2MAN %d, MCMAN %d, MCSERV %d\n", a, b, c);
}
#endif

static int run(int argc, char **argv) {
  if (argc < 3)
    return usage();
  const char *cmd = argv[1];
#ifdef MXBOOT_INSTALL_MXOS
  mc_drivers();
#endif
  if (!strcmp(cmd, "copy"))
    return argc == 4 ? copy(argv[2], argv[3]) : usage();
  if (!strcmp(cmd, "rm") && argc == 3) {
    mx_iop_patch_fileio();
    int r = mx_fio_remove(argv[2]);
    mx_printf("mxboot-install: remove %s: %d\n", argv[2], r);
    return r < 0 ? 74 : 0;
  }
  if (!strcmp(cmd, "copydir"))
    return argc == 4 ? copy_dir(argv[2], argv[3]) : usage();
  if (slot_arg(argv[2]) < 0)
    return usage();
#ifdef MXBOOT_INSTALL_MXOS
  if (!strcmp(cmd, "update") || !strcmp(cmd, "bindtest")) {
    mx_printf("mxboot-install: %s reboots the IOP: the standalone mxboot-install.elf has it\n", cmd);
    return 64;
  }
#else
  if (!strcmp(cmd, "update") && (argc == 5 || (argc == 6 && !strcmp(argv[5], "force"))))
    return update(argv[3], argv[4], argc == 6, 0);
  if (!strcmp(cmd, "bindtest") && argc == 4)
    return update(argv[3], 0, 0, 1);
#endif
  if (!strcmp(cmd, "boot") && argc == 4)
    return config("MXBOOT/BOOT.CFG", argv[3]);
  if (!strcmp(cmd, "once") && argc == 4)
    return config("MXBOOT/BOOTONCE.CFG", argv[3]);
  if (!strcmp(cmd, "list") && argc == 3)
    return list();
  if (!strcmp(cmd, "mods") && argc == 3)
    return mods();
  if (!strcmp(cmd, "format") && argc == 4 && !strcmp(argv[3], "erase"))
    return format();
#ifndef MXBOOT_INSTALL_MXOS
  if (!strcmp(cmd, "log") && argc == 3) {
    if (DIAG[0] != DIAG_MAGIC) {
      mx_printf("mxboot-install: no log\n");
      return 0;
    }
    for (u32 i = 0; i < DIAG[1]; i++)
      mx_printf("log %u: step %u value %d\n", (unsigned)i, (unsigned)DIAG[2 + 2 * i],
                (int)DIAG[3 + 2 * i]);
    return 0;
  }
#endif
  if (!strcmp(cmd, "remove") && argc == 3)
    return remove_update();
  return usage();
}

int main(int argc, char **argv) {
  int r = run(argc, argv);
  mx_printf("mxboot-install: status %d\n", r);
  MX_TEST_END(r);
  return r;
}

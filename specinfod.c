#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <mach/mach.h>

static double now_s(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

/* Per-core busy % since the previous call (host_processor_info), and the CPU frequency if the kernel tells. */
static unsigned long long prev_busy[16], prev_total[16];
static void cpuload(FILE *f) {
  natural_t n = 0; processor_info_array_t info = NULL; mach_msg_type_number_t cnt = 0;
  if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n, &info, &cnt) != 0 || info == NULL) {
    fprintf(f, "cpu: host_processor_info failed\n");
    return;
  }
  processor_cpu_load_info_data_t *li = (processor_cpu_load_info_data_t *)info;
  fprintf(f, "cpu:");
  double sum = 0;
  for (natural_t i = 0; i < n && i < 16; i++) {
    unsigned long long busy = (unsigned long long)li[i].cpu_ticks[CPU_STATE_USER] + li[i].cpu_ticks[CPU_STATE_SYSTEM] + li[i].cpu_ticks[CPU_STATE_NICE];
    unsigned long long total = busy + li[i].cpu_ticks[CPU_STATE_IDLE];
    unsigned long long db = busy - prev_busy[i], dt = total - prev_total[i];
    double pct = dt ? 100.0 * db / dt : 0;
    fprintf(f, " c%u=%.0f%%", (unsigned)i, pct);
    sum += pct;
    prev_busy[i] = busy; prev_total[i] = total;
  }
  uint64_t hz = 0, hzmax = 0; size_t l = sizeof(hz);
  sysctlbyname("hw.cpufrequency", &hz, &l, 0, 0);
  l = sizeof(hzmax); sysctlbyname("hw.cpufrequency_max", &hzmax, &l, 0, 0);
  fprintf(f, " avg=%.0f%% freq=%lluMHz max=%lluMHz\n", n ? sum / n : 0, hz / 1000000ULL, hzmax / 1000000ULL);
  vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(integer_t));
}

/*
 * Which kernel services did each process open? Every IOUserClient carries IOUserClientCreator
 * ("pid N, name"). backboardd holding an accelerator/GPU user client = hardware rendering;
 * only IOSurfaceRoot + IOMobileFramebuffer = CPU rendering. IOKit and CoreFoundation are
 * loaded with dlopen, so no extra frameworks are needed at build time.
 */
typedef unsigned int io_obj_t;
typedef char io_name_t[128];
typedef const void *CFRefT;
static int (*p_Iter)(unsigned, const char *, unsigned, io_obj_t *);
static io_obj_t (*p_Next)(io_obj_t);
static int (*p_Rel)(io_obj_t);
static int (*p_Class)(io_obj_t, io_name_t);
static int (*p_Name)(io_obj_t, io_name_t);
static CFRefT (*p_Prop)(io_obj_t, CFRefT, CFRefT, unsigned);
static CFRefT (*p_MkStr)(CFRefT, const char *, unsigned);
static unsigned char (*p_GetCStr)(CFRefT, char *, long, unsigned);
static void (*p_CFRel)(CFRefT);
static unsigned long (*p_TypeID)(CFRefT);
static unsigned long (*p_StrTypeID)(void);
static int io_ready = -1;

static int io_load(void) {
  if (io_ready >= 0) return io_ready;
  void *k = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
  void *c = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
  if (!k || !c) return io_ready = 0;
  *(void **)&p_Iter = dlsym(k, "IORegistryCreateIterator");
  *(void **)&p_Next = dlsym(k, "IOIteratorNext");
  *(void **)&p_Rel = dlsym(k, "IOObjectRelease");
  *(void **)&p_Class = dlsym(k, "IOObjectGetClass");
  *(void **)&p_Name = dlsym(k, "IORegistryEntryGetName");
  *(void **)&p_Prop = dlsym(k, "IORegistryEntryCreateCFProperty");
  *(void **)&p_MkStr = dlsym(c, "CFStringCreateWithCString");
  *(void **)&p_GetCStr = dlsym(c, "CFStringGetCString");
  *(void **)&p_CFRel = dlsym(c, "CFRelease");
  *(void **)&p_TypeID = dlsym(c, "CFGetTypeID");
  *(void **)&p_StrTypeID = dlsym(c, "CFStringGetTypeID");
  return io_ready = (p_Iter && p_Next && p_Rel && p_Class && p_Name && p_Prop && p_MkStr && p_GetCStr && p_CFRel && p_TypeID && p_StrTypeID) ? 1 : 0;
}

static int has_word(const char *s, const char *w) {
  size_t n = strlen(w);
  for (; *s; s++) {
    size_t i = 0;
    while (i < n && s[i] && tolower((unsigned char)s[i]) == w[i]) i++;
    if (i == n) return 1;
  }
  return 0;
}

static void iosnap(FILE *f, double t) {
  if (!io_load()) { fprintf(f, "io: IOKit/CoreFoundation tidak bisa dimuat\n"); return; }
  io_obj_t it = 0;
  if (p_Iter(0, "IOService", 1, &it) != 0 || !it) { fprintf(f, "io: registry iterator gagal\n"); return; }
  CFRefT key = p_MkStr(NULL, "IOUserClientCreator", 0x08000100);
  static const char *svcwords[] = {"accel", "gpu", "adreno", "topaz", "redwood", "agx", "sgx", "metal", "framebuffer", "clcd", "iosurface", NULL};
  char seen[48][200]; int nseen = 0, lines = 0;
  char tcr[24][96]; int tcn[24]; int ntc = 0;     /* jumlah user client IOSurface per pembuat */
  fprintf(f, "io: t=%.0fs\n", t);
  io_obj_t e;
  while ((e = p_Next(it)) != 0) {
    io_name_t cls = {0}, nm = {0};
    p_Class(e, cls); p_Name(e, nm);
    char creator[96] = "";
    CFRefT v = p_Prop(e, key, NULL, 0);
    if (v) {
      if (p_TypeID(v) == p_StrTypeID()) p_GetCStr(v, creator, sizeof(creator), 0x08000100);
      p_CFRel(v);
    }
    char line[200] = "";
    if (creator[0] && has_word(cls, "iosurface")) {
      int k = 0;
      while (k < ntc && strcmp(tcr[k], creator)) k++;
      if (k == ntc && ntc < 24) { snprintf(tcr[k], sizeof(tcr[k]), "%s", creator); tcn[k] = 0; ntc++; }
      if (k < ntc) tcn[k]++;
    }
    if (creator[0]) {
      snprintf(line, sizeof(line), "io: uc %s <- %s", cls, creator);
    } else {
      for (int i = 0; svcwords[i]; i++) {
        if (has_word(cls, svcwords[i]) || has_word(nm, svcwords[i])) { snprintf(line, sizeof(line), "io: svc %s (%s)", cls, nm); break; }
      }
    }
    if (line[0] && lines < 70) {
      int dup = 0;
      for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], line)) { dup = 1; break; }
      if (!dup) {
        if (nseen < 48) { strncpy(seen[nseen], line, 199); seen[nseen][199] = 0; nseen++; }
        fprintf(f, "%s\n", line); lines++;
      }
    }
    p_Rel(e);
  }
  p_Rel(it);
  for (int k = 0; k < ntc; k++) fprintf(f, "io: iosurface-clients %s x%d\n", tcr[k], tcn[k]);
  if (key) p_CFRel(key);
}

static void bootinfo(FILE *f) {
  char b[512] = {0}; size_t l = sizeof(b) - 1;
  if (sysctlbyname("kern.bootargs", b, &l, 0, 0) == 0) fprintf(f, "bootargs: %s\n", b);
  else fprintf(f, "bootargs: tidak terbaca\n");
  char v[128] = {0}; l = sizeof(v) - 1;
  if (sysctlbyname("kern.osversion", v, &l, 0, 0) == 0) fprintf(f, "osversion: %s\n", v);
}

static void mem(FILE *f) {
  uint64_t m = 0; size_t l = sizeof(m);
  sysctlbyname("hw.memsize", &m, &l, 0, 0);
  int ncpu = 0, act = 0;
  l = sizeof(ncpu); sysctlbyname("hw.ncpu", &ncpu, &l, 0, 0);
  l = sizeof(act);  sysctlbyname("hw.activecpu", &act, &l, 0, 0);
  vm_size_t ps = 0; host_page_size(mach_host_self(), &ps);
  vm_statistics64_data_t v; mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
  host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &c);
  double mb = 1048576.0;
  fprintf(f, "mem: hw.memsize=%.0fMB ncpu=%d activecpu=%d free=%.0f active=%.0f wired=%.0f (MB)\n",
          m / mb, ncpu, act, v.free_count * ps / mb, v.active_count * ps / mb, v.wire_count * ps / mb);
}

/* ======================================================================
 * memtop: siapa yang memegang RAM, dan kenapa tidak dilepas?
 *  1. footprint per proses (proc_pid_rusage) + pertumbuhannya sejak pertama terlihat
 *  2. selisih (RAM terpakai sistem) - (jumlah footprint proses) = RAM di sisi kernel / cache
 *  3. rincian vm per proses teratas dan per proses yang paling tumbuh, dikelompokkan per user_tag
 *  4. zone kernel terbesar dan pertumbuhannya (jika host_priv bisa didapat)
 *  5. status memorystatus / tekanan memori (apakah jetsam & compressor jalan)
 * Semua simbol libproc / task_for_pid / mach_zone_info diambil dengan dlsym supaya build tidak
 * butuh header privat dan tidak gagal link di SDK iOS.
 * ====================================================================== */
#include <stdlib.h>

struct ru0 {   /* tata letak rusage_info_v0 */
  uint8_t uuid[16];
  uint64_t user, sys, pkg_idle, intr_wkups, pageins, wired, resident, footprint, start, exit_t;
};
typedef struct { char name[80]; } zname_t;
typedef struct { uint64_t count, cur, max, elem, alloc, sum, exh, coll; } zinfo_t;

static int (*p_lp)(uint32_t, uint32_t, void *, int);
static int (*p_ru)(int, int, void *);
static int (*p_pname)(int, void *, uint32_t);
static kern_return_t (*p_tfp)(mach_port_t, int, mach_port_t *);
static kern_return_t (*p_hpriv)(mach_port_t, mach_port_t *);
static kern_return_t (*p_zi)(mach_port_t, zname_t **, mach_msg_type_number_t *, zinfo_t **, mach_msg_type_number_t *);
static kern_return_t (*p_hsp)(mach_port_t, int, int, mach_port_t *);   /* host_get_special_port */
static kern_return_t (*p_mmi)(mach_port_t, zname_t **, mach_msg_type_number_t *, zinfo_t **, mach_msg_type_number_t *, void **, mach_msg_type_number_t *);
static int sym_ready = 0;

static void sym_load(void) {
  if (sym_ready) return;
  sym_ready = 1;
  void *h = RTLD_DEFAULT;
  *(void **)&p_lp = dlsym(h, "proc_listpids");
  *(void **)&p_ru = dlsym(h, "proc_pid_rusage");
  *(void **)&p_pname = dlsym(h, "proc_name");
  *(void **)&p_tfp = dlsym(h, "task_for_pid");
  *(void **)&p_hpriv = dlsym(h, "host_get_host_priv_port");
  *(void **)&p_zi = dlsym(h, "mach_zone_info");
  *(void **)&p_hsp = dlsym(h, "host_get_special_port");
  *(void **)&p_mmi = dlsym(h, "mach_memory_info");
  void *k = dlopen("/usr/lib/system/libsystem_kernel.dylib", RTLD_NOW);
  if (k) {
    if (!p_tfp)   *(void **)&p_tfp = dlsym(k, "task_for_pid");
    if (!p_hpriv) *(void **)&p_hpriv = dlsym(k, "host_get_host_priv_port");
    if (!p_zi)    *(void **)&p_zi = dlsym(k, "mach_zone_info");
    if (!p_hsp)   *(void **)&p_hsp = dlsym(k, "host_get_special_port");
    if (!p_mmi)   *(void **)&p_mmi = dlsym(k, "mach_memory_info");
  }
}

static double MB(uint64_t x) { return x / 1048576.0; }

static long sysint(const char *name, int *okp) {
  long long v = 0; size_t l = sizeof(v);
  int r = sysctlbyname(name, &v, &l, NULL, 0);
  if (r != 0) { *okp = 0; return 0; }
  *okp = 1;
  if (l == sizeof(int)) return *(int *)&v;
  return (long)v;
}

/* Ringkasan vm lengkap: anonim vs file-cache vs wired vs terkompresi, plus tekanan memori. */
static void sysmem(FILE *f) {
  vm_size_t ps = 0; host_page_size(mach_host_self(), &ps);
  vm_statistics64_data_t v; mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &c) != 0) { fprintf(f, "vm: host_statistics64 gagal\n"); return; }
  double pm = ps / 1048576.0;
  fprintf(f, "vm: free=%.0f spec=%.0f active=%.0f inactive=%.0f wired=%.0f compressor=%.0f purgeable=%.0f "
             "internal(anon)=%.0f external(file)=%.0f throttled=%.0f (MB) compressions=%llu decompress=%llu purges=%llu pageins=%llu pageouts=%llu\n",
          v.free_count * pm, v.speculative_count * pm, v.active_count * pm, v.inactive_count * pm, v.wire_count * pm,
          v.compressor_page_count * pm, v.purgeable_count * pm, v.internal_page_count * pm, v.external_page_count * pm,
          v.throttled_count * pm,
          (unsigned long long)v.compressions, (unsigned long long)v.decompressions, (unsigned long long)v.purges,
          (unsigned long long)v.pageins, (unsigned long long)v.pageouts);
  int o1, o2, o3, o4; long lvl = sysint("kern.memorystatus_level", &o1);
  long pr = sysint("kern.memorystatus_vm_pressure_level", &o2);
  long cm = sysint("vm.compressor_mode", &o3);
  long jt = sysint("kern.memorystatus_jetsam_enabled", &o4);
  fprintf(f, "pressure: memorystatus_level=%ld%% vm_pressure_level=%ld compressor_mode=%ld jetsam_enabled=%ld (-1 = sysctl tidak ada)\n",
          o1 ? lvl : -1, o2 ? pr : -1, o3 ? cm : -1, o4 ? jt : -1);
}

/* ---- per proses ---- */
#define MAXP 512
static struct { int pid; char name[32]; uint64_t first, last; int seen; } ptab[MAXP];
static int nptab = 0;
struct row { int pid; char name[32]; uint64_t fp, res, wired, first, prev; };

static int cmp_fp(const void *a, const void *b) {
  const struct row *x = a, *y = b;
  return x->fp < y->fp ? 1 : (x->fp > y->fp ? -1 : 0);
}

/* Rincian ruang alamat satu proses per user_tag (MALLOC, IOKit, IOSurface, ...). Perlu task_for_pid. */
static const struct { int tag; const char *nm; } tagnames[] = {
#define TG(x) {VM_MEMORY_##x, #x},
#ifdef VM_MEMORY_MALLOC
  TG(MALLOC)
#endif
#ifdef VM_MEMORY_MALLOC_SMALL
  TG(MALLOC_SMALL)
#endif
#ifdef VM_MEMORY_MALLOC_LARGE
  TG(MALLOC_LARGE)
#endif
#ifdef VM_MEMORY_MALLOC_TINY
  TG(MALLOC_TINY)
#endif
#ifdef VM_MEMORY_MALLOC_NANO
  TG(MALLOC_NANO)
#endif
#ifdef VM_MEMORY_STACK
  TG(STACK)
#endif
#ifdef VM_MEMORY_IOKIT
  TG(IOKIT)
#endif
#ifdef VM_MEMORY_IOSURFACE
  TG(IOSURFACE)
#endif
#ifdef VM_MEMORY_IOACCELERATOR
  TG(IOACCELERATOR)
#endif
#ifdef VM_MEMORY_COREGRAPHICS
  TG(COREGRAPHICS)
#endif
#ifdef VM_MEMORY_COREGRAPHICS_DATA
  TG(COREGRAPHICS_DATA)
#endif
#ifdef VM_MEMORY_COREGRAPHICS_FRAMEBUFFERS
  TG(COREGRAPHICS_FRAMEBUFFERS)
#endif
#ifdef VM_MEMORY_COREGRAPHICS_BACKINGSTORES
  TG(COREGRAPHICS_BACKINGSTORES)
#endif
#ifdef VM_MEMORY_OPENGL_GL
  TG(OPENGL_GL)
#endif
#ifdef VM_MEMORY_DYLIB
  TG(DYLIB)
#endif
#ifdef VM_MEMORY_ANALYTICS
  TG(ANALYTICS)
#endif
#ifdef VM_MEMORY_LAYERKIT
  TG(LAYERKIT)
#endif
#ifdef VM_MEMORY_UNSHARED_PMAP
  TG(UNSHARED_PMAP)
#endif
#ifdef VM_MEMORY_CGIMAGE
  TG(CGIMAGE)
#endif
#ifdef VM_MEMORY_IMAGEIO
  TG(IMAGEIO)
#endif
#ifdef VM_MEMORY_COREANIMATION
  TG(COREANIMATION)
#endif
#ifdef VM_MEMORY_GLSL
  TG(GLSL)
#endif
#ifdef VM_MEMORY_SHARED_PMAP
  TG(SHARED_PMAP)
#endif
  {-1, NULL}
};

static void vmdetail(FILE *f, int pid, const char *name) {
  if (!p_tfp) { fprintf(f, "vmdetail: task_for_pid tidak ada\n"); return; }
  mach_port_t task = MACH_PORT_NULL;
  if (p_tfp(mach_task_self(), pid, &task) != KERN_SUCCESS || task == MACH_PORT_NULL) {
    fprintf(f, "vmdetail: task_for_pid(%d %s) ditolak kernel\n", pid, name);
    return;
  }
  vm_size_t ps = 0; host_page_size(mach_host_self(), &ps);
  static uint64_t res[256], swp[256], reg[256], dty[256], big[256];
  memset(res, 0, sizeof(res)); memset(swp, 0, sizeof(swp)); memset(reg, 0, sizeof(reg));
  memset(dty, 0, sizeof(dty)); memset(big, 0, sizeof(big));
  vm_address_t a = 0; int guard = 0;
  for (;;) {
    vm_size_t sz = 0; natural_t depth = 0;
    vm_region_submap_info_data_64_t inf; memset(&inf, 0, sizeof(inf));
    mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (vm_region_recurse_64(task, &a, &sz, &depth, (vm_region_recurse_info_t)&inf, &cnt) != KERN_SUCCESS) break;
    if (!inf.is_submap) {
      unsigned tg = inf.user_tag & 255;
      res[tg] += (uint64_t)inf.pages_resident * ps;
      swp[tg] += (uint64_t)inf.pages_swapped_out * ps;
      dty[tg] += (uint64_t)inf.pages_dirtied * ps;
      reg[tg]++;
      if (sz > big[tg]) big[tg] = sz;
    }
    a += sz;
    if (++guard > 300000) break;
  }
  mach_port_deallocate(mach_task_self(), task);
  fprintf(f, "vmdetail: pid=%d %s (shared cache dilewati; res=resident, swp=terkompresi, dirty=pernah ditulis)\n", pid, name);
  int used[256] = {0};
  for (int k = 0; k < 8; k++) {
    int best = -1; uint64_t bv = 0;
    for (int i = 0; i < 256; i++) if (!used[i] && res[i] + swp[i] > bv) { bv = res[i] + swp[i]; best = i; }
    if (best < 0 || bv < 2 * 1048576ULL) break;
    used[best] = 1;
    char nm[40]; snprintf(nm, sizeof(nm), "tag%d", best);
    for (int j = 0; tagnames[j].nm; j++) if (tagnames[j].tag == best) { snprintf(nm, sizeof(nm), "%s", tagnames[j].nm); break; }
    fprintf(f, "vmdetail:   %-26s res=%.0fMB swp=%.0fMB dirty=%.0fMB regions=%llu largest=%.0fMB\n",
            nm, MB(res[best]), MB(swp[best]), MB(dty[best]), (unsigned long long)reg[best], MB(big[best]));
  }
}

/* Port host_priv: coba host_get_host_priv_port, lalu host_get_special_port(HOST_PRIV_PORT=1) dengan node -1 dan 0. */
static const char *hp_how = "belum dicoba";
static mach_port_t get_hpriv(void) {
  static mach_port_t cached = MACH_PORT_NULL;
  if (cached != MACH_PORT_NULL) return cached;
  mach_port_t hp = MACH_PORT_NULL; kern_return_t kr;
  if (p_hpriv) {
    kr = p_hpriv(mach_host_self(), &hp);
    if (kr == KERN_SUCCESS && hp != MACH_PORT_NULL) { hp_how = "host_get_host_priv_port"; return cached = hp; }
  }
  if (p_hsp) {
    int nodes[2] = {-1, 0};
    for (int i = 0; i < 2; i++) {
      hp = MACH_PORT_NULL;
      kr = p_hsp(mach_host_self(), nodes[i], 1, &hp);
      if (kr == KERN_SUCCESS && hp != MACH_PORT_NULL) { hp_how = "host_get_special_port"; return cached = hp; }
    }
  }
  hp_how = (!p_hpriv && !p_hsp) ? "kedua fungsi (host_get_host_priv_port, host_get_special_port) hilang"
                                : "kernel menolak (butuh root / host_priv)";
  return MACH_PORT_NULL;
}

static void zones(FILE *f) {
  if (!p_zi) { fprintf(f, "zone: simbol mach_zone_info HILANG di sistem ini\n"); return; }
  mach_port_t hp = get_hpriv();
  if (hp == MACH_PORT_NULL) { fprintf(f, "zone: host_priv tidak didapat: %s\n", hp_how); return; }
  zname_t *nm = NULL; zinfo_t *zi = NULL; mach_msg_type_number_t nn = 0, ni = 0;
  kern_return_t kr = p_zi(hp, &nm, &nn, &zi, &ni);
  if (kr != KERN_SUCCESS || !nm || !zi) { fprintf(f, "zone: mach_zone_info gagal kr=%d\n", kr); return; }
  enum { MAXZ = 2048 };
  static char pname[MAXZ][80]; static uint64_t pcur[MAXZ], fcur[MAXZ]; static int pn = 0;
  mach_msg_type_number_t n = nn < ni ? nn : ni; if (n > MAXZ) n = MAXZ;
  struct { int i; uint64_t cur; } o[MAXZ];
  for (mach_msg_type_number_t i = 0; i < n; i++) { o[i].i = i; o[i].cur = zi[i].cur; }
  for (mach_msg_type_number_t a = 0; a < n; a++)           /* urut turun, n kecil (±300) */
    for (mach_msg_type_number_t b = a + 1; b < n; b++)
      if (o[b].cur > o[a].cur) { int ti = o[a].i; uint64_t tc = o[a].cur; o[a] = o[b]; o[b].i = ti; o[b].cur = tc; }
  uint64_t total = 0; for (mach_msg_type_number_t i = 0; i < n; i++) total += zi[i].cur;
  fprintf(f, "zone: %u zone, total %.0fMB (d=perubahan sejak sampel lalu, d0=sejak sampel pertama)\n", (unsigned)n, MB(total));
  for (int k = 0; k < 10 && k < (int)n; k++) {
    int i = o[k].i; double d = 0, d0 = 0;
    if (pn == (int)n && !strncmp(pname[i], nm[i].name, 80)) { d = (double)zi[i].cur - (double)pcur[i]; d0 = (double)zi[i].cur - (double)fcur[i]; }
    fprintf(f, "zone:   %-28.28s cur=%.1fMB count=%llu elem=%lluB d=%+.1fMB d0=%+.1fMB\n",
            nm[i].name, MB(zi[i].cur), (unsigned long long)zi[i].count, (unsigned long long)zi[i].elem, d / 1048576.0, d0 / 1048576.0);
  }
  /* zone yang paling tumbuh sejak sampel pertama, walau bukan yang terbesar */
  if (pn == (int)n) {
    int bi = -1; double bd = 0;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
      if (strncmp(pname[i], nm[i].name, 80)) continue;
      double d0 = (double)zi[i].cur - (double)fcur[i];
      if (d0 > bd) { bd = d0; bi = i; }
    }
    if (bi >= 0 && bd > 1048576.0) fprintf(f, "zone: paling tumbuh sejak awal: %.80s +%.1fMB (cur=%.1fMB)\n", nm[bi].name, bd / 1048576.0, MB(zi[bi].cur));
  }
  int first = (pn != (int)n);
  for (mach_msg_type_number_t i = 0; i < n; i++) {
    int same = !first && !strncmp(pname[i], nm[i].name, 80);
    strncpy(pname[i], nm[i].name, 79); pname[i][79] = 0;
    pcur[i] = zi[i].cur;
    if (!same) fcur[i] = zi[i].cur;
  }
  pn = (int)n;
  vm_deallocate(mach_task_self(), (vm_address_t)nm, nn * sizeof(zname_t));
  vm_deallocate(mach_task_self(), (vm_address_t)zi, ni * sizeof(zinfo_t));
}

/* ---- kmem: memori kernel per "site"/tag via mach_memory_info ----
 * Tata letak struct berubah antar versi XNU, jadi kita menebak: 176 byte (versi baru, ada name[80])
 * atau 64 byte (versi lama). Pilihan yang dipakai dicetak supaya bisa dicek. */
typedef struct { uint64_t flags, site, size, freeb, largest, coll, mapped, peak;
                 uint16_t tag, zone, ra[2]; uint64_t rv[3]; char name[80]; } kmi176_t;
typedef struct { uint64_t flags, site, size, freeb, largest, rv[3]; } kmi64_t;
typedef char kmi_chk176[(sizeof(kmi176_t) == 176) ? 1 : -1];
typedef char kmi_chk64[(sizeof(kmi64_t) == 64) ? 1 : -1];

static int name_ok(const char *nm) {         /* 80 byte: karakter tercetak lalu NUL */
  int i = 0;
  for (; i < 80 && nm[i]; i++) if ((unsigned char)nm[i] < 32 || (unsigned char)nm[i] > 126) return 0;
  return i < 80;
}

static void kmem(FILE *f) {
  if (!p_mmi) { fprintf(f, "kmem: simbol mach_memory_info HILANG di sistem ini\n"); return; }
  mach_port_t hp = get_hpriv();
  if (hp == MACH_PORT_NULL) { fprintf(f, "kmem: host_priv tidak didapat: %s\n", hp_how); return; }
  zname_t *nm = NULL; zinfo_t *zi = NULL; void *mi = NULL;
  mach_msg_type_number_t nn = 0, ni = 0, nmi = 0;
  int stride = 64;
  kern_return_t kr = p_mmi(hp, &nm, &nn, &zi, &ni, &mi, &nmi);
  if (kr != KERN_SUCCESS || !mi || !nmi) { fprintf(f, "kmem: mach_memory_info gagal kr=%d n=%u\n", kr, (unsigned)nmi); goto out; }

  {
    unsigned chk = nmi < 8 ? nmi : 8; int good = 0, named = 0;
    kmi176_t *e = (kmi176_t *)mi;
    for (unsigned i = 0; i < chk; i++) if (name_ok(e[i].name)) { good++; if (e[i].name[0]) named++; }
    if (chk && good >= (int)chk - 1 && named >= 1) stride = 176;
  }

  enum { MAXK = 1024 };
  static uint64_t fl[MAXK], st[MAXK], sz[MAXK], pk[MAXK];
  static char nmx[MAXK][48];
  static uint64_t pf[MAXK], pst[MAXK], psz[MAXK], f0[MAXK];
  static int pn = 0;
  int n = nmi > MAXK ? MAXK : (int)nmi;
  uint64_t tot = 0; int nz = 0;
  for (int i = 0; i < n; i++) {
    nmx[i][0] = 0; pk[i] = 0;
    if (stride == 176) {
      kmi176_t *e = &((kmi176_t *)mi)[i];
      fl[i] = e->flags; st[i] = e->site; sz[i] = e->size; pk[i] = e->peak;
      snprintf(nmx[i], sizeof(nmx[i]), "%.47s", e->name);
      if (!nmx[i][0]) snprintf(nmx[i], sizeof(nmx[i]), "tag%u", (unsigned)e->tag);
    } else {
      kmi64_t *e = &((kmi64_t *)mi)[i];
      fl[i] = e->flags; st[i] = e->site; sz[i] = e->size;
      snprintf(nmx[i], sizeof(nmx[i]), "site=0x%llx", (unsigned long long)e->site);
    }
    if (sz[i]) { tot += sz[i]; nz++; }
  }
  fprintf(f, "kmem: stride=%d entri=%u tak-nol=%d jumlah=%.0fMB host_priv=%s "
             "(type=flags&0xFF: 0=tag 1=kmod 2=kernel 3=zone 4=counter, dugaan dari vm_kern.h; counter bukan alokasi terpisah)\n",
          stride, (unsigned)nmi, nz, MB(tot), hp_how);

  double dl[MAXK], d0[MAXK];
  int same_layout = (pn == n);
  for (int i = 0; i < n; i++) {
    int same = same_layout && pf[i] == fl[i] && pst[i] == st[i];
    dl[i] = same ? (double)sz[i] - (double)psz[i] : 0;
    d0[i] = same ? (double)sz[i] - (double)f0[i] : 0;
    if (!same) f0[i] = sz[i];
    pf[i] = fl[i]; pst[i] = st[i]; psz[i] = sz[i];
  }
  pn = n;

  static int ord[MAXK];
  for (int i = 0; i < n; i++) ord[i] = i;
  for (int a = 1; a < n; a++) {                       /* insertion sort turun menurut size */
    int x = ord[a], b = a - 1;
    while (b >= 0 && sz[ord[b]] < sz[x]) { ord[b + 1] = ord[b]; b--; }
    ord[b + 1] = x;
  }
  for (int k = 0, shown = 0; k < n && shown < 18; k++) {
    int i = ord[k];
    if (sz[i] < 4 * 1048576ULL) break;
    fprintf(f, "kmem:   %-30.30s type=%llu flags=0x%llx size=%.1fMB peak=%.1fMB d=%+.1fMB d0=%+.1fMB\n",
            nmx[i], (unsigned long long)(fl[i] & 0xFF), (unsigned long long)fl[i], MB(sz[i]), MB(pk[i]),
            dl[i] / 1048576.0, d0[i] / 1048576.0);
    shown++;
  }
  for (int k = 0; k < 4; k++) {                      /* 4 entri yang paling tumbuh sejak sampel pertama */
    int bi = -1; double bd = 8 * 1048576.0;
    for (int i = 0; i < n; i++) if (d0[i] > bd) { bd = d0[i]; bi = i; }
    if (bi < 0) break;
    fprintf(f, "kmem: tumbuh: %-30.30s +%.1fMB sejak awal (size=%.1fMB type=%llu)\n",
            nmx[bi], bd / 1048576.0, MB(sz[bi]), (unsigned long long)(fl[bi] & 0xFF));
    d0[bi] = 0;
  }
out:
  if (nm) vm_deallocate(mach_task_self(), (vm_address_t)nm, nn * sizeof(zname_t));
  if (zi) vm_deallocate(mach_task_self(), (vm_address_t)zi, ni * sizeof(zinfo_t));
  if (mi) vm_deallocate(mach_task_self(), (vm_address_t)mi, nmi * (size_t)stride);
}

static void memtop(FILE *f, double t, int detail) {
  sym_load();
  sysmem(f);
  if (!p_lp || !p_ru) { fprintf(f, "top: proc_listpids/proc_pid_rusage tidak ada\n"); return; }
  int sz = p_lp(1 /*PROC_ALL_PIDS*/, 0, NULL, 0);
  if (sz <= 0) { fprintf(f, "top: proc_listpids gagal\n"); return; }
  int *pids = malloc(sz + 64 * sizeof(int));
  if (!pids) return;
  sz = p_lp(1, 0, pids, sz + 64 * sizeof(int));
  int np = sz > 0 ? sz / (int)sizeof(int) : 0;
  struct row *rows = calloc(np ? np : 1, sizeof(struct row));
  int nr = 0; uint64_t sumfp = 0, sumwired = 0;
  for (int i = 0; i < nptab; i++) ptab[i].seen = 0;
  for (int i = 0; i < np; i++) {
    int pid = pids[i];
    if (pid < 0) continue;
    struct ru0 ru; memset(&ru, 0, sizeof(ru));
    if (p_ru(pid, 0, &ru) != 0) continue;
    struct row *r = &rows[nr++];
    r->pid = pid;
    char nm[64] = {0};
    if (p_pname) p_pname(pid, nm, sizeof(nm) - 1);
    if (!nm[0]) snprintf(nm, sizeof(nm), pid == 0 ? "kernel_task" : "?");
    snprintf(r->name, sizeof(r->name), "%s", nm);
    r->fp = ru.footprint; r->res = ru.resident; r->wired = ru.wired;
    sumfp += ru.footprint; sumwired += ru.wired;
    int k = -1;
    for (int j = 0; j < nptab; j++) if (ptab[j].pid == pid && !strcmp(ptab[j].name, r->name)) { k = j; break; }
    if (k < 0 && nptab < MAXP) {
      k = nptab++; ptab[k].pid = pid; snprintf(ptab[k].name, sizeof(ptab[k].name), "%s", r->name);
      ptab[k].first = ru.footprint; ptab[k].last = ru.footprint;
    }
    if (k >= 0) { r->first = ptab[k].first; r->prev = ptab[k].last; ptab[k].last = ru.footprint; ptab[k].seen = 1; }
    else { r->first = r->prev = ru.footprint; }
  }
  int w = 0;                                   /* buang proses yang sudah mati */
  for (int i = 0; i < nptab; i++) if (ptab[i].seen) ptab[w++] = ptab[i];
  nptab = w;
  qsort(rows, nr, sizeof(struct row), cmp_fp);

  uint64_t m = 0; size_t l = sizeof(m); sysctlbyname("hw.memsize", &m, &l, 0, 0);
  vm_size_t ps = 0; host_page_size(mach_host_self(), &ps);
  vm_statistics64_data_t v; mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
  host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &c);
  uint64_t used = m - (uint64_t)v.free_count * ps;
  fprintf(f, "top: t=%.0fs proses=%d jumlah_footprint=%.0fMB sistem_terpakai=%.0fMB selisih=%.0fMB "
             "(selisih = wired kernel + file-cache + IOSurface/IOKit yang tidak dihitung ke proses)\n",
          t, nr, MB(sumfp), MB(used), MB(used) - MB(sumfp));
  for (int i = 0; i < nr && i < 8; i++) {
    struct row *r = &rows[i];
    fprintf(f, "top:   pid=%d %-16.16s fp=%.0fMB res=%.0fMB wired=%.0fMB d=%+.0fMB d_awal=%+.0fMB\n",
            r->pid, r->name, MB(r->fp), MB(r->res), MB(r->wired),
            ((double)r->fp - (double)r->prev) / 1048576.0, ((double)r->fp - (double)r->first) / 1048576.0);
  }
  /* proses yang paling tumbuh sejak pertama terlihat */
  int gi = -1; double gd = 0;
  for (int i = 0; i < nr; i++) {
    if (rows[i].pid == 0) continue;
    double d = (double)rows[i].fp - (double)rows[i].first;
    if (d > gd) { gd = d; gi = i; }
  }
  if (gi >= 0 && gd > 20 * 1048576.0)
    fprintf(f, "top: paling tumbuh: pid=%d %s +%.0fMB sejak pertama terlihat (fp=%.0fMB)\n", rows[gi].pid, rows[gi].name, gd / 1048576.0, MB(rows[gi].fp));
  if (detail) {
    int big = -1;
    for (int i = 0; i < nr; i++) if (rows[i].pid != 0) { big = i; break; }
    if (big >= 0) vmdetail(f, rows[big].pid, rows[big].name);
    if (gi >= 0 && gi != big && gd > 20 * 1048576.0) vmdetail(f, rows[gi].pid, rows[gi].name);
    zones(f);
    kmem(f);
  }
  free(rows); free(pids);
}

/* wired (MB) dan free (MB) saat ini; murah, dipanggil tiap detik untuk menangkap lonjakan. */
static double wired_mb(double *freemb) {
  vm_size_t ps = 0; host_page_size(mach_host_self(), &ps);
  vm_statistics64_data_t v; mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &c) != 0) return -1;
  if (freemb) *freemb = v.free_count * (double)ps / 1048576.0;
  return v.wire_count * (double)ps / 1048576.0;
}

/* Every second: if the kernel finished a new SWEET FPS second (kern.sweet_fps), log it.
 * The kernel only updates it while frames come, so an idle screen writes nothing. */
int main(void) {
  double t0 = now_s(), tmem = -100, tio = -1000, tmt = -1000, tdet = -1000, tburst = -1000, lastw = -1;
  int boot_done = 0;
  unsigned last_seq = 0;
  int warned = 0;
  while (now_s() - t0 < 1500) {
    double t = now_s();
    char buf[160] = {0}; size_t l = sizeof(buf) - 1;
    unsigned seq = 0, frames = 0, late = 0, ticks = 0, modes = 0, n79 = 0, s0 = 0, osz = 0, fl = 0;
    unsigned long long ms = 0, gap = 0, pr = 0, af = 0, tgap = 0;
    int n = 0;
    int have = sysctlbyname("kern.sweet_fps", buf, &l, NULL, 0) == 0;
    if (have && buf[0] != 0) {
      n = sscanf(buf, "%u %u %llu %llu %u %llu %llu %u %llu %u %u %x %u %x",
          &seq, &frames, &ms, &gap, &late, &pr, &af, &ticks, &tgap, &modes, &n79, &s0, &osz, &fl);
    }
    int ok = n >= 7;
    int fresh = ok && seq != last_seq;
    int domem = t - tmem >= 30;
    int doio = t - t0 >= 20 && t - tio >= 120;
    int domt = t - t0 >= 10 && t - tmt >= 60;          /* memtop tiap 60 dtk, rincian vm+zone tiap 120 dtk */
    int dodet = domt && t - tdet >= 120;
    /* lonjakan wired: naik >= 40MB (atau turun >= 100MB) sejak catatan terakhir, maks sekali per 10 dtk */
    double fmb = 0, wmb = wired_mb(&fmb);
    if (lastw < 0) lastw = wmb;
    int burst = wmb >= 0 && t - tburst >= 10 && (wmb - lastw >= 40 || lastw - wmb >= 100);
    if (!fresh) {
      FILE *nul = fopen("/dev/null", "w");     /* keep the deltas current without logging */
      if (nul) { cpuload(nul); fclose(nul); }
    }
    if (fresh || domem || doio || domt || burst || (!have && !warned)) {
      FILE *f = fopen("/var/mobile/specinfo.txt", "a");
      if (!f) f = fopen("/var/tmp/specinfo.txt", "a");
      if (f) {
        if (fresh) {
          fprintf(f, "t=%.0fs fps seq=%u frames=%u ms=%llu gap=%llums late=%u present=%lluus after=%lluus",
                  t - t0, seq, frames, ms, gap, late, pr, af);
          if (n >= 9) {
            fprintf(f, " ticks=%u tickgap=%llums", ticks, tgap);
          }
          if (n >= 14) {
            fprintf(f, " modes=%u n79=%u s0=%#x osz=%u fl=%#x", modes, n79, s0, osz, fl);
          }
          fprintf(f, "\n");
          cpuload(f);
          last_seq = seq;
        }
        if (!have && !warned) { fprintf(f, "kern.sweet_fps belum ada di kernel ini\n"); warned = 1; }
        if (!boot_done) { bootinfo(f); boot_done = 1; }
        if (domem) { mem(f); tmem = t; }
        if (doio) { iosnap(f, t - t0); tio = t; }
        if (burst) {
          fprintf(f, "wired: t=%.0fs wired=%.0fMB d=%+.0fMB free=%.0fMB  <-- lonjakan, rincian di bawah\n", t - t0, wmb, wmb - lastw, fmb);
          iosnap(f, t - t0); tio = t;
          memtop(f, t - t0, 1); tmt = t; tdet = t;
          lastw = wmb; tburst = t;
        } else if (domt) { memtop(f, t - t0, dodet); tmt = t; if (dodet) tdet = t; }
        if (domem && !burst) lastw = wmb;
        fflush(f); fsync(fileno(f)); fclose(f);
      }
    }
    sleep(1);
  }
  return 0;
}

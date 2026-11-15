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

/* Every second: if the kernel finished a new SWEET FPS second (kern.sweet_fps), log it.
 * The kernel only updates it while frames come, so an idle screen writes nothing. */
int main(void) {
  double t0 = now_s(), tmem = -100, tio = -1000;
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
    if (!fresh) {
      FILE *nul = fopen("/dev/null", "w");     /* keep the deltas current without logging */
      if (nul) { cpuload(nul); fclose(nul); }
    }
    if (fresh || domem || doio || (!have && !warned)) {
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
        fflush(f); fsync(fileno(f)); fclose(f);
      }
    }
    sleep(1);
  }
  return 0;
}

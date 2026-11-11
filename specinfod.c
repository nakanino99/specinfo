#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/sysctl.h>
#include <mach/mach.h>

static double now_s(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
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
  double t0 = now_s(), tmem = -100;
  unsigned last_seq = 0;
  int warned = 0;
  while (now_s() - t0 < 1500) {
    double t = now_s();
    char buf[160] = {0}; size_t l = sizeof(buf) - 1;
    unsigned seq = 0, frames = 0, late = 0, ticks = 0;
    unsigned long long ms = 0, gap = 0, pr = 0, af = 0, tgap = 0;
    int n = 0;
    int have = sysctlbyname("kern.sweet_fps", buf, &l, NULL, 0) == 0;
    if (have && buf[0] != 0) {
      n = sscanf(buf, "%u %u %llu %llu %u %llu %llu %u %llu", &seq, &frames, &ms, &gap, &late, &pr, &af, &ticks, &tgap);
    }
    int ok = n >= 7;
    int fresh = ok && seq != last_seq;
    int domem = t - tmem >= 30;
    if (fresh || domem || (!have && !warned)) {
      FILE *f = fopen("/var/mobile/specinfo.txt", "a");
      if (!f) f = fopen("/var/tmp/specinfo.txt", "a");
      if (f) {
        if (fresh) {
          fprintf(f, "t=%.0fs fps seq=%u frames=%u ms=%llu gap=%llums late=%u present=%lluus after=%lluus",
                  t - t0, seq, frames, ms, gap, late, pr, af);
          if (n >= 9) {
            fprintf(f, " ticks=%u tickgap=%llums", ticks, tgap);
          }
          fprintf(f, "\n");
          last_seq = seq;
        }
        if (!have && !warned) { fprintf(f, "kern.sweet_fps belum ada di kernel ini\n"); warned = 1; }
        if (domem) { mem(f); tmem = t; }
        fflush(f); fsync(fileno(f)); fclose(f);
      }
    }
    sleep(1);
  }
  return 0;
}

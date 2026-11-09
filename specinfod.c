#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <mach/mach.h>

static void dump(FILE *f) {
  uint64_t mem = 0; size_t l = sizeof(mem);
  sysctlbyname("hw.memsize", &mem, &l, 0, 0);
  int ncpu = 0, act = 0;
  l = sizeof(ncpu); sysctlbyname("hw.ncpu", &ncpu, &l, 0, 0);
  l = sizeof(act);  sysctlbyname("hw.activecpu", &act, &l, 0, 0);
  vm_size_t ps = 0; host_page_size(mach_host_self(), &ps);
  vm_statistics64_data_t v; mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
  host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &c);
  double mb = 1048576.0;
  fprintf(f, "hw.memsize=%.0fMB ncpu=%d activecpu=%d pagesize=%lu\n"
             "free=%.0f active=%.0f inactive=%.0f wired=%.0f compressed=%.0f (MB)\n",
          mem / mb, ncpu, act, (unsigned long)ps,
          v.free_count * ps / mb, v.active_count * ps / mb,
          v.inactive_count * ps / mb, v.wire_count * ps / mb,
          v.compressor_page_count * ps / mb);
}

int main(void) {
  for (int i = 0; i < 12; i++) {
    FILE *f = fopen("/var/mobile/specinfo.txt", "a");
    if (!f) f = fopen("/var/tmp/specinfo.txt", "a");
    if (f) { fprintf(f, "--- t=%ds\n", i * 5); dump(f); fclose(f); }
    sleep(5);
  }
  return 0;
}

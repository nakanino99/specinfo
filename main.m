#import <UIKit/UIKit.h>
#include <sys/sysctl.h>
#include <mach/mach.h>

@interface AD : UIResponder <UIApplicationDelegate>
@property (strong) UIWindow *w;
@end

@implementation AD
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
  uint64_t mem = 0; size_t l = sizeof(mem);
  sysctlbyname("hw.memsize", &mem, &l, 0, 0);
  int ncpu = 0; l = sizeof(ncpu);
  sysctlbyname("hw.ncpu", &ncpu, &l, 0, 0);
  vm_size_t ps = 0; host_page_size(mach_host_self(), &ps);
  vm_statistics64_data_t v; mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
  host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &c);
  double mb = 1048576.0;
  NSString *s = [NSString stringWithFormat:
    @"hw.memsize: %.0f MB\nphysicalMemory: %.0f MB\nncpu: %d\npagesize: %lu\n"
    @"free: %.0f MB\nactive: %.0f MB\ninactive: %.0f MB\nwired: %.0f MB\ncompressed: %.0f MB",
    mem / mb, [NSProcessInfo processInfo].physicalMemory / mb, ncpu, (unsigned long)ps,
    v.free_count * ps / mb, v.active_count * ps / mb, v.inactive_count * ps / mb,
    v.wire_count * ps / mb, v.compressor_page_count * ps / mb];
  self.w = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  UIViewController *vc = [UIViewController new];
  vc.view.backgroundColor = UIColor.whiteColor;
  UILabel *t = [[UILabel alloc] initWithFrame:CGRectInset(vc.view.bounds, 16, 60)];
  t.numberOfLines = 0;
  t.textColor = UIColor.blackColor;
  t.text = s;
  t.font = [UIFont monospacedSystemFontOfSize:16 weight:UIFontWeightRegular];
  [vc.view addSubview:t];
  self.w.rootViewController = vc;
  [self.w makeKeyAndVisible];
  return YES;
}
@end

int main(int argc, char *argv[]) {
  @autoreleasepool { return UIApplicationMain(argc, argv, nil, @"AD"); }
}

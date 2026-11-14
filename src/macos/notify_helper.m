/* moroder-notify (macOS)
- Tiny helper app ("Moroder.app") that shows a track notification with the
  Moroder icon and the track's cover, which a plain terminal process cannot
  (osascript notifications carry Script Editor's icon)
- Usage: moroder-notify <title> <body> [cover url or path]
- Uses NSUserNotification: UNUserNotificationCenter refuses ad-hoc-signed apps
*/

#import <AppKit/AppKit.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

@interface MoroderNotifyDelegate : NSObject <NSUserNotificationCenterDelegate>
@end

@implementation MoroderNotifyDelegate
// Show it even if this (background) app counts as frontmost.
- (BOOL)userNotificationCenter:(NSUserNotificationCenter*)center shouldPresentNotification:(NSUserNotification*)n {
    return YES;
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) return 2;

        NSUserNotificationCenter* center = NSUserNotificationCenter.defaultUserNotificationCenter;
        if (!center) return 1;

        MoroderNotifyDelegate* delegate = [MoroderNotifyDelegate new];
        center.delegate = delegate;

        NSUserNotification* n = [NSUserNotification new];
        n.title = [NSString stringWithUTF8String:argv[1]];
        n.informativeText = [NSString stringWithUTF8String:argv[2]];

        if (argc > 3 && argv[3][0]) {
            NSString* src = [NSString stringWithUTF8String:argv[3]];
            NSURL* url = [src hasPrefix:@"http"] ? [NSURL URLWithString:src] : [NSURL fileURLWithPath:src];
            NSData* data = url ? [NSData dataWithContentsOfURL:url] : nil;   // short, the helper is throwaway
            if (data) n.contentImage = [[NSImage alloc] initWithData:data];
        }

        // Replace the previous track's notification instead of stacking them.
        n.identifier = @"moroder.now-playing";
        [center removeDeliveredNotification:n];
        [center deliverNotification:n];

        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    }
    return 0;
}

// Sparkle 2 half of updater.h. The feed comes from the delegate so the channel can differ from Info.plist's SUFeedURL;
// the EdDSA key is Info.plist's SUPublicEDKey.
#include "updater.h"

#import <Sparkle/Sparkle.h>

@interface SVUpdaterDelegate : NSObject <SPUUpdaterDelegate>
@property(copy) NSString *feed;
@end

@implementation SVUpdaterDelegate
- (nullable NSString *)feedURLStringForUpdater:(SPUUpdater *)updater {
    return self.feed;
}
@end

namespace sv::updater {

static SPUStandardUpdaterController *controller;
static SVUpdaterDelegate *delegate;

bool supported() { return true; }

void start(const QString &channel) {
    delegate = [SVUpdaterDelegate new];
    delegate.feed =
        (QStringLiteral(SV_APPCAST_BASE) + (channel == "test" ? "/appcast-test.xml" : "/appcast.xml")).toNSString();
    controller = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                               updaterDelegate:delegate
                                                            userDriverDelegate:nil];
}

void stop() {}
void check_now() { [controller checkForUpdates:nil]; }
bool automatic() { return controller.updater.automaticallyChecksForUpdates; }
void set_automatic(bool on) { controller.updater.automaticallyChecksForUpdates = on; }

} // namespace sv::updater

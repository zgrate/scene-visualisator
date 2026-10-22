#include "updater.h"

#if defined(SV_APPCAST_BASE) && defined(_WIN32)
#include <QCoreApplication>
#include <winsparkle.h>
#endif

namespace sv::updater {

// The feed for a channel; the test channel lets AT-7 find a test release without touching stable users.
[[maybe_unused]] static QString feed(const QString &channel) {
#ifdef SV_APPCAST_BASE
    return QStringLiteral(SV_APPCAST_BASE) + (channel == "test" ? "/appcast-test.xml" : "/appcast.xml");
#else
    Q_UNUSED(channel);
    return {};
#endif
}

#if defined(SV_APPCAST_BASE) && defined(_WIN32)

bool supported() { return true; }

void start(const QString &channel) {
    win_sparkle_set_appcast_url(feed(channel).toUtf8().constData());
#ifdef SV_UPDATE_PUBLIC_KEY
    win_sparkle_set_eddsa_public_key(SV_UPDATE_PUBLIC_KEY);
#endif
    win_sparkle_set_app_details(L"Scene Visualiser", L"Scene Visualiser",
                                reinterpret_cast<const wchar_t *>(QCoreApplication::applicationVersion().utf16()));
    win_sparkle_init();
}

void stop() { win_sparkle_cleanup(); }
void check_now() { win_sparkle_check_update_with_ui(); }
bool automatic() { return win_sparkle_get_automatic_check_for_updates() != 0; }
void set_automatic(bool on) { win_sparkle_set_automatic_check_for_updates(on ? 1 : 0); }

#elif !(defined(SV_APPCAST_BASE) && defined(__APPLE__)) // macOS with a feed: updater_mac.mm

bool supported() { return false; }
void start(const QString &) {}
void stop() {}
void check_now() {}
bool automatic() { return false; }
void set_automatic(bool) {}

#endif

} // namespace sv::updater

#pragma once
// B7 updates: WinSparkle on Windows, Sparkle 2 on macOS, both reading the EdDSA-signed appcast that release CI
// publishes. Builds without SV_APPCAST_BASE (Linux, local builds) get no updater and `supported()` is false.
// Checks stay off until the user opts in: the libraries ask on the second launch, and the Project screen toggles it.
#include <QString>

namespace sv::updater {

bool supported();
void start(const QString &channel); // "stable" or "test"; the channel is read once per launch
void stop();
void check_now(); // with UI
bool automatic();
void set_automatic(bool on);

} // namespace sv::updater

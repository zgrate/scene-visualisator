#pragma once
// B7 crash reports. With SV_SENTRY_DSN, Sentry Native writes minidumps to `dir` and uploads them only while the user
// has opted in. Without it, Windows still writes a minidump to `dir` on a crash; macOS keeps its own reports in
// ~/Library/Logs/DiagnosticReports, and Linux (dev only) relies on core dumps.
#include <QString>

namespace sv::crash {

bool uploads_supported(); // built with a DSN
void start(const QString &dir, bool consent);
void set_consent(bool on);
void stop();

} // namespace sv::crash

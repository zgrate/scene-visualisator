#include "crash_reports.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>

#ifdef SV_SENTRY_DSN
#include <sentry.h>
#elif defined(_WIN32)
#include <windows.h> // before dbghelp.h

#include <dbghelp.h>
#endif

namespace sv::crash {

#ifdef SV_SENTRY_DSN

bool uploads_supported() { return true; }

void start(const QString &dir, bool consent) {
    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, SV_SENTRY_DSN);
    sentry_options_set_release(options, ("scene-visualiser@" + QCoreApplication::applicationVersion()).toUtf8());
    sentry_options_set_require_user_consent(options, 1);
#ifdef _WIN32
    sentry_options_set_database_pathw(options, reinterpret_cast<const wchar_t *>(dir.utf16()));
#else
    sentry_options_set_database_path(options, QFile::encodeName(dir).constData());
#endif
    sentry_init(options);
    set_consent(consent);
}

void set_consent(bool on) { on ? sentry_user_consent_give() : sentry_user_consent_revoke(); }
void stop() { sentry_close(); }

#else

bool uploads_supported() { return false; }

#ifdef _WIN32
static wchar_t dump_path[MAX_PATH];

static LONG WINAPI write_dump(EXCEPTION_POINTERS *info) {
    HANDLE file = CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &exception, nullptr,
                          nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void start(const QString &dir, bool) {
#ifdef _WIN32
    QDir().mkpath(dir);
    // The name is fixed at launch: the handler must not allocate. One dump per launch, named by start time.
    const QString path = QDir::toNativeSeparators(dir + "/scene-visualiser-" +
                                                  QString::number(QDateTime::currentSecsSinceEpoch()) + ".dmp");
    if (path.size() >= MAX_PATH)
        return;
    dump_path[path.toWCharArray(dump_path)] = 0;
    SetUnhandledExceptionFilter(write_dump);
#else
    Q_UNUSED(dir);
#endif
}

void set_consent(bool) {}
void stop() {}

#endif

} // namespace sv::crash

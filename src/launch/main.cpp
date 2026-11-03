#include "main.hpp"
#include <cerrno>

#include <fcntl.h>
#include <qcoreapplication.h>
#include <qdatastream.h>
#include <qdatetime.h>
#include <qdebug.h>
#include <qlogging.h>
#include <qtenvironmentvariables.h>
#include <unistd.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "../core/instanceinfo.hpp"
#include "../core/logging.hpp"
#include "../core/paths.hpp"
#include "build.hpp"
#include "launch_p.hpp"

#if CRASH_HANDLER
#include "../crash/main.hpp"
#endif

namespace qs::launch {

namespace {

#ifdef __GLIBC__

// Allocator defaults for a shell process: dozens of threads alive for days.
// Glibc's per-thread arenas strand freed pages of transient allocations
// (wallpaper-sized image decodes) in RSS and swap for the process lifetime;
// a shell that ran two days was observed at 620 MB RSS plus 890 MB swap.
// Capping arenas and pinning the mmap/trim thresholds sends large ranges
// straight back to the kernel instead.
//
// Explicit configuration wins: glibc consumes MALLOC_* from the environment
// at process startup (nosDshell's launcher script sets the same values), so
// any of them being present means the user already chose.
void configureGlibcAllocator() {
	if (qEnvironmentVariableIsSet("MALLOC_ARENA_MAX")
	    || qEnvironmentVariableIsSet("MALLOC_MMAP_THRESHOLD_")
	    || qEnvironmentVariableIsSet("MALLOC_TRIM_THRESHOLD_"))
	{
		return;
	}

	// Runs before any thread exists, so the mt-unsafe mallopt is safe here;
	// clang-tidy cannot see the ordering.
	mallopt(M_ARENA_MAX, 4);               // NOLINT(concurrency-mt-unsafe)
	mallopt(M_MMAP_THRESHOLD, 128 * 1024); // NOLINT(concurrency-mt-unsafe)
	mallopt(M_TRIM_THRESHOLD, 128 * 1024); // NOLINT(concurrency-mt-unsafe)
}

#endif

void checkCrashRelaunch(char** argv) {
#if CRASH_HANDLER
	auto lastInfoFdStr = qEnvironmentVariable("__QUICKSHELL_CRASH_INFO_FD");
	auto dumpPid = qEnvironmentVariable("__QUICKSHELL_CRASH_DUMP_PID").toInt();

	if (!lastInfoFdStr.isEmpty()) {
		auto lastInfoFd = lastInfoFdStr.toInt();

		RelaunchInfo info;

		{
			QFile file;
			if (!file.open(lastInfoFd, QFile::ReadOnly, QFile::AutoCloseHandle)) {
				qFatal() << "Failed to open crash info fd. Cannot restart.";
			}

			file.seek(0);

			auto ds = QDataStream(&file);
			ds >> info;
		}

		qunsetenv("__QUICKSHELL_CRASH_INFO_FD");
		qunsetenv("__QUICKSHELL_CRASH_DUMP_PID");
		qunsetenv("__QUICKSHELL_CRASH_SIGNAL");

		LogManager::init(
		    !info.noColor,
		    info.timestamp,
		    info.sparseLogsOnly,
		    info.defaultLogLevel,
		    info.logRules
		);

		qCritical().nospace() << "Quickshell has crashed under pid " << dumpPid
		                      << " (Coredumps will be available under that pid.)";

		qCritical() << "Further crash information is stored under"
		            << QsPaths::crashDir(info.instance.instanceId).path();

		if (info.instance.launchTime.msecsTo(QDateTime::currentDateTime()) < 10000) {
			qCritical() << "Quickshell crashed within 10 seconds of launching. Not restarting to avoid "
			               "a crash loop.";
			exit(-1); // NOLINT
		} else {
			qCritical() << "Quickshell has been restarted.";

			launch({.configPath = info.instance.configPath}, argv);
		}
	}
#endif
}

} // namespace

int DAEMON_PIPE = -1; // NOLINT

void exitDaemon(int code) {
	if (DAEMON_PIPE == -1) return;

	if (write(DAEMON_PIPE, &code, sizeof(int)) == -1) {
		qCritical().nospace() << "Failed to write daemon exit command with error code " << errno << ": "
		                      << qt_error_string();
	}

	close(DAEMON_PIPE);

	auto fd = open("/dev/null", O_RDWR);
	if (fd == -1) {
		qCritical().nospace() << "Failed to open /dev/null for daemon stdio" << errno << ": "
		                      << qt_error_string();
		return;
	}

	if (dup2(fd, STDIN_FILENO) != STDIN_FILENO) { // NOLINT
		qCritical().nospace() << "Failed to set daemon stdin to /dev/null" << errno << ": "
		                      << qt_error_string();
	}

	if (dup2(fd, STDOUT_FILENO) != STDOUT_FILENO) { // NOLINT
		qCritical().nospace() << "Failed to set daemon stdout to /dev/null" << errno << ": "
		                      << qt_error_string();
	}

	if (dup2(fd, STDERR_FILENO) != STDERR_FILENO) { // NOLINT
		qCritical().nospace() << "Failed to set daemon stderr to /dev/null" << errno << ": "
		                      << qt_error_string();
	}

	close(fd);
}

int main(int argc, char** argv) {
	QCoreApplication::setApplicationName("quickshell");

#ifdef __GLIBC__
	configureGlibcAllocator();
#endif

#if CRASH_HANDLER
	qsCheckCrash(argc, argv);
#endif

	checkCrashRelaunch(argv);
	auto code = runCommand(argc, argv);

	exitDaemon(code);
	return code;
}

} // namespace qs::launch

#include "../../include/services/notify.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <mutex>
#include <spawn.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <sys/wait.h>
#include <thread>
#include <vector>

extern char** environ;

namespace {

// Runs argv without a shell, so titles never need escaping.
void spawnAndReap(std::vector<std::string> args) {
    std::thread([args = std::move(args)] {
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        pid_t pid = 0;
        if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) {
            spdlog::debug("NOTIFY: could not start {}", args[0]);
            return;
        }

        int status = 0;
        waitpid(pid, &status, 0);
    }).detach();
}

}

#ifdef __APPLE__
namespace {

/* helperApp
- Moroder.app sits next to the binary in a build tree, and in
  <prefix>/share/moroder once installed
- Registered with LaunchServices once, so Notification Center knows its name
  and icon even if it was never opened from Finder
*/
std::string helperBinary() {
    static std::once_flag once;
    static std::string binary;

    std::call_once(once, [] {
        namespace fs = std::filesystem;

        char buf[4096];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) != 0) return;

        std::error_code ec;
        const fs::path exe = fs::canonical(buf, ec);
        if (ec) return;

        for (const fs::path& app : { exe.parent_path() / "Moroder.app",
                                     exe.parent_path().parent_path() / "share" / "moroder" / "Moroder.app" }) {
            const fs::path bin = app / "Contents" / "MacOS" / "moroder-notify";
            if (fs::exists(bin, ec)) {
                binary = bin.string();
                spawnAndReap({ "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister",
                               "-f", app.string() });
                return;
            }
        }
    });

    return binary;
}

}
#endif

void services::notifyTrack(const std::string& title, const std::string& subtitle, const std::string& cover) {
    if (title.empty()) return;

#ifdef __APPLE__
    // The helper app gives the notification Moroder's icon and the cover.
    const std::string helper = helperBinary();
    if (!helper.empty()) {
        spawnAndReap({ helper, title, subtitle, cover });
        return;
    }

    // Fallback: osascript. Title and text go in as script arguments, never
    // spliced into the script.
    spawnAndReap({
        "osascript",
        "-e", "on run argv",
        "-e", "display notification (item 2 of argv) with title (item 1 of argv)",
        "-e", "end run",
        title, subtitle,
    });
#else
    spawnAndReap({ "notify-send", "--app-name=Moroder", "--expire-time=4000", title, subtitle });
#endif
}

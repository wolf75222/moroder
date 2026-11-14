#include "../../include/services/notify.hpp"

#include <spdlog/spdlog.h>

#include <spawn.h>
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

void services::notifyTrack(const std::string& title, const std::string& subtitle) {
    if (title.empty()) return;

#ifdef __APPLE__
    // Title and text go in as script arguments, never spliced into the script.
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

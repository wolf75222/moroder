#include "../../include/services/update.hpp"
#include "../../include/config/config.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#ifndef MORODER_VERSION
#define MORODER_VERSION "0.0.0-dev"
#endif
#ifndef MORODER_REPO
#define MORODER_REPO "wolf75222/moroder"
#endif

namespace fs = std::filesystem;

namespace {

const std::string INSTALLER = "https://raw.githubusercontent.com/" MORODER_REPO "/main/install.sh";

fs::path cacheFile() {
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / ".cache" / "moroder" / "update.json" : fs::path();
}

nlohmann::json readCache() {
    std::ifstream in(cacheFile());
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

void writeCache(const nlohmann::json& j) {
    const fs::path f = cacheFile();
    if (f.empty()) return;
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    std::ofstream(f, std::ios::trunc) << j.dump();
}

size_t collect(char* d, size_t s, size_t n, void* out) {
    static_cast<std::string*>(out)->append(d, s * n);
    return s * n;
}

// The latest release's version ("1.2.0"), "" when unknown (offline, timeout).
std::string latestRelease(long timeout_ms) {
    std::string body;
    CURL* c = curl_easy_init();
    if (!c) return "";
    curl_easy_setopt(c, CURLOPT_URL, "https://api.github.com/repos/" MORODER_REPO "/releases/latest");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "moroder");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    long status = 0;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    if (status != 200) return "";
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("tag_name") || !j["tag_name"].is_string()) return "";
    std::string tag = j["tag_name"];
    if (!tag.empty() && tag[0] == 'v') tag.erase(0, 1);
    return tag;
}

std::vector<int> parts(const std::string& v) {
    std::vector<int> out;
    std::stringstream ss(v.substr(0, v.find('-')));
    std::string p;
    while (std::getline(ss, p, '.')) out.push_back(std::atoi(p.c_str()));
    while (out.size() < 3) out.push_back(0);
    return out;
}

bool isNewer(const std::string& latest, const std::string& current) {
    return !latest.empty() && parts(latest) > parts(current);
}

bool devBuild() { return std::string(MORODER_VERSION).find("dev") != std::string::npos; }

bool askYes(const std::string& question) {
    std::cout << question << " [Y/n] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) return false;
    return answer.empty() || answer[0] == 'y' || answer[0] == 'Y' || answer[0] == 'o' || answer[0] == 'O';
}

int runInstaller() {
    const std::string cmd = "curl -fsSL '" + INSTALLER + "' | sh";
    return std::system(cmd.c_str());
}

bool have(const char* tool) {
    const std::string cmd = std::string("command -v ") + tool + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string firstLine(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[512];
        if (fgets(buf, sizeof(buf), p)) out = buf;
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

}

const char* services::update::version() { return MORODER_VERSION; }

bool services::update::offerAtLaunch() {
    if (devBuild() || !Config::get().CHECK_FOR_UPDATES || std::getenv("MORODER_JUST_UPDATED")) return false;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;

    nlohmann::json cache = readCache();
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string latest = cache.value("latest", std::string());

    // Ask GitHub at most every 12 h; otherwise what it said last time.
    if (now - cache.value("checked_at", 0LL) > 12 * 3600) {
        const std::string fresh = latestRelease(1500);
        if (!fresh.empty()) {
            latest = fresh;
            cache["latest"] = latest;
            cache["checked_at"] = now;
            writeCache(cache);
        }
    }

    if (!isNewer(latest, MORODER_VERSION) || cache.value("skipped", std::string()) == latest) return false;

    std::cout << "\n  \033[1;31mMoroder " << latest << "\033[0m is out (you have " << MORODER_VERSION << ").\n"
              << "  Release notes: https://github.com/" MORODER_REPO "/releases/tag/v" << latest << "\n\n  ";
    if (!askYes("Update now?")) {
        cache["skipped"] = latest;      // not asked again for this one; `moroder update` installs it
        writeCache(cache);
        std::cout << "  Later then: `moroder update` installs it.\n\n";
        return false;
    }
    return runInstaller() == 0;
}

int services::update::runUpdate() {
    std::cout << "Moroder " << MORODER_VERSION << "\n";
    const std::string latest = latestRelease(10000);
    if (latest.empty()) std::cout << "Couldn't reach GitHub: reinstalling the latest release anyway.\n";
    else if (!isNewer(latest, MORODER_VERSION) && !devBuild()) std::cout << "Already the latest (" << latest << ").\n";

    int rc = 0;
    if (latest.empty() || isNewer(latest, MORODER_VERSION) || devBuild()) rc = runInstaller();

    // yt-dlp follows YouTube's changes: keep it current too.
    if (have("brew")) {
        std::cout << "Updating yt-dlp...\n";
        std::system("brew upgrade yt-dlp >/dev/null 2>&1 || true");
    } else if (have("yt-dlp")) {
        std::system("yt-dlp -U >/dev/null 2>&1 || true");
    }
    std::cout << "yt-dlp " << firstLine("yt-dlp --version 2>/dev/null") << "\n";
    return rc == 0 ? 0 : 1;
}

int services::update::doctor() {
    auto line = [](bool ok, const std::string& what, const std::string& detail) {
        std::cout << (ok ? "  \033[32m✓\033[0m " : "  \033[31m✗\033[0m ") << what;
        if (!detail.empty()) std::cout << "  \033[2m" << detail << "\033[0m";
        std::cout << "\n";
    };
    const char* home = std::getenv("HOME");
    const fs::path cfg = fs::path(home ? home : "") / ".config" / "moroder";
    std::error_code ec;

    std::cout << "Moroder " << MORODER_VERSION << "\n\n";
    const std::string ytdlp = firstLine("yt-dlp --version 2>/dev/null");
    line(!ytdlp.empty(), "yt-dlp", ytdlp.empty() ? "missing: brew install yt-dlp" : ytdlp);
    line(have("mpv") || fs::exists("/opt/homebrew/lib/libmpv.dylib", ec) || fs::exists("/usr/lib/x86_64-linux-gnu/libmpv.so.2", ec), "mpv", "");
    line(fs::exists(cfg / "browser.json", ec), "signed in", fs::exists(cfg / "browser.json", ec) ? "" : "run: moroder setup");
#ifdef __APPLE__
    line(fs::exists("/opt/homebrew/share/moroder/Moroder.app", ec) || fs::exists("/usr/local/share/moroder/Moroder.app", ec),
         "notification helper", "Moroder.app (cover in notifications)");
#endif
    const char* term = std::getenv("TERM_PROGRAM");
    line(true, "terminal", std::string(term ? term : "unknown") + (term && std::string(term) == "WarpTerminal" ? " (sideways swipes aren't passed to apps: use the ‹ › buttons)" : ""));
    const std::string latest = latestRelease(5000);
    if (devBuild())
        line(true, "development build", latest.empty() ? "" : "latest release is " + latest);
    else
        line(latest.empty() || !isNewer(latest, MORODER_VERSION), "up to date",
             latest.empty() ? "couldn't reach GitHub" : isNewer(latest, MORODER_VERSION) ? latest + " is out: moroder update" : "latest is " + latest);
    std::cout << "\n  logs: ~/.local/state/moroder/logs   config: " << cfg.string() << "\n";
    return 0;
}

int services::update::uninstall() {
    std::string bin = firstLine("command -v moroder 2>/dev/null");
    if (bin.empty()) bin = "/opt/homebrew/bin/moroder";
    const fs::path share = fs::path(bin).parent_path().parent_path() / "share" / "moroder";

    std::cout << "This removes " << bin << " and " << share.string() << ".\n";
    if (!askYes("Uninstall Moroder?")) return 0;
    std::error_code ec;
    fs::remove(bin, ec);
    fs::remove_all(share, ec);

    const char* home = std::getenv("HOME");
    if (home && askYes("Also delete your settings, session and caches (~/.config/moroder, ~/.cache/moroder, ~/.local/state/moroder)?")) {
        for (const char* d : {".config/moroder", ".cache/moroder", ".local/state/moroder"}) fs::remove_all(fs::path(home) / d, ec);
    }
    std::cout << "Moroder is uninstalled.\n";
    return 0;
}

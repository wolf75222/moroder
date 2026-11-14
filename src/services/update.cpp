#include "../../include/services/update.hpp"
#include "../../include/config/config.hpp"
#include "../../include/services/cli_ui.hpp"
#include "../../include/ytm/ytmusic.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <thread>
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

// A repository's latest release tag without the "v" ("1.2.0"), "" when unknown (offline, timeout).
std::string latestRelease(long timeout_ms, const std::string& repo = MORODER_REPO) {
    std::string body;
    CURL* c = curl_easy_init();
    if (!c) return "";
    const std::string url = "https://api.github.com/repos/" + repo + "/releases/latest";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
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

int runInstaller() {
    setenv("MORODER_UPDATING", "1", 1);         // the installer then also updates yt-dlp
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

// Milliseconds for a HEAD request, -1 when it fails.
long pingMs(const std::string& url, long timeout_ms) {
    CURL* c = curl_easy_init();
    if (!c) return -1;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "moroder");
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = curl_easy_perform(c) == CURLE_OK;
    curl_easy_cleanup(c);
    if (!ok) return -1;
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

uintmax_t sizeOf(const fs::path& p) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) return fs::file_size(p, ec);
    uintmax_t n = 0;
    for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) n += it->file_size(ec);
    return n;
}

std::string human(uintmax_t b) {
    char buf[32];
    if (b >= 1024ull * 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f GB", b / 1073741824.0);
    else if (b >= 1024 * 1024)     std::snprintf(buf, sizeof buf, "%.1f MB", b / 1048576.0);
    else if (b >= 1024)            std::snprintf(buf, sizeof buf, "%.0f KB", b / 1024.0);
    else                           std::snprintf(buf, sizeof buf, "%ju B", b);
    return buf;
}

#ifdef __APPLE__
const std::string INSTALL_YTDLP = "brew install yt-dlp";
const std::string INSTALL_MPV = "brew install mpv";
#else
const std::string INSTALL_YTDLP = "install yt-dlp with your package manager";
const std::string INSTALL_MPV = "install mpv (libmpv) with your package manager";
#endif

// yt-dlp in its own Python environment, set up by install.sh on Debian/Ubuntu.
fs::path privateYtdlp() {
    const std::string bin = firstLine("command -v moroder 2>/dev/null");
    if (bin.empty()) return {};
    const fs::path venv = fs::path(bin).parent_path().parent_path() / "share" / "moroder" / "yt-dlp";
    std::error_code ec;
    return fs::exists(venv / "bin" / "pip", ec) ? venv : fs::path();
}

fs::path home() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path();
}

std::string tilde(const fs::path& p) {
    const std::string h = home().string(), s = p.string();
    return !h.empty() && s.rfind(h, 0) == 0 ? "~" + s.substr(h.size()) : s;
}

}

const char* services::update::version() { return MORODER_VERSION; }

bool services::update::offerAtLaunch() {
    if (devBuild() || !Config::get().CHECK_FOR_UPDATES || std::getenv("MORODER_JUST_UPDATED")) return false;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;

    nlohmann::json cache = readCache();
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string latest = cache.value("latest", std::string());

    // Ask GitHub at most once an hour; otherwise what it said last time.
    if (now - cache.value("checked_at", 0LL) > 3600) {
        const std::string fresh = latestRelease(1500);
        if (!fresh.empty()) {
            latest = fresh;
            cache["latest"] = latest;
            cache["checked_at"] = now;
            writeCache(cache);
        }
    }

    if (!isNewer(latest, MORODER_VERSION) || cache.value("skipped", std::string()) == latest) return false;

    using namespace cli;
    title("Moroder " + latest + " is out", std::string("you have ") + MORODER_VERSION);
    row("notes", "https://github.com/" MORODER_REPO "/releases/tag/v" + latest);
    if (!ask("Update now?")) {
        cache["skipped"] = latest;      // not asked again for this one; `moroder update` installs it
        writeCache(cache);
        std::cout << "  " << dim() << "Later then: moroder update installs it." << reset() << "\n\n";
        return false;
    }
    return runInstaller() == 0;
}

int services::update::runUpdate() {
    using namespace cli;
    title("Moroder update", std::string("installed: ") + MORODER_VERSION);

    Live live;
    const size_t rel = live.add("Release");
    live.set(rel, State::Run, "asking GitHub");
    live.start();
    const std::string latest = latestRelease(10000);
    const bool install = latest.empty() || isNewer(latest, MORODER_VERSION) || devBuild();

    if (latest.empty())                         live.set(rel, State::Warn, "GitHub unreachable, reinstalling the latest anyway");
    else if (devBuild())                        live.set(rel, State::Ok, std::string(MORODER_VERSION) + " → " + latest + " (development build)");
    else if (isNewer(latest, MORODER_VERSION))  live.set(rel, State::Ok, std::string(MORODER_VERSION) + " → " + latest);
    else                                        live.set(rel, State::Ok, latest + ", already the latest");

    if (install) {
        live.finish();
        // The installer draws its own progress (and updates yt-dlp too).
        return runInstaller() == 0 ? 0 : 1;
    }

    // Nothing to install: still keep yt-dlp current, it follows YouTube's changes.
    const size_t yt = live.add("yt-dlp");
    const std::string before = firstLine("yt-dlp --version 2>/dev/null");
    live.set(yt, State::Run, "updating " + before);
    const fs::path venv = privateYtdlp();
    if (!venv.empty()) std::system(("'" + (venv / "bin" / "pip").string() + "' install -q -U 'yt-dlp[default]' >/dev/null 2>&1").c_str());
    else if (have("brew")) std::system("brew upgrade yt-dlp >/dev/null 2>&1");
    else if (have("yt-dlp")) std::system("yt-dlp -U >/dev/null 2>&1");     // a distribution's yt-dlp refuses: its updates do it
    const std::string after = firstLine("yt-dlp --version 2>/dev/null");
    if (after.empty())         live.set(yt, State::Fail, "missing: " + INSTALL_YTDLP);
    else if (after != before)  live.set(yt, State::Ok, before + " → " + after);
    else                       live.set(yt, State::Ok, after + ", already the latest");
    live.finish();
    std::cout << "\n  " << green() << "✓" << reset() << " Up to date\n\n";
    return 0;
}

int services::update::doctor() {
    using namespace cli;
    const fs::path cfg = home() / ".config" / "moroder";
    const fs::path logs = home() / ".local" / "state" / "moroder" / "logs";
    std::error_code ec;

    title("Moroder doctor", std::string("version ") + MORODER_VERSION);

    Live live;
    struct Check { size_t id; std::function<void(size_t)> run; };
    std::vector<Check> checks;
    auto check = [&](const std::string& label, std::function<void(size_t)> fn) {
        const size_t id = live.add(label);
        live.set(id, State::Run, "checking");
        checks.push_back({id, std::move(fn)});
    };

    check("Moroder", [&](size_t id) {
        const std::string latest = latestRelease(6000);
        if (devBuild())                            live.set(id, State::Ok, std::string("development build") + (latest.empty() ? "" : ", latest release is " + latest));
        else if (latest.empty())                   live.set(id, State::Warn, std::string(MORODER_VERSION) + ", couldn't reach GitHub");
        else if (isNewer(latest, MORODER_VERSION)) live.set(id, State::Warn, latest + " is out: moroder update");
        else                                       live.set(id, State::Ok, latest + ", the latest");
    });
    check("yt-dlp", [&](size_t id) {
        const std::string mine = firstLine("yt-dlp --version 2>/dev/null");
        if (mine.empty()) { live.set(id, State::Fail, "missing: " + INSTALL_YTDLP); return; }
        live.set(id, State::Run, mine + ", comparing with the latest");
        const std::string latest = latestRelease(6000, "yt-dlp/yt-dlp");
        if (!latest.empty() && latest != mine && latest > mine) live.set(id, State::Warn, mine + ", " + latest + " is out: moroder update");
        else live.set(id, State::Ok, mine + (latest.empty() ? "" : ", the latest"));
    });
    check("mpv", [&](size_t id) {
        for (const char* lib : {"/opt/homebrew/lib/libmpv.dylib", "/usr/local/lib/libmpv.dylib",
                                "/usr/lib/x86_64-linux-gnu/libmpv.so.2", "/usr/lib/aarch64-linux-gnu/libmpv.so.2",
                                "/usr/lib64/libmpv.so.2", "/usr/lib/libmpv.so.2"})
            if (fs::exists(lib, ec)) { live.set(id, State::Ok, lib); return; }
        const std::string ld = firstLine("ldconfig -p 2>/dev/null | grep -o '/[^ ]*libmpv.so[^ ]*' | head -n1");
        if (!ld.empty()) live.set(id, State::Ok, ld);
        else if (have("mpv")) live.set(id, State::Ok, firstLine("mpv --version 2>/dev/null | head -n1"));
        else live.set(id, State::Fail, "libmpv not found: " + INSTALL_MPV);
    });
    check("Sign-in", [&](size_t id) {
        const fs::path session = cfg / "browser.json";
        if (!fs::exists(session, ec)) { live.set(id, State::Warn, "not signed in: moroder setup (or moroder -a)"); return; }
        std::string from;
        try { from = Config::get().COOKIE_BROWSER; } catch (...) {}
        // Whose account it is, as YouTube Music sees it.
        ytm::YTMusic client(session);
        const nlohmann::json account = client.getAccountInfo();
        if (!account.is_object()) { live.set(id, State::Fail, "YouTube Music doesn't recognise the session: moroder setup"); return; }
        std::string who = account.value("name", std::string());
        if (!account.value("handle", std::string()).empty()) who += " " + account.value("handle", std::string());
        if (!from.empty()) live.set(id, State::Ok, who + ", from " + from + " (kept fresh)");
        else live.set(id, State::Ok, who);
    });
    check("Network", [&](size_t id) {
        const long ms = pingMs("https://music.youtube.com/", 6000);
        if (ms < 0) live.set(id, State::Fail, "music.youtube.com unreachable");
        else live.set(id, ms > 1500 ? State::Warn : State::Ok, "music.youtube.com in " + std::to_string(ms) + " ms");
    });
#ifdef __APPLE__
    check("Notifications", [&](size_t id) {
        for (const char* app : {"/opt/homebrew/share/moroder/Moroder.app", "/usr/local/share/moroder/Moroder.app"})
            if (fs::exists(app, ec)) { live.set(id, State::Ok, "Moroder.app, cover art shown"); return; }
        live.set(id, State::Warn, "helper app missing: notifications without cover art");
    });
#endif
    check("Terminal", [&](size_t id) {
        const char* term = std::getenv("TERM_PROGRAM");
        const char* ct = std::getenv("COLORTERM");
        const std::string name = term ? term : (std::getenv("TERM") ? std::getenv("TERM") : "unknown");
        const bool truecolor = ct && (std::string(ct) == "truecolor" || std::string(ct) == "24bit");
        if (name == "WarpTerminal") live.set(id, State::Warn, "Warp: sideways swipes aren't passed on, use the ‹ › buttons");
        else live.set(id, truecolor ? State::Ok : State::Warn, name + (truecolor ? ", true colour" : ", no true colour: covers look flat"));
    });

    live.start();
    std::vector<std::thread> threads;
    for (auto& c : checks) threads.emplace_back([&c] { c.run(c.id); });
    for (auto& t : threads) t.join();
    live.finish();

    const int problems = live.count(State::Fail), warnings = live.count(State::Warn);
    std::cout << "\n";
    if (problems)      std::cout << "  " << red() << "✗" << reset() << " " << problems << (problems == 1 ? " problem" : " problems") << " to fix\n";
    else if (warnings) std::cout << "  " << yellow() << "!" << reset() << " Works, " << warnings << (warnings == 1 ? " thing" : " things") << " to look at\n";
    else               std::cout << "  " << green() << "✓" << reset() << " All good\n";
    std::cout << "\n";
    const std::string bin = firstLine("command -v moroder 2>/dev/null");
    if (!bin.empty()) row("program", tilde(bin));
    row("settings", tilde(cfg / "moroder.conf"));
    row("logs", tilde(logs));
    std::cout << "\n";
    return problems ? 1 : 0;
}

int services::update::uninstall() {
    using namespace cli;
    std::string bin = firstLine("command -v moroder 2>/dev/null");
    if (bin.empty()) bin = "/opt/homebrew/bin/moroder";
    const fs::path prefix = fs::path(bin).parent_path().parent_path();
    const fs::path share = prefix / "share" / "moroder";
    std::error_code ec;

    title("Uninstall Moroder");
    std::cout << "  " << dim() << "Removes" << reset() << "\n";
    row("program", tilde(bin), human(sizeOf(bin)));
    if (fs::exists(share, ec)) row("helper", tilde(share), human(sizeOf(share)));

    // The PATH line install.sh may have added.
    std::vector<fs::path> rcs;
    const std::string marker = "export PATH=\"" + (prefix / "bin").string() + ":$PATH\"";
    for (const char* rc : {".zshrc", ".bashrc", ".profile"}) {
        std::ifstream in(home() / rc);
        std::string l;
        while (std::getline(in, l)) if (l == marker) { rcs.push_back(home() / rc); break; }
    }
    for (const auto& rc : rcs) row("PATH line", tilde(rc));

    if (!ask("Uninstall?")) { std::cout << "\n"; return 0; }

    std::vector<std::pair<std::string, fs::path>> data = {
        {"settings", home() / ".config" / "moroder"},
        {"caches", home() / ".cache" / "moroder"},
        {"logs, state", home() / ".local" / "state" / "moroder"},
    };
    uintmax_t data_size = 0;
    std::cout << "\n  " << dim() << "Your data (sign-in session, settings, caches)" << reset() << "\n";
    for (const auto& [what, dir] : data)
        if (fs::exists(dir, ec)) { const auto n = sizeOf(dir); data_size += n; row(what, tilde(dir), human(n)); }
    const bool wipe = data_size > 0 && ask("Delete it too?", false);

    std::cout << "\n";
    Live live;
    auto remove = [&](const std::string& label, const fs::path& p) {
        const size_t id = live.add(label);
        const auto n = sizeOf(p);
        std::error_code e;
        fs::remove_all(p, e);
        live.set(id, e ? State::Fail : State::Ok, e ? e.message() : tilde(p) + "  " + human(n));
    };
    remove("program", bin);
    const fs::path yt_link = prefix / "bin" / "yt-dlp";      // install.sh's private yt-dlp (Debian/Ubuntu)
    if (fs::is_symlink(yt_link, ec) && fs::read_symlink(yt_link, ec).string().find(share.string()) == 0) remove("yt-dlp link", yt_link);
    if (fs::exists(share, ec)) remove("helper", share);
    for (const auto& rc : rcs) {
        const size_t id = live.add("PATH line");
        std::ifstream in(rc);
        std::vector<std::string> lines;
        std::string l;
        while (std::getline(in, l)) lines.push_back(l);
        in.close();
        std::ofstream out(rc, std::ios::trunc);
        for (size_t i = 0; i < lines.size(); i++) {
            if (lines[i] == marker) continue;
            if (lines[i] == "# Moroder" && i + 1 < lines.size() && lines[i + 1] == marker) continue;
            out << lines[i] << "\n";
        }
        live.set(id, State::Ok, "removed from " + tilde(rc));
    }
    if (wipe) for (const auto& [what, dir] : data) if (fs::exists(dir, ec)) remove(what, dir);
    live.finish();

    std::cout << "\n  " << green() << "✓" << reset() << " Moroder is uninstalled";
    if (!wipe && data_size) std::cout << dim() << "  (your data is kept)" << reset();
    std::cout << "\n\n";
    return 0;
}

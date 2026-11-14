#include "../../include/setup/setup.hpp"
#include "../../include/services/browser_cookies.hpp"

#include "../../include/config/config.hpp"
#include "../../include/utils/utils.hpp"
#include "../../include/services/cli_ui.hpp"
#include "../../include/utils/platform.hpp"
#include "../../include/setup/browser_window.hpp"
#include "../../include/ytm/ytmusic.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <map>


namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// ---------------------------------------------------------------------------
// output
// ---------------------------------------------------------------------------

/* ANSI
- Off when stdout is not a terminal, so a piped run or a pasted bug report does
  not come out full of escape codes
*/
bool colour_enabled = true;

// Same look as the other commands (cli_ui.hpp); --no-colour turns it off.
std::string dim()    { return colour_enabled ? cli::dim()    : ""; }
std::string bold()   { return colour_enabled ? cli::bold()   : ""; }
std::string red()    { return colour_enabled ? cli::red()    : ""; }
std::string green()  { return colour_enabled ? cli::green()  : ""; }
std::string yellow() { return colour_enabled ? cli::yellow() : ""; }
std::string reset()  { return colour_enabled ? cli::reset()  : ""; }

void step(const std::string& t) { std::cout << "\n  " << red() << "▸ " << reset() << bold() << t << reset() << "\n"; }
void info(const std::string& t) { std::cout << "     " << t << "\n"; }
void note(const std::string& t) { std::cout << "     " << dim() << t << reset() << "\n"; }
void ok(const std::string& t)   { std::cout << "  " << green() << "✓" << reset() << "  " << t << "\n"; }
void warn(const std::string& t) { std::cout << "  " << yellow() << "!" << reset() << "  " << t << "\n"; }
void fail(const std::string& t) { std::cout << "  " << red() << "✗" << reset() << "  " << t << "\n"; }

/* ask
- A bare return means yes, which is what every prompt here wants
- Non-interactive stdin answers yes so the paste path still works under a pipe
*/
bool ask(const std::string& question) {
    if (!platform::isTerminal(0)) return true;

    std::cout << "\n  " << bold() << "?" << reset() << "  " << question << dim() << " [Y/n] " << reset() << std::flush;

    std::string answer;
    if (!std::getline(std::cin, answer)) return false;

    for (char& c : answer) c = std::tolower(static_cast<unsigned char>(c));

    return answer.empty() || answer == "y" || answer == "yes";
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// The cookie is a live credential, so it is only ever shown as a stub.
std::string preview(const std::string& value, size_t keep = 12) {
    if (value.size() <= keep) return value;
    return value.substr(0, keep) + "... (" + std::to_string(value.size()) + " chars)";
}

// ---------------------------------------------------------------------------
// subprocess
// ---------------------------------------------------------------------------

struct Command {
    int exit_code = -1;
    std::string output;

    bool ok() const { return exit_code == 0; }
};

/* run
- popen with stderr folded in, which is where yt-dlp puts its complaints
- Every argument that comes from user input goes through shellQuote
*/
Command run(const std::string& command_line) {
    Command result;
    result.exit_code = platform::shell(command_line, &result.output, true);
    if (result.exit_code < 0) result.output = "could not start: " + command_line;
    return result;
}

std::string shellQuote(const std::string& value) {
    std::string out = "'";

    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }

    return out + "'";
}

bool haveCommand(const std::string& name) {
    return !platform::which(name).empty();
}

std::string commandVersion(const std::string& name, const std::string& flag = "--version") {
    Command r = run(shellQuote(name) + " " + flag + " | head -1");
    return r.ok() ? utils::trim(r.output) : "";
}

void openInBrowser(const std::string& url) {
    platform::openUrl(url);
}

// ---------------------------------------------------------------------------
// dependencies
// ---------------------------------------------------------------------------

/* checkDependencies
- libmpv and libcurl are linked into this binary, so their absence would have
  stopped it from starting; what matters at runtime is the mpv and yt-dlp the
  ytdl hook shells out to when a stream is opened
- Nothing here is fatal, a user can sign in now and install the rest after
*/
bool checkDependencies() {
    bool complete = true;

    struct Dependency {
        const char* name;
        const char* why;
        bool required;
    };

    const Dependency deps[] = {
#ifndef _WIN32
        {"mpv",    "plays the audio",                                  true},     // Windows: libmpv ships with moroder.exe
#endif
        {"yt-dlp", "resolves YouTube streams, mpv calls it internally", true},
        {"curl",   "not required at runtime, libcurl is linked in",     false},
    };

    std::string present;
    for (const Dependency& dep : deps) {
        if (!dep.required) continue;
        if (haveCommand(dep.name)) {
            // Just the version: "mpv v0.42.0 Copyright ..." -> "v0.42.0".
            std::string version;
            std::istringstream words(commandVersion(dep.name));
            for (std::string w; words >> w; )
                if (std::isdigit(static_cast<unsigned char>(w[w[0] == 'v' && w.size() > 1 ? 1 : 0]))) { version = w; break; }
            present += (present.empty() ? "" : dim() + " · " + reset()) + dep.name + " " + dim() + version + reset();
            continue;
        }
        warn(std::string(dep.name) + " is not installed, it " + dep.why);
        complete = false;
    }
    if (complete) ok(present);

    if (!complete) {
        note("Install the missing ones with your package manager, for example:");
        note("  arch    sudo pacman -S mpv yt-dlp");
        note("  debian  sudo apt install mpv yt-dlp");
        note("  fedora  sudo dnf install mpv yt-dlp");
        note("Signing in still works without them, playback will not.");
    }

    return complete;
}

// ---------------------------------------------------------------------------
// browsers
// ---------------------------------------------------------------------------

/* BrowserSpec
- The session itself comes from the pasted headers, so this exists only to
  locate the browser profile directory that goes into MPV_COOKIES_PATH, which
  is what yt-dlp reads when a stream is opened
*/
struct BrowserSpec {
    std::string name;
    std::string label;
    fs::path profile;

    bool firefox_fork = false;
    bool chromium_family = false;
    bool resolved = true;

    std::string problem;
};

/* forkRoots
- Every layout a Firefox fork profile turns up in: the native dotfile, the XDG
  style some distros use, flatpak, and ~/Library/Application Support on macOS
*/
std::vector<fs::path> forkRoots(const std::string& name) {
    const fs::path h = utils::home();
    if (h.empty()) return {};

#ifdef __APPLE__
    const fs::path mac = h / "Library" / "Application Support";

    if (name == "librewolf") return { mac / "librewolf" };
    if (name == "floorp")    return { mac / "Floorp" };
    if (name == "waterfox")  return { mac / "Waterfox" };
    if (name == "zen")       return { mac / "zen" };
    if (name == "tor")       return { mac / "TorBrowser-Data" / "Browser" };
    return { mac / "Firefox" };
#endif

    if (name == "librewolf") {
        return {
            h / ".librewolf",
            h / ".config" / "librewolf",
            h / ".var" / "app" / "io.gitlab.librewolf-community" / ".librewolf",
            h / ".mozilla" / "librewolf",
        };
    }

    if (name == "floorp") {
        return { h / ".floorp", h / ".var" / "app" / "one.ablaze.floorp" / ".floorp" };
    }

    if (name == "waterfox") {
        return { h / ".waterfox", h / ".var" / "app" / "net.waterfox.waterfox" / ".waterfox" };
    }

    if (name == "zen") {
        return { h / ".zen", h / ".var" / "app" / "app.zen_browser.zen" / ".zen" };
    }

    if (name == "mercury")  return { h / ".mercury" };
    if (name == "icecat")   return { h / ".mozilla" / "icecat" };
    if (name == "tor")      return { h / ".tor project" / "firefox" };

    return {
        h / ".mozilla" / "firefox",
        h / ".var" / "app" / "org.mozilla.firefox" / ".mozilla" / "firefox",
    };
}

/* iniSections
- profiles.ini is a plain INI and only a few keys matter, so this is a small
  reader rather than a dependency
*/
std::vector<std::pair<std::string, std::map<std::string, std::string>>> iniSections(const fs::path& file) {
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> sections;

    std::ifstream in(file);
    if (!in.is_open()) return sections;

    std::string line;

    while (std::getline(in, line)) {
        line = utils::trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[' && line.back() == ']') {
            sections.push_back({line.substr(1, line.size() - 2), {}});
            continue;
        }

        if (sections.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        sections.back().second[utils::lower(utils::trim(line.substr(0, eq)))] = utils::trim(line.substr(eq + 1));
    }

    return sections;
}

bool hasCookieStore(const fs::path& profile) {
    return fs::exists(profile / "cookies.sqlite");
}

/* profileFromIni
- Mirrors how Firefox picks a profile: the Install section names the one in
  use, which beats the Default flag because that often still points at a
  profile left over from an older install
- A profile that has never been opened has no cookies.sqlite, so candidates are
  checked rather than trusted
*/
fs::path profileFromIni(const fs::path& root) {
    auto sections = iniSections(root / "profiles.ini");
    if (sections.empty()) return {};

    auto resolve = [&](const std::map<std::string, std::string>& keys) -> fs::path {
        auto path = keys.find("path");
        if (path == keys.end() || path->second.empty()) return {};

        auto relative = keys.find("isrelative");
        bool is_relative = relative == keys.end() || relative->second != "0";

        return is_relative ? root / path->second : fs::path(path->second);
    };

    for (const auto& [name, keys] : sections) {
        if (utils::lower(name).rfind("install", 0) != 0) continue;

        auto def = keys.find("default");
        if (def == keys.end() || def->second.empty()) continue;

        fs::path candidate = root / def->second;
        if (hasCookieStore(candidate)) return candidate;
    }

    for (const auto& [name, keys] : sections) {
        if (utils::lower(name).rfind("profile", 0) != 0) continue;

        auto def = keys.find("default");
        if (def == keys.end() || def->second != "1") continue;

        fs::path candidate = resolve(keys);
        if (!candidate.empty() && hasCookieStore(candidate)) return candidate;
    }

    for (const auto& [name, keys] : sections) {
        if (utils::lower(name).rfind("profile", 0) != 0) continue;

        fs::path candidate = resolve(keys);
        if (!candidate.empty() && hasCookieStore(candidate)) return candidate;
    }

    return {};
}

/* scanForProfile
- Some packagings nest the profiles one level down, so ~/.config/librewolf
  holds a librewolf directory which holds the profiles
- Among several profiles the busiest cookie store is the live one
*/
fs::path scanForProfile(const fs::path& root, int depth = 2) {
    std::error_code ec;
    if (depth < 0 || !fs::is_directory(root, ec)) return {};

    fs::path profile = profileFromIni(root);
    if (!profile.empty()) return profile;

    fs::path best;
    uintmax_t best_size = 0;

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;

        if (hasCookieStore(entry.path())) {
            uintmax_t size = fs::file_size(entry.path() / "cookies.sqlite", ec);
            if (ec) continue;

            if (size > best_size) {
                best_size = size;
                best = entry.path();
            }
        }
    }

    if (!best.empty()) return best;

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;

        fs::path nested = scanForProfile(entry.path(), depth - 1);
        if (!nested.empty()) return nested;
    }

    return {};
}

fs::path findForkProfile(const std::string& name) {
    for (const fs::path& root : forkRoots(name)) {
        fs::path profile = scanForProfile(root);
        if (!profile.empty()) return profile;
    }

    return {};
}

/* resolveBrowser
- Finds the profile directory for MPV_COOKIES_PATH
- Not finding one is not fatal: the session still comes from the paste, only
  stream cookies are lost, so failures here are reported and carried past
*/
BrowserSpec resolveBrowser(const std::string& browser, const fs::path& profile_override) {
    BrowserSpec spec;
    spec.name = browser;
    spec.label = browser;

    const std::string name = utils::lower(browser);

    /* The name mapping lives in Config, so setup and MPV agree on what each
    browser means and a new one only has to be added in one place.
    */
    const std::string token = Config::ytdlBrowser(browser);

    spec.firefox_fork = token == "firefox";
    spec.chromium_family = !token.empty() && !spec.firefox_fork;

    if (token.empty() && !contains(browser, ":")) {
        spec.resolved = false;
        spec.problem = "yt-dlp cannot read '" + browser + "'";
        return spec;
    }

    if (!profile_override.empty()) {
        if (!hasCookieStore(profile_override)) {
            spec.resolved = false;
            spec.problem = "no cookies.sqlite in " + profile_override.string();
            return spec;
        }

        spec.profile = profile_override;
        spec.label = browser + " (" + profile_override.filename().string() + ")";
        spec.firefox_fork = true;
        return spec;
    }

    // An explicit browser:profile spec is left for yt-dlp to interpret.
    if (contains(browser, ":")) {
        return spec;
    }

    if (spec.firefox_fork) {
        fs::path profile = findForkProfile(name);

        if (profile.empty()) {
            spec.resolved = false;
            spec.problem = "could not find a " + browser + " profile with a cookie store";
            return spec;
        }

        spec.profile = profile;
        spec.label = browser + " (" + profile.filename().string() + ")";

        return spec;
    }

    return spec;
}

/* parseHeaderBlob
- Accepts what the browser actually puts on the clipboard:
    name: value            DevTools "copy request headers"
    -H 'name: value'       a copied cURL command
- Names are lower-cased, matching what YTMusic's auth loader expects
*/
std::map<std::string, std::string> parseHeaderBlob(const std::string& blob) {
    std::map<std::string, std::string> headers;

    std::istringstream in(blob);
    std::string line;

    while (std::getline(in, line)) {
        line = utils::trim(line);
        if (line.empty()) continue;

        /* Chromium's "Copy as cURL" (Chrome, Arc, Edge, Brave...) puts the
        cookies in -b '...' rather than -H 'cookie: ...'; without this the
        cookie line was misread as a header and setup found no cookie
        */
        if (line.rfind("-b ", 0) == 0 || line.rfind("--cookie ", 0) == 0) {
            size_t open = line.find_first_of("'\"");
            if (open == std::string::npos) continue;

            size_t close = line.find(line[open], open + 1);
            if (close == std::string::npos) continue;

            headers["cookie"] = utils::trim(line.substr(open + 1, close - open - 1));
            continue;
        }

        if (contains(line, "-H ")) {
            size_t pos = 0;

            while ((pos = line.find("-H ", pos)) != std::string::npos) {
                size_t open = line.find_first_of("'\"", pos);
                if (open == std::string::npos) break;

                char quote = line[open];
                size_t close = line.find(quote, open + 1);
                if (close == std::string::npos) break;

                std::string pair = line.substr(open + 1, close - open - 1);
                size_t colon = pair.find(':');

                if (colon != std::string::npos) {
                    headers[utils::lower(utils::trim(pair.substr(0, colon)))] = utils::trim(pair.substr(colon + 1));
                }

                pos = close + 1;
            }

            continue;
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string name = utils::lower(utils::trim(line.substr(0, colon)));

        // HTTP/2 pseudo headers are noise, and a bare URL line would otherwise
        // parse as a header called "https".
        if (name.empty() || name[0] == ':' || name == "https" || name == "http") continue;

        std::string value = utils::trim(line.substr(colon + 1));
        if (value.empty()) continue;

        headers[name] = value;
    }

    return headers;
}

std::string readUntilEof() {
    std::ostringstream out;
    std::string line;

    while (std::getline(std::cin, line)) out << line << "\n";

    return out.str();
}

// ---------------------------------------------------------------------------
// credentials
// ---------------------------------------------------------------------------

struct Credentials {
    std::string cookie;
    std::string user_agent;
    std::string origin;
    std::string visitor_id;
    std::string accept_language;
    std::string source;
    int authuser = 0;      // which Google account of the browser (x-goog-authuser)
};

const char* DEFAULT_USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:88.0) Gecko/20100101 Firefox/88.0";

/* cookieValue
- The name must start the pair, otherwise PAPISID would match inside
  __Secure-3PAPISID and report a credential that is not there
*/
std::string cookieValue(const std::string& raw, const std::string& name) {
    size_t pos = 0;

    while ((pos = raw.find(name + "=", pos)) != std::string::npos) {
        if (pos != 0) {
            char prev = raw[pos - 1];

            if (prev != ' ' && prev != ';') {
                pos += name.size();
                continue;
            }
        }

        size_t start = pos + name.size() + 1;
        size_t end   = raw.find(';', start);

        return utils::trim(raw.substr(start, end == std::string::npos ? end : end - start));
    }

    return "";
}

/* checkStructure
- Catches the two mistakes that otherwise produce a file the app rejects on
  startup: a cookie copied from a signed-out tab, and one from another site
*/
bool checkStructure(const Credentials& creds) {
    if (creds.cookie.empty()) {
        fail("no cookie header found");
        return false;
    }

    std::string sapisid = cookieValue(creds.cookie, "__Secure-3PAPISID");
    if (sapisid.empty()) sapisid = cookieValue(creds.cookie, "SAPISID");

    if (sapisid.empty()) {
        fail("the cookie has no SAPISID");
        note("That almost always means the tab was signed out, or the headers came");
        note("from a different site. Sign in at music.youtube.com and try again.");
        return false;
    }


    return true;
}

// ---------------------------------------------------------------------------
// the session, straight from a browser
// ---------------------------------------------------------------------------

/* installedBrowsers
- Browsers with a profile on this machine, the configured one first: where
  setup looks for a YouTube Music session before asking for a copy-paste
- Each is a name and the directory yt-dlp is pointed at (Firefox forks are
  read as Firefox from their own profile directory)
*/
struct FoundBrowser { std::string name; std::string ytdlp_spec; fs::path dir; };

#ifdef _WIN32
fs::path appData()      { const char* p = std::getenv("APPDATA");      return p ? fs::path(p) : utils::home() / "AppData/Roaming"; }
fs::path localAppData() { const char* p = std::getenv("LOCALAPPDATA"); return p ? fs::path(p) : utils::home() / "AppData/Local"; }
#endif

std::vector<FoundBrowser> installedBrowsers(const std::string& preferred) {
    const fs::path h = utils::home();
    std::vector<std::pair<std::string, std::vector<fs::path>>> known = {
#ifdef __APPLE__
        {"arc",      {h / "Library/Application Support/Arc/User Data"}},
        {"chrome",   {h / "Library/Application Support/Google/Chrome"}},
        {"brave",    {h / "Library/Application Support/BraveSoftware/Brave-Browser"}},
        {"edge",     {h / "Library/Application Support/Microsoft Edge"}},
        {"vivaldi",  {h / "Library/Application Support/Vivaldi"}},
        {"chromium", {h / "Library/Application Support/Chromium"}},
        {"firefox",  {h / "Library/Application Support/Firefox/Profiles"}},
        {"zen",      {h / "Library/Application Support/zen/Profiles"}},
        {"librewolf",{h / "Library/Application Support/librewolf/Profiles"}},
#elif defined(_WIN32)
        {"chrome",   {localAppData() / "Google/Chrome/User Data"}},
        {"edge",     {localAppData() / "Microsoft/Edge/User Data"}},
        {"brave",    {localAppData() / "BraveSoftware/Brave-Browser/User Data"}},
        {"vivaldi",  {localAppData() / "Vivaldi/User Data"}},
        {"chromium", {localAppData() / "Chromium/User Data"}},
        {"opera",    {appData() / "Opera Software/Opera Stable"}},
        {"firefox",  {appData() / "Mozilla/Firefox/Profiles"}},
        {"zen",      {appData() / "zen/Profiles"}},
        {"librewolf",{appData() / "librewolf/Profiles"}},
        {"floorp",   {appData() / "Floorp/Profiles"}},
#else
        {"firefox",  {h / ".mozilla/firefox", h / "snap/firefox/common/.mozilla/firefox", h / ".var/app/org.mozilla.firefox/.mozilla/firefox"}},
        {"chrome",   {h / ".config/google-chrome"}},
        {"chromium", {h / ".config/chromium", h / "snap/chromium/common/chromium"}},
        {"brave",    {h / ".config/BraveSoftware/Brave-Browser"}},
        {"edge",     {h / ".config/microsoft-edge"}},
        {"vivaldi",  {h / ".config/vivaldi"}},
        {"opera",    {h / ".config/opera"}},
        {"librewolf",{h / ".librewolf", h / ".var/app/io.gitlab.librewolf-community/.librewolf"}},
        {"zen",      {h / ".zen", h / ".var/app/app.zen_browser.zen/.zen"}},
        {"floorp",   {h / ".floorp"}},
#endif
    };
    const std::vector<std::string> firefox_like = {"firefox", "zen", "librewolf", "floorp"};
    std::vector<FoundBrowser> out;
    std::error_code ec;
    for (const auto& [name, dirs] : known) {
        for (const auto& d : dirs) {
            if (!fs::is_directory(d, ec)) continue;
            const bool ff = std::find(firefox_like.begin(), firefox_like.end(), name) != firefox_like.end();
            out.push_back({name, ff ? "firefox:" + d.string() : name, d});
            break;
        }
    }
    std::stable_partition(out.begin(), out.end(), [&](const FoundBrowser& b) { return b.name == utils::lower(preferred); });
    return out;
}

// The Python yt-dlp runs on (its shebang), so its cookie reader can be used.
std::string ytdlpPython() {
    if (std::getenv("MORODER_YTDLP_CLI")) return "";      // force the Python-free path (tests, debugging)
#ifdef _WIN32
    // yt-dlp is an .exe there: a Python that can import yt_dlp, if any.
    for (const char* candidate : {"python", "py"})
        if (!platform::which(candidate).empty() && run(std::string(candidate) + " -c \"import yt_dlp\"").ok()) return candidate;
    return "";
#else
    const std::string path = platform::which("yt-dlp");
    std::ifstream in(path);
    std::string line;
    if (path.empty() || !in || !std::getline(in, line) || line.rfind("#!", 0) != 0) return "";
    std::istringstream words(line.substr(2));
    std::string interp, arg;
    words >> interp >> arg;
    if (interp.size() >= 4 && interp.compare(interp.size() - 4, 4, "/env") == 0) interp = arg;
    return interp.find("python") != std::string::npos ? interp : "";
#endif
}

/* cookiesViaYtdlpCli
- The same with no Python at hand (yt-dlp.exe on Windows, a standalone
  build): yt-dlp itself saves the browser's cookies to a file on its way out,
  after an empty search that never touches the network
*/
std::string cookiesViaYtdlpCli(const FoundBrowser& b, std::string& error) {
    const fs::path file = fs::temp_directory_path() / ("moroder-cookies-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    platform::Process p;
    if (!p.start({ "yt-dlp", "--cookies-from-browser", b.ytdlp_spec, "--cookies", file.string(),
                   "--skip-download", "--no-warnings", "--quiet", "ytsearch0:moroder" })) { error = "couldn't start yt-dlp"; return ""; }
    p.closeStdin();
    std::string ignored;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (p.read(ignored, 200) >= 0) { ignored.clear(); if (std::chrono::steady_clock::now() > deadline) { p.kill(); break; } }
    p.wait();

    // Netscape format: domain, subdomains, path, secure, expiry, name, value.
    std::ifstream in(file);
    std::map<std::string, std::string> jar;
    std::vector<std::string> order;
    for (std::string line; std::getline(in, line); ) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("#HttpOnly_", 0) == 0) line.erase(0, 10);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string x; std::getline(ss, x, '\t'); ) f.push_back(x);
        if (f.size() < 7) continue;
        std::string domain = f[0];
        if (!domain.empty() && domain[0] == '.') domain.erase(0, 1);
        if (domain != "youtube.com" && domain != "music.youtube.com" && domain != "www.youtube.com") continue;
        if (!jar.count(f[5])) { order.push_back(f[5]); jar[f[5]] = f[6]; }
    }
    in.close();
    std::error_code ec;
    fs::remove(file, ec);

    std::string header;
    for (const auto& name : order) header += (header.empty() ? "" : "; ") + name + "=" + jar[name];
    if (header.empty()) error = "couldn't read its cookies";
    return header;
}

/* cookiesViaYtdlp
- yt-dlp knows how to open every browser's cookie store (Firefox's sqlite,
  Chromium's encrypted one with the system keyring): the youtube.com cookies
  as one Cookie header, "" with error set when there are none
*/
std::string cookiesViaYtdlp(const FoundBrowser& b, std::string& error) {
    const std::string python = ytdlpPython();
    if (python.empty()) return cookiesViaYtdlpCli(b, error);
    static const char* script =
        "import sys\n"
        "from yt_dlp.cookies import extract_cookies_from_browser\n"
        "spec = sys.argv[1]; name, _, profile = spec.partition(':')\n"
        "jar = extract_cookies_from_browser(name, profile or None)\n"
        "out = {}\n"
        "for c in jar:\n"
        "    d = (c.domain or '').lstrip('.')\n"
        "    if d in ('youtube.com', 'music.youtube.com', 'www.youtube.com'):\n"
        "        out.setdefault(c.name, c.value)\n"
        "print('; '.join(k + '=' + v for k, v in out.items()))\n";
    // No shell: the script is several lines, which cmd.exe can't pass.
    platform::Process p;
    if (!p.start({ python, "-I", "-c", script, b.ytdlp_spec })) { error = "couldn't start yt-dlp's Python"; return ""; }
    p.closeStdin();
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (p.read(out, 200) >= 0) if (std::chrono::steady_clock::now() > deadline) { p.kill(); break; }
    const bool ok = p.wait() == 0;
    if (!ok) error = "couldn't read its cookies";
    return ok ? utils::trim(out) : "";
}

/* choose
- A numbered pick: the number typed, or the default on a bare Return; the
  default too when input is not a terminal
*/
size_t choose(const std::string& question, size_t count, size_t fallback = 0) {
    if (!platform::isTerminal(0)) return fallback;
    while (true) {
        std::cout << "\n  " << bold() << "?" << reset() << "  " << question << dim() << " [" << (fallback + 1) << "] " << reset() << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return fallback;
        line = utils::trim(line);
        if (line.empty()) return fallback;
        const int n = std::atoi(line.c_str());
        if (n >= 1 && static_cast<size_t>(n) <= count) return static_cast<size_t>(n - 1);
        note("a number from 1 to " + std::to_string(count));
    }
}

/* Account
- One Google account signed in to one browser: several can share a browser,
  told apart by x-goog-authuser (0 = the first signed in)
*/
struct Account {
    std::string browser;
    bool native = false;          // macOS Chromium store: refreshed later (COOKIE_BROWSER)
    std::string cookie;
    int authuser = 0;
    std::string name, handle;
    long playlists = -1;
};

Credentials credentialsFor(const Account& a) {
    Credentials c;
    c.cookie = a.cookie;
    c.user_agent = DEFAULT_USER_AGENT;
    c.origin = "https://music.youtube.com";
    c.accept_language = "en-US,en;q=0.9";
    c.source = a.browser + " cookie store";
    c.authuser = a.authuser;
    return c;
}

bool writeHeaders(const fs::path& target, const Credentials& creds);

/* accountsIn
- Asks YouTube Music who each account of this browser's session is (its name
  and @handle, and how many playlists it has), trying x-goog-authuser 0, 1,
  2... until one answers signed out or repeats an earlier one
*/
std::vector<Account> accountsIn(const std::string& browser, bool native, const std::string& cookie, const fs::path& scratch) {
    std::vector<Account> out;
    for (int authuser = 0; authuser < 6; authuser++) {
        Account a;
        a.browser = browser; a.native = native; a.cookie = cookie; a.authuser = authuser;
        if (!writeHeaders(scratch, credentialsFor(a))) break;
        ytm::YTMusic client(scratch);
        const json info = client.getAccountInfo();
        if (!info.is_object()) {
            // The account menu didn't say; a library that answers still proves the sign-in.
            if (authuser > 0) break;
            const json lists = client.getLibraryPlaylists(50);
            if (!lists.is_array() || lists.empty()) break;
            a.name = "YouTube account";
            a.playlists = static_cast<long>(lists.size());
            out.push_back(a);
            break;
        }
        a.name = info.value("name", std::string());
        a.handle = info.value("handle", std::string());
        const bool seen = std::any_of(out.begin(), out.end(), [&](const Account& o) { return o.name == a.name && o.handle == a.handle; });
        if (seen) break;      // past the last account: Google answers with the first one again
        const json lists = client.getLibraryPlaylists(50);
        a.playlists = lists.is_array() ? static_cast<long>(lists.size()) : -1;
        out.push_back(a);
    }
    std::error_code ec;
    fs::remove(scratch, ec);
    return out;
}

// A YouTube login: SAPISID, or the __Secure- ones recent Chromium keeps instead.
bool hasLogin(const std::string& cookie) {
    return contains(cookie, "SAPISID=") || contains(cookie, "3PAPISID=") || contains(cookie, "1PAPISID=");
}

std::string prettyBrowser(const std::string& name) {
    static const std::map<std::string, std::string> names = {
        {"arc", "Arc"}, {"chrome", "Chrome"}, {"brave", "Brave"}, {"edge", "Edge"}, {"vivaldi", "Vivaldi"},
        {"chromium", "Chromium"}, {"firefox", "Firefox"}, {"zen", "Zen"}, {"librewolf", "LibreWolf"},
        {"floorp", "Floorp"}, {"opera", "Opera"}, {"safari", "Safari"},
    };
    auto it = names.find(utils::lower(name));
    return it == names.end() ? name : it->second;
}

std::string accountDetail(const Account& a) {
    std::string s = a.handle;
    if (a.playlists >= 0) s += (s.empty() ? "" : " · ") + (a.playlists >= 50 ? std::string("50+") : std::to_string(a.playlists)) + (a.playlists == 1 ? " playlist" : " playlists");
    return s;
}

std::string accountLine(const Account& a) {
    const std::string d = accountDetail(a);
    return a.name + (d.empty() ? "" : "  " + d);
}

// ---------------------------------------------------------------------------
// acquisition
// ---------------------------------------------------------------------------

/* fromPaste
- The only way the session is obtained
- Reading the cookie out of the browser store with yt-dlp also worked, but it
  recovered the cookie alone: no visitor id and no real user-agent, which left
  every start scraping a visitor id from the homepage and made requests slower
  and easier to rate limit
*/
bool fromPaste(Credentials& out) {
    std::cout << "\n";
    info("1. Open " + std::string(bold()) + "music.youtube.com" + reset() + " and make sure you are signed in");
#ifdef __APPLE__
    info("2. Open DevTools with Cmd+Option+I, then the Network tab");
#else
    info("2. Open DevTools with F12, then the Network tab");
#endif
    info("3. Reload the page and filter the requests for " + std::string(bold()) + "/youtubei/" + reset());
    info("4. Click any POST request in the list");
    info("5. Under Headers, copy everything below Request Headers");
    note("   Firefox: right click a header, Copy All. Chrome: Copy as cURL works too.");
    std::cout << "\n";
    info("Paste it below, then press " + std::string(bold()) + "Ctrl-D" + reset() + " on a blank line:");
    std::cout << "\n";

    std::string blob = readUntilEof();

    // Ctrl-D left the stream at eof and later prompts still need to read.
    std::cin.clear();

    std::map<std::string, std::string> headers = parseHeaderBlob(blob);

    if (headers.empty()) {
        fail("nothing that looked like a header was pasted");
        return false;
    }

    ok("parsed " + std::to_string(headers.size()) + " headers");

    auto get = [&](const std::string& name) -> std::string {
        auto it = headers.find(name);
        return it == headers.end() ? "" : it->second;
    };

    out.cookie          = get("cookie");
    out.user_agent      = get("user-agent");
    out.origin          = get("origin").empty() ? get("x-origin") : get("origin");
    out.visitor_id      = get("x-goog-visitor-id");
    out.accept_language = get("accept-language");
    out.source          = "pasted headers";

    if (out.user_agent.empty()) {
        note("no user-agent in the paste, using a default");
        out.user_agent = DEFAULT_USER_AGENT;
    }

    if (out.origin.empty()) out.origin = "https://music.youtube.com";
    if (out.accept_language.empty()) out.accept_language = "en-US,en;q=0.5";

    return true;
}

// ---------------------------------------------------------------------------
// writing and verifying
// ---------------------------------------------------------------------------

json toHeaders(const Credentials& creds) {
    json out;

    out["cookie"] = creds.cookie;
    out["user-agent"] = creds.user_agent;
    out["origin"] = creds.origin;
    out["x-origin"] = creds.origin;
    out["accept-language"] = creds.accept_language;

    if (!creds.visitor_id.empty()) out["x-goog-visitor-id"] = creds.visitor_id;
    if (creds.authuser > 0) out["x-goog-authuser"] = std::to_string(creds.authuser);

    return out;
}

/* writeHeaders
- 0600 because the default umask would leave a Google session credential
  readable by every other account on the machine
*/
bool writeHeaders(const fs::path& target, const Credentials& creds) {
    try {
        utils::ensure_dir(target.parent_path());

    } catch (const std::exception& e) {
        fail(e.what());
        return false;
    }

    std::ofstream file(target, std::ios::trunc);
    if (!file.is_open()) {
        fail("could not write " + target.string());
        return false;
    }

    file << toHeaders(creds).dump(2) << "\n";
    file.close();

    if (!platform::restrictToOwner(target)) {
        note("could not tighten permissions on " + target.string());
    }

    return true;
}

/* verify
- A structurally sound cookie can still be expired, and without this the app
  only finds out when the library page fails
- Goes through YTMusic so the check exercises the same auth path the app uses,
  SAPISIDHASH included, against a file written exactly as the app will read it
*/
bool verify(const fs::path& headers_file, bool show = true) {
    ytm::YTMusic client(headers_file);

    if (!client.lastError().empty()) {
        fail("the headers were rejected, " + client.lastError());
        return false;
    }

    if (!client.isAuthenticated()) {
        fail("the headers did not produce an authenticated session");
        return false;
    }

    json playlists = client.getLibraryPlaylists(5);
    const ytm::Error& error = client.lastErrorInfo();

    if (!error.ok()) {
        fail("the test request failed, " + error.detail);
        note("A network error here is worth retrying. Anything else usually means");
        note("the cookie is expired, so sign in again and re-run this.");
        return false;
    }

    const json account = client.getAccountInfo();
    if (!account.is_object() && !playlists.empty()) {
        if (show) ok("signed in, " + std::to_string(playlists.size()) + (playlists.size() == 1 ? " playlist" : " playlists"));
        return true;
    }
    if (!account.is_object()) {
        fail("YouTube Music sees no signed-in account with this session");
        note("Sign in at music.youtube.com in that browser, then run moroder setup again.");
        return false;
    }
    const std::string who = account.value("name", std::string()) + (account.value("handle", std::string()).empty() ? "" : " " + account.value("handle", std::string()));
    if (show) ok("signed in as " + bold() + who + reset() + ", " + std::to_string(playlists.size()) + (playlists.size() == 1 ? " playlist" : " playlists") + (playlists.size() >= 5 ? "+" : ""));
    return true;
}

// ---------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------

/* recordPaths
- YTM_COOKIES_PATH is the directory holding browser.json, matching how Music
  builds the path: cfg.YTM_COOKIES_PATH / "browser.json"
- MPV_COOKIES_PATH is the browser profile directory, which MPV passes to yt-dlp
  as cookies-from-browser=firefox:<path>, so only a Firefox-family profile can
  go in there
*/
bool recordPaths(const fs::path& cookies_dir, const BrowserSpec& browser) {
    std::vector<std::pair<std::string, std::string>> updates;

    updates.push_back({"YTM_COOKIES_PATH", Config::quote(utils::tildify(cookies_dir))});
    updates.push_back({"BROWSER", Config::quote(browser.name)});

    if (!browser.profile.empty() && browser.firefox_fork) {
        updates.push_back({"MPV_COOKIES_PATH", Config::quote(utils::tildify(browser.profile))});
    }

    if (!Config::setValues(updates)) {
        fail("could not update " + Config::path());
        return false;
    }

    return true;
}

void usage() {
    std::cout << "\n  " << bold() << red() << "▶ moroder setup" << reset() << "  " << dim() << "[options]" << reset() << "\n\n";
    auto opt = [](const std::string& o, const std::string& what) {
        std::string pad = o;
        while (pad.size() < 18) pad += ' ';
        std::cout << "    " << pad << dim() << what << reset() << "\n";
    };
    std::cout << "    " << dim() << "Without options it looks for your YouTube Music session in every browser" << reset() << "\n"
              << "    " << dim() << "you have (asking first), and only asks for a copy-paste if none has one." << reset() << "\n\n";
    opt("--browser NAME", "only that browser (chrome, arc, brave, edge, firefox, zen, ...)");
    opt("--profile DIR", "browser profile directory, for stream cookies");
    opt("--no-colour", "plain output");
    std::cout << "\n    " << dim() << "Guide: https://github.com/" MORODER_REPO "/blob/main/docs/sign-in.md" << reset() << "\n\n";
}

}

int setup::run(int argc, char* argv[]) {
    colour_enabled = platform::isTerminal(1);

    std::vector<std::string> args(argv + 1, argv + argc);

    fs::path profile_override;

    /* The configured browser is the default, so a second run does not have to
    repeat --browser. Config::get() throws on a config it cannot parse, and
    setup has to work in exactly that situation, so a failure here falls back
    to the built-in default rather than stopping.
    */
    std::string browser = "firefox";

    try {
        const std::string configured = Config::get().BROWSER;
        if (!configured.empty()) browser = configured;

    } catch (const std::exception& e) {
        std::cerr << "note: could not read the existing config (" << e.what() << ")\n";
    }

    bool browser_given = false;

    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];

        if (arg == "setup") continue;

        if (arg == "--no-colour") { colour_enabled = false; continue; }
        if (arg == "--help")      { usage(); return 0; }

        if (arg == "--browser" && i + 1 < args.size()) { browser = args[++i]; browser_given = true; continue; }
        if (arg == "--profile" && i + 1 < args.size()) { profile_override = args[++i]; continue; }

        std::cerr << "unknown argument: " << arg << "\n\n";
        usage();
        return 2;
    }

    if (utils::home().empty()) {
        std::cerr << "HOME is not set, cannot find the config directory\n";
        return 1;
    }

    // Same directory the config lives in, so browser.json sits beside
    // moroder.conf and YTM_COOKIES_PATH is simply that directory.
    const fs::path cookies_dir = fs::path(Config::path()).parent_path();
    const fs::path headers_file = cookies_dir / "browser.json";

    std::cout << "\n  " << bold() << red() << "▶ Moroder setup" << reset() << "  " << dim() << "sign in to YouTube Music, once" << reset() << "\n\n";

    checkDependencies();

    Credentials creds;
    bool got = false;
    bool from_store = false;
    std::string cookie_spec;          // what the app re-reads to stay signed in (COOKIE_BROWSER)

    // Piped in (pbpaste | moroder setup): the headers are on stdin, use them.
    const bool interactive = platform::isTerminal(0);

    std::vector<FoundBrowser> found = installedBrowsers(browser);
    if (browser_given) {
        found.erase(std::remove_if(found.begin(), found.end(), [&](const FoundBrowser& b) { return b.name != utils::lower(browser); }), found.end());
        if (found.empty()) found.push_back({utils::lower(browser), utils::lower(browser), {}});
    }
    if (!interactive) found.clear();

    // When a browser last touched its cookies: the one you use comes first.
    auto lastUsed = [](const fs::path& dir, const std::string& profile) -> long long {
        long long newest = 0;
        std::error_code ec;
        if (dir.empty()) return 0;
        if (!profile.empty()) {
            for (const fs::path& p : {dir / profile / "Network" / "Cookies", dir / profile / "Cookies"})
                newest = std::max(newest, platform::modifiedAt(p));
            return newest;
        }
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
            if (it->path().filename() == "cookies.sqlite") newest = std::max(newest, platform::modifiedAt(it->path()));
        }
        return newest;
    };

    const fs::path scratch = headers_file.string() + ".probe";
    bool paste = !interactive;

    /* The browsers signed in to YouTube, found without prompting (macOS
    Chromium: the login cookie is there, nothing decrypted; others through
    yt-dlp's reader). You pick one with the arrow keys; only that one is read
    (one Keychain prompt on macOS), then YouTube Music says whose account it
    is, and you pick the account when the browser has several.
    */
    while (!got && !paste && !found.empty()) {
        struct Source { FoundBrowser fb; std::string profile_dir, profile_name; bool native; std::string cookie; long long used = 0; };
        std::vector<Source> sources;
        {
            cli::Live live;
            const size_t id = live.add("Browsers");
            live.set(id, cli::State::Run, "looking for a YouTube Music sign-in");
            live.start();
            std::mutex m;
            std::vector<std::thread> threads;
            for (const auto& b : found) {
                threads.emplace_back([&, b] {
                    std::vector<Source> mine;
                    if (services::isCookieStoreBrowser(b.name)) {
                        for (const auto& [dir, name] : services::signedInProfiles(b.name)) mine.push_back({b, dir, name, true, ""});
                    } else {
                        std::string error;
                        const std::string cookie = cookiesViaYtdlp(b, error);
                        if (hasLogin(cookie)) mine.push_back({b, "", "", false, cookie});
                    }
                    std::lock_guard lock(m);
                    sources.insert(sources.end(), mine.begin(), mine.end());
                });
            }
            for (auto& t : threads) t.join();
            // The most recently used first.
            for (auto& src : sources) src.used = lastUsed(src.fb.dir, src.profile_dir);
            std::stable_sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) { return a.used > b.used; });
            std::string names;
            for (const auto& s : sources) if (names.find(prettyBrowser(s.fb.name)) == std::string::npos) names += (names.empty() ? "" : ", ") + prettyBrowser(s.fb.name);
            live.set(id, sources.empty() ? cli::State::Warn : cli::State::Ok, sources.empty() ? "none signed in to YouTube" : "signed in: " + names);
            live.finish();
        }

        std::vector<cli::Option> options;
        for (const auto& s : sources) {
            const bool several = std::count_if(sources.begin(), sources.end(), [&](const Source& o) { return o.fb.name == s.fb.name; }) > 1;
            options.push_back({prettyBrowser(s.fb.name), several ? s.profile_name : ""});
        }
        // A browser window of its own: for browsers whose cookies are locked
        // (Chrome / Edge on Windows), or to use another account.
        const std::string window_browser = browserWindowName();
        const int window = window_browser.empty() ? -1 : static_cast<int>(options.size());
        if (window >= 0) options.push_back({"New window", "sign in there, in " + window_browser + " (its own profile)"});
        const int sign_in = static_cast<int>(options.size());
        options.push_back({"Sign in first", "opens music.youtube.com, then looks again"});
        const int by_hand = static_cast<int>(options.size());
        options.push_back({"Paste headers", "copied from your browser by hand"});

        const int pick = cli::select(sources.empty() ? "No browser is signed in. Now what?" : "Use which browser?", options,
                                     sources.empty() ? (window >= 0 ? window : sign_in) : 0);
        if (pick < 0) { std::cout << "\n"; return 1; }
        if (pick == by_hand) { paste = true; break; }
        if (pick == sign_in) {
            openInBrowser("https://music.youtube.com/");
            std::cout << "  " << bold() << "?" << reset() << "  Sign in there, then press Return " << std::flush;
            std::string ignored;
            std::getline(std::cin, ignored);
            continue;
        }

        Source s;
        if (pick == window) {
            std::string cookie, error;
            {
                cli::Live live;
                const size_t id = live.add(window_browser);
                live.set(id, cli::State::Run, "sign in to YouTube Music in the window that opened");
                live.start();
                cookie = signInWithBrowserWindow(cookies_dir / "browser-profile", [] { return false; }, error);
                live.set(id, cookie.empty() ? cli::State::Warn : cli::State::Ok, cookie.empty() ? error : "signed in");
                live.finish();
            }
            if (cookie.empty()) continue;
            s = {FoundBrowser{utils::lower(window_browser), utils::lower(window_browser), {}}, "", "", false, cookie, 0};
        } else {
            s = sources[static_cast<size_t>(pick)];
        }
        std::string spec = s.fb.name;
        if (s.native && s.cookie.empty()) {
            if (s.profile_dir != "Default") spec += ":" + s.profile_dir;
            std::string error;
            s.cookie = services::readYouTubeCookies(spec, error);
            if (!hasLogin(s.cookie)) { warn(prettyBrowser(s.fb.name) + ": " + (error.empty() ? "couldn't read its session" : error)); continue; }
        }

        std::vector<Account> accounts;
        {
            cli::Live live;
            const size_t id = live.add("Account");
            live.set(id, cli::State::Run, "asking YouTube Music");
            live.start();
            accounts = accountsIn(s.fb.name, s.native, s.cookie, scratch);
            if (accounts.size() == 1) live.set(id, cli::State::Ok, accountLine(accounts[0]));
            else if (accounts.empty()) live.set(id, cli::State::Warn, "YouTube Music doesn't recognise this session: sign in again there");
            else live.set(id, cli::State::Ok, std::to_string(accounts.size()) + " accounts in " + prettyBrowser(s.fb.name));
            live.finish();
        }
        if (accounts.empty()) continue;

        size_t chosen = 0;
        if (accounts.size() > 1) {
            std::vector<cli::Option> list;
            size_t best = 0;
            for (size_t i = 0; i < accounts.size(); i++) {
                list.push_back({accounts[i].name, accountDetail(accounts[i])});
                if (accounts[i].playlists > accounts[best].playlists) best = i;
            }
            const int a = cli::select("Which account?", list, static_cast<int>(best));
            if (a < 0) continue;
            chosen = static_cast<size_t>(a);
        }
        if (accounts[chosen].playlists == 0) note("No playlists on this account. Not yours? Run moroder setup again and pick another.");

        creds = credentialsFor(accounts[chosen]);
        browser = s.fb.name;
        from_store = s.native;
        cookie_spec = s.native ? spec : "";
        got = true;
    }

    // Stream cookies (age-restricted / Premium tracks): the browser's profile.
    BrowserSpec spec = resolveBrowser(browser, profile_override);
    if (!spec.resolved) warn(spec.problem + " (only age-restricted tracks are affected; --profile DIR sets it)");

    if (!got) {
        step("Paste the headers");
        got = fromPaste(creds);
    }

    if (!got || !checkStructure(creds)) {
        std::cout << "\n";
        fail("setup did not complete, nothing was written");
        return 1;
    }

    /* The verification reads the file through YTMusic, so it has to exist
    first. It goes to a temp name and is only promoted once the session has
    answered, which keeps a broken sign-in from replacing a working one.
    */
    const fs::path staged = headers_file.string() + ".new";
    if (!writeHeaders(staged, creds)) return 1;

    std::error_code ec;
    const bool verified = verify(staged, !from_store && creds.source == "pasted headers");
    if (!verified) {
        // Only on request: piped in, an unverified session is not kept.
        if (!interactive || !ask("Keep these credentials anyway?")) {
            fs::remove(staged, ec);
            std::cout << "\n";
            fail("setup did not complete, nothing was written");
            if (fs::exists(headers_file)) note("Your previous session is untouched.");
            return 1;
        }
    }

    if (fs::exists(headers_file)) fs::copy_file(headers_file, headers_file.string() + ".bak", fs::copy_options::overwrite_existing, ec);
    fs::rename(staged, headers_file, ec);
    if (ec) {
        fail("could not move the new session into place, " + ec.message());
        fs::remove(staged, ec);
        return 1;
    }

    if (!recordPaths(cookies_dir, spec)) return 1;

    // Remember where the session comes from, so the app keeps it fresh.
    Config::setValues({{"COOKIE_BROWSER", Config::quote(cookie_spec)}});

    if (!verified) {
        std::cout << "\n  " << yellow() << "!" << reset() << " " << bold() << "Saved, unverified." << reset()
                  << " If your library stays empty, run " << bold() << "moroder setup" << reset() << " again.\n\n";
        return 0;
    }
    std::cout << "\n  " << green() << "✓" << reset() << " " << bold() << "Done." << reset()
              << " Run " << bold() << "moroder" << reset() << dim() << (from_store ? "  (stays signed in with " + prettyBrowser(browser) + ")" : "") << reset() << "\n\n";
    return 0;
}

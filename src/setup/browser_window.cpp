/* SIGN IN THROUGH A BROWSER WINDOW
- For browsers whose cookies can't be read from disk (Chrome and Edge on
  Windows lock them to the browser since Chrome 127, "app-bound encryption"),
  or when no browser is signed in: a Chromium browser is opened on its own
  profile (~/.config/moroder/browser-profile) with remote debugging on, you
  sign in to YouTube Music there, and the browser itself hands over the
  cookies through the DevTools protocol (Storage.getCookies): nothing is
  decrypted, nothing touches your everyday profile
- Only 127.0.0.1, an ephemeral port Chrome picks (DevToolsActivePort), and
  the browser is closed as soon as the session is read
- The WebSocket is a minimal client over a raw libcurl connection
  (CONNECT_ONLY): no new dependency
*/
#include "../../include/setup/browser_window.hpp"
#include "../../include/utils/platform.hpp"
#include "../../include/utils/utils.hpp"

#include "../../include/utils/curl.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// ---------------------------------------------------------------------------
// the browser
// ---------------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> candidates() {
#ifdef _WIN32
    auto env = [](const char* n) { const char* v = std::getenv(n); return std::string(v ? v : ""); };
    const std::string pf = env("ProgramFiles"), pf86 = env("ProgramFiles(x86)"), local = env("LOCALAPPDATA");
    return {
        {"Chrome", pf + "\\Google\\Chrome\\Application\\chrome.exe"},
        {"Chrome", pf86 + "\\Google\\Chrome\\Application\\chrome.exe"},
        {"Chrome", local + "\\Google\\Chrome\\Application\\chrome.exe"},
        {"Edge", pf86 + "\\Microsoft\\Edge\\Application\\msedge.exe"},
        {"Edge", pf + "\\Microsoft\\Edge\\Application\\msedge.exe"},
        {"Brave", pf + "\\BraveSoftware\\Brave-Browser\\Application\\brave.exe"},
        {"Brave", local + "\\BraveSoftware\\Brave-Browser\\Application\\brave.exe"},
        {"Vivaldi", local + "\\Vivaldi\\Application\\vivaldi.exe"},
    };
#elif defined(__APPLE__)
    const std::string h = utils::home().string();
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& root : {std::string("/Applications"), h + "/Applications"}) {
        out.push_back({"Chrome", root + "/Google Chrome.app/Contents/MacOS/Google Chrome"});
        out.push_back({"Brave", root + "/Brave Browser.app/Contents/MacOS/Brave Browser"});
        out.push_back({"Edge", root + "/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"});
        out.push_back({"Chromium", root + "/Chromium.app/Contents/MacOS/Chromium"});
        out.push_back({"Vivaldi", root + "/Vivaldi.app/Contents/MacOS/Vivaldi"});
    }
    return out;
#else
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [name, cmd] : std::vector<std::pair<std::string, std::string>>{
             {"Chrome", "google-chrome"}, {"Chrome", "google-chrome-stable"}, {"Chromium", "chromium"},
             {"Chromium", "chromium-browser"}, {"Brave", "brave-browser"}, {"Brave", "brave"},
             {"Edge", "microsoft-edge"}, {"Vivaldi", "vivaldi"}}) {
        const std::string path = platform::which(cmd);
        if (!path.empty()) out.push_back({name, path});
    }
    return out;
#endif
}

// ---------------------------------------------------------------------------
// a WebSocket over a raw libcurl connection
// ---------------------------------------------------------------------------

class WebSocket {
public:
    ~WebSocket() { if (curl_) curl_easy_cleanup(curl_); }

    bool open(int port, const std::string& path) {
        curl_ = utils::newCurl();
        if (!curl_) return false;
        curl_easy_setopt(curl_, CURLOPT_URL, ("http://127.0.0.1:" + std::to_string(port)).c_str());
        curl_easy_setopt(curl_, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        if (curl_easy_perform(curl_) != CURLE_OK) return false;

        const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: bW9yb2Rlci1zaWduLWluIQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        if (!sendAll(req)) return false;
        std::string head;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (head.find("\r\n\r\n") == std::string::npos) {
            if (!recvSome(head, deadline)) return false;
        }
        const size_t end = head.find("\r\n\r\n") + 4;
        buffer_ = head.substr(end);
        return head.find(" 101") != std::string::npos;
    }

    // One JSON message out (a masked text frame, as clients must send).
    bool send(const std::string& text) {
        std::string f;
        f.push_back(static_cast<char>(0x81));
        const size_t n = text.size();
        if (n < 126) f.push_back(static_cast<char>(0x80 | n));
        else if (n < 65536) { f.push_back(static_cast<char>(0x80 | 126)); f.push_back(static_cast<char>(n >> 8)); f.push_back(static_cast<char>(n & 0xff)); }
        else { f.push_back(static_cast<char>(0x80 | 127)); for (int i = 7; i >= 0; i--) f.push_back(static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xff)); }
        static std::mt19937 rng(std::random_device{}());
        unsigned char mask[4];
        for (auto& m : mask) m = static_cast<unsigned char>(rng());
        f.append(reinterpret_cast<char*>(mask), 4);
        for (size_t i = 0; i < n; i++) f.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
        return sendAll(f);
    }

    // The next whole text message, "" on error or timeout.
    std::string receive(std::chrono::steady_clock::time_point deadline) {
        std::string message;
        while (true) {
            while (buffer_.size() < 2) if (!recvSome(buffer_, deadline)) return "";
            const auto b0 = static_cast<unsigned char>(buffer_[0]), b1 = static_cast<unsigned char>(buffer_[1]);
            size_t header = 2;
            uint64_t len = b1 & 0x7f;
            if (len == 126) header = 4;
            else if (len == 127) header = 10;
            while (buffer_.size() < header) if (!recvSome(buffer_, deadline)) return "";
            if (len == 126) len = (static_cast<unsigned char>(buffer_[2]) << 8) | static_cast<unsigned char>(buffer_[3]);
            else if (len == 127) { len = 0; for (int i = 2; i < 10; i++) len = (len << 8) | static_cast<unsigned char>(buffer_[i]); }
            while (buffer_.size() < header + len) if (!recvSome(buffer_, deadline)) return "";
            const std::string payload = buffer_.substr(header, static_cast<size_t>(len));
            buffer_.erase(0, header + static_cast<size_t>(len));
            const int opcode = b0 & 0x0f;
            if (opcode == 0x8) return "";                       // close
            if (opcode == 0x1 || opcode == 0x0) message += payload;
            if ((b0 & 0x80) && (opcode == 0x1 || opcode == 0x0)) return message;   // FIN
        }
    }

private:
    bool sendAll(const std::string& data) {
        size_t done = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (done < data.size()) {
            size_t n = 0;
            const CURLcode rc = curl_easy_send(curl_, data.data() + done, data.size() - done, &n);
            if (rc == CURLE_AGAIN) {
                if (std::chrono::steady_clock::now() > deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (rc != CURLE_OK) return false;
            done += n;
        }
        return true;
    }

    bool recvSome(std::string& into, std::chrono::steady_clock::time_point deadline) {
        char buf[65536];
        while (true) {
            size_t n = 0;
            const CURLcode rc = curl_easy_recv(curl_, buf, sizeof buf, &n);
            if (rc == CURLE_OK) { if (n == 0) return false; into.append(buf, n); return true; }
            if (rc != CURLE_AGAIN) return false;
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    CURL* curl_ = nullptr;
    std::string buffer_;
};

// The youtube.com cookies the browser holds, as one Cookie header ("" if none).
std::string youtubeCookies(int port, const std::string& path) {
    WebSocket ws;
    if (!ws.open(port, path)) return "";
    if (!ws.send(R"({"id":1,"method":"Storage.getCookies"})")) return "";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true) {
        const std::string msg = ws.receive(deadline);
        if (msg.empty()) return "";
        const json j = json::parse(msg, nullptr, false);
        if (!j.is_object() || j.value("id", 0) != 1) continue;
        const json* cookies = j.contains("result") ? &j["result"]["cookies"] : nullptr;
        if (!cookies || !cookies->is_array()) return "";
        std::string header;
        for (const auto& c : *cookies) {
            std::string domain = c.value("domain", std::string());
            if (!domain.empty() && domain[0] == '.') domain.erase(0, 1);
            if (domain != "youtube.com" && domain != "music.youtube.com" && domain != "www.youtube.com") continue;
            header += (header.empty() ? "" : "; ") + c.value("name", std::string()) + "=" + c.value("value", std::string());
        }
        return header;
    }
}

size_t collect(char* d, size_t s, size_t n, void* out) {
    static_cast<std::string*>(out)->append(d, s * n);
    return s * n;
}

// Open tabs (pages); -1 when the browser doesn't answer.
int pageCount(int port) {
    CURL* c = utils::newCurl();
    if (!c) return -1;
    std::string body;
    curl_easy_setopt(c, CURLOPT_URL, ("http://127.0.0.1:" + std::to_string(port) + "/json/list").c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 2000L);
    const bool ok = curl_easy_perform(c) == CURLE_OK;
    curl_easy_cleanup(c);
    if (!ok) return -1;
    const json j = json::parse(body, nullptr, false);
    if (!j.is_array()) return -1;
    int pages = 0;
    for (const auto& t : j) pages += t.value("type", std::string()) == "page";
    return pages;
}

void closeBrowser(int port, const std::string& path) {
    WebSocket ws;
    if (ws.open(port, path)) ws.send(R"({"id":2,"method":"Browser.close"})");
}

bool signedIn(const std::string& cookie) {
    // The sign-in cookies (see hasLogin in setup.cpp).
    auto has = [&](const char* name) {
        for (size_t at = cookie.find(name); at != std::string::npos; at = cookie.find(name, at + 1))
            if (at == 0 || cookie[at - 1] == ' ' || cookie[at - 1] == ';') return true;
        return false;
    };
    return has("SAPISID=") || has("__Secure-1PAPISID=") || (has("__Secure-3PAPISID=") && has("LOGIN_INFO="));
}

}

std::string setup::browserWindowName() {
    std::error_code ec;
    for (const auto& [name, path] : candidates())
        if (!path.empty() && fs::exists(path, ec)) return name;
    return "";
}

std::string setup::signInWithBrowserWindow(const fs::path& profile_dir, const std::function<bool()>& cancelled, std::string& error,
                                           const WindowOptions& options) {
    const auto done = options.done ? options.done : signedIn;
    std::string exe;
    std::error_code ec;
    for (const auto& [name, path] : candidates())
        if (!path.empty() && fs::exists(path, ec)) { exe = path; break; }
    if (exe.empty()) { error = "no Chrome, Edge, Brave or Chromium found"; return ""; }

    fs::create_directories(profile_dir, ec);
    const fs::path port_file = profile_dir / "DevToolsActivePort";
    fs::remove(port_file, ec);

    platform::Process browser;
    std::vector<std::string> args = { exe, "--user-data-dir=" + profile_dir.string(), "--remote-debugging-port=0",
                                      "--no-first-run", "--no-default-browser-check", "--new-window" };
    if (options.headless) args.push_back("--headless=new");
    args.push_back(options.url);
    if (!browser.start(args)) {
        error = "couldn't start " + exe;
        return "";
    }
    browser.closeStdin();

    // The port Chrome picked, once it's up.
    int port = 0;
    std::string path;
    for (int i = 0; i < 200 && port == 0; i++) {
        std::ifstream in(port_file);
        std::string p;
        if (in && std::getline(in, p) && std::getline(in, path)) port = std::atoi(p.c_str());
        else std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (port == 0) { browser.kill(); error = "the browser didn't open its DevTools port"; return ""; }

    /* Until you're signed in (already, if this profile was used before): a
    look every second; stops when you close the window (no page left: on
    macOS the browser itself stays open), quit the browser, cancel, or after
    ten minutes.
    */
    std::string cookie;
    bool closed = false, was_cancelled = false;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    while (true) {
        cookie = youtubeCookies(port, path);
        if (done(cookie)) break;
        if (!browser.running() || pageCount(port) == 0) { closed = true; break; }
        if (std::chrono::steady_clock::now() > give_up) break;
        bool stop = false;
        for (int i = 0; i < 10 && !stop; i++) {
            if (cancelled()) { was_cancelled = stop = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (stop) break;
    }
    closeBrowser(port, path);
    for (int i = 0; i < 20 && browser.running(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    browser.kill();
    if (!done(cookie)) {
        error = was_cancelled ? "cancelled" : closed ? "the window closed before you signed in" : "no sign-in after 10 minutes";
        return "";
    }
    return cookie;
}

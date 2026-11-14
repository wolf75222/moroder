#include "../../include/services/scrobble.hpp"
#include "../../include/services/cli_ui.hpp"
#include "../../include/services/notify.hpp"
#include "../../include/config/config.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// MD5 (RFC 1321): Last.fm signs requests with it. Small and self-contained.
std::string md5(const std::string& message) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    static const int R[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                               4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;

    std::string msg = message;
    const uint64_t bits = static_cast<uint64_t>(message.size()) * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 64 != 56) msg += '\0';
    for (int i = 0; i < 8; i++) msg += static_cast<char>((bits >> (8 * i)) & 0xff);

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(msg.data() + off + 4 * i);
            w[i] = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 64; i++) {
            uint32_t f; int g;
            if (i < 16)      { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) % 16; }
            else             { f = c ^ (b | ~d);       g = (7 * i) % 16; }
            const uint32_t t = d; d = c; c = b;
            const uint32_t x = a + f + K[i] + w[g];
            b = b + ((x << R[i]) | (x >> (32 - R[i])));
            a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }
    char out[33];
    const uint32_t hs[4] = { h0, h1, h2, h3 };
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) std::snprintf(out + 8 * i + 2 * j, 3, "%02x", (hs[i] >> (8 * j)) & 0xff);
    return std::string(out, 32);
}

fs::path sessionFile() {
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / ".config" / "moroder" / "lastfm.json" : fs::path();
}

std::mutex& m = *new std::mutex;
std::string& session_key = *new std::string;
bool loaded = false;

const std::string& session() {
    std::lock_guard lock(m);
    if (!loaded) {
        loaded = true;
        std::ifstream in(sessionFile());
        const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_object() && j.contains("key") && j["key"].is_string()) session_key = j["key"];
    }
    return session_key;
}

size_t collect(char* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(data, size * n);
    return size * n;
}

// Signed call (api_sig over the sorted parameters + secret), JSON reply.
nlohmann::json call(std::map<std::string, std::string> params, bool post) {
    const Config& cfg = Config::get();
    params["api_key"] = cfg.LASTFM_API_KEY;
    std::string sig;
    for (const auto& [k, v] : params) sig += k + v;
    params["api_sig"] = md5(sig + cfg.LASTFM_API_SECRET);
    params["format"] = "json";

    CURL* c = curl_easy_init();
    if (!c) return nullptr;
    std::string body;
    for (const auto& [k, v] : params) {
        char* e = curl_easy_escape(c, v.c_str(), static_cast<int>(v.size()));
        body += (body.empty() ? "" : "&") + k + "=" + (e ? e : "");
        curl_free(e);
    }
    std::string response;
    const std::string url = "https://ws.audioscrobbler.com/2.0/" + (post ? std::string() : "?" + body);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    if (post) curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
    return nlohmann::json::parse(response, nullptr, false);
}

void send(std::map<std::string, std::string> params) {
    std::thread([params]() mutable {
        params["sk"] = session();
        const nlohmann::json r = call(params, true);
        if (!r.is_object() || r.contains("error"))
            spdlog::warn("SCROBBLE: {} failed: {}", params["method"], r.is_object() ? r.value("message", std::string("?")) : std::string("no answer"));
    }).detach();
}

}

bool services::scrobble::enabled() {
    const Config& cfg = Config::get();
    return !cfg.LASTFM_API_KEY.empty() && !cfg.LASTFM_API_SECRET.empty() && !session().empty();
}

void services::scrobble::nowPlaying(const std::string& artist, const std::string& track, const std::string& album, int duration_s) {
    if (!enabled() || artist.empty() || track.empty()) return;
    std::map<std::string, std::string> p = { {"method", "track.updateNowPlaying"}, {"artist", artist}, {"track", track} };
    if (!album.empty()) p["album"] = album;
    if (duration_s > 0) p["duration"] = std::to_string(duration_s);
    send(p);
}

void services::scrobble::scrobble(const std::string& artist, const std::string& track, const std::string& album, int duration_s, long long started_at) {
    if (!enabled() || artist.empty() || track.empty()) return;
    std::map<std::string, std::string> p = { {"method", "track.scrobble"}, {"artist[0]", artist}, {"track[0]", track},
                                             {"timestamp[0]", std::to_string(started_at)} };
    if (!album.empty()) p["album[0]"] = album;
    if (duration_s > 0) p["duration[0]"] = std::to_string(duration_s);
    send(p);
}

int services::scrobble::connect() {
    const Config& cfg = Config::get();
    if (cfg.LASTFM_API_KEY.empty() || cfg.LASTFM_API_SECRET.empty()) {
        std::cerr << "\n  " << cli::yellow() << "!" << cli::reset() << "  Set LASTFM_API_KEY and LASTFM_API_SECRET in the config first\n"
                  << "     " << cli::dim() << "An API account: https://www.last.fm/api/account/create" << cli::reset() << "\n\n";
        return 1;
    }
    const nlohmann::json t = call({ {"method", "auth.getToken"} }, false);
    if (!t.is_object() || !t.contains("token") || !t["token"].is_string()) {
        std::cerr << "\n  " << cli::red() << "✗" << cli::reset() << "  Last.fm refused the API key: " << (t.is_object() ? t.value("message", std::string("no answer")) : "no answer") << "\n\n";
        return 1;
    }
    const std::string token = t["token"];
    const std::string url = "https://www.last.fm/api/auth/?api_key=" + cfg.LASTFM_API_KEY + "&token=" + token;
    cli::title("Connect Last.fm", "scrobble what you play");
    std::cout << "     Allow Moroder in the page that opens (or open it yourself):\n"
              << "     " << cli::dim() << url << cli::reset() << "\n"
              << "\n  " << cli::bold() << "?" << cli::reset() << "  Press Return once it's allowed " << std::flush;
    services::openUrl(url);
    std::string line;
    std::getline(std::cin, line);

    const nlohmann::json s = call({ {"method", "auth.getSession"}, {"token", token} }, false);
    if (!s.is_object() || !s.contains("session") || !s["session"].contains("key")) {
        std::cerr << "\n  " << cli::red() << "✗" << cli::reset() << "  No session: " << (s.is_object() ? s.value("message", std::string("not allowed yet?")) : "no answer") << "\n\n";
        return 1;
    }
    const fs::path f = sessionFile();
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    const int fd = ::open(f.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) { std::cerr << "\n  " << cli::red() << "✗" << cli::reset() << "  Can't write " << f.string() << "\n\n"; return 1; }
    const std::string body = nlohmann::json{ {"key", s["session"]["key"]}, {"name", s["session"].value("name", "")} }.dump();
    const bool ok = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
    ::close(fd);
    if (!ok) return 1;
    std::cout << "\n  " << cli::green() << "✓" << cli::reset() << "  Scrobbling to Last.fm as " << cli::bold() << s["session"].value("name", std::string("you")) << cli::reset() << "\n\n";
    return 0;
}

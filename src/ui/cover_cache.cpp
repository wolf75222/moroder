#include "../../include/ui/cover_cache.hpp"

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

/* State
- Heap-allocated and never freed: detached download threads may still touch
  it while the process exits, after static destructors would have run
- Per URL: a local path, "pending" (download running or waiting for a slot),
  or "failed until T" (retried after a minute), so a frame never repeats the
  disk checks for a cover it already looked up
*/
struct Entry {
    std::string path;                                    // set once on disk
    bool pending = false;
    std::chrono::steady_clock::time_point retry_after{};
};

std::mutex& m = *new std::mutex;
std::unordered_map<std::string, Entry>& entries = *new std::unordered_map<std::string, Entry>;
std::atomic<int> in_flight{0};
constexpr int MAX_IN_FLIGHT = 4;

const fs::path& dir() {
    static const fs::path* d = [] {
        const char* home = std::getenv("HOME");
        auto* p = new fs::path(home ? fs::path(home) / ".cache" / "moroder" / "covers" : fs::path());
        std::error_code ec;
        if (!p->empty()) fs::create_directories(*p, ec);
        return p;
    }();
    return *d;
}

size_t collect(char* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(data, size * n);
    return size * n;
}

void download(std::string url, fs::path target) {
    std::string body;
    if (CURL* c = curl_easy_init()) {
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
        long status = 0;
        if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
        if (status != 200) body.clear();
    }

    // JPEG or PNG only (what the decoder handles); name by actual format.
    std::string saved;
    const bool jpeg = body.size() > 3 && (unsigned char)body[0] == 0xFF && (unsigned char)body[1] == 0xD8;
    const bool png = body.size() > 8 && body.compare(1, 3, "PNG") == 0;
    if (jpeg || png) {
        target.replace_extension(jpeg ? ".jpg" : ".png");
        const fs::path tmp = target.string() + ".part";
        { std::ofstream out(tmp, std::ios::binary); out.write(body.data(), (std::streamsize)body.size()); }
        std::error_code ec;
        fs::rename(tmp, target, ec);
        if (!ec) saved = target.string();
    }

    std::lock_guard lock(m);
    Entry& e = entries[url];
    e.pending = false;
    if (!saved.empty()) e.path = saved;
    else e.retry_after = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    in_flight--;
}

}

std::string ui::cover(const std::string& url) {
    if (url.rfind("http", 0) != 0 || dir().empty()) return url;

    std::lock_guard lock(m);
    const auto now = std::chrono::steady_clock::now();

    auto [it, fresh] = entries.try_emplace(url);
    Entry& e = it->second;

    if (!e.path.empty()) return e.path;

    // First sight: maybe on disk from an earlier run (one lookup, ever).
    if (fresh) {
        const fs::path base = dir() / std::to_string(std::hash<std::string>{}(url));
        std::error_code ec;
        for (const char* ext : {".jpg", ".png"}) {
            fs::path p = base; p.replace_extension(ext);
            if (fs::exists(p, ec)) { e.path = p.string(); return e.path; }
        }
    }

    if (!e.pending && now >= e.retry_after && in_flight < MAX_IN_FLIGHT) {
        e.pending = true;
        in_flight++;
        std::thread(download, url, dir() / std::to_string(std::hash<std::string>{}(url))).detach();
    }
    return url;     // image_view fetches it meanwhile, as before
}

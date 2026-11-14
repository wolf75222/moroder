#include "../../include/ui/cover_cache.hpp"

#include <curl/curl.h>

#define cimg_display 0      // cimg_use_jpeg / cimg_use_png come from CMake, when the libs exist
#include "CImg.h"

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
std::function<void()>& ready_callback = *new std::function<void()>;
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

/* squareCover
- Every cover is shown square; video thumbnails are 16:9, often letterboxed
  in a 4:3 frame (black bars), and looked out of place among album covers
- Trims black bars, crops the centre square, and scales it to 320 px (also
  makes it quicker to decode each time it is drawn)
*/
bool squareCover(const fs::path& in, const fs::path& out) {
    try {
        cimg_library::CImg<unsigned char> img(in.string().c_str());
        if (img.width() < 8 || img.height() < 8) return false;
        if (img.spectrum() < 3) { img.channel(0); img.resize(-100, -100, 1, 3); }   // grey (+ alpha)
        if (img.spectrum() > 3) img.channels(0, 2);                                  // drop alpha

        // Bars are near-pure black across the whole line; a dark cover isn't.
        auto bar_row = [&](int y) {
            int peak = 0; long sum = 0;
            for (int x = 0; x < img.width(); x++)
                for (int c = 0; c < 3; c++) { const int v = img(x, y, 0, c); sum += v; peak = std::max(peak, v); }
            return peak < 40 && sum / (3L * img.width()) < 12;
        };
        auto bar_col = [&](int x, int y0, int y1) {
            int peak = 0; long sum = 0;
            for (int y = y0; y <= y1; y++)
                for (int c = 0; c < 3; c++) { const int v = img(x, y, 0, c); sum += v; peak = std::max(peak, v); }
            return peak < 40 && sum / (3L * (y1 - y0 + 1)) < 12;
        };

        // Only non-square images can be letterboxed thumbnails.
        const bool square = std::abs(img.width() - img.height()) * 20 < std::max(img.width(), img.height());
        int top = 0, bottom = img.height() - 1;
        int left = 0, right = img.width() - 1;
        if (!square) {
            while (top < img.height() / 4 && bar_row(top)) top++;
            while (bottom > img.height() * 3 / 4 && bar_row(bottom)) bottom--;
            while (left < img.width() / 4 && bar_col(left, top, bottom)) left++;
            while (right > img.width() * 3 / 4 && bar_col(right, top, bottom)) right--;
        }

        const int w = right - left + 1, h = bottom - top + 1;
        const int side = std::min(w, h);
        const int x0 = left + (w - side) / 2, y0 = top + (h - side) / 2;
        img.crop(x0, y0, x0 + side - 1, y0 + side - 1);
        if (side > 320) img.resize(320, 320, 1, 3, 5);
        img.save_jpeg(out.string().c_str(), 90);
        return true;
    } catch (...) {
        return false;
    }
}

// Shown while a cover downloads: a dark square, the same for all.
const std::string& placeholder() {
    static const std::string* p = [] {
        auto* path = new std::string;
        if (dir().empty()) return path;
        const fs::path f = dir() / "placeholder.png";
        std::error_code ec;
        if (!fs::exists(f, ec)) {
            try {
                cimg_library::CImg<unsigned char> img(8, 8, 1, 3);
                img.get_shared_channel(0).fill(32); img.get_shared_channel(1).fill(32); img.get_shared_channel(2).fill(38);
                img.save_png(f.string().c_str());
            } catch (...) { return path; }
        }
        *path = f.string();
        return path;
    }();
    return *p;
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

    // JPEG or PNG only (what the decoder handles).
    std::string saved;
    const bool jpeg = body.size() > 3 && (unsigned char)body[0] == 0xFF && (unsigned char)body[1] == 0xD8;
    const bool png = body.size() > 8 && body.compare(1, 3, "PNG") == 0;
    if (jpeg || png) {
        const fs::path raw = target.string() + (jpeg ? ".part.jpg" : ".part.png");
        { std::ofstream out(raw, std::ios::binary); out.write(body.data(), (std::streamsize)body.size()); }
        target.replace_extension(".jpg");
        const fs::path tmp = target.string() + ".sq.jpg";
        std::error_code ec;
        if (squareCover(raw, tmp)) {
            fs::rename(tmp, target, ec);
            if (!ec) saved = target.string();
        }
        fs::remove(raw, ec);
        fs::remove(tmp, ec);
    }

    std::function<void()> notify;
    {
        std::lock_guard lock(m);
        Entry& e = entries[url];
        e.pending = false;
        if (!saved.empty()) e.path = saved;
        else e.retry_after = std::chrono::steady_clock::now() + std::chrono::minutes(1);
        in_flight--;
        notify = ready_callback;
    }
    if (notify) notify();
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
        // "-sq": squared covers; older unsquared files are left unused.
        const fs::path p = dir() / (std::to_string(std::hash<std::string>{}(url)) + "-sq.jpg");
        std::error_code ec;
        if (fs::exists(p, ec)) { e.path = p.string(); return e.path; }
    }

    if (!e.pending && now >= e.retry_after && in_flight < MAX_IN_FLIGHT) {
        e.pending = true;
        in_flight++;
        std::thread(download, url, dir() / (std::to_string(std::hash<std::string>{}(url)) + "-sq")).detach();
    }

    // Meanwhile (or while waiting for a download slot) a neutral square: the
    // raw image would show uncropped and be downloaded twice. Only after a
    // failed download does image_view get the URL to try itself.
    const std::string& ph = placeholder();
    const bool failed_before = e.retry_after != std::chrono::steady_clock::time_point{};
    return (!failed_before && !ph.empty()) ? ph : url;
}

void ui::onCoverReady(std::function<void()> callback) {
    std::lock_guard lock(m);
    ready_callback = std::move(callback);
}

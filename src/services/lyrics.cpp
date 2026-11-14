#include "../../include/services/lyrics.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <regex>
#include <sstream>

namespace {

size_t collect(char* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(data, size * n);
    return size * n;
}

std::string get(const std::string& url) {
    std::string body;
    CURL* c = curl_easy_init();
    if (!c) return body;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Moroder (https://github.com/orrnithogalum/moroder)");
    long status = 0;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    return status == 200 ? body : std::string();
}

std::string escape(const std::string& s) {
    std::string out;
    if (CURL* c = curl_easy_init()) {
        if (char* e = curl_easy_escape(c, s.c_str(), (int)s.size())) { out = e; curl_free(e); }
        curl_easy_cleanup(c);
    }
    return out;
}

// "Title (feat. X) [Remastered]" -> "Title": LRCLIB matches on the bare title.
std::string bareTitle(std::string t) {
    for (const char* cut : {" (", " [", " - "}) {
        auto p = t.find(cut);
        if (p != std::string::npos && p > 0) t = t.substr(0, p);
    }
    return t;
}

services::Lyrics parse(const nlohmann::json& j) {
    services::Lyrics l;
    const std::string synced = j.value("syncedLyrics", nlohmann::json()).is_string() ? j["syncedLyrics"].get<std::string>() : "";
    const std::string plain = j.value("plainLyrics", nlohmann::json()).is_string() ? j["plainLyrics"].get<std::string>() : "";

    static const std::regex stamp(R"(\[(\d+):(\d+(?:\.\d+)?)\]\s?(.*))");
    std::istringstream in(synced);
    std::string line;
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_match(line, m, stamp)) {
            l.lines.push_back({ std::stod(m[1]) * 60 + std::stod(m[2]), m[3] });
        }
    }

    if (l.lines.empty()) {
        std::istringstream p(plain);
        while (std::getline(p, line)) l.plain.push_back(line);
    }
    l.found = !l.lines.empty() || !l.plain.empty();
    return l;
}

}

services::Lyrics services::fetchLyrics(const std::string& title, const std::string& artist, double duration) {
    const std::string t = bareTitle(title);

    // Exact match first (title, artist, duration), then a looser search.
    std::string url = "https://lrclib.net/api/get?track_name=" + escape(t) + "&artist_name=" + escape(artist);
    if (duration > 0) url += "&duration=" + std::to_string((int)(duration + 0.5));

    nlohmann::json j = nlohmann::json::parse(get(url), nullptr, false);
    if (j.is_object()) {
        Lyrics l = parse(j);
        if (l.found) return l;
    }

    j = nlohmann::json::parse(get("https://lrclib.net/api/search?q=" + escape(t + " " + artist)), nullptr, false);
    if (j.is_array()) {
        for (const auto& hit : j) {
            Lyrics l = parse(hit);
            if (!l.lines.empty()) return l;     // prefer a synced one
        }
        if (!j.empty()) return parse(j[0]);
    }

    spdlog::info("LYRICS: none for {} - {}", artist, t);
    return {};
}

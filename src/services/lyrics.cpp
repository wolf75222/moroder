#include "../../include/services/lyrics.hpp"

#include "../../include/utils/curl.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <regex>
#include <sstream>

namespace {

size_t collect(char* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(data, size * n);
    return size * n;
}

std::string get(const std::string& url) {
    std::string body;
    CURL* c = utils::newCurl();
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
    if (CURL* c = utils::newCurl()) {
        if (char* e = curl_easy_escape(c, s.c_str(), (int)s.size())) { out = e; curl_free(e); }
        curl_easy_cleanup(c);
    }
    return out;
}

// "Title (feat. X) [Remastered]" -> "Title": LRCLIB matches on the bare title.
std::string bareTitle(std::string t) {
    for (const char* cut : {" (", " [", " - ", " | ", " \u2013 ", " \u2014 ", " ft. ", " ft ", " feat. ", " feat ", " featuring "}) {
        auto p = t.find(cut);
        if (p != std::string::npos && p > 0) t = t.substr(0, p);
    }
    // Quoted titles ("\u201cTitle\u201d", "'Title'") lose their quotes.
    for (const char* q : {"\"", "\u201c", "\u201d", "\u00ab", "\u00bb"}) {
        for (auto p = t.find(q); p != std::string::npos; p = t.find(q)) t.erase(p, std::string(q).size());
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    // 'Title' loses its quotes; 'Till I Collapse and Rockin' keep theirs.
    if (t.size() > 2 && t.front() == '\'' && t.back() == '\'') t = t.substr(1, t.size() - 2);
    return t;
}

// A channel's name, not the artist's: "EminemVEVO", "Daft Punk - Topic".
std::string bareArtist(std::string a) {
    for (const char* tail : {" - Topic", "VEVO", "Vevo", " Official", " official"}) {
        const std::string s = tail;
        if (a.size() > s.size() && a.compare(a.size() - s.size(), s.size(), s) == 0) a.erase(a.size() - s.size());
    }
    while (!a.empty() && a.back() == ' ') a.pop_back();
    return a;
}

double firstStamp(const nlohmann::json& hit) {
    static const std::regex stamp(R"(\[(\d+):(\d+(?:\.\d+)?)\])");
    if (!hit.is_object() || !hit.value("syncedLyrics", nlohmann::json()).is_string()) return -1;
    const std::string& s = hit["syncedLyrics"].get_ref<const std::string&>();
    std::smatch m;
    return std::regex_search(s, m, stamp) ? std::stod(m[1]) * 60 + std::stod(m[2]) : -1;
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

services::Lyrics services::fetchLyrics(const std::string& title, const std::string& artist_in, double duration, bool video) {
    const std::string artist = bareArtist(artist_in);
    /* Music videos are titled "Artist - Title": cutting at " - " left the
    artist's name as the title ("Therapie TAXI - SALOP(E)" looked up
    "Therapie TAXI"). The artist in front goes first, then the rest. */
    std::string name = title;
    for (const char* dash : {" - ", " \u2013 ", " \u2014 "}) {
        const std::string prefix = artist + dash;
        if (!artist.empty() && name.size() > prefix.size()) {
            std::string head = name.substr(0, prefix.size());
            std::string want = prefix;
            for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (auto& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (head == want) { name = name.substr(prefix.size()); break; }
        }
    }
    const std::string t = bareTitle(name);
    nlohmann::json hits;
    // Close enough to share timings: within 8 s, or (a video) up to 25 s longer than it.
    auto fits = [&](double d) { return d > 0 && (std::abs(d - duration) <= 8 || (video && d < duration && duration - d <= 25)); };
    auto find = [&]() -> Lyrics {

        // Exact match first (title, artist, duration), then a looser search.
        std::string url = "https://lrclib.net/api/get?track_name=" + escape(t) + "&artist_name=" + escape(artist);
        if (duration > 0) url += "&duration=" + std::to_string((int)(duration + 0.5));

        // An entry of a line or two is a stub someone uploaded (Nightcall's exact
        // match is "There is a nightcall" alone): not lyrics, look further.
        auto real = [](const Lyrics& l) { return l.lines.size() >= 6 || l.plain.size() >= 6; };

        nlohmann::json j = nlohmann::json::parse(get(url), nullptr, false);
        if (j.is_object()) {
            Lyrics l = parse(j);
            if (l.found && real(l)) return l;
        }

        /* The search: the synced hit whose length is closest to this track's.
        The first one was often another cut (radio edit, remaster, live) whose
        timings drift from what plays. */
        // By title and artist first (precise), then free text.
        j = nlohmann::json::parse(get("https://lrclib.net/api/search?track_name=" + escape(t) + "&artist_name=" + escape(artist)), nullptr, false);
        if (!j.is_array() || j.empty()) j = nlohmann::json::parse(get("https://lrclib.net/api/search?q=" + escape(t + " " + artist)), nullptr, false);
        hits = j;
        if (j.is_array()) {
            // Among those of the right length (within 8 s), the fullest.
            const nlohmann::json* best = nullptr;
            double best_gap = 1e9;
            size_t best_lines = 0;
            double best_d = 0;
            for (const auto& hit : j) {
                if (!hit.is_object() || !hit.value("syncedLyrics", nlohmann::json()).is_string()) continue;
                const double d = hit.value("duration", nlohmann::json()).is_number() ? hit["duration"].get<double>() : 0;
                const double gap = duration > 0 && d > 0 ? std::abs(d - duration) : 1e6;
                const size_t n = static_cast<size_t>(std::count(hit["syncedLyrics"].get_ref<const std::string&>().begin(),
                                                                hit["syncedLyrics"].get_ref<const std::string&>().end(), '\n')) + 1;
                if (n < 6) continue;
                const bool close = fits(d), best_close = best && fits(best_d);
                if (!best || (close && !best_close) || (close == best_close && (close ? n > best_lines : gap < best_gap))) {
                    best = &hit; best_gap = gap; best_lines = n; best_d = d;
                }
            }
            // Off by more than 8 s: another version; its timings would be wrong.
            if (best && (duration <= 0 || fits(best_d))) {
                Lyrics l = parse(*best);
                if (!l.lines.empty()) return l;
            }
            // Words without timings rather than mistimed ones.
            for (const auto& hit : j) {
                if (!hit.is_object()) continue;
                Lyrics l = parse(hit);
                l.lines.clear();
                if (l.plain.empty() && hit.value("plainLyrics", nlohmann::json()).is_string()) {
                    std::istringstream p(hit["plainLyrics"].get<std::string>());
                    std::string line;
                    while (std::getline(p, line)) l.plain.push_back(line);
                }
                l.found = l.plain.size() >= 6;     // not a stub
                if (l.found) return l;
            }
        }

        return {};
    };
    Lyrics l = find();

    /* A music video runs longer than the song (an intro, a scene at the end),
    while LRCLIB's timings are the album's (even entries filed under the
    video's length often are). When the lyrics found start like an album
    version a few seconds shorter, delay them by the difference: shown as
    "+8.0 s" above them, and , / . still adjust it. */
    if (video && duration > 0 && !l.lines.empty()) {
        if (!hits.is_array())
            hits = nlohmann::json::parse(get("https://lrclib.net/api/search?track_name=" + escape(t) + "&artist_name=" + escape(artist)), nullptr, false);
        const double first = l.lines.front().first;
        // The same timings are filed under many lengths (compilations, copies):
        // the song's own is the one most of them give.
        std::map<int, int> lengths;
        if (hits.is_array()) for (const auto& hit : hits) {
            const double d = hit.is_object() && hit.value("duration", nlohmann::json()).is_number() ? hit["duration"].get<double>() : 0;
            const double gap = duration - d;
            if (gap < 4 || gap > 25) continue;                         // a few seconds: the same cut, rounded
            if (std::abs(firstStamp(hit) - first) > 0.3) continue;   // other timings: another cut
            lengths[static_cast<int>(std::lround(d))]++;
        }
        double album = 0;
        int most = 0;
        for (const auto& [d, n] : lengths) if (n > most) { most = n; album = d; }
        if (album > 0) l.shift = std::round((duration - album) * 2) / 2;   // half seconds, like the keys
    }
    if (l.found) return l;

    spdlog::info("LYRICS: none for {} - {}", artist, t);
    return {};
}

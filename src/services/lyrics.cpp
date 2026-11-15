#include "../../include/services/lyrics.hpp"

#include "../../include/utils/curl.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
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

double lastStamp(const nlohmann::json& hit) {
    static const std::regex stamp(R"(\[(\d+):(\d+(?:\.\d+)?)\])");
    if (!hit.is_object() || !hit.value("syncedLyrics", nlohmann::json()).is_string()) return -1;
    const std::string& s = hit["syncedLyrics"].get_ref<const std::string&>();
    double last = -1;
    for (std::sregex_iterator it(s.begin(), s.end(), stamp), end; it != end; ++it) last = std::stod((*it)[1]) * 60 + std::stod((*it)[2]);
    return last;
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

    /* Every entry LRCLIB has for the song, from two searches: the title
    without what YouTube adds ("(Official Video)", "[Lyrics]"...) but with
    the rest ("Habits (Stay High)"), and the bare title ("Habits"). The free
    text search when both find nothing. */
    std::string kept = name;
    {
        static const std::regex noise(R"(\s*[\(\[][^\)\]]*(official|video|audio|lyric|clip|visuali[sz]er|remaster|explicit|\bhd\b|4k|feat\.?|ft\.)[^\)\]]*[\)\]])",
                                      std::regex::icase);
        kept = std::regex_replace(kept, noise, "");
        while (!kept.empty() && kept.back() == ' ') kept.pop_back();
    }
    nlohmann::json hits = nlohmann::json::array();
    std::set<long long> seen;
    auto add = [&](const std::string& url) {
        nlohmann::json j = nlohmann::json::parse(get(url), nullptr, false);
        if (!j.is_array()) return;
        for (auto& hit : j) {
            if (!hit.is_object()) continue;
            const long long id = hit.value("id", nlohmann::json()).is_number() ? hit["id"].get<long long>() : -1;
            if (id >= 0 && !seen.insert(id).second) continue;
            hits.push_back(std::move(hit));
        }
    };
    const std::string by = "&artist_name=" + escape(artist);
    if (!kept.empty() && kept != t) add("https://lrclib.net/api/search?track_name=" + escape(kept) + by);
    add("https://lrclib.net/api/search?track_name=" + escape(t) + by);
    if (hits.empty()) add("https://lrclib.net/api/search?q=" + escape(t + " " + artist));

    /* Which timings. LRCLIB files one set of timings under many lengths
    (compilations, copies, uploads with the wrong length: Habits' original
    timings sit under 3:29 and under 4:25), so an entry's own length can't be
    trusted. Entries are grouped by their timings (same first and last line);
    a group's real length is the one most of its entries give; the group
    closest to this track wins. Within 8 s: those timings. A music video up to
    25 s longer than the song: the song's timings, delayed by its intro
    (shown as "+8.0 s", , / . adjust it). Further off: another cut, whose
    timings would run ahead or behind, so the words without timings. */
    struct Group { double first, last; std::map<int, int> lengths; const nlohmann::json* fullest; size_t lines; };
    std::vector<Group> groups;
    for (const auto& hit : hits) {
        if (!hit.value("syncedLyrics", nlohmann::json()).is_string()) continue;
        const std::string& sync = hit["syncedLyrics"].get_ref<const std::string&>();
        const size_t n = static_cast<size_t>(std::count(sync.begin(), sync.end(), '\n')) + 1;
        if (n < 6) continue;                                  // a stub ("There is a nightcall")
        const double first = firstStamp(hit), last = lastStamp(hit);
        if (first < 0) continue;
        const double d = hit.value("duration", nlohmann::json()).is_number() ? hit["duration"].get<double>() : 0;
        auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) { return std::abs(x.first - first) <= 0.6 && std::abs(x.last - last) <= 2; });
        if (g == groups.end()) { groups.push_back({first, last, {}, &hit, n}); g = groups.end() - 1; }
        if (d > 0) g->lengths[static_cast<int>(std::lround(d))]++;
        if (n > g->lines) { g->fullest = &hit; g->lines = n; }
    }
    const Group* best = nullptr;
    double best_score = 1e9, best_shift = 0;
    int best_len = 0;
    for (const auto& g : groups) {
        int length = 0, most = 0, total = 0;
        for (const auto& [len, count] : g.lengths) { total += count; if (count > most) { most = count; length = len; } }
        double score, shift = 0;
        if (duration <= 0) score = -static_cast<double>(total);         // no length to go by: the most common timings
        else if (length == 0) continue;
        else {
            const double gap = duration - length;
            if (video && gap >= 4 && gap <= 25) { score = gap; shift = std::round(gap * 2) / 2; }   // a video: the song plus an intro
            else if (std::abs(gap) <= 8) score = std::abs(gap);
            else continue;
        }
        if (!best || score < best_score || (score == best_score && g.lengths.size() > best->lengths.size())) {
            best = &g; best_score = score; best_shift = shift; best_len = length;
        }
    }
    if (best) {
        Lyrics l = parse(*best->fullest);
        if (!l.lines.empty()) {
            l.shift = best_shift;
            spdlog::info("LYRICS: {} - {}: timings of a {} s cut, shift {:.1f}", artist, t, best_len, best_shift);
            return l;
        }
    }

    // Words without timings rather than mistimed ones (not a stub).
    for (const auto& hit : hits) {
        Lyrics l;
        if (hit.value("plainLyrics", nlohmann::json()).is_string()) {
            std::istringstream p(hit["plainLyrics"].get<std::string>());
            for (std::string line; std::getline(p, line); ) l.plain.push_back(line);
        }
        l.found = l.plain.size() >= 6;
        if (l.found) return l;
    }

    spdlog::info("LYRICS: none for {} - {}", artist, t);
    return {};
}

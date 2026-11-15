#include "../../include/services/suggest.hpp"

#include "../../include/utils/curl.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <mutex>
#include <deque>
#include <map>

namespace {
size_t collect(char* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(data, size * n);
    return size * n;
}
}

std::vector<std::string> services::searchSuggestions(const std::string& input, std::function<bool()> cancelled) {
    std::vector<std::string> out;
    if (input.empty() || input.size() > 200) return out;

    // The last answers: typing back over a prefix (backspace) is instant.
    static std::mutex m;
    static std::map<std::string, std::vector<std::string>> recent;
    static std::deque<std::string> order;
    {
        std::lock_guard lock(m);
        auto it = recent.find(input);
        if (it != recent.end()) return it->second;
    }

    nlohmann::json body = {
        {"context", {{"client", {{"clientName", "WEB_REMIX"}, {"clientVersion", "1.20251015.03.00"}, {"hl", "en"}}}}},
        {"input", input},
    };
    const std::string payload = body.dump();

    std::string response;
    CURL* c = utils::newCurl();
    if (!c) return out;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Origin: https://music.youtube.com");
    curl_easy_setopt(c, CURLOPT_URL, "https://music.youtube.com/youtubei/v1/music/get_search_suggestions?prettyPrint=false");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    // Dropped as soon as another letter is typed, instead of finishing for nothing.
    if (cancelled) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &cancelled);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, +[](void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return (*static_cast<std::function<bool()>*>(p))() ? 1 : 0;
        });
    }
    long status = 0;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (status != 200) return out;

    const nlohmann::json j = nlohmann::json::parse(response, nullptr, false);
    if (!j.is_object() || !j.contains("contents") || !j["contents"].is_array()) return out;

    for (const auto& section : j["contents"]) {
        const auto* list = section.contains("searchSuggestionsSectionRenderer") ? &section["searchSuggestionsSectionRenderer"] : nullptr;
        if (!list || !list->contains("contents") || !(*list)["contents"].is_array()) continue;
        for (const auto& item : (*list)["contents"]) {
            if (!item.contains("searchSuggestionRenderer")) continue;
            const auto& runs = item["searchSuggestionRenderer"].value("suggestion", nlohmann::json::object()).value("runs", nlohmann::json::array());
            std::string text;
            for (const auto& r : runs) if (r.contains("text") && r["text"].is_string()) text += r["text"].get<std::string>();
            if (!text.empty()) out.push_back(text);
        }
    }
    {
        std::lock_guard lock(m);
        if (!recent.count(input)) { order.push_back(input); recent[input] = out; }
        while (order.size() > 50) { recent.erase(order.front()); order.pop_front(); }
    }
    return out;
}

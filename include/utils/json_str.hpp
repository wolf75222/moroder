#pragma once

/* jstr
- j[key] as a string, or "" when the key is missing, null or not a string.
- nlohmann's j.value(key, "") throws type_error.302 when the key exists but
  holds null, and the parsers emit null for every field they could not find
  (an unlinked album column, a missing play button...). That dropped whole
  tracks, albums and playlists, and crashed outside tryAdd.
*/

#include <nlohmann/json.hpp>
#include <string>

namespace utils {

inline std::string jstr(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return "";
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

}

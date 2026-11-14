/* PODCAST
- The podcast object for a ytmusicapi search result of type "podcast"
- Only includes information that is present in all search results of type "podcast"
*/

#pragma once

#include "../utils/json_str.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace music {

struct PodcastRef {
    std::string id;
    std::string name;

    std::string thumbnail_large;
    std::string thumbnail_small;

    static PodcastRef from_json(const nlohmann::json& j) {
        PodcastRef a;
        a.id = utils::jstr(j, "browseId");
        a.name = utils::jstr(j, "title");

        if (j.contains("thumbnails") && j["thumbnails"].is_array() && !j["thumbnails"].empty()) {
            a.thumbnail_large = utils::jstr(j["thumbnails"].back(), "url");
            a.thumbnail_small = utils::jstr(j["thumbnails"].front(), "url");
        } else {
            a.thumbnail_large = "";
            a.thumbnail_small = "";
        }

        return a;
    }
};

}

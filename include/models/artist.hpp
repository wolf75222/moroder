/* ARTIST
- The artist object for a ytmusicapi search result of type "artist"
- Only includes information that is present in all search results of type "artist"
*/

#pragma once

#include "../utils/json_str.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace music {

struct ArtistRef {
    std::string id;
    std::string name;

    std::string thumbnail_large;
    std::string thumbnail_small;

    static ArtistRef from_json(const nlohmann::json& j) {
        ArtistRef a;

        if (j.contains("artists") && j["artists"].is_array() && !j["artists"].empty()) {
            const auto& first_artist = j["artists"].front();
            a.id = utils::jstr(first_artist, "id");
            a.name = utils::jstr(first_artist, "name");
            if (a.id.empty() && j.contains("browseId") && j["browseId"].is_string()) a.id = j["browseId"];

        } else if (j.contains("title")) {
            a.name = utils::jstr(j, "title");
            a.id = utils::jstr(j, "browseId");     // artist shelves carry it here

        } else {
            a.id = utils::jstr(j, "browseId");
            a.name = utils::jstr(j, "artist");
        }

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

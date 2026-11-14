/* ALBUM
- The album object for a ytmusicapi search result of type "album"
- Only includes information that is present in all search results of type "album"
*/

#pragma once

#include "../utils/json_str.hpp"

#include "../interfaces/streamable.hpp"
#include "../interfaces/container.hpp"
#include "artist.hpp"

#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

namespace music {

struct AlbumRef {
    std::string id;
    std::string title;
    std::string year;

    std::string thumbnail_large;
    std::string thumbnail_small;

    std::vector<ArtistRef> artists;

    static AlbumRef from_json(const nlohmann::json& j) {
        AlbumRef a;
        a.id = utils::jstr(j, "browseId");
        a.title = utils::jstr(j, "title");
        a.year = utils::jstr(j, "year");

        if (j.contains("artists") && j["artists"].is_array()) {
            for (const auto& artist : j["artists"]) {
                ArtistRef ref;

                ref.name = utils::jstr(artist, "name");

                if (artist.contains("id") && artist["id"].is_string()) {
                    ref.id = artist["id"].get<std::string>();
                } else {
                    ref.id = "";
                }

                a.artists.emplace_back(std::move(ref));
            }
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

struct Album : IStreamableContainer {
    AlbumRef ref;

    std::vector<std::shared_ptr<music::IStreamable>> tracks;

    static Album from_json(const nlohmann::json& j) {
        Album a;
        a.ref = AlbumRef::from_json(j);

        return a;
    }

    void setRef(const music::AlbumRef& ref) {
        this->ref = ref;
    }
};

}

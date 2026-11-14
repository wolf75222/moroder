/* EPISODE
- The episode object for a ytmusicapi search result of type "episode"
- Only includes information that is present in all search results of type "episode"
- Only used for podcasts (I think)
*/

#pragma once

#include "../utils/json_str.hpp"

#include "../interfaces/streamable.hpp"
#include "podcast.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace music {

struct EpisodeRef {
    std::string id;
    std::string title;

    std::string thumbnail_large;
    std::string thumbnail_small;

    music::PodcastRef podcast;

    bool is_live;

    static EpisodeRef from_json(const nlohmann::json& j) {
        EpisodeRef e;

        e.id = utils::jstr(j, "videoId");
        e.title = utils::jstr(j, "title");

        if (j.contains("podcast") && j["podcast"].is_object()) {
            e.podcast.id = utils::jstr(j["podcast"], "id");
            e.podcast.name = utils::jstr(j["podcast"], "name");
        } else {
            e.podcast.id = "";
            e.podcast.name = "";
        }

        if (j.contains("thumbnails") && j["thumbnails"].is_array() && !j["thumbnails"].empty()) {
            e.thumbnail_large = utils::jstr(j["thumbnails"].back(), "url");
            e.thumbnail_small = utils::jstr(j["thumbnails"].front(), "url");
        } else {
            e.thumbnail_large = "";
            e.thumbnail_small = "";
        }

        e.is_live = j.value("live", false);

        return e;
    }
};

struct Episode : IStreamable {
    EpisodeRef ref;

    static Episode from_json(const nlohmann::json& j) {
        Episode e;

        e.duration = 0;
        e.ref = EpisodeRef::from_json(j);
        e.url = "https://www.youtube.com/watch?v=" + e.ref.id;

        return e;
    }

    void setRef(const music::EpisodeRef& ref) {
        this->ref = ref;
        this->url = "https://www.youtube.com/watch?v=" + ref.id;
        this->duration = 0;
    }

    std::shared_ptr<IStreamable> clone() const override {
        return std::make_shared<Episode>(*this);
    }
};

}

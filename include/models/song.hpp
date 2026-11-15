/* SONG REF & SONG
- The song object for a ytmusicapi search result of type "song" or "video" (same thing)
- SongRef only includes information that is present in all search results of type "song" or "video"
- Song is the full song object with all details, only reliably obtainable after fetching song info from ytmusicapi
*/

#pragma once

#include "../utils/json_str.hpp"

#include "../interfaces/streamable.hpp"
#include "artist.hpp"
#include "album.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace music {

struct SongRef {
    std::string id;
    std::string title;

    std::string thumbnail_large;
    std::string thumbnail_small;

    std::vector<ArtistRef> artists;

    // Its place in one of the user's playlists (removing it needs this).
    std::string set_video_id;

    int duration_s = 0;     // 0: unknown

    // In a chart (Explore > Trending): its rank, and how it moved (1, -1, 0).
    std::string rank;
    int trend = 0;

    // Inverse of from_json (search-result shape), for history / session files.
    nlohmann::json to_json() const {
        nlohmann::json j;
        j["resultType"] = "song";
        j["videoId"] = id;
        j["title"] = title;
        j["artists"] = nlohmann::json::array();
        for (const auto& a : artists) j["artists"].push_back({{"name", a.name}, {"id", a.id}});
        j["thumbnails"] = nlohmann::json::array({ {{"url", thumbnail_small}}, {{"url", thumbnail_large}} });
        if (duration_s > 0) j["duration_seconds"] = duration_s;
        return j;
    }

    static SongRef from_json(const nlohmann::json& j) {
        SongRef s;

        s.id = utils::jstr(j, "videoId");
        s.title = utils::jstr(j, "title");
        s.set_video_id = utils::jstr(j, "setVideoId");
        s.rank = utils::jstr(j, "rank");
        if (j.contains("trend") && j["trend"].is_number_integer()) s.trend = j["trend"].get<int>();
        if (j.contains("duration_seconds") && j["duration_seconds"].is_number()) {
            s.duration_s = j["duration_seconds"].get<int>();
        } else if (j.contains("duration") && j["duration"].is_string()) {
            // "3:45" or "1:02:03"
            const std::string d = j["duration"].get<std::string>();
            int acc = 0, part = 0;
            bool any = false;
            for (char ch : d) {
                if (ch >= '0' && ch <= '9') { part = part * 10 + (ch - '0'); any = true; }
                else if (ch == ':') { acc = acc * 60 + part; part = 0; }
            }
            s.duration_s = any ? acc * 60 + part : 0;
        }

        if (j.contains("artists") && j["artists"].is_array()) {
            for (const auto& a : j["artists"]) {
                ArtistRef ref;

                ref.name = utils::jstr(a, "name");

                if (a.contains("id") && a["id"].is_string()) {
                    ref.id = a["id"].get<std::string>();
                } else {
                    ref.id = "";
                }

                s.artists.emplace_back(std::move(ref));
            }
        }

        /* Formats:
        - Search result: thumbnails
        - Radio result: thumbnail
        - Don't ask me why that is
        */
        if ((j.contains("thumbnails") && j["thumbnails"].is_array() && !j["thumbnails"].empty())) {
            s.thumbnail_large = utils::jstr(j["thumbnails"].back(), "url");
            s.thumbnail_small = utils::jstr(j["thumbnails"].front(), "url");

        } else if (j.contains("thumbnail")  && j["thumbnail"].is_array() && !j["thumbnail"].empty()) {
            s.thumbnail_large = utils::jstr(j["thumbnail"].back(), "url");
            s.thumbnail_small = utils::jstr(j["thumbnail"].front(), "url");

        } else {
            s.thumbnail_large = "";
            s.thumbnail_small = "";
        }

        return s;
    }
};

struct Song : IStreamable {
    SongRef ref;

    music::AlbumRef album;

    static Song from_json(const nlohmann::json& j) {
        Song s;

        /* Formats:
        - Search result: album: {id, title}
        - Radio result: album: [{id, title}]
        - Don't ask me why that is
        */
        if (j.contains("album")) {
            if (j["album"].is_object()) {
                const auto& album_json = j["album"];
                s.album.title = utils::jstr(album_json, "name");
                s.album.id = utils::jstr(album_json, "id");

            } else if (j["album"].is_array() && !j["album"].empty()) {
                const auto& album_json = j["album"].front();
                s.album.title = utils::jstr(album_json, "name");
                s.album.id = utils::jstr(album_json, "id");

            } else if (j["album"].is_string()) {
                s.album.title = j["album"].get<std::string>();
            }

        } else {
            s.album = music::AlbumRef{};
        }

        s.duration = 0;
        s.ref = SongRef::from_json(j);
        s.url = "https://www.youtube.com/watch?v=" + s.ref.id;

        return s;
    }

    void setRef(const music::SongRef& ref) {
        this->ref = ref;
        this->url = "https://www.youtube.com/watch?v=" + ref.id;
        this->duration = 0;
    }

    std::shared_ptr<IStreamable> clone() const override {
        return std::make_shared<Song>(*this);
    }
};

}

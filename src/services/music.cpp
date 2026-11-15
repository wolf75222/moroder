#include "../../include/services/music.hpp"
#include "../../include/utils/platform.hpp"
#include "../../include/utils/utils.hpp"
#include "../../include/utils/json_str.hpp"
#include <fstream>
#include <cstdlib>
#include "../../include/services/browser_cookies.hpp"

#include "../../include/ytm/metadata.hpp"

#include "../../include/ipc/playlist/library_playlists_response.hpp"
#include "../../include/ipc/playlist/playlist_response.hpp"

#include "../../include/ipc/podcast/library_podcasts_response.hpp"
#include "../../include/ipc/artist/library_artists_response.hpp"

#include "../../include/ipc/album/library_albums_response.hpp"
#include "../../include/ipc/album/album_response.hpp"

#include "../../include/ipc/song/library_songs_response.hpp"

#include "../../include/ipc/radio/radio_next_response.hpp"
#include "../../include/ipc/radio/radio_response.hpp"

#include "../../include/ipc/search/search_response.hpp"

#include "../../include/ipc/browse/song_response.hpp"
#include "../../include/ipc/browse/home_response.hpp"

#include "../../include/config/config.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace {

// Tunables that used to live in the Python request objects.
constexpr int SEARCH_LIMIT   = 20;
constexpr int RADIO_LIMIT    = 50;
constexpr int HOME_LIMIT     = -1;  // every shelf YouTube Music has
constexpr int LIBRARY_LIMIT  = 25;

/* LIBRARY_ALL
- The Python endpoints passed limit=None for everything except playlists, so the
- whole library was paged in. -1 means the same here.
*/
constexpr int LIBRARY_ALL = -1;

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

/* tryAdd
- The parsers emit JSON null for absent fields, matching ytmusicapi exactly.
- Several *Ref::from_json helpers read those with j.value(key, ""), which throws type_error.302 on null rather than falling back.
- sendStreamed used to swallow that in a catch(...) and abandon the rest of the stream. This keeps the rest of the results and logs what was dropped.
*/
std::string userMessage(ytm::Error::Kind kind) {
    switch (kind) {
        case ytm::Error::Kind::Network:
            return "Couldn't reach YouTube Music. Check your connection.";
        case ytm::Error::Kind::Auth:
            return "Your YouTube Music session has expired. Re-run the cookie setup to sign in again.";
        case ytm::Error::Kind::RateLimit:
            return "YouTube Music is rate limiting requests. Give it a moment.";
        case ytm::Error::Kind::Server:
            return "YouTube Music is having trouble right now.";
        case ytm::Error::Kind::Request:
            return "YouTube Music rejected the request. This is probably a bug in moroder.";
        case ytm::Error::Kind::Parse:
            return "Couldn't make sense of YouTube Music's response. The API may have changed.";
        case ytm::Error::Kind::NotFound:
            return "That doesn't seem to exist anymore.";
        default:
            return "Something went wrong.";
    }
}

template <typename Fn> bool tryAdd(const nlohmann::json& item, const char* what, Fn&& add) {
    if (item.is_null()) return false;

    try {
        add();
        return true;

    } catch (const std::exception& e) {
        spdlog::warn("MUSIC: skipped a malformed {} item, {}", what, e.what());
        return false;
    }
}

}

services::Music::Music(const std::string_view& app_name) {
    const Config& cfg = Config::get();

    lastfm_api_key = cfg.LASTFM_API_KEY;

    if (!lastfm_api_key.empty()) {
        spdlog::info("MUSIC: last.fm key loaded ({}...)", lastfm_api_key.substr(0, 4));
    }

    fs::path cookies_path = cfg.YTM_COOKIES_PATH / "browser.json";

    if (fs::exists(cookies_path)) {
        spdlog::info("MUSIC: found cookies at {}", cookies_path.string());
    } else {
        spdlog::warn("MUSIC: no cookies at {}, running unauthenticated", cookies_path.string());
        cookies_path.clear();
    }

    ytm = std::make_unique<ytm::YTMusic>(cookies_path);

    if (!ytm->lastError().empty()) {
        spdlog::warn("MUSIC: auth setup, {}", ytm->lastError());
    }

    spdlog::info("MUSIC: ready ({})", ytm->isAuthenticated() ? "authenticated" : "anonymous");
}

services::Music::~Music() = default;

void services::Music::captureError(const char* what) {
    const ytm::Error& err = ytm->lastErrorInfo();

    last_error = RequestError{};

    if (err.ok()) return;

    last_error.failed = true;
    last_error.cancelled = (err.kind == ytm::Error::Kind::Cancelled);
    last_error.retryable = err.retryable();
    last_error.detail = err.detail;
    last_error.message = userMessage(err.kind);

    if (last_error.cancelled) {
        spdlog::info("MUSIC: {} cancelled", what);
        return;
    }

    spdlog::warn("MUSIC: {} failed, {}", what, err.detail);
}

std::vector<music::ApiResult> services::Music::getSearch(const std::string& query) {
    ipc::SearchResponse response;

    nlohmann::json results = ytm->search(query, SEARCH_LIMIT);
    captureError("search");

    for (const nlohmann::json& item : results) {
        tryAdd(item, "search", [&] { response.addItem(item); });
    }

    spdlog::info("MUSIC: search returned {} results", results.size());

    return response.results;
}

music::Radio services::Music::getRadio(const std::string id, const std::string type) {
    music::Radio r;
    r.type = type;

    last_error = RequestError{};

    if (id.empty()) return r;

    ytm::WatchPlaylist wp;

    if (type == "song") {
        wp = ytm->getWatchPlaylist(id, "", -1);

    } else {
        std::string pid = id;

        /* An album browseId can't seed a radio; resolve it to the album's audio
        playlist first. The Python side never did this, which is why album
        radio only worked when the caller happened to pass an OLAK id.
        */
        if (startsWith(pid, "MPRE")) {
            nlohmann::json album = ytm->getAlbum(pid);
            std::string    audio = utils::jstr(album, "audioPlaylistId");   // null without a play button

            if (audio.empty()) {
                captureError("radio");

                if (!last_error.failed) {
                    last_error.failed = true;
                    last_error.retryable = false;
                    last_error.detail = "album " + pid + " has no audioPlaylistId";
                    last_error.message = "That album can't be used to start a radio.";
                }

                spdlog::warn("MUSIC: could not resolve audio playlist for album {}", pid);
                return r;
            }

            pid = audio;
        }

        /* Strip VL before prepending PL. Doing it the other way round, as the
        Python did, turned "VLPLxxx" into the unusable "PLVLPLxxx".
        */
        if (startsWith(pid, "VL")) pid = pid.substr(2);
        if (!startsWith(pid, "PL")) pid = "PL" + pid;

        wp = ytm->getWatchPlaylist("", "RDAM" + pid, -1);
    }

    captureError("radio");

    ipc::RadioResponse response;

    for (const nlohmann::json& track : wp.tracks) {
        tryAdd(track, "radio", [&] { response.addItem(track); });
    }

    r.seed_id      = wp.seedId;
    r.continuation = wp.continuation;

    for (const auto& s : response.results) {
        r.addStreamable(s);
    }

    spdlog::info("MUSIC: radio returned {} tracks", wp.tracks.size());

    return r;
}

music::Radio services::Music::getRadio(const music::SongRef& song) {
    return this->getRadio(song.id, "song");
}

music::Radio services::Music::getRadio(const music::AlbumRef& album) {
    return this->getRadio(album.id, "playlist");
}

music::Radio services::Music::getRadio(const music::EpisodeRef& episode) {
    return this->getRadio(episode.id, "song");
}

music::Radio services::Music::getRadio(const music::PlaylistRef& playlist) {
    return this->getRadio(playlist.id, "playlist");
}

music::Radio services::Music::getRadioNext(const music::Radio& radio) {
    music::Radio r;

    // The Python version dropped the type here, so a second continuation always
    // took the song-radio path regardless of what the queue actually was.
    r.type = radio.type;

    last_error = RequestError{};

    if (radio.continuation.empty()) return r;

    ytm::WatchPlaylist wp = (radio.type == "song")
        ? ytm->getWatchPlaylistNext(radio.seed_id, "", radio.continuation, RADIO_LIMIT)
        : ytm->getWatchPlaylistNext("", radio.seed_id, radio.continuation, RADIO_LIMIT);

    captureError("radio continuation");

    ipc::RadioNextResponse response;

    for (const nlohmann::json& track : wp.tracks) {
        tryAdd(track, "radio", [&] { response.addItem(track); });
    }

    r.seed_id      = wp.seedId.empty() ? radio.seed_id : wp.seedId;
    r.continuation = wp.continuation;

    for (const auto& s : response.results) {
        r.addStreamable(s);
    }

    spdlog::info("MUSIC: radio continuation returned {} tracks", wp.tracks.size());

    return r;
}

music::Album services::Music::getAlbum(const music::AlbumRef& album) {
    nlohmann::json data = ytm->getAlbum(album.id);
    captureError("album");

    ipc::AlbumResponse response;

    if (data.contains("tracks")) {
        for (nlohmann::json& track : data["tracks"]) {
            tryAdd(track, "album track", [&] { response.addItem(track, album); });
        }
    }

    music::Album a;
    a.setRef(album);

    for (const auto& s : response.results) {
        a.addStreamable(s);
    }

    return a;
}

music::Playlist services::Music::getPlaylist(const music::PlaylistRef& playlist, bool first_page_only, bool* more) {
    nlohmann::json data = ytm->getPlaylist(playlist.id, first_page_only ? 0 : -1);
    captureError("playlist");
    // Counted before parsing: a track that fails to parse must not make a
    // long playlist look complete.
    if (more) *more = first_page_only && data.contains("tracks") && data["tracks"].is_array() && data["tracks"].size() >= 100;

    ipc::PlaylistResponse response;

    if (data.contains("tracks")) {
        for (nlohmann::json& track : data["tracks"]) {
            tryAdd(track, "playlist track", [&] { response.addItem(track); });
        }
    }

    music::Playlist p;
    p.setRef(playlist);

    for (const auto& s : response.results) {
        p.addStreamable(s);
    }

    return p;
}

namespace {

// Only for a signed-in session: an anonymous home isn't worth keeping.
fs::path cacheFile(const std::string& name) {
    const std::string home_s = utils::home().string(); const char* home = home_s.empty() ? nullptr : home_s.c_str();
    if (!home) return {};
    return fs::path(home) / ".cache" / "moroder" / (name + ".json");
}

nlohmann::json readCache(const std::string& name) {
    std::ifstream in(cacheFile(name));
    if (!in.is_open()) return nlohmann::json();
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    return j.is_discarded() ? nlohmann::json() : j;
}

void writeCache(const std::string& name, const nlohmann::json& j) {
    const fs::path file = cacheFile(name);
    if (file.empty()) return;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) return;
        out << j.dump();
    }
    fs::rename(tmp, file, ec);     // atomic: a crash never leaves half a file
}

}

std::unordered_map<std::string, std::vector<music::ApiResult>> services::Music::getHome(bool cached_only, bool first_page_only) {
    nlohmann::json sections;

    if (cached_only) {
        if (!ytm->isAuthenticated()) return {};
        sections = readCache("home");
        last_error = RequestError{};
        if (!sections.is_array()) return {};
    } else {
        sections = ytm->getHome(first_page_only ? 0 : HOME_LIMIT);
        captureError("home");
        if (!first_page_only && ytm->isAuthenticated() && !last_error.failed && sections.is_array() && !sections.empty()) writeCache("home", sections);
    }

    ipc::HomeResponse response;

    for (const nlohmann::json& section : sections) {
        std::string category = utils::jstr(section, "title");

        if (!section.contains("contents")) continue;

        for (const nlohmann::json& item : section["contents"]) {
            /* parse_mixed_content yields null for renderer types it doesn't
            recognise; the Python protocol used that as a stream terminator,
            which silently truncated the page. Skip them instead.
            */
            tryAdd(item, "home", [&] { response.addItem(category, item); });
        }
    }

    home_order = response.order;
    return response.results;
}

std::unordered_map<std::string, std::vector<music::ApiResult>> services::Music::getExplore(const std::string& browseId, const std::string& params) {
    nlohmann::json sections = ytm->getBrowse(browseId, 6, params);
    captureError("explore");

    ipc::HomeResponse response;
    if (sections.is_array()) {
        for (const nlohmann::json& section : sections) {
            const std::string category = utils::jstr(section, "title");
            if (!section.contains("contents") || !section["contents"].is_array()) continue;
            for (const nlohmann::json& item : section["contents"]) {
                tryAdd(item, "explore", [&] { response.addItem(category, item); });
            }
        }
    }
    explore_order = response.order;
    return response.results;
}

services::Music::ArtistPage services::Music::getArtist(const music::ArtistRef& artist) {
    ArtistPage page;
    page.ref = artist;

    nlohmann::json data = ytm->getArtist(artist.id);
    captureError("artist");

    if (!utils::jstr(data, "name").empty()) page.ref.name = utils::jstr(data, "name");
    if (!utils::jstr(data, "thumbnail").empty()) page.ref.thumbnail_large = utils::jstr(data, "thumbnail");

    ipc::HomeResponse response;
    if (data.contains("sections") && data["sections"].is_array()) {
        for (const nlohmann::json& section : data["sections"]) {
            const std::string category = utils::jstr(section, "title");
            if (!section.contains("contents") || !section["contents"].is_array()) continue;
            for (const nlohmann::json& item : section["contents"]) {
                tryAdd(item, "artist", [&] { response.addItem(category, item); });
            }
        }
    }

    page.shelves = response.results;
    page.order = response.order;
    return page;
}

bool services::Music::refreshCookies() {
    const Config& cfg = Config::get();
    if (Config::anonymous || cfg.COOKIE_BROWSER.empty()) return false;

    // For automated tests: use the saved session, never touch the Keychain.
    if (std::getenv("MORODER_NO_COOKIE_REFRESH")) return false;

    std::string error;
    const std::string cookie = services::readYouTubeCookies(cfg.COOKIE_BROWSER, error);
    if (cookie.empty()) {
        spdlog::warn("MUSIC: could not refresh the session from {}: {}", cfg.COOKIE_BROWSER, error);
        return false;
    }

    if (!ytm->setCookie(cookie)) {
        spdlog::warn("MUSIC: {} cookies carry no SAPISID, signed out there?", cfg.COOKIE_BROWSER);
        return false;
    }

    // Persist, so the next launch (and yt-dlp's cookie file) start fresh too.
    const fs::path file = cfg.YTM_COOKIES_PATH / "browser.json";
    nlohmann::json headers = nlohmann::json::object();
    {
        std::ifstream in(file);
        if (in.is_open()) headers = nlohmann::json::parse(in, nullptr, false);
        if (!headers.is_object()) headers = nlohmann::json::object();
    }
    headers["cookie"] = cookie;
    if (!headers.contains("user-agent")) headers["user-agent"] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
    if (!headers.contains("origin")) { headers["origin"] = "https://music.youtube.com"; headers["x-origin"] = "https://music.youtube.com"; }

    // Atomic and 0600 from creation: a reader never sees half a file, and the
    // session is never briefly world-readable.
    {
        const fs::path tmp = file.string() + ".tmp";
        std::error_code ec;
        fs::remove(tmp, ec);
        if (platform::writePrivateFile(tmp, headers.dump(4))) {
            fs::rename(tmp, file, ec);
            if (ec) fs::remove(tmp, ec);
        }
    }

    spdlog::info("MUSIC: session refreshed from {}", cfg.COOKIE_BROWSER);
    return true;
}

bool services::Music::rateSong(const std::string& videoId, bool like) {
    if (!ytm->isAuthenticated()) return false;

    bool ok = ytm->rateSong(videoId, like);
    captureError("rate song");
    return ok && !last_error.failed;
}

int services::Music::likeStatus(const std::string& videoId) {
    if (!ytm->isAuthenticated()) return -1;
    const int status = ytm->likeStatus(videoId);
    captureError("like status");
    return last_error.failed ? -1 : status;
}

std::string services::Music::createPlaylist(const std::string& title, const std::string& videoId) {
    if (!ytm->isAuthenticated()) return "";
    std::string id = ytm->createPlaylist(title, videoId);
    captureError("create playlist");
    return last_error.failed ? "" : id;
}

bool services::Music::addToPlaylist(const std::string& playlistId, const std::string& videoId) {
    if (!ytm->isAuthenticated()) return false;
    bool ok = ytm->addToPlaylist(playlistId, videoId);
    captureError("add to playlist");
    return ok && !last_error.failed;
}

bool services::Music::removeFromPlaylist(const std::string& playlistId, const std::string& videoId, const std::string& setVideoId) {
    if (!ytm->isAuthenticated()) return false;
    bool ok = ytm->removeFromPlaylist(playlistId, videoId, setVideoId);
    captureError("remove from playlist");
    return ok && !last_error.failed;
}

bool services::Music::renamePlaylist(const std::string& playlistId, const std::string& title) {
    if (!ytm->isAuthenticated()) return false;
    bool ok = ytm->renamePlaylist(playlistId, title);
    captureError("rename playlist");
    return ok && !last_error.failed;
}

bool services::Music::deletePlaylist(const std::string& playlistId) {
    if (!ytm->isAuthenticated()) return false;
    bool ok = ytm->deletePlaylist(playlistId);
    captureError("delete playlist");
    return ok && !last_error.failed;
}

std::vector<music::Playlist> services::Music::getLibraryPlaylists(bool cached_only) {
    if (!ytm->isAuthenticated()) {
        spdlog::info("MUSIC: not logged in, no library playlists");

        last_error = RequestError{};
        return {};
    }

    nlohmann::json playlists;

    if (cached_only) {
        playlists = readCache("library_playlists");
        last_error = RequestError{};
        if (!playlists.is_array()) return {};
    } else {
        playlists = ytm->getLibraryPlaylists(LIBRARY_LIMIT);
        captureError("library playlists");
        if (!last_error.failed && playlists.is_array()) writeCache("library_playlists", playlists);
    }

    ipc::LibraryPlaylistsResponse response;

    for (const nlohmann::json& p : playlists) {
        tryAdd(p, "library playlist", [&] { response.addItem(p); });
    }

    return response.results;
}

std::vector<music::Album> services::Music::getLibraryAlbums() {
    if (!ytm->isAuthenticated()) {
        spdlog::info("MUSIC: not logged in, no library albums");

        last_error = RequestError{};
        return {};
    }

    nlohmann::json albums = ytm->getLibraryAlbums(LIBRARY_ALL);
    captureError("library albums");

    ipc::LibraryAlbumsResponse response;

    for (const nlohmann::json& album : albums) {
        tryAdd(album, "library album", [&] { response.addItem(album); });
    }

    spdlog::info("MUSIC: library returned {} albums", albums.size());

    return response.results;
}

std::vector<std::shared_ptr<music::IStreamable>> services::Music::getLibrarySongs() {
    if (!ytm->isAuthenticated()) {
        spdlog::info("MUSIC: not logged in, no library songs");

        last_error = RequestError{};
        return {};
    }

    nlohmann::json songs = ytm->getLibrarySongs(LIBRARY_ALL);
    captureError("library songs");

    ipc::LibrarySongsResponse response;

    for (const nlohmann::json& song : songs) {
        tryAdd(song, "library song", [&] { response.addItem(song); });
    }

    spdlog::info("MUSIC: library returned {} songs", songs.size());

    return response.results;
}

std::vector<music::ArtistRef> services::Music::getLibraryArtists() {
    if (!ytm->isAuthenticated()) {
        spdlog::info("MUSIC: not logged in, no library artists");

        last_error = RequestError{};
        return {};
    }

    nlohmann::json artists = ytm->getLibraryArtists(LIBRARY_ALL);
    captureError("library artists");

    ipc::LibraryArtistsResponse response;

    for (const nlohmann::json& artist : artists) {
        tryAdd(artist, "library artist", [&] { response.addItem(artist); });
    }

    spdlog::info("MUSIC: library returned {} artists", artists.size());

    return response.results;
}

std::vector<music::PodcastRef> services::Music::getLibraryPodcasts() {
    if (!ytm->isAuthenticated()) {
        spdlog::info("MUSIC: not logged in, no library podcasts");

        last_error = RequestError{};
        return {};
    }

    nlohmann::json podcasts = ytm->getLibraryPodcasts(LIBRARY_ALL);
    captureError("library podcasts");

    ipc::LibraryPodcastsResponse response;

    for (const nlohmann::json& podcast : podcasts) {
        tryAdd(podcast, "library podcast", [&] { response.addItem(podcast); });
    }

    spdlog::info("MUSIC: library returned {} podcasts", podcasts.size());

    return response.results;
}

bool services::Music::isLoggedIn() {
    last_error = RequestError{};

    return ytm->isAuthenticated();
}

music::Song services::Music::getSong(const music::SongRef& song) {
    // ipc::SongRequest read artists[0].name unguarded, which was undefined
    // behaviour for a result with no artists.
    std::string artist = song.artists.empty() ? std::string() : song.artists.front().name;

    nlohmann::json payload = ytm::lookupAlbum(metadata_http, lastfm_api_key, song.title, artist);

    last_error = RequestError{};

    if (payload.value("status", std::string()) != "ok") {
        last_error.failed = true;
        last_error.retryable = true;
        last_error.detail = payload.value("message", std::string());
        last_error.message = "Couldn't look up album details for this track.";

        spdlog::warn("MUSIC: album lookup failed for '{}', {}", song.title, last_error.detail);
    }

    ipc::SongResponse response(payload.dump());

    response.song.setRef(song);
    return response.song;
}

music::Episode services::Music::getEpisode(const music::EpisodeRef& episode) {
    last_error = RequestError{};

    // No request needed, we already have the data.
    music::Episode e;
    e.setRef(episode);

    return e;
}

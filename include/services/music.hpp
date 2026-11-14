/* MUSIC
- Service responsible for all communications with the YouTube Music API
- Talks to InnerTube directly; no Python process, no pipes, no IPC framing
- Still builds models through the existing ipc::*Response classes
- Everything is logged
*/

#pragma once

#include "../models/api_result.hpp"
#include "../models/radio.hpp"

#include "../ytm/ytmusic.hpp"
#include "../ytm/http.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace services {

struct RequestError {
    bool failed = false;
    bool retryable = false;
    bool cancelled = false;
    std::string message;
    std::string detail;
};

class Music {
public:
    /* app_name
    - Retained for source compatibility only
    - It used to name the Python IPC socket, which no longer exists
    */
    Music(const std::string_view& app_name = {});
    ~Music();

    bool isLoggedIn();

    const RequestError& lastError() const { return last_error; }

    std::vector<music::Playlist>   getLibraryPlaylists(bool cached_only = false);
    std::vector<music::Album>      getLibraryAlbums();
    std::vector<music::ArtistRef>  getLibraryArtists();
    std::vector<music::PodcastRef> getLibraryPodcasts();

    std::vector<std::shared_ptr<music::IStreamable>> getLibrarySongs();
    std::vector<music::ApiResult> getSearch(const std::string& query);

    /* Cache
- The last home page and library playlists are kept on disk (raw API JSON,
  ~/.cache/moroder), so the next launch shows them at once while fresh
  copies load. cached_only reads that file instead of the network
    */
    std::unordered_map<std::string, std::vector<music::ApiResult>> getHome(bool cached_only = false);

    /* refreshCookies
    - With COOKIE_BROWSER set: re-read the session from that browser, use it
      at once and save it to browser.json. False when unset or unreadable
    */
    bool refreshCookies();

    // Like / un-like a track on the account. False if signed out or it failed.
    bool rateSong(const std::string& videoId, bool like);
    std::string createPlaylist(const std::string& title, const std::string& videoId);
    bool addToPlaylist(const std::string& playlistId, const std::string& videoId);
    // Shelf titles from the last getHome(), in YouTube Music's order.
    const std::vector<std::string>& homeOrder() const { return home_order; }

    music::Song     getSong(const music::SongRef& song);
    music::Episode  getEpisode(const music::EpisodeRef& episode);

    music::Radio    getRadio(const music::SongRef& song);
    music::Radio    getRadio(const music::AlbumRef& album);
    music::Radio    getRadio(const music::EpisodeRef& episode);
    music::Radio    getRadio(const music::PlaylistRef& playlist);

    music::Radio    getRadioNext(const music::Radio& radio);

    music::Album    getAlbum(const music::AlbumRef& album);

    // Artist page: name, picture and its shelves (Top songs, Albums...).
    struct ArtistPage {
        music::ArtistRef ref;
        std::unordered_map<std::string, std::vector<music::ApiResult>> shelves;
        std::vector<std::string> order;
    };
    ArtistPage getArtist(const music::ArtistRef& artist);
    music::Playlist getPlaylist(const music::PlaylistRef& playlist);

private:
    std::vector<std::string> home_order;
    /* ytm
    - The InnerTube client: transport, auth, endpoints, continuations
    - Reads the same browser.json ytmusicapi's setup writes
    */
    std::unique_ptr<ytm::YTMusic> ytm;

    /* metadata_http
    - last.fm / iTunes only, for the album-name enrichment in getSong
    - This never went through ytmusicapi
    */
    ytm::Http   metadata_http;
    std::string lastfm_api_key;

    RequestError last_error;

    void captureError(const char* what);

    music::Radio getRadio(const std::string id, const std::string type);
};

}

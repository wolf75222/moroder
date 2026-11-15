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
    // first_page_only: the first shelves (one request), not cached.
    std::unordered_map<std::string, std::vector<music::ApiResult>> getHome(bool cached_only = false, bool first_page_only = false);

    /* refreshCookies
    - With COOKIE_BROWSER set: re-read the session from that browser, use it
      at once and save it to browser.json. False when unset or unreadable
    */
    bool refreshCookies();

    // Like / un-like a track on the account. False if signed out or it failed.
    bool rateSong(const std::string& videoId, bool like);
    // 1 liked, 0 not, -1 unknown.
    int likeStatus(const std::string& videoId);
    std::string createPlaylist(const std::string& title, const std::string& videoId);
    bool addToPlaylist(const std::string& playlistId, const std::string& videoId);
    bool removeFromPlaylist(const std::string& playlistId, const std::string& videoId, const std::string& setVideoId);
    bool renamePlaylist(const std::string& playlistId, const std::string& title);
    bool deletePlaylist(const std::string& playlistId);
    // Shelf titles from the last getHome(), in YouTube Music's order.
    const std::vector<std::string>& homeOrder() const { return home_order; }
    // YouTube Music's Explore page (new albums & singles, trending, videos...).
    std::unordered_map<std::string, std::vector<music::ApiResult>> getExplore(const std::string& browseId = "FEmusic_explore", const std::string& params = "");
    const std::vector<std::string>& exploreOrder() const { return explore_order; }
    // Each section's "More" page (browse id), by title.
    const std::unordered_map<std::string, std::string>& exploreMore() const { return explore_more; }

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
    // first_page_only: the first ~100 tracks (one request), to show / play at once.
    // more (optional): YouTube returned a full first page, so there are likely more.
    music::Playlist getPlaylist(const music::PlaylistRef& playlist, bool first_page_only = false, bool* more = nullptr);

private:
    std::vector<std::string> home_order;
    std::vector<std::string> explore_order;
    std::unordered_map<std::string, std::string> explore_more;
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

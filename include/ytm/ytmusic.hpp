/* YTMUSIC
- A native client for YouTube Music's InnerTube API, covering all the endpoints moroder used to reach through the Python ytmusicapi process
- Every method returns JSON in the same shape ytmusicapi returns, so existing ipc:: response classes can consume it unchanged.
- This is probably a performance loss, compared to a ground up cpp design, but was the most convenient option as a replacement.
- Removing python wasn't done for perfomance reasons anyway, more for installation convenience.
*/

#pragma once

#include <chrono>

#include "http.hpp"
#include "nav.hpp"

#include <filesystem>
#include <mutex>
#include <string>

namespace ytm {

struct Error {
    enum class Kind {
        None,
        Cancelled,
        Network,
        Auth,
        NotFound,
        RateLimit,
        Server,
        Request,
        Parse,
    };

    Kind kind = Kind::None;
    std::string detail;

    bool ok() const { return kind == Kind::None; }

    bool retryable() const {
        switch (kind) {
            case Kind::Network:
            case Kind::RateLimit:
            case Kind::Server:
            case Kind::Parse:
                return true;
            default:
                return false;
        }
    }
};

/* WatchPlaylist
- The result of a "next" (watch playlist / radio) request.
*/
struct WatchPlaylist {
    json tracks = json::array();
    std::string continuation;
    std::string seedId;
};

class YTMusic {
public:
    /* YTMusic
    - "browserJson" is the same headers file ytmusicapi's `setup` writes, so an existing browser.json keeps working without regeneration.
    - Pass an empty path to run unauthenticated.
    */
    explicit YTMusic(const std::filesystem::path& browserJson = {}, std::string language = "en", std::string location = "");

    bool isAuthenticated() const {
        return authenticated;
    }

    /* setCookie
    - Swap in a fresh cookie header (re-read from the browser) without
      rebuilding the client; false if it carries no SAPISID
    */
    bool setCookie(const std::string& cookie);

    /* rateSong
    - Like a track, or clear its rating, as the web player does
    - The web player sends params "OAI=" with it; without it YouTube answers
      "Precondition check failed"
    - Writes need fresh session cookies (Google rotates them); reads don't, so
      a session can browse fine and still be refused here
    */
    bool rateSong(const std::string& videoId, bool like);

    // The account's rating of a track: 1 liked, 0 not, -1 unknown (signed out, failed).
    int likeStatus(const std::string& videoId);

    // New private playlist (optionally with a first track); its id, "" on failure.
    std::string createPlaylist(const std::string& title, const std::string& videoId = "");

    // Append a track to one of the user's playlists.
    bool addToPlaylist(const std::string& playlistId, const std::string& videoId);

    // Remove a track (setVideoId: its place in the list), rename, delete.
    bool removeFromPlaylist(const std::string& playlistId, const std::string& videoId, const std::string& setVideoId);
    bool renamePlaylist(const std::string& playlistId, const std::string& title);
    bool deletePlaylist(const std::string& playlistId);

    /* lastError
    - Empty when the last call succeeded.
    */
    const std::string& lastError() const { return last_error.detail; }

    const Error& lastErrorInfo() const { return last_error; }

    /* debugHeaderNames
    - Names only, never values; The cookie and authorization headers are secrets.
    - Useful for diagnosing duplicate-header rejections.
    */
    std::vector<std::string> debugHeaderNames();



    /* Search
    - Array of search results.
    - "filter" is optional; when set, continuations are followed until "limit" results are collected.
    */
    json search(const std::string& query, int limit = 20, const std::string& filter = "");

    /* getHome
    - Array of {title, contents} sections.
    */
    json getHome(int limit = 3);
    // Any browse page made of shelves like Home (FEmusic_explore...).
    json getBrowse(const std::string& browseId, int limit = 3);

    /* getAlbum
    - Album object with a "tracks" array. browseId must start with MPRE.
    */
    json getAlbum(const std::string& browseId);

    /* getArtist
    - {name, thumbnail, sections}: sections as in getHome (title + contents),
      "Top songs" first, then Albums, Singles, Playlists, related artists...
    */
    json getArtist(const std::string& browseId);

    /* getPlayList
    - Playlist object with a "tracks" array. limit < 0 means "all tracks".
    */
    json getPlaylist(const std::string& playlistId, int limit = -1);

    /* getLibraryPlaylists
    - Array of playlist objects. Requires authentication
    - Returns [] without it.
    */
    json getLibraryPlaylists(int limit = 25);

    /* getAccountInfo
    - The signed-in account, as YouTube Music's account menu shows it:
      {"name", "handle", "photo"}; null when signed out or on error
    - With several Google accounts in one browser, the x-goog-authuser header
      (saved in browser.json) picks which one this is
    */
    json getAccountInfo();

    /* getLibraryAlbums / Songs / Artists / Podcasts
    - The rest of the library pages. All require authentication and return [] without it.
    - Albums and podcasts come back as grids, songs and artists as shelves.
    */
    json getLibraryAlbums(int limit = 25);
    json getLibrarySongs(int limit = 25);
    json getLibraryArtists(int limit = 25);
    json getLibraryPodcasts(int limit = 25);

    /* getWatchPlaylist
    -  Radio / autoplay queue.
    - Exactly one of videoId and playlistId is used as the seed, matching ytmusicapi's get_watch_playlist.
    */
    WatchPlaylist getWatchPlaylist(const std::string& videoId, const std::string& playlistId, int limit = 25);

    /* getWatchPlaylistNext
    - Follows a continuation token from a previous getWatchPlaylist.
    - The seed is passed again rather than cached, so continuations survive restarts.
    */
    WatchPlaylist getWatchPlaylistNext(const std::string& videoId, const std::string& playlistId, const std::string& ctoken, int limit = 25);

private:
    json sendRequest(const std::string& endpoint, const json& body, const std::string& additionalParams = "");
    std::vector<std::string> buildHeaders();

    void setError(Error::Kind kind, std::string detail);

    void ensureVisitorId();
    void loadAuth(const std::filesystem::path& path);

    // continuations.py equivalents
    json getContinuations(json results, const std::string& continuationType, int limit, const std::string& endpoint, const json& body, const std::function<json(const json&)>& parseFunc);
    json getContinuations2025(const json& results, int limit, const std::string& endpoint, const json& baseBody, const std::function<json(const json&)>& parseFunc);

    Http http;
    std::mutex mtx;

    json auth_headers = json::object();
    std::string sapisid;
    std::chrono::steady_clock::time_point visitor_attempt{};   // last homepage scrape
    std::string origin;
    std::string visitor_id;
    std::string params;
    std::string language;
    std::string location;

    bool        authenticated = false;
    Error       last_error;
};

}

/* PLAYER
- Service that wraps: Music, Mpris and Social into one service
- Adds queuing support
- Makes sure all music_service commands are synchronous
- Give a shared pointer on a struct ref and it will queue the thing for you
*/

#pragma once

#include <chrono>
#include <map>
#include <atomic>
#include <array>
#include <nlohmann/json.hpp>
#include <unordered_set>

#include "social.hpp"
#include "mpris.hpp"
#include "music.hpp"
#include "lyrics.hpp"
#include "mpv.hpp"

#include <cstdint>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace services {

class Player {
private:
    struct SearchCommand {
        std::string query;
    };

    struct QueueStreamableRadioCommand {
        std::shared_ptr<music::IStreamable> streamable;
    };

    struct QueueStreamableCommand {
        std::shared_ptr<music::IStreamable> streamable;
        bool fetch_album = true;
        bool start_radio = true;
        bool start_fresh = false;
        bool queue_in_radio = false;
        bool from_container = false;    // part of a playlist / album being queued
        uint64_t epoch = 0;             // a playlist's late rest: dropped if the queue was replaced since
    };

    struct QueueStreamableContainerRadioCommand {
        std::shared_ptr<music::IStreamableContainer> container;
    };

    struct QueueStreamableContainerCommand {
        std::shared_ptr<music::IStreamableContainer> container;

        bool fetch_albums = false;
        bool start_fresh  = false;
        bool start_radio  = true;
        bool queue_in_radio = false;
        size_t skip = 0;                // "play from track N": the first N were left out
    };

    struct QueueNextRadioCommand {
        music::Radio radio;

        bool auto_play = false;
    };

    struct HomeCommand {};
    struct ExploreCommand {
        std::string browse_id, params;
    };
    struct LibraryPlaylistsCommand {};
    struct LibraryAlbumsCommand {};
    struct LibrarySongsCommand {};
    struct LibraryArtistsCommand {};
    struct LibraryPodcastsCommand {};
    struct IsLoggedInCommand {};

    struct RefreshSessionCommand {};

    struct ShuffleCommand {};

    struct PlayNextCommand {
        std::shared_ptr<music::IStreamable> streamable;
    };

    struct PrefetchPlaylistCommand {
        music::PlaylistRef ref;
    };

    struct MoveCommand {
        size_t from = 0, to = 0;
    };

    // Swap a queued entry's YouTube URL for its pre-resolved stream URL.
    struct PreloadedCommand {
        size_t index = 0;
        std::string expected_url, stream_url;
    };

    // The track that starts was resolved off the worker thread: load it now.
    struct LoadCurrentCommand {
        uint64_t generation = 0;
        std::string youtube_url, stream_url;
    };

    // Create a playlist (title set) or add a track to one (playlist_id set).
    struct PlaylistEditCommand {
        std::string title, playlist_id, video_id;
        enum class Kind { Create, Add, Remove, Rename, Delete } kind = Kind::Add;
        std::string set_video_id;
    };

    // The rest of a playlist whose first page is shown / playing.
    struct PlaylistRestCommand {
        music::PlaylistRef ref;
        size_t have = 0;                    // tracks already there
        size_t append_from = SIZE_MAX;      // "add to queue" of its first page: append from here
        uint64_t epoch = 0;                 // ... if the queue is still that one
    };

    // A skip while the starting track resolves: resolve the new current one.
    struct ResolveCurrentCommand {};

    struct OpenAlbumCommand {
        music::AlbumRef ref;
    };

    struct OpenArtistCommand {
        music::ArtistRef ref;
    };

    struct OpenPlaylistCommand {
        music::PlaylistRef ref;
    };

    struct RateCommand {
        std::string video_id;
        bool like = true;
    };

    struct LikeStatusCommand {
        std::string video_id;
    };

public:
    /* PlayerState
    - Shared player state between all threads
    */
    enum class Flags {
        Ongoing,
        Done,
        Error,

        True,
        False
    };

    enum class LoopMode {
        None,
        Queue,
        Track
    };

    struct ErrorInfo {
        std::string message;
        bool retryable = false;
    };

    struct PlayerState {
        int queue_position = -1;

        std::deque<std::shared_ptr<music::IStreamable>> user_queue;
        std::deque<std::shared_ptr<music::IStreamable>> radio_queue;

        std::shared_ptr<music::IStreamable> current;

        std::unordered_map<std::string, std::vector<music::ApiResult>> home;
        std::vector<std::string> home_order;    // shelf titles, YouTube Music's order
        std::unordered_map<std::string, std::vector<music::ApiResult>> explore;
        std::vector<std::string> explore_order;
        std::string explore_tab = "FEmusic_explore";   // which page of Explore is shown
        std::string explore_title;                      // a mood / genre's name, else ""
        std::string explore_params;                     // a mood's params (its page), else ""
        std::unordered_map<std::string, std::string> explore_more;   // section title -> its "More" page

        /* library
        - Filled by the Library*Commands, all require the user to be logged in
        - library_songs holds IStreamable (music::Song) since library songs are
          directly playable, unlike the other library entries which are browsable
        */
        std::vector<music::Playlist> library_playlists;
        std::vector<music::Album> library_albums;
        std::vector<std::shared_ptr<music::IStreamable>> library_songs;
        std::vector<music::ArtistRef> library_artists;
        std::vector<music::PodcastRef> library_podcasts;

        std::vector<music::ApiResult> search_results;

        std::unordered_map<std::string, Flags> flags;
        std::unordered_map<std::string, ErrorInfo> errors;

        bool is_logged_in = false;
        bool autoplay = true;

        music::Radio radio;
        LoopMode loop_mode = LoopMode::None;

        int volume = 100;                                   // 0..100
        std::unordered_map<std::string, bool> liked;        // video id -> liked, this session

        // Playlist shown in the playlist view, with its tracks once loaded.
        std::shared_ptr<music::Playlist> open_playlist;
        std::string open_kind = "playlist";                 // "playlist" or "album", for the header

        // Artist page shown in the artist view.
        std::shared_ptr<services::Music::ArtistPage> open_artist;

        // Lyrics of the current track (Now Playing view), and whose they are.
        std::shared_ptr<services::Lyrics> lyrics;
        std::string lyrics_id;

        // Title of the track being previewed, empty when none.
        std::string preview_title;

        // Short message for the playback bar (e.g. why a like failed).
        std::string notice;
        std::chrono::steady_clock::time_point notice_until;
    };

    PlayerState state;
    std::mutex state_mutex;

    static constexpr uint64_t LOOP_REWIND_EPSILON = 2000000ULL;

    Player(const std::string_view& app_name, const std::string_view& app_name_human, const uint64_t app_id);
    ~Player();

    /* Player controls
    - Basic playback operations
    */
    void skipForward();
    void skipBackward();
    void skipTo(uint16_t index);

    /* cycleLoop
    - off, then the queue, then the current track
    - Returns the mode it settled on, so a caller that wants to say so can
    */
    LoopMode cycleLoop();
    LoopMode loopMode();
    void setLoop(LoopMode mode);

    void getHome();
    void isLoggedIn();
    void getLibraryPlaylists();
    void getLibraryAlbums();
    void getLibrarySongs();
    void getLibraryArtists();
    void getLibraryPodcasts();

    /* retryLibrary
    - Clears every library error and re-fires all five library requests
    - The clearing happens up front and under one lock, so the page drops back
      to its loading state the moment the button is pressed, instead of holding
      the error box until the slowest request lands
    */
    void retryLibrary();
    void search(const std::string& query);

    /* Recent search results, by query: going back to a search shows them at
    once instead of an empty page while YouTube answers again. 10 minutes,
    the last 20 queries.
    */
    struct CachedSearch { std::chrono::steady_clock::time_point at; std::vector<music::ApiResult> results; };
    std::map<std::string, CachedSearch> search_cache;
    // The search the screen is showing (under state_mutex): a slower, older
    // one finishing late must not replace its results.
    std::string search_current;
    std::mutex search_cache_mutex;

    void queue(std::shared_ptr<music::IStreamable> streamable, const bool fresh = true, const bool radio = true);
    void queue(std::shared_ptr<music::IStreamableContainer> container, const bool fresh = true, const bool radio = true, size_t skip = 0);
    void removeAt(uint16_t index);

    void pause();
    void resume();
    void togglePause();

    // Relative seek in the current track, in microseconds (negative = back).
    void seekBy(int64_t delta);

    // Jump to a fraction (0..1) of the current track, e.g. a click on the bar.
    void seekToFraction(double fraction);

    // Volume step in percent, clamped to 0..100.
    void changeVolume(int delta);

    // Like the current track, or remove the like if it was liked here.
    void toggleLikeCurrent();

    // Asks the account whether this track is liked (once per track), so the
    // heart is right whatever the track was started from.
    void fetchLikeStatus(const std::string& video_id);

    // Load a playlist's tracks for the playlist view (state.open_playlist).
    void openPlaylist(const music::PlaylistRef& ref);

    // Fetch a playlist's tracks ahead (it's selected), so opening it is instant.
    void prefetchPlaylist(const music::PlaylistRef& ref);

    // Save / restore the listening session (see the private notes).
    void saveSession();
    bool restoreSession();

    /* Preview
    - 15 s of a track from its middle, on a second mpv, without touching the
      queue; what was playing pauses and comes back where it was
    - A new preview replaces the running one; stopPreview ends it early
    */
    void preview(const music::SongRef& song);
    void stopPreview(bool resume_main = true);
    bool previewing();

    // Resolve a track's stream in the background (it is selected), so
    // starting it is near-instant. One at a time; no-op if already done.
    void warmStream(const music::SongRef& song);
    void warmNow(const std::string& youtube_url);     // no debounce (a playlist's first track)

    // New playlist (with video_id as its first track if set) / add to one.
    void seekTo(double seconds);

    // Queue view: drop everything after the current track (radio included),
    // or save the queue as a new playlist.
    void clearUpcoming();
    void saveQueueAsPlaylist(const std::string& title);
    void createPlaylist(const std::string& title, const std::string& video_id = "");
    void addToPlaylist(const std::string& playlist_id, const std::string& title, const std::string& video_id);
    void removeFromPlaylist(const std::string& playlist_id, const std::string& video_id, const std::string& set_video_id);
    void renamePlaylist(const std::string& playlist_id, const std::string& title);
    void deletePlaylist(const std::string& playlist_id, const std::string& title);

    // Insert a track right after the current one.
    void playNext(std::shared_ptr<music::IStreamable> streamable);

    // Shuffle everything after the current track (queue and mpv together).
    void shuffleUpcoming();

    // Move a queued track (user queue indexes, both after the current one).
    void moveInQueue(size_t from, size_t to);

    // Album page: an album's tracks in the playlist view.
    void openAlbum(const music::AlbumRef& ref);

    // Artist page: top songs, albums, singles, related artists...
    void openArtist(const music::ArtistRef& ref);

    // Play the open playlist starting at `index`, then carry on with the rest.
    void playOpenPlaylistFrom(size_t index);

    void setOnStreamStart(std::function<void()> cb) {
        on_stream_start = std::move(cb);
    }

    void setOnStreamEnd(std::function<void()> cb) {
        on_stream_end = std::move(cb);
    }

    void setOnRequestCompletedCallback(std::function<void()> cb) {
        std::lock_guard lock(callback_mutex);
        on_request_completed = std::move(cb);
    }

    void setOnPositionTick(std::function<void(uint64_t position, uint64_t duration)> cb) {
        on_position_tick = std::move(cb);
    }

    void detachUI();
    bool isInitialized();

private:
    std::string app_name;

    std::condition_variable command_cv;
    std::mutex callback_mutex;
    /* While the starting track resolves (worker thread stays free), mpv's
    playlist is one entry short: commands that touch mpv's playlist by index
    wait in `postponed` until it is loaded. Worker thread only.
    */
    std::atomic<bool> load_pending{false};
    std::atomic<bool> fading_out{false};
    std::atomic<bool> advancing{false};
    std::string latest_prefetch;

    /* Playlist changes made here, applied over what YouTube returns for a
    minute: its library list lags a few seconds behind a create / rename /
    delete, and the sidebar showed the old list (a new playlist "missing").
    */
    struct PendingPlaylistEdit {
        int kind = 0;               // 0 create, 1 rename, 2 delete
        std::string id, title;
        std::chrono::steady_clock::time_point until;
    };
    std::vector<PendingPlaylistEdit> pending_playlist_edits;     // under state_mutex
    void applyPendingPlaylistEdits(std::vector<music::Playlist>& lists);                 // the playlist selected last (older prefetches are dropped)          // a track ended and mpv is opening the next one
    // The playlist the queue was last started from (fresh start), and how many
    // of its tracks were queued: its rest is appended when it arrives.
    std::string queued_from_id;
    size_t queued_from_have = 0;
    std::atomic<uint64_t> queue_epoch{0};  // bumped on every fresh start of the queue
    std::atomic<uint64_t> file_generation{0};    // bumped when a file starts
    std::string refreshed_url;                   // stream-error handler: link already refreshed once
    // Last.fm: the track being listened to (artist, title, album), when, how long.
    std::array<std::string, 3> scrobble_track;
    std::atomic<long long> scrobble_started_at{0};
    std::atomic<int> scrobble_played_s{0};
    std::atomic<bool> scrobble_done{true};         // FADE_SECONDS: the end of a track is fading
    std::atomic<uint64_t> fade_generation{0};    // a newer fade (or a volume change) stops the older
    std::atomic<uint64_t> load_generation{0};
    std::mutex command_mutex;

    using Command = std::variant<
        SearchCommand,
        QueueStreamableRadioCommand,
        QueueStreamableCommand,

        QueueStreamableContainerRadioCommand,
        QueueStreamableContainerCommand,

        QueueNextRadioCommand,

        HomeCommand,
        ExploreCommand,
        LibraryPlaylistsCommand,
        LibraryAlbumsCommand,
        LibrarySongsCommand,
        LibraryArtistsCommand,
        LibraryPodcastsCommand,
        IsLoggedInCommand,
        RateCommand,
        LikeStatusCommand,
        OpenPlaylistCommand,
        RefreshSessionCommand,
        OpenAlbumCommand,
        OpenArtistCommand,
        ShuffleCommand,
        MoveCommand,
        PreloadedCommand,
        LoadCurrentCommand,
        ResolveCurrentCommand,
        PlaylistEditCommand,
        PlaylistRestCommand,
        PrefetchPlaylistCommand,
        PlayNextCommand
    >;
    std::vector<Command> postponed;
    std::unordered_set<std::string> like_status_asked;   // under state_mutex


    std::queue<Command> command_queue;
    void pushUrgent(Command c);              // to the front (command_mutex held)
public:
    // A page of Explore (state.explore): FEmusic_explore, FEmusic_new_releases,
    // FEmusic_charts, FEmusic_moods_and_genres, or a mood (its params, title).
    void getExplore(const std::string& browse_id = "", const std::string& params = "", const std::string& title = "");
private:
    std::atomic<bool> home_queued{false};    // a HomeCommand waits: no second one
    std::atomic<bool> explore_queued{false}; // same for Explore

    std::unique_ptr<MPV> mpv_service;
    std::unique_ptr<Mpris> mpris_service;
    std::unique_ptr<Music> music_service;
    std::unique_ptr<Social> social_service;

    std::thread worker_thread;

    bool initialised = false;
    bool running = true;

    void worker_loop();

    std::string last_notified_id;   // track already announced by notifyTrack

    std::unique_ptr<MPV> preview_mpv;
    std::mutex preview_mutex;
    std::atomic<int> preview_gen{0};
    bool preview_paused_main = false;       // under preview_mutex

    // Playlists already fetched (prefetch or open), by id; under state_mutex.
    std::unordered_map<std::string, std::shared_ptr<music::Playlist>> playlist_cache;       // complete
    std::unordered_map<std::string, std::shared_ptr<music::Playlist>> playlist_first_page;  // long ones: first page

    /* History, session
    - Recently played: last 50 tracks, shown as the first home shelf
    - Session: current track, position and what follows, restored paused
      at launch ("resume where you left off")
    */
    void recordHistory(const music::SongRef& song);
    void mergeHistoryShelf(const nlohmann::json* known = nullptr);   // caller holds state_mutex
    double resume_at = -1;                  // seconds; >= 0 while a restore is pending (state_mutex)
    std::string resume_id;                  // the restored track it applies to

    // Resolve the next track's stream ahead of time (yt-dlp), see preloadNext.
    void preloadNext();

    /* resolveStream
    - yt-dlp for a YouTube URL -> "<direct stream URL>\n<its User-Agent>"
      ("" on failure); MPV::load / insertAt take that form
    - Blocking (~2.5 s): call it off the UI thread
    */
    // priority: the track the user is waiting for (its own resolver, never
    // queued behind preloads).
    std::string resolveStream(const std::string& youtube_url, bool priority = false);
    std::string parseResolved(const std::string& yt_dlp_json);  // "<url>\n<agent>"
    void resolveCurrentAsync(const std::string& youtube_url);   // -> LoadCurrentCommand
    std::string warm_latest;                // last song asked by warmStream

    // Pre-resolved stream URLs by YouTube URL, for tracks not in mpv yet
    // (autoplay radio); taken by streamUrlFor().
    std::unordered_map<std::string, std::string> preloaded;
    std::unordered_map<std::string, std::string> recent_streams;   // YouTube URL -> resolved, kept (saved at exit)
    void rememberStream(const std::string& url, const std::string& stream);
    void forgetStream(const std::string& url);
    std::unordered_set<std::string> preloading;             // in flight
    std::mutex preloaded_mutex;
    std::string streamUrlFor(const std::string& url);

    /* updateMprisControls
    - Updates the mpris buttons (activated / deactivated) based on queue position.
    */
    void updateMprisControls();

    /* update...Data
    - Updates the album / thumbnail / other song data to external services.
    */
    void updateMprisData();
    void updateSocialData();

    void resetMprisData();
    void removeFromUserQueue(uint16_t index);
    void removeFromRadioQueue(uint16_t index);

    std::function<void()> on_stream_start;
    std::function<void()> on_stream_end;

    std::function<void()> on_request_completed;
    std::function<void(uint64_t position, uint64_t duration)> on_position_tick;

    std::mutex position_tick_thread_mutex;    // guards position_tick_thread itself
    std::thread position_tick_thread;
    std::mutex position_tick_mutex;
    std::condition_variable position_tick_cv;
    bool position_tick_running = false;

    void startPositionTick();
    void stopPositionTick();

    void applyLoop(LoopMode mode, bool notify_mpris);
};

}

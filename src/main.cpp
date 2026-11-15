#include <cctype>
#include "../include/ui/cover_cache.hpp"
#include "../include/ui/icons.hpp"
#include <ftxui/screen/terminal.hpp>
#include "../include/ui/viewport.hpp"
#include "../include/ui/see_through.hpp"
#include "../include/ui/anim.hpp"
#include "../include/ui/fade.hpp"
#include "../include/services/suggest.hpp"
#include "../include/services/scrobble.hpp"
#include "../include/services/update.hpp"
#include "../include/services/cli_ui.hpp"
#include "../include/utils/platform.hpp"
#include "../include/ui/components/playback_bar.hpp"
#include "../include/ui/components/now_playing.hpp"
#include "../include/ui/components/search_bar.hpp"
#include "../include/ui/components/sidebar.hpp"
#include "../include/ui/components/content.hpp"
#include "../include/ui/constants/states.hpp"
#include "../include/ui/constants/colors.hpp"

#include "../include/services/player.hpp"
#include "../include/services/notify.hpp"
#include "../include/config/config.hpp"
#include "../include/setup/setup.hpp"
#include "../include/utils/utils.hpp"

#include "image_view.hpp"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <curl/curl.h>
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <optional>
#include <nlohmann/json.hpp>
#include <fstream>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <cstdint>
#include <string>
#include <memory>
#include <mutex>

#define APP_NAME_HUMAN "Moroder"
#define APP_NAME "moroder"

namespace fs = std::filesystem;

using namespace ftxui;

namespace {

/* FocusGate
- Lets a component take focus only while *open is set, so arrow keys never
  wander into it: the search bar is entered on purpose (f, /, a click) and left
  with Enter or Esc, which keeps single-key shortcuts like q working everywhere
- A click on the child opens the gate for it
*/
class FocusGate : public ComponentBase {
public:
    FocusGate(Component child, bool* open) : child_(child), open_(open) { Add(std::move(child)); }

    bool Focusable() const override { return *open_ && child_->Focusable(); }

    bool OnEvent(Event event) override {
        if (event.is_mouse() && event.mouse().button == Mouse::Left && event.mouse().motion == Mouse::Pressed && !*open_) {
            *open_ = true;
            if (ChildAt(0)->OnEvent(event)) return true;
            *open_ = false;
            return false;
        }
        return ChildAt(0)->OnEvent(event);
    }

private:
    Component child_;
    bool* open_;
};

}

/* moroder keys
- Shows what the terminal sends for each key, and what moroder makes of it:
  Shift / Option + arrows differ between Terminal.app, iTerm2, Ghostty...
*/
static int keyTest() {
    platform::RawInput raw;
    if (!raw.ok()) { std::cerr << "not a terminal\n"; return 1; }

    // Mouse reporting too (any event, SGR), so wheel / trackpad swipes show up.
    std::cout << "\x1b[?1003h\x1b[?1006h";
    cli::title("moroder keys", "what your terminal sends");
    std::cout << "     Press keys (Shift/Option + arrows, h, m...), scroll or swipe. " << cli::bold() << "q" << cli::reset() << " quits.\n"
              << "     " << cli::dim() << "Also written to ~/.local/state/moroder/logs/keys.log" << cli::reset() << "\n\n" << std::flush;
    std::ofstream log(utils::resolve_path(MORODER_LOG_PATH) / "keys.log", std::ios::trunc);
    const std::vector<std::pair<std::string, std::string>> known = {
        {"\x1b[1;2D", "previous track (Shift+Left)"},  {"\x1b[1;3D", "previous track (Option+Left)"},
        {"\x1b[1;9D", "previous track (Meta+Left)"},   {"\x1b" "b",  "previous track (Option+Left, as Esc b)"},
        {"\x1b[1;2C", "next track (Shift+Right)"},     {"\x1b[1;3C", "next track (Option+Right)"},
        {"\x1b[1;9C", "next track (Meta+Right)"},      {"\x1b" "f",  "next track (Option+Right, as Esc f)"},
        {"\x1b[1;2A", "volume up (Shift+Up)"},         {"\x1b[1;3A", "volume up (Option+Up)"},
        {"\x1b[1;2B", "volume down (Shift+Down)"},     {"\x1b[1;3B", "volume down (Option+Down)"},
        {"<", "previous track"}, {">", "next track"},
        {"\x1b[D", "left (moves around)"}, {"\x1b[C", "right (moves around)"},
        {"\x1b[A", "up (moves around)"},   {"\x1b[B", "down (moves around)"},
    };
    while (true) {
        char buf[32];
        const int n = raw.read(buf, sizeof(buf), 1000);
        if (n < 0) break;
        if (n == 0) continue;
        // One read can hold several keys (pasted, or fast): split them.
        const std::string all(buf, static_cast<size_t>(n));
        std::vector<std::string> keys;
        for (size_t i = 0; i < all.size(); ) {
            size_t j = i + 1;
            if (all[i] == '\x1b' && j < all.size()) {
                if (all[j] == '[' || all[j] == 'O') {
                    j++;
                    while (j < all.size() && !(all[j] >= 0x40 && all[j] <= 0x7e)) j++;
                    j = std::min(all.size(), j + 1);
                } else {
                    j++;
                }
            }
            keys.push_back(all.substr(i, j - i));
            i = j;
        }
        bool quit = false;
        for (const std::string& seq : keys) {
        if (seq == "q") { quit = true; break; }
        std::string shown;
        for (unsigned char ch : seq) {
            if (ch == 0x1b) shown += "ESC ";
            else if (ch < 0x20 || ch == 0x7f) { char h[8]; std::snprintf(h, sizeof(h), "0x%02x ", ch); shown += h; }
            else shown += static_cast<char>(ch);
        }
        std::string meaning = "-";
        for (const auto& [k, v] : known) if (k == seq) meaning = v;
        // SGR mouse: ESC [ < button ; x ; y M/m
        if (seq.rfind("\x1b[<", 0) == 0) {
            const int b = std::atoi(seq.c_str() + 3);
            const int base = b & ~(4 | 8 | 16);
            meaning = base == 64 ? "wheel up" : base == 65 ? "wheel down" : base == 66 ? "wheel LEFT (swipe)" : base == 67 ? "wheel RIGHT (swipe)"
                    : (b & 32) ? "mouse move" : "mouse button " + std::to_string(base & 3);
            if (b & 4) meaning += " +shift";
            if (b & 8) meaning += " +option";
            if (meaning == "mouse move") { log << shown << " move\n" << std::flush; continue; }   // too many to print
        }
        log << shown << "   ->  " << meaning << "\n" << std::flush;
        {
            std::string key = shown;
            while (key.size() < 18) key += ' ';
            std::cout << "  " << cli::dim() << key << cli::reset() << " " << cli::red() << "→" << cli::reset() << "  " << meaning << "\r\n" << std::flush;
        }
        }
        if (quit) break;
    }
    std::cout << "\x1b[?1003l\x1b[?1006l" << std::flush;
    return 0;
}

/* detectSyncOutput
- Synchronized output (DEC mode 2026) makes the terminal show each frame only
  once it has all of it: no half-drawn, flickering frames (see
  cmake/patch_ftxui.cmake). Used only where the terminal says it has the mode,
  so a terminal without it, or with a shaky one, draws as it always did
- Asked with DECRQM ("CSI ? 2026 $ p"); the reply is "CSI ? 2026 ; Ps $ y",
  Ps 1-3 meaning known. DA1 ("CSI c") follows as a fence: every terminal
  answers it, so its reply means no answer about 2026 is coming
- MORODER_SYNC_OUTPUT=0 or 1 in the environment decides instead
*/
static void detectSyncOutput() {
    if (std::getenv("MORODER_SYNC_OUTPUT")) return;
    if (!platform::isTerminal(0) || !platform::isTerminal(1)) return;

    // Warp answers that it has the mode, but then held whole frames back:
    // the page stayed blank for seconds after going back.
    if (const char* term = std::getenv("TERM_PROGRAM"); term && std::string(term) == "WarpTerminal") {
        spdlog::info("TERMINAL: synchronized output off (Warp)");
        return;
    }

    platform::RawInput raw;
    if (!raw.ok()) return;

    std::cout << std::flush;
    const bool sent = platform::writeTerminal("\033[?2026$p\033[c");

    // The DA1 reply: ESC [ ? digits/semicolons c
    auto fenced = [](const std::string& s) {
        for (size_t at = s.find("\033[?"); at != std::string::npos; at = s.find("\033[?", at + 1)) {
            size_t i = at + 3;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == ';')) i++;
            if (i < s.size() && s[i] == 'c') return true;
        }
        return false;
    };

    std::string got;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (sent && std::chrono::steady_clock::now() < deadline && !fenced(got)) {
        char buf[256];
        const int n = raw.read(buf, sizeof buf, 20);
        if (n < 0) break;
        if (n > 0) got.append(buf, static_cast<size_t>(n));
    }

    const size_t at = got.find("\033[?2026;");
    const char ps = at != std::string::npos && at + 8 < got.size() ? got[at + 8] : '0';
    const bool supported = ps == '1' || ps == '2' || ps == '3';
    if (supported) platform::setEnv("MORODER_SYNC_OUTPUT", "1");
    spdlog::info("TERMINAL: synchronized output {}", supported ? "supported, used" : "not reported, off");
}

int main(int argc, char *argv[]) {
    platform::initConsole();
#ifdef _WIN32
    /* FTXUI's Windows defaults are the old console's: 16 colours when the
    console refuses one of the modes it asks for (covers come out as grey
    noise), ASCII widgets, no block characters. Windows 10 / 11 consoles and
    Windows Terminal do true colour and Unicode.
    */
    if (platform::isTerminal(1)) {
        auto quirks = ftxui::Terminal::GetQuirks();
        quirks.SetColorSupport(ftxui::Terminal::Color::TrueColor);
        quirks.SetBlockCharacters(true);
        quirks.SetComponentAscii(false);
        quirks.SetCursorHiding(true);
        ftxui::Terminal::SetQuirks(quirks);
    }
#endif
    // Once, before any thread touches libcurl (doctor's checks run side by side).
    curl_global_init(CURL_GLOBAL_DEFAULT);
    Config::app_name = APP_NAME;

    utils::ensure_dir(utils::resolve_path(MORODER_LOG_PATH));

    auto logger = spdlog::basic_logger_mt(APP_NAME, utils::resolve_path(MORODER_LOG_PATH).string() + "/" + APP_NAME + ".log", true);
    logger->flush_on(spdlog::level::info);

    spdlog::set_default_logger(logger);

    /* Subcommands run before anything else is built.
    - setup has to work on a machine with no config and no session, so it must
      not go through Player, which needs both
    */
    /* Arguments: -a / --anonymous anywhere, then a command and its words
    ("moroder -a search daft punk").
    */
    bool anonymous_flag = false;
    std::vector<std::string> words;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "-a" || a == "--anonymous") anonymous_flag = true;
        else words.push_back(a);
    }
    const std::string command = words.empty() ? (anonymous_flag ? "--anonymous" : "") : words[0];
    std::string start_query;      // moroder search <words>: opens on those results
    if (command == "search") {
        for (size_t i = 1; i < words.size(); i++) start_query += (start_query.empty() ? "" : " ") + words[i];
    }

    if (command == "setup") {
        return setup::run(argc, argv);
    }
    if (command == "keys") {
        return keyTest();
    }
    if (command == "lastfm") {
        return services::scrobble::connect();
    }
    if (command == "--version" || command == "-v" || command == "version") {
        std::cout << "moroder " << services::update::version() << "\n";
        return 0;
    }
    if (command == "update")    return services::update::runUpdate();
    if (command == "doctor")    return services::update::doctor();
    if (command == "uninstall") return services::update::uninstall();

    /* Anonymous mode
    - Everything except the account works without a session: search, radio and
      playback all go out unauthenticated
    - The library and the home page have nothing to fill themselves with, and
      every request is slower, so this is opt-in rather than a silent fallback
    */
    const bool anonymous = anonymous_flag;
    Config::anonymous = anonymous;

    if (command == "--help" || command == "-h") {
        using namespace cli;
        title("Moroder", std::string("YouTube Music in your terminal, ") + services::update::version());
        std::cout << "  " << bold() << "moroder" << reset() << dim() << "  starts the player" << reset() << "\n\n";
        auto cmd = [](const std::string& c, const std::string& what) {
            std::string pad = c;
            while (pad.size() < 18) pad += ' ';
            std::cout << "    " << pad << dim() << what << reset() << "\n";
        };
        cmd("setup", "sign in with your browser, once");
        cmd("-a, --anonymous", "play without an account");
        cmd("search WORDS", "open on the results for WORDS");
        std::cout << "\n";
        cmd("update", "install the latest release (and update yt-dlp)");
        cmd("doctor", "check yt-dlp, mpv, sign-in, network, terminal");
        cmd("uninstall", "remove it (asks before deleting your data)");
        std::cout << "\n";
        cmd("keys", "show what your terminal sends for each key");
        cmd("lastfm", "connect Last.fm scrobbling");
        cmd("--version", "print the version");
        std::cout << "\n  " << dim() << "Sign-in help: https://github.com/" MORODER_REPO "/blob/main/docs/sign-in.md" << reset() << "\n\n";
        return 0;
    }

    if (!command.empty() && command != "--anonymous" && command != "search") {
        std::cerr << "\n  " << cli::red() << "✗" << cli::reset() << "  unknown command: " << cli::bold() << command << cli::reset() << "\n"
                  << "     " << cli::dim() << "moroder --help lists them" << cli::reset() << "\n\n";
        return 2;
    }

    // A newer release: offer it before the interface starts, then restart into it.
    services::update::tidyLaunchers();
    if (services::update::offerAtLaunch()) {
        std::cout << "\n  " << cli::bold() << cli::red() << "▶" << cli::reset() << " Starting the new version\n\n";
        platform::setEnv("MORODER_JUST_UPDATED", "1");     // no second offer in the restarted one
        platform::restart(argv);
    }

    /* A missing browser.json means the user has not run setup, and every
    library request would fail with an auth error they cannot act on. Say so
    once, here, rather than letting the library page explain it later.
    */
    if (!anonymous && !fs::exists(Config::get().YTM_COOKIES_PATH / "browser.json")) {
        std::cerr << "\n  " << cli::yellow() << "!" << cli::reset() << "  " << cli::bold() << "Not signed in yet" << cli::reset() << "\n\n"
                  << "     " << cli::bold() << "moroder setup" << cli::reset() << "   " << cli::dim() << "sign in with your browser, once" << cli::reset() << "\n"
                  << "     " << cli::bold() << "moroder -a" << cli::reset() << "      " << cli::dim() << "play without an account" << cli::reset() << "\n\n";
        return 1;
    }

    if (anonymous) {
        spdlog::info("MAIN: starting without an account");
    }

    services::Player player(APP_NAME, APP_NAME_HUMAN, 1481401025964540125);
    if(!player.isInitialized()) {
        return 1;
    }

    detectSyncOutput();
    // The console window's fonts have no quadrants or partial blocks: covers
    // drawn with half blocks only (read by the image view, see icons.hpp).
    if (ui::consoleGlyphs()) platform::setEnv("MORODER_COVER_BLOCKS", "half");
#ifdef _WIN32
    spdlog::info("TERMINAL: console font \"{}\", icons {}, {}", platform::consoleFontName(),
                 ui::plainIcons() ? "plain" : "nerd", ui::consoleGlyphs() ? "console glyphs" : "full glyphs");
#endif
    auto screen = ScreenInteractive::Fullscreen();

    // Mouse: click items and controls, wheel to scroll (see the handler below).
    screen.TrackMouse(true);

    player.setOnRequestCompletedCallback([&screen]() {
        screen.PostEvent(Event::Custom);
    });

    /* Cover loads land one by one, dozens at a time on a big page; a redraw
    each would redraw the whole screen dozens of times a second. Coalesce
    them into at most one redraw every 80 ms.
    */
    static std::atomic<bool> image_redraw_pending{false};
    ftxui::setOnImageLoadedCallback([] { image_redraw_pending = true; });
    std::thread([&screen] {
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            if (image_redraw_pending.exchange(false)) screen.PostEvent(Event::Custom);
        }
    }).detach();

    const Config& cfg = Config::get();

    ftxui::setImageCacheMaxSize(cfg.MAX_IMAGE_CACHE_SIZE);
    ftxui::setImageResizeCacheMaxSize(cfg.MAX_RESIZED_IMAGE_CACHE_SIZE);
    ftxui::setImageCharCacheMaxSize(cfg.MAX_IMAGE_CHAR_CACHE_SIZE);

    // Caps concurrent background image-loader threads (default is 6).
    // With 40+ thumbnails potentially uncached at once (grid + carousels +
    // queue + playback bar), leave this in place rather than letting every
    // uncached image spawn its own thread simultaneously.
    ftxui::setMaxConcurrentImageLoads(cfg.MAX_CONCURRENT_IMAGE_LOADS);



    int spinner_frame = 0;
    static double ctx_bar = -1; // the context menu's red bar, gliding to the row picked (-1: closed)
    // Inertia: a fast sideways swipe on a row coasts on, slowing down, after its last event.
    struct { std::string row; int dir = 0, burst = 0; double speed = 0; std::chrono::steady_clock::time_point last{}; } glide;

    // ~12 fps redraws while `animating` (spinners), none otherwise.
    static std::atomic<bool> animating{false};
    static std::atomic<int> anim_period_ms{80};     // spinners 80 ms, lyrics 250 ms
    static std::atomic<bool> help_held{false};      // h held down: cheatsheet shown while it repeats
    // Sleep timer: pause at a time (ms since epoch, 0: off), or at the end of the track.
    static std::atomic<long long> sleep_at_ms{0};
    static std::atomic<bool> sleep_end_of_track{false};
    static std::chrono::steady_clock::time_point last_h{};
    std::thread([&screen] {
        auto last = std::chrono::steady_clock::now();
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ui::anim::wanted ? 11 : 40));
            const auto now = std::chrono::steady_clock::now();
            // Something in motion (a scroll, a fade): ~30 fps until it settles.
            const int period = ui::anim::wanted ? 33 : anim_period_ms.load();
            if ((animating || ui::anim::wanted) && now - last >= std::chrono::milliseconds(period)) {
                last = now;
                screen.PostEvent(Event::Custom);
            }
        }
    }).detach();
    int sidebar_selected = 0;

    bool sidebar_hidden  = false;
    bool sidebar_focused = false;
    bool audio_playing = false;
    bool has_track = false;      // something loaded (playing or paused)

    auto current_state = ui::State::HOME;

    ui::SearchBarData search_bar_data = {};
    auto search_bar = ui::SearchBar(&search_bar_data);

    // Search is entered on purpose and left after Enter / Esc (see FocusGate).
    bool search_active = false;
    Box sidebar_box;            // where the sidebar is drawn, for the wheel
    int content_column = 1;     // 0 = search bar, 1 = page content
    auto search_gate = Make<FocusGate>(search_bar, &search_active);

    ui::PlaybackData playback_bar_data = {};
    static std::chrono::steady_clock::time_point last_tick_at{};   // when the position last ticked
    ui::State before_now_playing = ui::State::HOME;
    ui::State before_queue = ui::State::HOME;    // where the queue view was opened from

    /* Page history, like a browser's: Esc / Backspace / [ go back, ] forward,
    and a two-finger swipe sideways on the trackpad does the same. Pages are
    recorded as they are reached, whatever led there (click, key, menu).
    */
    struct NavEntry {
        ui::State state = ui::State::HOME;
        music::PlaylistRef playlist;     // PLAYLIST: the list or album shown
        std::string kind;                // "playlist" / "album"
        music::ArtistRef artist;         // ARTIST
        std::string query;               // SEARCH
        std::string filter;              // SEARCH: its tab ("" all, "albums"...)
        // Where the page was when left (scroll, selected row): back / forward
        // land there, not at the top.
        ui::ViewportState view;
        int active = -1;
        std::string key() const {
            return std::to_string(static_cast<int>(state)) + "|" + (state == ui::State::PLAYLIST ? kind + playlist.id : "")
                 + (state == ui::State::ARTIST ? artist.id : "") + (state == ui::State::SEARCH ? query + "\x1f" + filter : "");
        }
        std::string label() const {
            switch (state) {
                case ui::State::HOME: return "Home";
                case ui::State::LIBRARY: return "Library";
                case ui::State::EXPLORE: return "Explore";
                case ui::State::PLAYLIST: return playlist.title.empty() ? "Playlist" : playlist.title;
                case ui::State::ARTIST: return artist.name.empty() ? "Artist" : artist.name;
                case ui::State::SEARCH: return "Search: " + query;
                case ui::State::QUEUE: return "Queue";
                case ui::State::NOWPLAYING: return "Now playing";
                default: return "";
            }
        }
    };
    std::vector<NavEntry> nav_back, nav_forward;
    Box nav_back_box{-1, -1, -1, -1}, nav_forward_box{-1, -1, -1, -1};   // the ‹ › buttons
    NavEntry nav_current;
    bool nav_restoring = false;
    std::string last_query;
    auto playback_bar = ui::PlaybackBar(&playback_bar_data);

    /* These run on player / mpv threads while the UI thread reads
    playback_bar_data every frame: hand the writes to the UI thread
    (screen.Post) instead of racing it.
    */
    player.setOnPositionTick([&screen, &playback_bar_data, &player](uint64_t position, uint64_t duration) {
        // Sleep timer: pause when the time is up, or as this track ends.
        const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (sleep_at_ms > 0 && now_ms >= sleep_at_ms) {
            sleep_at_ms = 0;
            std::thread([&player] {
                player.pause();
                std::lock_guard lock(player.state_mutex);
                player.state.notice = "Sleep timer: paused";
                player.state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            }).detach();
        }
        if (sleep_end_of_track && duration > 0 && position + 1300000ULL >= duration) {
            sleep_end_of_track = false;
            const auto left = std::chrono::microseconds(duration > position + 150000ULL ? duration - position - 150000ULL : 0);
            std::thread([&player, left] {
                std::this_thread::sleep_for(left);
                player.pause();
            }).detach();
        }

        int progress = -1;
        // Remove two seconds to account for UI refresh period, so the bar
        // reaches the end. duration is unsigned: guard before subtracting.
        if (duration > services::Player::LOOP_REWIND_EPSILON) {
            const uint64_t d = duration - services::Player::LOOP_REWIND_EPSILON;
            progress = std::clamp(static_cast<int>((position * 100) / d), 0, 100);
        }

        screen.Post([&playback_bar_data, position, duration, progress] {
            last_tick_at = std::chrono::steady_clock::now();
            playback_bar_data.position = position / 1000000ULL;
            playback_bar_data.position_exact = static_cast<double>(position) / 1e6;
            playback_bar_data.tick_at = last_tick_at;
            playback_bar_data.duration = duration / 1000000ULL;
            if (progress >= 0) playback_bar_data.progress = progress;
        });
        screen.PostEvent(Event::Custom);
    });

    auto reset_bar = [&screen, &playback_bar_data] {
        screen.Post([&playback_bar_data] {
            playback_bar_data.progress = 0;
            playback_bar_data.position = 0;
            playback_bar_data.position_exact = 0;
            playback_bar_data.duration = 0;
        });
        screen.PostEvent(Event::Custom);
    };
    player.setOnStreamStart([reset_bar, &player] {
        reset_bar();
        // Sleep timer "end of track" missed its last tick: stop at the next one.
        if (sleep_end_of_track.exchange(false)) std::thread([&player] { player.pause(); }).detach();
    });
    player.setOnStreamEnd(reset_bar);

    /* A cover finished downloading: redraw, at most every 60 ms (a page of
    covers lands within a few hundred ms).
    */
    ui::onCoverReady([&screen] {
        static std::atomic<bool> scheduled{false};
        if (scheduled.exchange(true)) return;
        std::thread([&screen] {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            scheduled = false;
            screen.PostEvent(Event::Custom);
        }).detach();
    });

    // Resume where the last session stopped (paused, same track and position);
    // after the callbacks above are set, which the restored track will fire.
    player.restoreSession();

    // After the resume: the worker runs commands in order, and these are
    // network calls that held the resumed track back by seconds.
    player.isLoggedIn();
    player.getHome();
    player.getLibraryPlaylists();
    player.getLibraryAlbums();
    player.getLibraryArtists();
    player.getLibraryPodcasts();
    player.getLibrarySongs();

    std::deque<ui::ContentEntry> main_content_items;
    auto main_content = Container::Vertical({ Renderer([]{ return emptyElement(); }) });

    std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_item_press =
    [&current_state, &player, &main_content_items, &main_content, &screen, &cfg](const ftxui::Event& event, const music::ApiResult& result) {
        if(!Config::isKey(event, cfg.KEY_ADD_TO_QUEUE) && !Config::isKey(event, cfg.KEY_PLAY_NOW)) {
            return false;
        }

        spdlog::info("ITEM: key pressed on result, " + result.resultType);
        bool should_queue = Config::isKey(event, cfg.KEY_ADD_TO_QUEUE);

        bool queued = false;
        bool opened_playlist = false;
        bool opened_artist = false;
        bool opened_mood = false;

        std::visit([&](auto&& data) {
            using T = std::decay_t<decltype(data)>;

            auto make_streamable = [&](auto&& obj) {
                obj.setRef(data);
                return std::make_shared<std::decay_t<decltype(obj)>>(std::move(obj));
            };

            if constexpr (std::is_same_v<T, music::SongRef>) {
                auto streamable = make_streamable(music::Song{});
                player.queue(streamable, !should_queue, !should_queue);
                queued = true;

            } else if constexpr (std::is_same_v<T, music::AlbumRef>) {
                if (should_queue) {
                    auto container = make_streamable(music::Album{});
                    player.queue(container, false, false);
                    queued = true;
                } else {
                    // Enter / double-click opens the album page.
                    player.openAlbum(data);
                    opened_playlist = true;
                }

            } else if constexpr (std::is_same_v<T, music::ArtistRef>) {
                if (!should_queue && !data.id.empty()) {
                    player.openArtist(data);
                    opened_artist = true;
                }

            } else if constexpr (std::is_same_v<T, music::EpisodeRef>) {
                auto streamable = make_streamable(music::Episode{});
                player.queue(streamable, !should_queue, !should_queue);
                queued = true;

            } else if constexpr (std::is_same_v<T, music::PlaylistRef>) {
                // A mood / genre (Explore): its page of playlists, in Explore.
                if (data.id.rfind("mood:", 0) == 0) {
                    if (!should_queue) {
                        player.getExplore("FEmusic_moods_and_genres_category", data.id.substr(5), data.title);
                        opened_mood = current_state != ui::State::EXPLORE;     // from Home, say: go there
                    }
                    return;
                }
                if (should_queue) {
                    auto container = make_streamable(music::Playlist{});
                    player.queue(container, false, false);
                    queued = true;
                } else {
                    // Like the sidebar: Enter / double-click opens the playlist.
                    player.openPlaylist(data);
                    opened_playlist = true;
                }
            }

        }, result.data);

        if (opened_mood) {
            current_state = ui::State::EXPLORE;
            main_content_items.clear();
            main_content->DetachAllChildren();
            main_content->Add(Renderer([] { return emptyElement(); }));
            screen.PostEvent(Event::Custom);
            return true;
        }
        if (opened_playlist || opened_artist) {
            current_state = opened_artist ? ui::State::ARTIST : ui::State::PLAYLIST;
            main_content_items.clear();
            main_content->DetachAllChildren();
            main_content->Add(Renderer([] { return emptyElement(); }));
            screen.PostEvent(Event::Custom);
            return true;
        }

        if (!queued) spdlog::info("ITEM: nothing queued for result type, " + result.resultType);

        // Playing or queueing keeps you where you are (the queue is one key
        // away): no jump to the queue view and its big cover.
        screen.PostEvent(Event::Custom);
        return true;
    };

    std::function<bool(const ftxui::Event&, const music::Playlist&)> on_sidebar_press =
    [&current_state, &player, &main_content_items, &main_content, &screen, &cfg](const ftxui::Event& event, const music::Playlist& playlist) {
        if(!Config::isKey(event, cfg.KEY_ADD_TO_QUEUE) && !Config::isKey(event, cfg.KEY_PLAY_NOW)) {
            return false;
        }

        spdlog::info("SIDEBAR: key pressed on playlist, " + playlist.ref.title);

        // d queues the whole playlist and stays put.
        if (Config::isKey(event, cfg.KEY_ADD_TO_QUEUE)) {
            auto container = std::make_shared<music::Playlist>(playlist);
            player.queue(container, false, false);
            return true;
        }

        // Enter / click opens it, like any music app: its tracks, Play, etc.
        player.openPlaylist(playlist.ref);
        current_state = ui::State::PLAYLIST;

        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));

        screen.PostEvent(Event::Custom);

        return true;
    };

    std::function<bool(const ftxui::Event&, const int)> on_queue_press =
    [&current_state, &player, &main_content_items, &main_content, &screen, &cfg](const ftxui::Event& event, const int queue_index) {
        // K / J reorder the queue (user queue rows only, after the current track).
        if(Config::isKey(event, cfg.KEY_MOVE_UP) || Config::isKey(event, cfg.KEY_MOVE_DOWN)) {
            const int to = queue_index + (Config::isKey(event, cfg.KEY_MOVE_UP) ? -1 : 1);
            player.moveInQueue(static_cast<size_t>(queue_index), static_cast<size_t>(std::max(0, to)));
            ui::queue_focus_index = std::max(0, to);     // the moved row stays selected
            return true;
        }

        if(!Config::isKey(event, cfg.KEY_REMOVE_FROM_QUEUE) && !Config::isKey(event, cfg.KEY_PLAY_NOW)) {
            return false;
        }

        spdlog::info("QUEUE: key pressed on item, " + std::to_string(queue_index));
        bool should_remove = Config::isKey(event, cfg.KEY_REMOVE_FROM_QUEUE);

        if(!should_remove) {
            player.skipTo(queue_index);

        } else {
            player.removeAt(queue_index);
        }

        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));

        screen.PostEvent(Event::Custom);

        return true;
    };

    std::function<bool(const ftxui::Event&, const ui::ChipEntry&)> on_chip_press =
    [&main_content_items, &screen, &cfg](const ftxui::Event& event, const ui::ChipEntry& chip) {
        if(!Config::isKey(event, cfg.KEY_PLAY_NOW)) {
            return false;
        }

        spdlog::info("CHIPS: pressed chip, " + chip.id + " (" + chip.label + ")");

        for (auto& entry : main_content_items) {
            if (entry.category == "__library_filters__") {
                ui::toggleChip(&entry.chips_data, chip.id);
                break;
            }
        }

        screen.PostEvent(Event::Custom);
        return true;
    };

    ui::SidebarData sidebar_data = {};
    auto sidebar = ui::Sidebar(&sidebar_data, &sidebar_selected, &sidebar_focused, on_sidebar_press);

    // The page, kept (focus and scroll included) but out of reach of keys and
    // clicks while Now Playing is over it.
    auto main_content_gate = Maybe(main_content, [&current_state] { return current_state != ui::State::NOWPLAYING; });

    auto sidebar_container = Container::Horizontal({
        sidebar
    });

    auto main_content_container = Container::Horizontal({
        sidebar_container,
        Renderer([] {
            return vbox({
                separator() | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)),
                text(" ")
            });
        }),
        Container::Vertical({
            search_gate,
            main_content_gate
        }, &content_column),
    });

    auto layout = Container::Vertical({
        main_content_container,
        playback_bar
    });

    // Opening search starts from an empty bar: typing used to append to the
    // last query ("veridis quoaround the world"). Up brings past ones back.
    auto open_search = [&] {
        search_active = true;
        content_column = 0;
        search_bar_data.value.clear();
        search_bar_data.cursor = 0;
        search_bar_data.history_index = -1;
        search_bar->TakeFocus();
    };

    /* Suggestions while typing: past searches that match first, then YouTube
    Music's, fetched 180 ms after the last keystroke (one at a time).
    */
    search_bar_data.onTyped = [&screen, &search_bar_data](const std::string& text) {
        static std::atomic<uint64_t> generation{0};
        const uint64_t gen = ++generation;
        if (text.empty()) { search_bar_data.suggestions.clear(); return; }

        std::vector<std::string> mine;
        std::string lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        for (const auto& h : search_bar_data.history) {
            std::string hl = h;
            std::transform(hl.begin(), hl.end(), hl.begin(), ::tolower);
            if (hl.rfind(lower, 0) == 0 && hl != lower) mine.push_back(h);
            if (mine.size() == 3) break;
        }

        std::thread([&screen, &search_bar_data, text, gen, mine] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            if (generation != gen) return;
            std::vector<std::string> list = mine;
            for (auto& s : services::searchSuggestions(text, [gen] { return generation != gen; })) {
                if (list.size() >= 8) break;
                if (std::find(list.begin(), list.end(), s) == list.end()) list.push_back(s);
            }
            if (generation != gen) return;
            screen.Post([&search_bar_data, text, list] {
                if (search_bar_data.value != text) return;
                search_bar_data.suggestions = list;
                search_bar_data.suggestions_for = text;
                search_bar_data.suggestion_index = -1;
            });
            screen.PostEvent(Event::Custom);
        }).detach();
    };

    // Back to the page; if it has nothing focusable yet (results loading),
    // focus lands on it as soon as it does.
    auto close_search = [&] {
        search_bar_data.suggestions.clear();
        search_active = false;
        content_column = 1;
        if (main_content->Focusable()) main_content->TakeFocus();
    };

    // Home / Library again while on it: fetched afresh (the page stays up
    // until the new one lands; a spinner beside it in the sidebar meanwhile).
    sidebar_data.onHome = [&current_state, &main_content_items, &main_content, &screen, &player] {
        spdlog::info("SIDEBAR: Home pressed");

        if (current_state == ui::State::HOME) player.getHome();     // its spinner in the sidebar says so
        current_state = ui::State::HOME;

        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));

        screen.PostEvent(Event::Custom);
    };

    /* Prompt: a one-line text box over the page (new playlist name) */
    struct Prompt {
        bool open = false;
        std::string title, value;
        std::function<void(const std::string&)> done;
    } prompt;

    sidebar_data.onThird = [&player, &prompt] {
        bool logged_in;
        {
            std::lock_guard lock(player.state_mutex);
            logged_in = player.state.is_logged_in;
        }
        if (!logged_in) {
            std::lock_guard lock(player.state_mutex);
            player.state.notice = "Sign in: log in to music.youtube.com in your browser, then restart";
            player.state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
            return;
        }
        prompt.open = true;
        prompt.title = "New playlist";
        prompt.value.clear();
        prompt.done = [&player](const std::string& name) { player.createPlaylist(name); };
    };

    sidebar_data.onExplore = [&current_state, &main_content_items, &main_content, &screen, &player] {
        spdlog::info("SIDEBAR: Explore pressed");
        // First visit, or again while on it: (re)load; otherwise what was fetched.
        bool have;
        {
            std::lock_guard lock(player.state_mutex);
            have = !player.state.explore_order.empty();
        }
        if (!have || current_state == ui::State::EXPLORE) player.getExplore();
        current_state = ui::State::EXPLORE;
        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));
        screen.PostEvent(Event::Custom);
    };

    sidebar_data.onLibrary = [&current_state, &main_content_items, &main_content, &screen, &player] {
        spdlog::info("SIDEBAR: Library pressed");
        if (current_state == ui::State::LIBRARY) {
            bool loading;     // already reloading: not a second time
            {
                std::lock_guard lock(player.state_mutex);
                auto it = player.state.flags.find("library_playlists");
                loading = it != player.state.flags.end() && it->second == services::Player::Flags::Ongoing;
            }
            if (!loading) player.retryLibrary();
        }
        current_state = ui::State::LIBRARY;

        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));

        screen.PostEvent(Event::Custom);
    };

    // Search history survives restarts (~/.cache/moroder/searches.json).
    /* Where each tab was left (Home, Explore, Library): scroll and
    selection, back when you return to it from the sidebar, and at the next
    launch (~/.cache/moroder/views.json). */
    struct TabView { int anchor = 0, line = 0, active = -1; std::string sub; };   // sub: which of the tab's pages (Charts, a Library filter)
    std::map<int, TabView> tab_views;
    const fs::path views_file = [] {
        const fs::path h = utils::home();
        return h.empty() ? fs::path() : h / ".cache" / "moroder" / "views.json";
    }();
    {
        std::ifstream in(views_file);
        nlohmann::json j = in.is_open() ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
        if (j.is_object()) for (auto it = j.begin(); it != j.end(); ++it) {
            const auto& v = it.value();
            if (!v.is_array() || v.size() != 4 || !v[0].is_number_integer() || !v[1].is_number_integer() || !v[2].is_number_integer() || !v[3].is_string()) continue;
            tab_views[std::atoi(it.key().c_str())] = { v[0].get<int>(), v[1].get<int>(), v[2].get<int>(), v[3].get<std::string>() };
        }
    }
    auto save_views = [&tab_views, views_file] {
        if (views_file.empty()) return;
        nlohmann::json j = nlohmann::json::object();
        for (const auto& [st, v] : tab_views) j[std::to_string(st)] = { v.anchor, v.line, v.active, v.sub };
        std::error_code ec;
        fs::create_directories(views_file.parent_path(), ec);
        std::ofstream(views_file, std::ios::trunc) << j.dump();
    };
    auto is_tab = [](ui::State st) { return st == ui::State::HOME || st == ui::State::EXPLORE || st == ui::State::LIBRARY; };
    // Which page of a tab is shown: Explore's tab / mood, the Library's filter.
    // A view is only put back on the same one (Charts' scroll on Explore's top
    // page hid its tabs).
    auto library_filter = [&]() -> std::string {
        for (auto& item : main_content_items)
            for (const auto& c : item.chips_data.entries) if (c.enabled) return c.id;
        return "all";
    };

    // Lyrics shifted by hand (, and .), kept across restarts.
    const fs::path offsets_file = [] {
        const fs::path h = utils::home();
        return h.empty() ? fs::path() : h / ".cache" / "moroder" / "lyrics_offsets.json";
    }();
    {
        std::ifstream in(offsets_file);
        if (in.is_open()) {
            nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_object()) for (auto it = j.begin(); it != j.end(); ++it) if (it.value().is_number()) ui::lyrics_offset[it.key()] = it.value().get<double>();
        }
    }
    auto save_offsets = [offsets_file] {
        if (offsets_file.empty()) return;
        nlohmann::json j = nlohmann::json::object();
        for (const auto& [id, off] : ui::lyrics_offset) j[id] = off;   // 0 too: it can undo a suggested shift
        std::error_code ec;
        fs::create_directories(offsets_file.parent_path(), ec);
        std::ofstream(offsets_file, std::ios::trunc) << j.dump();
    };

    const fs::path searches_file = [] {
        const std::string home_s = utils::home().string(); const char* home = home_s.empty() ? nullptr : home_s.c_str();
        return home ? fs::path(home) / ".cache" / "moroder" / "searches.json" : fs::path();
    }();
    {
        std::ifstream in(searches_file);
        if (in.is_open()) {
            nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_array()) for (const auto& v : j) if (v.is_string()) search_bar_data.history.push_back(v);
        }
    }

    search_bar_data.onSearch = [&current_state, &player, &main_content_items, &main_content, &screen, &close_search, &search_bar_data, searches_file, &last_query](const std::string& value) {
        spdlog::info("SEARCHBAR: enter pressed with value, " + value);
        if (!searches_file.empty()) {
            std::error_code ec;
            fs::create_directories(searches_file.parent_path(), ec);
            std::ofstream(searches_file, std::ios::trunc) << nlohmann::json(search_bar_data.history).dump();
        }
        close_search();
        current_state = ui::State::SEARCH;
        last_query = value;

        player.search(value);

        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));

        screen.PostEvent(Event::Custom);
    };

    /* render_page
- Home has ~20 shelves of ~10 covers; FTXUI renders every child of a frame,
  off-screen ones included, so each keypress redrew ~200 covers (~70 ms).
  Render only the shelves around the selected one: the frame scrolls to the
  selection anyway, and the window moves with it
    */
    /* Volume popover: a click on the volume opens a slider above it */
    struct VolumePop {
        bool open = false;
        Box bar, mute;
        std::chrono::steady_clock::time_point opened;
    } volume_pop;

    ui::ViewportState page_view;
    // Back / forward: where to put the page once it has its rows again.
    struct Restore {
        bool pending = false;
        std::string key;              // the page it is for (dropped on any other)
        ui::ViewportState view;
        int active = -1;
        std::chrono::steady_clock::time_point since;
    } restore;
    Component page_first;       // first child of the page drawn last: a new page resets the scroll

    auto render_page = [&]() -> Element {
        const int n = static_cast<int>(main_content->ChildCount());
        const bool home = current_state == ui::State::HOME || current_state == ui::State::ARTIST || current_state == ui::State::EXPLORE;

        if (n == 0 || main_content->ChildAt(0) != page_first) {
            page_view = {};
            page_first = n ? main_content->ChildAt(0) : nullptr;
        }

        // Back / forward: once the page has the row it was left on (it loads
        // anew), the selection and the scroll go back where they were.
        if (restore.pending && restore.key != nav_current.key()) restore.pending = false;   // another page now
        if (restore.pending) {
            if (n > std::max(0, restore.active) && n > 1) {
                if (restore.active >= 0 && main_content->ChildAt(restore.active)->Focusable())
                    main_content->SetActiveChild(main_content->ChildAt(restore.active));
                page_view.anchor = restore.view.anchor;
                page_view.line = restore.view.line;
                page_view.follow = false;
                restore.pending = false;
            } else if (std::chrono::steady_clock::now() - restore.since > std::chrono::seconds(5)) {
                restore.pending = false;      // it never got that long: leave it at the top
            }
        }

        int active = 0;
        Component a = main_content->ActiveChild();
        for (int i = 0; i < n; i++) if (main_content->ChildAt(i) == a) { active = i; break; }
        // Kept with the page in the history, for coming back to it.
        nav_current.view = page_view;
        nav_current.active = active;

        /* After a wheel scroll the selection may have left the view: it moves
        to the nearest row still in view (so keys and Enter act on what is
        seen), and the view doesn't jump back to it.
        */
        if (!page_view.follow && page_view.first_visible >= 0 && n > 0) {
            const int to = active < page_view.first_visible ? page_view.first_visible
                         : active > page_view.last_visible  ? page_view.last_visible : active;
            if (to != active && to < n && main_content->ChildAt(to)->Focusable()) {
                main_content->SetActiveChild(main_content->ChildAt(to));
                active = to;
            }
        }

        // Home shelves are tall (a window of 7); list rows are 2-3 lines (60).
        const int before = home ? 2 : 30;
        const int after  = home ? 5 : 30;
        const int lo = n <= before + after ? 0 : std::max(0, active - before);
        const int hi = n <= before + after ? n : std::min(n, active + after);

        // Only the drawn ones may take clicks (the others' areas are stale).
        for (auto& item : main_content_items) {
            bool drawn = false;
            for (int i = lo; i < hi && !drawn; i++) drawn = main_content->ChildAt(i) == item.component;
            *item.shown = drawn;
        }

        Elements shown;
        std::vector<int> indices;
        for (int i = lo; i < hi; i++) {
            shown.push_back(main_content->ChildAt(i)->Render());
            indices.push_back(i);
        }
        return ui::viewport(std::move(shown), std::move(indices), active, &page_view);
    };

    // MORODER_PROFILE=1 logs frame build times every 2 s.
    static const bool profile = std::getenv("MORODER_PROFILE") != nullptr;
    struct { double copy = 0, build = 0, render = 0; int n = 0; std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now(); } prof;

    auto ui = Renderer(layout, [&] {
        const auto p0 = std::chrono::steady_clock::now();
        sidebar_focused = sidebar->Focused();
        // Focus left the search bar (a click on the page): search mode ends.
        if (search_active && !search_bar->Focused()) { search_active = false; content_column = 1; search_bar_data.suggestions.clear(); }
        ui::search_has_focus = search_bar->Focused();

        services::Player::PlayerState state_copy;
        ui::cover_hit_boxes.clear();      // refilled as this frame draws the covers
        ui::more_boxes.clear();           // and the section titles with a "More" page
        {
            std::lock_guard lock(player.state_mutex);
            state_copy = player.state;

            if (player.state.current) {
                state_copy.current = player.state.current->clone();
            }

            // Deep copies of the queues are only needed by the queue view; a
            // queued 1,000-track playlist cloned every frame cost most of a core.
            state_copy.user_queue.clear();
            state_copy.radio_queue.clear();

            if (current_state == ui::State::QUEUE) {
                for (auto& item : player.state.user_queue) {
                    state_copy.user_queue.push_back(item->clone());
                }
                for (auto& item : player.state.radio_queue) {
                    state_copy.radio_queue.push_back(item->clone());
                }
            }

            // Now Playing's "up next": a few rows after the current one, then
            // autoplay, with their queue indexes (as the queue view counts them).
            ui::now_playing_upnext.clear();
            {
                static const music::IStreamable* last_current = nullptr;   // a new track: up next from the top
                if (player.state.current.get() != last_current) { last_current = player.state.current.get(); ui::now_playing_upnext_scroll = 0; }
            }
            if (current_state == ui::State::NOWPLAYING && cfg.NOW_PLAYING_UP_NEXT) {
                auto row = [](const std::shared_ptr<music::IStreamable>& item, int index, bool autoplay) {
                    ui::UpNextRow r; r.index = index; r.autoplay = autoplay;
                    if (auto s = std::dynamic_pointer_cast<music::Song>(item)) {
                        r.title = s->ref.title; r.artist = s->ref.artists.empty() ? "" : s->ref.artists[0].name; r.thumb = s->ref.thumbnail_small;
                    } else if (auto e = std::dynamic_pointer_cast<music::Episode>(item)) {
                        r.title = e->ref.title; r.artist = e->ref.podcast.name; r.thumb = e->ref.thumbnail_small;
                    }
                    return r;
                };
                const auto& uq = player.state.user_queue;
                for (int i = std::max(0, player.state.queue_position + 1); i < static_cast<int>(uq.size()) && ui::now_playing_upnext.size() < 40; i++)
                    if (uq[i]) ui::now_playing_upnext.push_back(row(uq[i], i, false));
                if (player.state.autoplay) {
                    const auto& rq = player.state.radio_queue;
                    for (size_t i = 0; i < rq.size() && i < 40; i++)
                        if (rq[i]) ui::now_playing_upnext.push_back(row(rq[i], static_cast<int>(uq.size() + i), true));
                }
            }

            audio_playing = state_copy.flags["audio"] == services::Player::Flags::Ongoing;
            has_track = state_copy.current != nullptr;
        }

        /* Prefetch the playlist under the selection (sidebar, or a shelf /
        search item), so opening it is instant.
        */
        {
            static std::string last_prefetch;
            const music::PlaylistRef* ref = nullptr;
            const music::ApiResult* r = nullptr;
            if (sidebar_focused && sidebar_selected >= ui::SIDEBAR_FIXED && static_cast<size_t>(sidebar_selected - ui::SIDEBAR_FIXED) < sidebar_data.results.size()) {
                ref = &sidebar_data.results[sidebar_selected - ui::SIDEBAR_FIXED].ref;
            } else if (ui::ContentEntry* e = [&]() -> ui::ContentEntry* {
                           if (current_state == ui::State::NOWPLAYING) return nullptr;   // page hidden
                           for (auto& item : main_content_items) if (item.component && item.component->Focused()) return &item;
                           return nullptr; }()) {
                r = (!e->carousel_data.results.empty() && e->selected >= 0 && static_cast<size_t>(e->selected) < e->carousel_data.results.size())
                    ? &e->carousel_data.results[e->selected] : &e->result;
                ref = std::get_if<music::PlaylistRef>(&r->data);
            }
            if (ref && ref->id.rfind("mood:", 0) == 0) ref = nullptr;     // a mood (Explore), not a playlist
            if (ref && ref->id != last_prefetch) {
                last_prefetch = ref->id;
                player.prefetchPlaylist(*ref);
            }
            // An album: the same, so opening it shows its tracks at once.
            if (!ref && r) {
                if (const auto* al = std::get_if<music::AlbumRef>(&r->data); al && al->id != last_prefetch) {
                    last_prefetch = al->id;
                    player.prefetchAlbum(*al);
                }
            }

            // Same for a song: resolve its stream, so Enter starts it at once.
            static std::string last_warm;
            if (!sidebar_focused && current_state != ui::State::NOWPLAYING) {
                for (auto& item : main_content_items) {
                    if (!item.component || !item.component->Focused()) continue;
                    const music::ApiResult* r = (!item.carousel_data.results.empty() && item.selected >= 0 && static_cast<size_t>(item.selected) < item.carousel_data.results.size())
                        ? &item.carousel_data.results[item.selected] : &item.result;
                    if (const auto* s = std::get_if<music::SongRef>(&r->data); s && s->id != last_warm) {
                        last_warm = s->id;
                        player.warmStream(*s);
                    }
                    break;
                }
            }
        }

        const auto p1 = std::chrono::steady_clock::now();
        // Spinners turn with time (one step per 80 ms), not with redraws:
        // a burst of redraws (scrolling) no longer spins them faster.
        spinner_frame = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 80);
        ui::anim::wanted = false;           // set again by whatever is still moving in this frame
        /* A page change (the sidebar, back / forward, an album opened, Now
        Playing) fades the new page in over ~180 ms. Navigation shows in the
        history's size, so loads and refreshes of the same page don't fade. */
        // The page's own request still running (the skeleton shows only then,
        // and for 10 s at most: a stalled request doesn't animate forever).
        bool page_loading = false;
        {
            const char* flag = current_state == ui::State::HOME ? "home" : current_state == ui::State::EXPLORE ? "explore"
                             : current_state == ui::State::LIBRARY ? "library_playlists" : current_state == ui::State::SEARCH ? "search"
                             : current_state == ui::State::PLAYLIST ? "open_playlist" : current_state == ui::State::ARTIST ? "open_artist" : nullptr;
            static ui::State loading_page = ui::State::HOME;
            static ui::anim::Clock::time_point loading_since{};
            auto f = flag ? state_copy.flags.find(flag) : state_copy.flags.end();
            const bool ongoing = f != state_copy.flags.end() && f->second == services::Player::Flags::Ongoing;
            if (!ongoing || loading_page != current_state) { loading_page = current_state; loading_since = ui::anim::Clock::now(); }
            page_loading = ongoing && ui::anim::msSince(loading_since) < 10000;
        }
        double page_fade = 1.0;
        {
            static std::string last_key;
            static ui::anim::Clock::time_point changed{};
            const std::string key = std::to_string(static_cast<int>(current_state)) + "|" + nav_current.key();
            if (key != last_key) { if (!last_key.empty()) changed = ui::anim::Clock::now(); last_key = key; }
            page_fade = ui::anim::easeOut(ui::anim::progress(changed, 180));
        }
        if (glide.speed > 0.6 && ui::anim::msSince(glide.last) <= 60) ui::anim::more();   // until the coast can start
        if (glide.speed > 0.6 && ui::anim::msSince(glide.last) > 60) {
            ui::HScroll* h = nullptr;
            for (auto& item : main_content_items)
                if (item.category == glide.row) h = !item.grid_data.results.empty() ? &item.grid_data.hscroll : &item.carousel_data.hscroll;
            if (h && ui::hscrollBy(h, glide.dir * std::max(1, static_cast<int>(std::lround(glide.speed))))) { glide.speed *= 0.72; ui::anim::more(); }
            else glide.speed = 0;
        }
        ui::anim_frame = spinner_frame;
        playback_bar_data.frame = spinner_frame;

        /* Animation clock (below) runs only while something visibly loads:
        a page, a playlist's tracks, or the stream that was just started.
        */
        {
            auto ongoing = [&](const char* k) {
                auto it = state_copy.flags.find(k);
                return it != state_copy.flags.end() && it->second == services::Player::Flags::Ongoing;
            };
            // Lyrics only need a few frames a second; spinners want more.
            const bool spinner_on = ongoing("home") || ongoing("explore") || ongoing("search") || ongoing("open_playlist") || ongoing("open_playlist_more")
                     || ongoing("library_playlists") || ongoing("library_albums") || ongoing("library_songs")
                     || (state_copy.current && ongoing("audio") && playback_bar_data.duration == 0)
                     || (current_state == ui::State::NOWPLAYING && ongoing("lyrics"));
            // Sung-along lyrics fill word by word: ~10 fps while on screen.
            const bool singing = current_state == ui::State::NOWPLAYING && playback_bar_data.is_playing
                              && state_copy.lyrics && !state_copy.lyrics->lines.empty();
            anim_period_ms = (spinner_on || singing) ? 90 : 250;

            animating = ongoing("home") || ongoing("explore") || ongoing("search") || ongoing("open_playlist") || ongoing("open_playlist_more")
                     || ongoing("library_playlists") || ongoing("library_albums") || ongoing("library_songs")
                     || (state_copy.current && ongoing("audio") && playback_bar_data.duration == 0)
                     // Synced lyrics scroll smoothly while playing.
                     || (current_state == ui::State::NOWPLAYING && playback_bar_data.is_playing && state_copy.lyrics && !state_copy.lyrics->lines.empty())
                     || (current_state == ui::State::NOWPLAYING && ongoing("lyrics"))
                     || help_held;
            if (help_held) anim_period_ms = 80;
        }

        ui::getSidebarData(&state_copy, &sidebar_data);
        {
            // The sidebar marks the page shown (under Now Playing / the queue: the one below).
            ui::State page = current_state == ui::State::NOWPLAYING ? before_now_playing : current_state;
            if (page == ui::State::QUEUE) page = before_queue;
            sidebar_data.current = page == ui::State::HOME ? 0 : page == ui::State::EXPLORE ? 1 : page == ui::State::LIBRARY ? 2 : -1;
            if (page == ui::State::PLAYLIST && state_copy.open_playlist) {
                for (size_t i = 0; i < sidebar_data.results.size(); i++)
                    if (sidebar_data.results[i].ref.id == state_copy.open_playlist->ref.id) { sidebar_data.current = ui::SIDEBAR_FIXED + static_cast<int>(i); break; }
            }
        }
        ui::getPlaybackData(&state_copy, &playback_bar_data, screen.dimx());
        playback_bar_data.queue_open = current_state == ui::State::QUEUE;

        // The heart of a track not rated this session: ask the account (once per track).
        if (auto song = std::dynamic_pointer_cast<music::Song>(state_copy.current);
            song && state_copy.is_logged_in && !state_copy.liked.count(song->ref.id)) {
            player.fetchLikeStatus(song->ref.id);
        }

        // History: a new page reached pushes the one we were on.
        {
            NavEntry now;
            now.state = current_state;
            if (current_state == ui::State::PLAYLIST && state_copy.open_playlist) { now.playlist = state_copy.open_playlist->ref; now.kind = state_copy.open_kind; }
            if (current_state == ui::State::ARTIST && state_copy.open_artist) now.artist = state_copy.open_artist->ref;
            if (current_state == ui::State::SEARCH) { now.query = last_query; now.filter = state_copy.search_filter; }
            // The queue's way back to Now Playing holds only while on the queue.
            if (now.state != ui::State::QUEUE && now.state != ui::State::NOWPLAYING) before_queue = ui::State::HOME;
            // The first page drawn after launch: where it was left last time.
            static bool launched = false;
            if (!launched && now.state != ui::State::NOWPLAYING) {
                launched = true;
                auto tv = tab_views.find(static_cast<int>(now.state));
                if (is_tab(now.state) && tv != tab_views.end() && (now.state == ui::State::EXPLORE ? tv->second.sub == state_copy.explore_tab + "|" + state_copy.explore_title
                                   : now.state == ui::State::LIBRARY ? tv->second.sub == "all" : true)) {
                    ui::ViewportState v;
                    v.anchor = tv->second.anchor; v.line = tv->second.line;
                    restore = { true, now.key(), v, tv->second.active, std::chrono::steady_clock::now() };
                }
            }
            if (now.state == ui::State::NOWPLAYING) {
                // Over the page, not a page of its own: not in the history.
            } else if (now.key() != nav_current.key()) {
                // Leaving a tab: remember where it was.
                if (is_tab(nav_current.state)) {
                    tab_views[static_cast<int>(nav_current.state)] = { nav_current.view.anchor, nav_current.view.line, nav_current.active,
                        nav_current.state == ui::State::EXPLORE ? state_copy.explore_tab + "|" + state_copy.explore_title
                        : nav_current.state == ui::State::LIBRARY ? library_filter() : std::string() };
                    save_views();
                }
                const bool fresh = !nav_restoring;
                if (!nav_restoring) {
                    nav_back.push_back(nav_current);
                    if (nav_back.size() > 50) nav_back.erase(nav_back.begin());
                    nav_forward.clear();
                }
                nav_current = now;
                // Coming to a tab anew (not back / forward, which restore their own):
                // where it was left.
                auto tv = tab_views.find(static_cast<int>(now.state));
                if (fresh && is_tab(now.state) && tv != tab_views.end() && (now.state == ui::State::EXPLORE ? tv->second.sub == state_copy.explore_tab + "|" + state_copy.explore_title
                                   : now.state == ui::State::LIBRARY ? tv->second.sub == "all" : true)) {
                    ui::ViewportState v;
                    v.anchor = tv->second.anchor; v.line = tv->second.line;
                    restore = { true, now.key(), v, tv->second.active, std::chrono::steady_clock::now() };
                }
            } else if (now.state == ui::State::PLAYLIST) {
                nav_current.playlist = now.playlist;     // title known once loaded
            }
            nav_restoring = false;
        }
        {
            const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            playback_bar_data.sleep_label = sleep_end_of_track ? "end of track"
                : sleep_at_ms > now_ms ? std::to_string((sleep_at_ms - now_ms + 59999) / 60000) + "m" : "";
        }

        if(current_state == ui::State::HOME) {
            ui::buildHome(&state_copy, main_content_items, main_content, on_item_press, [&player, &main_content, &main_content_items, &screen]{
                player.getHome();

                bool lists_done;
                {
                    std::lock_guard lock(player.state_mutex);   // read under the lock, without inserting
                    auto it = player.state.flags.find("library_playlists");
                    lists_done = it != player.state.flags.end() && it->second == services::Player::Flags::Done;
                }
                if(!lists_done) {
                    player.getLibraryPlaylists();
                }

                main_content_items.clear();
                main_content->DetachAllChildren();
                main_content->Add(Renderer([] { return emptyElement(); }));

                screen.PostEvent(Event::Custom);
            });

        } else if(current_state == ui::State::SEARCH && state_copy.flags["search"] == services::Player::Flags::Done) {
            ui::buildSearch(&state_copy, main_content_items, main_content, on_item_press, [&player, &last_query, &main_content, &main_content_items, &screen, filter = state_copy.search_filter] {
                player.search(last_query, filter);

                main_content_items.clear();
                main_content->DetachAllChildren();
                main_content->Add(Renderer([] { return emptyElement(); }));

                screen.PostEvent(Event::Custom);
            },
            [&player, &last_query, &cfg](const Event& event, const ui::ChipEntry& chip) {
                if (!Config::isKey(event, cfg.KEY_PLAY_NOW)) return false;
                player.search(last_query, chip.id);
                return true;
            });

        } else if(current_state == ui::State::QUEUE) {
            ui::buildQueue(&state_copy, main_content_items, main_content, on_queue_press);

        } else if(current_state == ui::State::PLAYLIST) {
            ui::buildPlaylist(&state_copy, main_content_items, main_content,
                [&player, &cfg](const Event& e, int index) {
                    if (Config::isKey(e, cfg.KEY_PLAY_NOW)) {
                        player.playOpenPlaylistFrom(static_cast<size_t>(index));
                        return true;
                    }
                    if (Config::isKey(e, cfg.KEY_ADD_TO_QUEUE)) {
                        std::shared_ptr<music::IStreamable> track;
                        {
                            std::lock_guard lock(player.state_mutex);
                            if (player.state.open_playlist) {
                                auto all = player.state.open_playlist->getStreamables();
                                if (index >= 0 && static_cast<size_t>(index) < all.size()) track = all[index];
                            }
                        }
                        if (track) player.queue(track, false, false);
                        return true;
                    }
                    return false;
                },
                [&player] { player.playOpenPlaylistFrom(0); },
                [&player] {
                    std::shared_ptr<music::Playlist> copy;
                    {
                        std::lock_guard lock(player.state_mutex);
                        if (player.state.open_playlist) copy = std::make_shared<music::Playlist>(*player.state.open_playlist);
                    }
                    if (copy) player.queue(std::static_pointer_cast<music::IStreamableContainer>(copy), false, false);
                },
                [&player, &main_content_items, &main_content, &screen] {
                    std::shared_ptr<music::Playlist> open;
                    {
                        std::lock_guard lock(player.state_mutex);
                        open = player.state.open_playlist;
                    }
                    if (open) player.openPlaylist(open->ref);
                    main_content_items.clear();
                    main_content->DetachAllChildren();
                    main_content->Add(Renderer([] { return emptyElement(); }));
                    screen.PostEvent(Event::Custom);
                });

        } else if(current_state == ui::State::EXPLORE) {
            ui::buildExplore(&state_copy, main_content_items, main_content, on_item_press,
                [&player, &main_content_items, &main_content, &screen] {
                    player.getExplore();
                    main_content_items.clear();
                    main_content->DetachAllChildren();
                    main_content->Add(Renderer([] { return emptyElement(); }));
                    screen.PostEvent(Event::Custom);
                },
                [&player, &cfg](const Event& event, const ui::ChipEntry& chip) {
                    if (!Config::isKey(event, cfg.KEY_PLAY_NOW)) return false;
                    // The tab shown, again: a reload; another: from the cache if fresh.
                    bool same;
                    {
                        std::lock_guard lock(player.state_mutex);
                        same = player.state.explore_tab == chip.id && player.state.explore_params.empty();
                    }
                    player.getExplore(same ? "" : chip.id);
                    return true;
                });

        } else if(current_state == ui::State::ARTIST) {
            ui::buildArtist(&state_copy, main_content_items, main_content, on_item_press,
                [&player, &main_content_items, &main_content, &screen] {
                    std::shared_ptr<services::Music::ArtistPage> open;
                    {
                        std::lock_guard lock(player.state_mutex);
                        open = player.state.open_artist;
                    }
                    if (open) player.openArtist(open->ref);
                    main_content_items.clear();
                    main_content->DetachAllChildren();
                    main_content->Add(Renderer([] { return emptyElement(); }));
                    screen.PostEvent(Event::Custom);
                });

        } else if(current_state == ui::State::LIBRARY) {
            // As many columns as fit, then rows downwards (the page scrolls).
            const int library_cols = std::max(1, (screen.dimx() - (sidebar_hidden ? 6 : 38)) / ui::GRID_TILE_WIDTH);
            const int library_rows = -library_cols;

            ui::buildLibrary(&state_copy, main_content_items, main_content, library_rows, search_active ? search_bar_data.value : std::string(), on_chip_press, on_item_press,
                [&player, &screen] {
                    player.retryLibrary();
                    screen.PostEvent(Event::Custom);
                });
        }

        const auto p2 = std::chrono::steady_clock::now();

        Element frame = vbox({
            hbox({
                !sidebar_hidden
                    ? vbox({
                        text(" "),
                        hbox({
                            text("") | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)),
                            text("  "),
                            text(APP_NAME_HUMAN) | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY))
                        }) | bold | center,

                        text(" "),
                        text(" "),

                        sidebar->Render() | yframe | reflect(sidebar_box)
                    }) | size(WIDTH, EQUAL, ui::sidebarWidth(screen.dimx()))
                    : emptyElement(),

                !sidebar_hidden
                    ? hbox({
                        text(" "),
                        separator() | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)),
                        text(" "),
                    })
                    : emptyElement(),

                vbox({
                    hbox({
                        // ‹ › : back / forward, clickable (Warp doesn't pass trackpad swipes on).
                        vbox({
                            text(" "),
                            hbox({
                                text(" \uf053 ") | (nav_back.empty() ? color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY))
                                                                    : color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY))) | reflect(nav_back_box),
                                text(" \uf054 ") | (nav_forward.empty() ? color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY))
                                                                       : color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY))) | reflect(nav_forward_box),
                            }),
                        }),
                        search_bar->Render() | flex,
                        vbox({
                            text(" "),
                            state_copy.is_logged_in ?
                                text("") | size(WIDTH, EQUAL, 3) | color(ui::GetColor(ui::MColor::SUCCESS)) :
                                text("") | size(WIDTH, EQUAL, 3) |  color(ui::GetColor(ui::MColor::ERROR)),
                            text(" ")
                        }) | align_right
                    }),

                    separator() | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)),

                    hbox({
                        // Room between the sidebar's line and the page (it sat
                        // right against it), and a little on the right too.
                        text("   "),
                        ui::fadeIn([&] {
                            const std::string error_key =
                                  current_state == ui::State::HOME    ? "home"
                                : current_state == ui::State::SEARCH  ? "search"
                                : current_state == ui::State::LIBRARY ? "library"
                                : current_state == ui::State::PLAYLIST ? "open_playlist"
                                : current_state == ui::State::ARTIST ? "open_artist"
                                                                      : "";

                            const bool has_error = !error_key.empty() && state_copy.errors.count(error_key) > 0;

                            /* The signed-out notice on home is an ErrorBox too,
                            and wants the same centred, flexed layout even though
                            there is no entry in errors for it.
                            */
                            const bool notice = current_state == ui::State::HOME
                                && state_copy.flags["is_logged_in"] == services::Player::Flags::Done
                                && !state_copy.is_logged_in;

                            if (current_state == ui::State::NOWPLAYING) {
                                const bool playing = playback_bar_data.is_playing;
                                // The exact position: whole seconds made the lyrics up to 1 s late.
                                double pos = playback_bar_data.position_exact;
                                if (playing && last_tick_at != std::chrono::steady_clock::time_point{}) {
                                    // Between the 1 s ticks, so synced lyrics move on time.
                                    pos += std::min(1.0, std::chrono::duration<double>(std::chrono::steady_clock::now() - last_tick_at).count());
                                }
                                return ui::renderNowPlaying(&state_copy, pos, static_cast<double>(playback_bar_data.duration),
                                                            playing, screen.dimx(), screen.dimy() - 8) | flex;
                            }

                            return (has_error || notice)
                                ? render_page() | flex
                                : current_state != ui::State::QUEUE
                                    ? render_page() | yflex
                                    : main_content->Render() | xflex;
                        }(), page_fade),
                        text("  "),
                    }) | yflex,

                    // A page still empty: its shape (titles, a row of covers,
                    // captions) shimmering, so nothing jumps when it arrives.
                    (main_content_items.empty() && page_loading) ? [&] {
                        auto section = [&] {
                            Elements tiles;
                            for (int i = 0; i < 6; i++) tiles.push_back(vbox({ ui::shimmer(20, 10), text(""), ui::shimmer(14, 1), text(""), ui::shimmer(9, 1) }) | size(WIDTH, EQUAL, 23));
                            return vbox({ text(""), text(""), hbox({ text("    "), ui::shimmer(18, 1) }), text(""), text(""), hbox({ text("    "), hbox(std::move(tiles)) }) });
                        };
                        return vbox({ section(), section(), filler() }) | flex | xframe;
                    }() :
                    (sidebar_data.is_loading && state_copy.is_logged_in && current_state != ui::State::NOWPLAYING) ?
                        vbox({
                            filler(),
                            hbox({
                                filler(),
                                spinner(15, spinner_frame)
                            })
                        }) | flex :
                        emptyElement(),

                }) | flex
            }) | flex,

            // Shown as soon as something is launched, so the loading state
            // has somewhere to appear (it used to pop in only once audio began).
            // When it isn't drawn, its click areas must not linger (or sit at
            // 0,0 before the first play, where a click skipped backwards).
            [&]() -> Element {
                if (state_copy.current) return playback_bar->Render();
                const Box none{-1, -1, -1, -1};
                for (Box* b : { &playback_bar_data.box_progress, &playback_bar_data.box_prev, &playback_bar_data.box_play,
                                &playback_bar_data.box_next, &playback_bar_data.box_like, &playback_bar_data.box_volume,
                                &playback_bar_data.box_loop, &playback_bar_data.box_shuffle, &playback_bar_data.box_track,
                                &playback_bar_data.box_queue }) *b = none;
                return emptyElement();
            }(),
            Config::get().EXTRA_BOTTOM_PADDING ? text("") : emptyElement()
        });

        if (profile) {
            using ms = std::chrono::duration<double, std::milli>;
            const auto p3 = std::chrono::steady_clock::now();
            prof.copy += ms(p1 - p0).count(); prof.build += ms(p2 - p1).count(); prof.render += ms(p3 - p2).count(); prof.n++;
            if (p3 - prof.since > std::chrono::seconds(2)) {
                spdlog::info("PROFILE: {} frames, avg copy {:.2f} ms, build {:.2f} ms, elements {:.2f} ms",
                             prof.n, prof.copy / prof.n, prof.build / prof.n, prof.render / prof.n);
                prof = {}; prof.since = p3;
            }
        }
        return frame;
    });

    bool show_help = false;

    /* Context menu (right-click)
    - Actions for whatever the pointer was over; each action replays the key
      that does the same thing, so mouse and keyboard can never disagree
    */
    struct ContextItem {
        std::string icon, label, hint;
        std::function<void()> run;
    };
    struct ContextMenu {
        bool open = false;
        int x = 0, y = 0;
        int hover = 0;
        std::string title;              // what the menu is about, dimmed on top
        std::vector<ContextItem> items;
        std::vector<Box> boxes;
    } ctx;

    auto state_logged_in = [&player] {
        std::lock_guard lock(player.state_mutex);
        return player.state.is_logged_in;
    };

    // Title of an API result, for the context menu header.
    auto result_title = [](const music::ApiResult& r) {
        std::string t;
        std::visit([&](auto&& d) {
            using T = std::decay_t<decltype(d)>;
            if constexpr (std::is_same_v<T, music::SongRef> || std::is_same_v<T, music::AlbumRef> ||
                          std::is_same_v<T, music::PlaylistRef> || std::is_same_v<T, music::EpisodeRef>) t = d.title;
            else if constexpr (std::is_same_v<T, music::ArtistRef> || std::is_same_v<T, music::PodcastRef>) t = d.name;
        }, r.data);
        return t;
    };

    // The song under the selection (shelf, list row, playlist track), if any.
    // Second menu at the same place: the user's playlists, to add ref to.
    auto open_playlist_picker = [&](const music::SongRef& ref) {
        std::vector<ContextItem> lists;
        lists.push_back({"\uf067", "New playlist\u2026", "", [&, ref] {
            prompt.open = true;
            prompt.title = "New playlist with \u201c" + utils::trimSuffix(ref.title, 30, "...") + "\u201d";
            prompt.value.clear();
            prompt.done = [&player, id = ref.id](const std::string& name) { player.createPlaylist(name, id); };
        }});
        for (const auto& pl : sidebar_data.results) {
            if (pl.ref.id == "LM" || pl.ref.id == "VLLM" || pl.ref.id == "SE" || pl.ref.id == "VLSE") continue;   // Liked Music / Episodes: not editable
            const std::string pid = pl.ref.id, ptitle = pl.ref.title;
            lists.push_back({"\uf03a", utils::trimSuffix(ptitle, 22, "..."), "", [&player, pid, ptitle, id = ref.id] { player.addToPlaylist(pid, ptitle, id); }});
            if (lists.size() >= 15) break;
        }
        ctx.title = "Add to playlist";
        ctx.items = std::move(lists);
        ctx.hover = 0;
        ctx.boxes.assign(ctx.items.size(), Box{});
        ctx.open = true;
    };

    // "Copy…": a second menu to pick what goes to the clipboard.
    auto open_copy_menu = [&](const music::SongRef& ref, const std::string& album_title) {
        auto copy = [&player](std::string what, std::string value) {
            return [&player, what, value] {
                const bool ok = services::copyToClipboard(value);
                std::lock_guard lock(player.state_mutex);
                player.state.notice = ok ? what + " copied" : "No clipboard tool found";
                player.state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            };
        };
        std::string artists;
        for (const auto& a : ref.artists) artists += (artists.empty() ? "" : ", ") + a.name;
        std::vector<ContextItem> items;
        items.push_back({"\uf0c5", "Title", "", copy("Title", ref.title)});
        if (!artists.empty()) {
            items.push_back({"\uf007", "Artist", "", copy("Artist", artists)});
            items.push_back({"\uf001", "Title - Artist", "", copy("Title and artist", ref.title + " - " + artists)});
        }
        if (!album_title.empty()) items.push_back({"\uf07c", "Album", "", copy("Album", album_title)});
        items.push_back({"\uf0c1", "Link", "", copy("Link", "https://music.youtube.com/watch?v=" + ref.id)});
        ctx.title = "Copy";
        ctx.items = std::move(items);
        ctx.hover = 0;
        ctx.boxes.assign(ctx.items.size(), Box{});
        ctx.open = true;
    };

    // "Search “title”": the title in the search, as if typed.
    auto search_for = [&](const std::string& q) {
        if (q.empty()) return;
        search_bar_data.value = q;
        search_bar_data.onSearch(q);
    };

    auto open_artist_page = [&](const music::ArtistRef& artist) {
        player.openArtist(artist);
        current_state = ui::State::ARTIST;
        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));
        screen.PostEvent(Event::Custom);
    };

    // Explore: a section's "More" page (the full chart is a playlist; the rest,
    // a page of Explore under that title). From a click on its title or the keys.
    ui::open_more = [&](const std::string& title) {
        std::string id;
        {
            std::lock_guard lock(player.state_mutex);
            auto it = player.state.explore_more.find(title);
            if (it != player.state.explore_more.end()) id = it->second;
        }
        if (id.rfind("VL", 0) == 0) {
            music::PlaylistRef ref;
            ref.id = id.substr(2);
            ref.title = title;
            player.openPlaylist(ref);
            current_state = ui::State::PLAYLIST;
            main_content_items.clear();
            main_content->DetachAllChildren();
            main_content->Add(Renderer([] { return emptyElement(); }));
        } else if (!id.empty()) {
            player.getExplore(id, "", title);
        }
    };

    auto open_album_page = [&](const music::AlbumRef& album) {
        player.openAlbum(album);
        current_state = ui::State::PLAYLIST;
        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));
        screen.PostEvent(Event::Custom);
    };

    // The track playing, with its album, for the menus on Now Playing and the bar.
    auto current_song = [&]() -> std::shared_ptr<music::Song> {
        std::lock_guard lock(player.state_mutex);
        return std::dynamic_pointer_cast<music::Song>(player.state.current);
    };

    /* Menu for the track playing: right-click on the Now Playing cover, title,
    artist or album, or on the track in the playback bar */
    auto open_current_menu = [&](int x, int y) -> bool {
        const auto song = current_song();
        if (!song) return false;
        const music::SongRef ref = song->ref;
        const music::AlbumRef album = song->album;
        ctx.items.clear();
        ctx.title = utils::trimSuffix(ref.title, 30, "...");
        if (state_logged_in()) {
            ctx.items.push_back({"\uf0fe", "Add to playlist\u2026", "", [&, ref] { open_playlist_picker(ref); }});
            ctx.items.push_back({playback_bar_data.liked ? "\uf004" : "\uf08a", playback_bar_data.liked ? "Unlike" : "Like", "", [&player] { player.toggleLikeCurrent(); }});
        }
        ctx.items.push_back({"\uf1be", "Start radio", "", [&player, ref] {
            auto s = std::make_shared<music::Song>();
            s->setRef(ref);
            player.queue(std::static_pointer_cast<music::IStreamable>(s), true, true);
        }});
        if (!ref.artists.empty() && !ref.artists[0].id.empty()) {
            const music::ArtistRef artist = ref.artists[0];
            ctx.items.push_back({"\uf007", "Go to " + utils::trimSuffix(artist.name, 16, "..."), "", [&, artist] { open_artist_page(artist); }});
        }
        if (!album.id.empty()) {
            ctx.items.push_back({"\uf07c", "Go to album", "", [&, album] { open_album_page(album); }});
        }
        const std::string link = "https://music.youtube.com/watch?v=" + ref.id;
        ctx.items.push_back({"\uf002", "Search \u201c" + utils::trimSuffix(ref.title, 12, "\u2026") + "\u201d", "", [&, t = ref.title] { search_for(t); }});
        ctx.items.push_back({"\uf0c5", "Copy\u2026", "", [&, ref, t = album.title] { open_copy_menu(ref, t); }});
        ctx.items.push_back({"\uf08e", "Open in browser", "", [link] { services::openUrl(link); }});
        ctx.open = true;
        ctx.x = x;
        ctx.y = y;
        ctx.hover = 0;
        ctx.boxes.assign(ctx.items.size(), Box{});
        return true;
    };

    auto focused_song = [&]() -> std::optional<music::SongRef> {
        if (current_state == ui::State::NOWPLAYING) return std::nullopt;   // the page is hidden
        for (auto& item : main_content_items) {
            if (!item.component || !item.component->Focused()) continue;
            const music::ApiResult* r = (!item.carousel_data.results.empty() && item.selected >= 0 && static_cast<size_t>(item.selected) < item.carousel_data.results.size())
                ? &item.carousel_data.results[item.selected] : &item.result;
            if (const auto* s = std::get_if<music::SongRef>(&r->data)) return *s;
        }
        return std::nullopt;
    };

    // Wheel pacing: a trackpad fires dozens of wheel events per swipe.
    auto last_wheel = std::chrono::steady_clock::time_point{};

    // Double-click detection.
    auto last_click = std::chrono::steady_clock::time_point{};
    int last_click_x = -1, last_click_y = -1;
    Component last_click_target;    // the page entry the first click selected
    auto double_click_played_at = std::chrono::steady_clock::time_point{};

    // Content entry that has focus (a row, a carousel, the filters...), if any.
    auto focused_entry = [&]() -> ui::ContentEntry* {
        if (current_state == ui::State::NOWPLAYING) return nullptr;    // the page is hidden
        for (auto& item : main_content_items)
            if (item.component && item.component->Focused()) return &item;
        return nullptr;
    };

    // Mute / unmute (popover icon and the mute key share the level to go back to).
    int before_mute = 100;
    auto toggle_mute = [&] {
        const int now = playback_bar_data.volume;
        if (now > 0) { before_mute = now; player.changeVolume(-now); playback_bar_data.volume = 0; }
        else         { player.changeVolume(before_mute); playback_bar_data.volume = before_mute; }
    };

    // Go to a page from the history (no new history entry for it).
    auto navigate_to = [&](const NavEntry& e) {
        nav_restoring = true;
        switch (e.state) {
            case ui::State::HOME:    sidebar_data.onHome(); return;
            case ui::State::LIBRARY: sidebar_data.onLibrary(); return;
            case ui::State::EXPLORE: sidebar_data.onExplore(); return;
            case ui::State::PLAYLIST:
                if (e.kind == "album") {
                    music::AlbumRef a;
                    a.id = e.playlist.id; a.title = e.playlist.title;
                    a.thumbnail_large = e.playlist.thumbnail_large; a.thumbnail_small = e.playlist.thumbnail_small;
                    // The header's "artist •" comes from here: back / forward lost it.
                    if (!e.playlist.author.empty()) {
                        music::ArtistRef by;
                        by.name = e.playlist.author;
                        a.artists.push_back(by);
                    }
                    player.openAlbum(a);
                } else {
                    player.openPlaylist(e.playlist);
                }
                break;
            case ui::State::ARTIST: player.openArtist(e.artist); break;
            case ui::State::SEARCH:
                last_query = e.query;
                search_bar_data.value = e.query;
                player.search(e.query, e.filter);
                break;
            case ui::State::NOWPLAYING: before_now_playing = current_state; break;
            default: break;
        }
        current_state = e.state;
        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));
        screen.PostEvent(Event::Custom);
    };
    auto nav_notice = [&](const std::string& s) {
        std::lock_guard lock(player.state_mutex);
        player.state.notice = s;
        player.state.notice_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    };
    auto go_back = [&]() -> bool {
        // A mood's page: back to the moods; a section's "More" page: back to Explore.
        if (current_state == ui::State::EXPLORE) {
            std::string tab, title;
            {
                std::lock_guard lock(player.state_mutex);
                tab = player.state.explore_tab;
                title = player.state.explore_title;
            }
            if (tab == "FEmusic_moods_and_genres_category") { player.getExplore("FEmusic_moods_and_genres"); return true; }
            if (!title.empty()) { player.getExplore("FEmusic_explore"); return true; }
        }
        // Now Playing sits over the page it was opened from: back is that
        // page, as it was (not a rebuilt one, scrolled to the top).
        if (current_state == ui::State::NOWPLAYING) { current_state = before_now_playing; return true; }
        // The queue opened from Now Playing: back to Now Playing (the page
        // under it is rebuilt: the queue took its place).
        if (current_state == ui::State::QUEUE && before_queue == ui::State::NOWPLAYING) {
            before_queue = ui::State::HOME;
            // The queue was recorded over the page under Now Playing: undo that,
            // so leaving Now Playing later lands on that page, not the queue.
            if (nav_current.state == ui::State::QUEUE && !nav_back.empty()) { nav_current = nav_back.back(); nav_back.pop_back(); }
            main_content_items.clear();
            main_content->DetachAllChildren();
            main_content->Add(Renderer([] { return emptyElement(); }));
            current_state = ui::State::NOWPLAYING;
            return true;
        }
        if (nav_back.empty()) return false;
        NavEntry target = nav_back.back();
        nav_back.pop_back();
        nav_forward.push_back(nav_current);
        nav_notice("\u2190 " + target.label());
        navigate_to(target);
        restore = { true, target.key(), target.view, target.active, std::chrono::steady_clock::now() };
        return true;
    };
    auto go_forward = [&]() -> bool {
        if (nav_forward.empty()) return false;
        NavEntry target = nav_forward.back();
        nav_forward.pop_back();
        nav_back.push_back(nav_current);
        nav_notice(target.label() + " \u2192");
        navigate_to(target);
        restore = { true, target.key(), target.view, target.active, std::chrono::steady_clock::now() };
        return true;
    };

    /* Two-finger swipe sideways (trackpad): a burst of horizontal wheel events.
    Over a carousel / grid it scrolls it; elsewhere a deliberate swipe (enough
    events in a row, one way) goes back or forward, once per swipe. On Now
    Playing's cover it skips tracks, like swiping an album cover.
    */
    struct Swipe {
        int dir = 0, count = 0;
        std::chrono::steady_clock::time_point last{}, cooldown_until{};
    } swipe;

    // Dragging along the progress line (see the mouse handler).
    struct Scrub {
        bool active = false;
        bool np = false;          // on Now Playing's line rather than the bar's
        std::chrono::steady_clock::time_point last{};
    } scrub;

    ui = CatchEvent(ui, [&](Event event){
        /* Mouse
        - A click on the playback bar hits its controls; the progress line seeks
        - Anywhere else, the page handles the click (that focuses the item under
          the pointer) and the same click then activates it, like Enter
        - The wheel scrolls; over the volume it changes the volume
        */
        if(event.is_mouse()) {
            const Mouse& m = event.mouse();
            const bool click = m.button == Mouse::Left && m.motion == Mouse::Pressed;
            const bool wheel = m.button == Mouse::WheelUp || m.button == Mouse::WheelDown;

            if(show_help) {
                if(click) show_help = false;
                return true;
            }

            if(prompt.open) {
                if(click) prompt.open = false;
                return true;
            }

            auto in = [&](const Box& b) { return b.Contain(m.x, m.y); };
            ui::mouse_x = m.x;      // for hover: what's under the pointer is drawn brighter
            ui::mouse_y = m.y;

            // Volume popover: click / drag on the bar sets the level, the icon mutes.
            if(volume_pop.open) {
                const bool drag = m.button == Mouse::Left && m.motion == Mouse::Moved;
                if((click || drag) && in(volume_pop.bar) && volume_pop.bar.x_max > volume_pop.bar.x_min) {
                    const int level = std::clamp(static_cast<int>(std::lround(100.0 * (m.x - volume_pop.bar.x_min) / (volume_pop.bar.x_max - volume_pop.bar.x_min))), 0, 100);
                    player.changeVolume(level - playback_bar_data.volume);
                    // Several drag events can come between two frames: keep the base current.
                    playback_bar_data.volume = level;
                    return true;
                }
                if(click && in(volume_pop.mute)) {
                    toggle_mute();
                    return true;
                }
                if(wheel && (in(volume_pop.bar) || in(volume_pop.mute))) {
                    player.changeVolume(m.button == Mouse::WheelUp ? cfg.VOLUME_STEP : -cfg.VOLUME_STEP);
                    return true;
                }
                if(m.motion == Mouse::Pressed && !in(playback_bar_data.box_volume)) volume_pop.open = false;
            }

            if(wheel && in(playback_bar_data.box_volume)) {
                const auto now = std::chrono::steady_clock::now();
                if(now - last_wheel < std::chrono::milliseconds(60)) return true;
                last_wheel = now;
                player.changeVolume(m.button == Mouse::WheelUp ? cfg.VOLUME_STEP : -cfg.VOLUME_STEP);
                return true;
            }

            /* Progress line: a press on it, or on the blank row just under
            it (a one-row target is easy to miss), seeks; holding the button
            and moving scrubs, wherever the pointer goes, until it's released.
            An open context menu takes the click first (it closes).
            */
            if (!ctx.open) {
                // The bar's line, or Now Playing's own (same behaviour); a drag
                // keeps the line it started on.
                auto on_line = [&](const Box& b) {
                    return b.x_max > b.x_min && b.y_min >= 0 && m.x >= b.x_min && m.x <= b.x_max && m.y >= b.y_min && m.y <= b.y_max + 1;
                };
                if (click) {
                    if (current_state == ui::State::NOWPLAYING && on_line(ui::now_playing_progress_box)) scrub.np = true;
                    else if (on_line(playback_bar_data.box_progress)) scrub.np = false;
                }
                const Box& p = scrub.np && current_state == ui::State::NOWPLAYING ? ui::now_playing_progress_box : playback_bar_data.box_progress;
                const bool has_bar = p.x_max > p.x_min && p.y_min >= 0;
                const auto fraction = [&] { return std::clamp(double(m.x - p.x_min) / double(p.x_max - p.x_min), 0.0, 1.0); };
                if(click && on_line(p)) {
                    scrub.active = true;
                    scrub.last = std::chrono::steady_clock::now();
                    player.seekToFraction(fraction());
                    return true;
                }
                if(scrub.active) {
                    if(m.button == Mouse::Left && m.motion == Mouse::Moved) {
                        const auto now = std::chrono::steady_clock::now();
                        if(has_bar && now - scrub.last > std::chrono::milliseconds(120)) {
                            scrub.last = now;
                            player.seekToFraction(fraction());
                        }
                        return true;
                    }
                    if(m.motion == Mouse::Released) {
                        scrub.active = false;
                        if(has_bar) player.seekToFraction(fraction());
                        return true;
                    }
                    scrub.active = false;   // any other press: the drag is over
                }
            }

            if(click) {
                // A carousel / grid's ‹ › (not those of the page under Now Playing).
                for (auto& item : main_content_items) {
                    if (current_state == ui::State::NOWPLAYING) break;
                    if (!*item.shown) continue;
                    if (ui::hscrollClick(&item.carousel_data.hscroll, m.x, m.y) || ui::hscrollClick(&item.grid_data.hscroll, m.x, m.y)) return true;
                }
                // Explore > Charts: its country button lists the countries.
                if (current_state == ui::State::EXPLORE && in(ui::charts_country_box)) {
                    ctx.items.clear();
                    ctx.title = "Charts";
                    for (const auto& [code, name] : ui::chartCountries()) {
                        const std::string c = code;
                        ctx.items.push_back({"", name, "", [&player, c] { player.setChartsCountry(c); }});
                    }
                    ctx.open = true;
                    ctx.x = ui::charts_country_box.x_min;
                    ctx.y = ui::charts_country_box.y_max + 1;
                    ctx.hover = 0;
                    ctx.boxes.assign(ctx.items.size(), Box{});
                    return true;
                }
                // Explore: a section title with a "More" page opens it (the full chart
                // is a playlist; the rest, a page of Explore under that title).
                if (current_state == ui::State::EXPLORE) {
                    for (const auto& [b, title] : ui::more_boxes) {
                        if (!in(b)) continue;
                        ui::open_more(title);
                        return true;
                    }
                }
                if(in(nav_back_box))    { go_back();    return true; }
                if(in(nav_forward_box)) { go_forward(); return true; }
                if(in(playback_bar_data.box_prev))   { player.skipBackward();      return true; }
                if(in(playback_bar_data.box_play))   { player.togglePause();       return true; }
                if(in(playback_bar_data.box_next))   { player.skipForward();       return true; }
                if(in(playback_bar_data.box_like))   { player.toggleLikeCurrent(); return true; }
                if(in(playback_bar_data.box_loop))   { player.cycleLoop();         return true; }
                if(in(playback_bar_data.box_shuffle)){ player.shuffleUpcoming();   return true; }
                if(in(playback_bar_data.box_queue)) {
                    // Open the queue; on it, back to where you were.
                    if (current_state == ui::State::QUEUE) { if (!go_back()) sidebar_data.onHome(); }
                    else ui->OnEvent(Config::keyEvent(cfg.KEY_QUEUE_VIEW));
                    return true;
                }
                if(current_state == ui::State::NOWPLAYING && (in(ui::now_playing_artist_box) || in(ui::now_playing_album_box))) {
                    if (const auto song = current_song()) {
                        if (in(ui::now_playing_artist_box) && !song->ref.artists.empty() && !song->ref.artists[0].id.empty()) { open_artist_page(song->ref.artists[0]); return true; }
                        if (in(ui::now_playing_album_box) && !song->album.id.empty()) { open_album_page(song->album); return true; }
                    }
                }
                if(in(playback_bar_data.box_track))  {
                    // Right after a double-click played something, the bar shows
                    // up (or its title moves) under the pointer: a click that
                    // quick is the end of that gesture, not a request for the
                    // full screen.
                    if (std::chrono::steady_clock::now() - double_click_played_at < std::chrono::milliseconds(800)) return true;
                    // Not as a key: with the search bar focused it would type "v".
                    if (search_active) close_search();
                    ui->OnEvent(Config::keyEvent(cfg.KEY_NOW_PLAYING));
                    return true;
                }
                if(in(playback_bar_data.box_volume)) {
                    volume_pop.open = !volume_pop.open;
                    volume_pop.opened = std::chrono::steady_clock::now();
                    return true;
                }
            }

            const bool wheel_h = m.button == Mouse::WheelLeft || m.button == Mouse::WheelRight;
            const bool moved = m.motion == Mouse::Moved || m.button == Mouse::None;

            // An open context menu takes the mouse first.
            if(ctx.open) {
                if(moved) {
                    for(size_t i = 0; i < ctx.boxes.size(); i++) if(in(ctx.boxes[i])) ctx.hover = static_cast<int>(i);
                    return true;
                }
                if(click) {
                    for(size_t i = 0; i < ctx.boxes.size(); i++) {
                        if(in(ctx.boxes[i])) {
                            ctx.open = false;
                            const auto run = ctx.items[i].run;   // an item may replace the items (submenu)
                            run();
                            return true;
                        }
                    }
                }
                if(m.motion == Mouse::Pressed) ctx.open = false;   // any press elsewhere closes it
                return true;
            }

            // A click on a search suggestion searches it.
            // Only the list on screen (it is hidden while new ones load).
            if(click && search_bar->Focused() && !search_bar_data.suggestions.empty() && search_bar_data.suggestions_for == search_bar_data.value) {
                for (size_t i = 0; i < search_bar_data.suggestion_boxes.size() && i < search_bar_data.suggestions.size(); i++) {
                    if (in(search_bar_data.suggestion_boxes[i])) {
                        search_bar_data.suggestion_index = static_cast<int>(i);
                        search_bar->OnEvent(Event::Return);
                        return true;
                    }
                }
            }

            // Now Playing: a click on a row of "up next" plays it.
            if(click && current_state == ui::State::NOWPLAYING) {
                for (const auto& [b, index] : ui::now_playing_upnext_boxes) {
                    if (in(b)) { player.skipTo(index); return true; }
                }
            }

            // Now Playing: a click on a lyrics line jumps to it.
            if(click && current_state == ui::State::NOWPLAYING && playback_bar_data.duration > 0) {
                for (const auto& [b, t] : ui::lyric_boxes) {
                    if (b && in(*b)) {
                        player.seekTo(t);
                        return true;
                    }
                }
            }

            /* Queue: drag a row onto another to move it (after the current
            track, like K / J). Press picks it up, moving shows where it lands,
            release drops it.
            */
            if(current_state == ui::State::QUEUE) {
                auto row_at = [&]() {
                    for (const auto& [idx, b] : ui::queue_row_boxes) if (b && in(*b)) return idx;
                    return -1;
                };
                const bool drag_move = m.button == Mouse::Left && m.motion == Mouse::Moved;
                if (click) { ui::queue_drag_from = row_at(); ui::queue_drag_over = ui::queue_drag_from; }
                else if (drag_move && ui::queue_drag_from >= 0) { const int r = row_at(); if (r >= 0) ui::queue_drag_over = r; return true; }
                else if (m.motion == Mouse::Released && ui::queue_drag_from >= 0) {
                    const int from = ui::queue_drag_from, to = ui::queue_drag_over;
                    ui::queue_drag_from = ui::queue_drag_over = -1;
                    // The marker sits above the target: moving down, that is one before it.
                    if (to >= 0 && to != from) {
                        const int dest = to > from ? to - 1 : to;
                        player.moveInQueue(static_cast<size_t>(from), static_cast<size_t>(dest));
                        ui::queue_focus_index = dest;
                    }
                    return true;
                }
            }

            /* Hover doesn't move the selection: following the pointer made the
            focus jump around while scrolling or just passing over the page.
            Releases are dropped too: everything acts on press, and a menu
            took the focus back on release (opening a sidebar playlist left
            the focus in the sidebar instead of on its first track).
            */
            if(moved || m.motion == Mouse::Released) return true;

            /* Wheel / trackpad
            - Vertical always scrolls vertically (the sidebar when the pointer is
              over it, the page otherwise), never sideways through a carousel
            - Horizontal (trackpad swipe, or Shift+wheel) slides the carousel /
              grid under the pointer by columns (see ui/components/hscroll.hpp)
            - Elsewhere a deliberate sideways swipe goes back / forward
            */
            // Now Playing's up next column: the wheel scrolls it, a row at a time.
            if(wheel && current_state == ui::State::NOWPLAYING && in(ui::now_playing_upnext_area)) {
                const auto now = std::chrono::steady_clock::now();
                if(now - last_wheel < std::chrono::milliseconds(40)) return true;
                last_wheel = now;
                ui::now_playing_upnext_scroll += m.button == Mouse::WheelUp ? -1 : 1;
                if (ui::now_playing_upnext_scroll < 0) ui::now_playing_upnext_scroll = 0;
                return true;
            }

            if(wheel || wheel_h) {
                const bool horizontal = wheel_h || m.shift;
                const bool back = m.button == Mouse::WheelUp || m.button == Mouse::WheelLeft;

                const auto now = std::chrono::steady_clock::now();

                // The row under the pointer: a carousel / grid there is what
                // sideways input (trackpad, or Shift+wheel) slides. The focus
                // stays put, so the page never jumps to follow it.
                ui::ContentEntry* under = nullptr;
                if(horizontal && !(!sidebar_hidden && in(sidebar_box)) && current_state != ui::State::NOWPLAYING && current_state != ui::State::QUEUE) {
                    for (const auto& [idx, b] : page_view.drawn) {
                        if (!in(b) || idx >= static_cast<int>(main_content->ChildCount())) continue;
                        Component child = main_content->ChildAt(idx);
                        for (auto& item : main_content_items) if (item.component == child) { under = &item; break; }
                    }
                }
                const bool sideways_list = under && (!under->carousel_data.results.empty() || !under->grid_data.results.empty());

                // Trackpad swipe sideways (not Shift+wheel, which is always sideways scrolling).
                if(wheel_h && !m.shift) {
                    const bool on_cover = current_state == ui::State::NOWPLAYING && in(ui::now_playing_cover_box);
                    if(!sideways_list) {
                        const int dir = m.button == Mouse::WheelLeft ? -1 : 1;
                        if(now < swipe.cooldown_until) return true;          // the rest of a swipe already acted on
                        if(dir != swipe.dir || now - swipe.last > std::chrono::milliseconds(300)) { swipe.dir = dir; swipe.count = 0; }
                        swipe.last = now;
                        if(++swipe.count < 6) return true;                   // a deliberate swipe, not a brush
                        swipe.count = 0;
                        swipe.cooldown_until = now + std::chrono::milliseconds(650);
                        if(on_cover) {
                            // Like flicking an album cover: towards the left, the next one.
                            ui::nowPlayingSwiped(dir);
                            if (dir > 0) player.skipForward(); else player.skipBackward();
                        } else if(dir < 0) {
                            if(!go_back() && current_state == ui::State::NOWPLAYING) ui->OnEvent(Event::Escape);
                        } else {
                            go_forward();
                        }
                        return true;
                    }
                }

                // Sideways over a carousel / grid: the row slides by columns,
                // smooth on a trackpad, the selection staying where it is.
                if(horizontal && sideways_list) {
                    ui::HScroll* h = !under->grid_data.results.empty() ? &under->grid_data.hscroll : &under->carousel_data.hscroll;
                    ui::hscrollBy(h, (back ? -1 : 1) * (wheel_h ? 3 : 8));
                    // A quick swipe keeps going a little once the fingers lift (see glide).
                    if (wheel_h) {
                        const int dir = back ? -1 : 1;
                        glide.burst = (dir == glide.dir && now - glide.last < std::chrono::milliseconds(120)) ? glide.burst + 1 : 1;
                        glide.dir = dir; glide.last = now; glide.row = under->category;
                        glide.speed = glide.burst >= 3 ? 3.0 : 0.0;
                    }
                    return true;
                }

                // The page scrolls by lines, like a web page: smooth on a
                // trackpad, and the selection stays put while it is in view.
                if(!horizontal && !(!sidebar_hidden && in(sidebar_box)) && current_state != ui::State::QUEUE && current_state != ui::State::NOWPLAYING) {
                    if(back && page_view.at_top && page_view.pending <= 0) return true;
                    if(!back && page_view.at_bottom && page_view.pending >= 0) return true;
                    page_view.pending += back ? -2 : 2;
                    page_view.follow = false;
                    return true;
                }

                const auto pace = horizontal ? std::chrono::milliseconds(70) : std::chrono::milliseconds(30);
                if(now - last_wheel < pace) return true;
                last_wheel = now;

                Event step = horizontal ? (back ? Event::ArrowLeft : Event::ArrowRight)
                                        : (back ? Event::ArrowUp   : Event::ArrowDown);

                if(!horizontal && !sidebar_hidden && in(sidebar_box)) {
                    if(!sidebar->Focused()) sidebar->TakeFocus();
                    sidebar->OnEvent(step);
                    return true;
                }

                // Not the page hidden under Now Playing.
                if(current_state != ui::State::NOWPLAYING && main_content->Focusable()) {
                    if(!main_content->Focused()) main_content->TakeFocus();
                    main_content->OnEvent(step);
                }
                return true;
            }

            // Middle click: preview the item under the pointer.
            if(m.button == Mouse::Middle && m.motion == Mouse::Pressed) {
                Event left = Event::Mouse(event.input(), m);
                left.mouse().button = Mouse::Left;
                ui::suppress_click = true;
                layout->OnEvent(left);
                ui::suppress_click = false;
                if (auto song = focused_song()) player.preview(*song);
                return true;
            }

            const bool right_click = m.button == Mouse::Right && m.motion == Mouse::Pressed;

            if(right_click) {
                const bool on_current = in(playback_bar_data.box_track) ||
                    (current_state == ui::State::NOWPLAYING && (in(ui::now_playing_cover_box) || in(ui::now_playing_title_box) ||
                                                                 in(ui::now_playing_artist_box) || in(ui::now_playing_album_box)));
                if (on_current && open_current_menu(m.x, m.y)) return true;

                // Focus what is under the pointer, as a left press would.
                Event left = Event::Mouse(event.input(), m);
                left.mouse().button = Mouse::Left;
                ui::suppress_click = true;      // focus only: Play / Retry must not fire
                const bool hit = layout->OnEvent(left);
                ui::suppress_click = false;
                if(!hit || search_bar->Focused()) return true;

                ctx.items.clear();
                ctx.title.clear();
                auto key = [&](const std::string& bind) { return [&, bind] { layout->OnEvent(Config::keyEvent(bind)); }; };
                const std::string play_hint = "enter";

                if(sidebar->Focused()) {
                    const int sel = sidebar_selected;
                    if(sel >= ui::SIDEBAR_FIXED && static_cast<size_t>(sel - ui::SIDEBAR_FIXED) < sidebar_data.results.size()) {
                        const music::Playlist pl = sidebar_data.results[sel - ui::SIDEBAR_FIXED];
                        ctx.title = pl.ref.title;
                        ctx.items.push_back({"\uf07c", "Open", play_hint, key(cfg.KEY_PLAY_NOW)});
                        ctx.items.push_back({"\uf04b", "Play", "", [&player, pl] {
                            player.queue(std::static_pointer_cast<music::IStreamableContainer>(std::make_shared<music::Playlist>(pl)), true, true);
                        }});
                        ctx.items.push_back({"\uf067", "Add to queue", cfg.KEY_ADD_TO_QUEUE, key(cfg.KEY_ADD_TO_QUEUE)});

                        // The user's own playlists (not Liked Music / Episodes).
                        const std::string pid = pl.ref.id, ptitle = pl.ref.title;
                        const bool editable = state_logged_in() && pid != "LM" && pid != "VLLM" && pid != "SE" && pid != "VLSE";
                        if(editable) {
                            ctx.items.push_back({"\uf040", "Rename\u2026", "", [&, pid, ptitle] {
                                prompt.open = true;
                                prompt.title = "Rename \u201c" + utils::trimSuffix(ptitle, 30, "...") + "\u201d";
                                prompt.value = ptitle;
                                prompt.done = [&player, pid](const std::string& name) { player.renamePlaylist(pid, name); };
                            }});
                            ctx.items.push_back({"\uf1f8", "Delete\u2026", "", [&, pid, ptitle] {
                                // Asked again: it can't be undone.
                                ctx.title = "Delete \u201c" + utils::trimSuffix(ptitle, 18, "...") + "\u201d?";
                                ctx.items = {
                                    {"\uf00d", "Cancel", "esc", [] {}},
                                    {"\uf1f8", "Delete for good", "", [&, pid, ptitle] {
                                        player.deletePlaylist(pid, ptitle);
                                        if (current_state == ui::State::PLAYLIST) sidebar_data.onHome();
                                    }},
                                };
                                ctx.hover = 0;
                                ctx.boxes.assign(ctx.items.size(), Box{});
                                ctx.open = true;
                            }});
                        }
                    }
                } else if(ui::ContentEntry* e = focused_entry()) {
                    const std::string& c = e->category;
                    const bool special = c.rfind("__library_filters__", 0) == 0 || c.rfind("__search_filters__", 0) == 0 || c == "__explore_tabs__" || c.rfind("__explore_moods__", 0) == 0 || c.rfind("__explore__", 0) == 0 || c.rfind("__error__", 0) == 0 || c.rfind("__playlist__", 0) == 0;
                    if(!special) {
                        const bool track = c.rfind("__track__", 0) == 0;

                        // Carousels hold their results apart from the entry.
                        if(!e->carousel_data.results.empty() && e->selected >= 0 && static_cast<size_t>(e->selected) < e->carousel_data.results.size())
                            ctx.title = result_title(e->carousel_data.results[e->selected]);
                        else
                            ctx.title = result_title(e->result);

                        // Acts on the item that was clicked, even if the page changes
                        // while the menu is open (only playlist rows replay the key).
                        // A carousel's, or a grid's (Explore's charts and covers), selected item.
                        const music::ApiResult clicked = (!e->carousel_data.results.empty() && e->selected >= 0 && static_cast<size_t>(e->selected) < e->carousel_data.results.size())
                            ? e->carousel_data.results[e->selected]
                            : (!e->grid_data.results.empty() && e->selected >= 0 && static_cast<size_t>(e->selected) < e->grid_data.results.size())
                                ? e->grid_data.results[e->selected] : e->result;
                        auto act = [&, clicked, track](const std::string& bind) -> std::function<void()> {
                            if (track || current_state == ui::State::QUEUE) return key(bind);
                            return [&, clicked, bind] { on_item_press(Config::keyEvent(bind), clicked); };
                        };
                        // A page (artist, album, playlist, mood) opens; a song plays.
                        const bool opens = !track && (std::holds_alternative<music::ArtistRef>(clicked.data) || std::holds_alternative<music::AlbumRef>(clicked.data)
                                                      || std::holds_alternative<music::PlaylistRef>(clicked.data) || std::holds_alternative<music::PodcastRef>(clicked.data));
                        ctx.items.push_back({opens ? "\uf07c" : "\uf04b", track ? "Play from here" : opens ? "Open" : "Play", play_hint, act(cfg.KEY_PLAY_NOW)});
                        // A playlist, album or chart plays straight away, without opening it.
                        if (const auto* pl = std::get_if<music::PlaylistRef>(&clicked.data); pl && pl->id.rfind("mood:", 0) != 0) {
                            const music::PlaylistRef ref = *pl;
                            ctx.items.push_back({"\uf04b", "Play", "", [&player, ref] {
                                auto p = std::make_shared<music::Playlist>(); p->setRef(ref);
                                player.queue(std::static_pointer_cast<music::IStreamableContainer>(p), true, true);
                            }});
                        } else if (const auto* al = std::get_if<music::AlbumRef>(&clicked.data)) {
                            const music::AlbumRef ref = *al;
                            ctx.items.push_back({"\uf04b", "Play", "", [&player, ref] {
                                auto a = std::make_shared<music::Album>(); a->setRef(ref);
                                player.queue(std::static_pointer_cast<music::IStreamableContainer>(a), true, true);
                            }});
                        }
                        if (const auto* ar = std::get_if<music::ArtistRef>(&clicked.data); ar && !ar->id.empty()) {
                            const music::ArtistRef artist = *ar;
                            ctx.items.push_back({"\uf1be", "Start radio", "", [&player, artist] { player.startArtistRadio(artist); }});
                        }

                        // Song under the pointer, for "radio" and "go to artist".
                        const music::ApiResult* r = nullptr;
                        if(!e->carousel_data.results.empty() && e->selected >= 0 && static_cast<size_t>(e->selected) < e->carousel_data.results.size())
                            r = &e->carousel_data.results[e->selected];
                        else
                            r = &e->result;
                        if(const auto* song = std::get_if<music::SongRef>(&r->data)) {
                            const music::SongRef ref = *song;
                            ctx.items.push_back({"\uf028", "Preview 15 s", cfg.KEY_PREVIEW, [&player, ref] { player.preview(ref); }});
                            ctx.items.push_back({"\uf051", "Play next", cfg.KEY_PLAY_NEXT, [&player, ref] {
                                auto s = std::make_shared<music::Song>();
                                s->setRef(ref);
                                player.playNext(std::static_pointer_cast<music::IStreamable>(s));
                            }});
                            // In one of the user's playlists: it can come out of it.
                            if(current_state == ui::State::PLAYLIST && !ref.set_video_id.empty()) {
                                std::string pid;
                                {
                                    std::lock_guard lock(player.state_mutex);
                                    if (player.state.open_playlist) pid = player.state.open_playlist->ref.id;
                                }
                                if(!pid.empty()) {
                                    ctx.items.push_back({"\uf068", "Remove from playlist", "", [&player, pid, ref] {
                                        player.removeFromPlaylist(pid, ref.id, ref.set_video_id);
                                    }});
                                }
                            }
                            const std::string link = "https://music.youtube.com/watch?v=" + ref.id;
                            ctx.items.push_back({"\uf002", "Search \u201c" + utils::trimSuffix(ref.title, 12, "\u2026") + "\u201d", "", [&, t = ref.title] { search_for(t); }});
                            ctx.items.push_back({"\uf0c5", "Copy\u2026", "", [&, ref] { open_copy_menu(ref, ""); }});
                            ctx.items.push_back({"\uf08e", "Open in browser", "", [link] { services::openUrl(link); }});
                            if(state_logged_in()) {
                                ctx.items.push_back({"\uf0fe", "Add to playlist\u2026", "", [&, ref] { open_playlist_picker(ref); }});
                            }
                            ctx.items.push_back({"\uf1be", "Start radio", "", [&player, ref] {
                                auto s = std::make_shared<music::Song>();
                                s->setRef(ref);
                                player.queue(std::static_pointer_cast<music::IStreamable>(s), true, true);
                            }});
                            if(!ref.artists.empty() && !ref.artists[0].id.empty()) {
                                const music::ArtistRef artist = ref.artists[0];
                                ctx.items.push_back({"\uf007", "Go to " + utils::trimSuffix(artist.name, 16, "..."), "", [&, artist] { open_artist_page(artist); }});
                            }
                        }
                        if(current_state == ui::State::QUEUE) {
                            // The row under the pointer, by queue index: the queue
                            // can be rebuilt (a track change) while the menu is open.
                            int qi = -1;
                            for (const auto& [idx, b] : ui::queue_row_boxes) if (b && b->Contain(m.x, m.y)) qi = idx;
                            if (qi >= 0) {
                                ctx.items.push_back({"\uf077", "Move up", cfg.KEY_MOVE_UP, [&player, qi] {
                                    player.moveInQueue(static_cast<size_t>(qi), static_cast<size_t>(std::max(0, qi - 1)));
                                    ui::queue_focus_index = std::max(0, qi - 1); }});
                                ctx.items.push_back({"\uf078", "Move down", cfg.KEY_MOVE_DOWN, [&player, qi] {
                                    player.moveInQueue(static_cast<size_t>(qi), static_cast<size_t>(qi + 1));
                                    ui::queue_focus_index = qi + 1; }});
                                ctx.items.push_back({"\uf00d", "Remove from queue", cfg.KEY_REMOVE_FROM_QUEUE, [&player, qi] {
                                    player.removeAt(static_cast<size_t>(qi)); }});
                            }
                            ctx.items.push_back({"\uf12d", "Clear what comes next", "", [&player] { player.clearUpcoming(); }});
                            if (state_logged_in()) {
                                ctx.items.push_back({"\uf0c7", "Save queue as playlist\u2026", "", [&] {
                                    prompt.open = true;
                                    prompt.title = "Save the queue as a playlist";
                                    prompt.value.clear();
                                    prompt.done = [&player](const std::string& name) { player.saveQueueAsPlaylist(name); };
                                }});
                            }
                        } else {
                            // A mood (Explore) is a page, not something to queue.
                            const auto* mpl = std::get_if<music::PlaylistRef>(&clicked.data);
                            if (!(mpl && mpl->id.rfind("mood:", 0) == 0))
                                ctx.items.push_back({"\uf067", "Add to queue", cfg.KEY_ADD_TO_QUEUE, act(cfg.KEY_ADD_TO_QUEUE)});
                        }
                    }
                }

                if(!ctx.items.empty()) {
                    ctx.open = true;
                    ctx.x = m.x;
                    ctx.y = m.y;
                    ctx.hover = 0;
                    ctx.boxes.assign(ctx.items.size(), Box{});
                }
                return true;
            }

            // Double-click: same spot, quickly.
            bool double_click = false;
            if(click) {
                const auto now = std::chrono::steady_clock::now();
                double_click = now - last_click < std::chrono::milliseconds(400)
                            && std::abs(m.x - last_click_x) <= 1 && m.y == last_click_y;
                last_click = double_click ? std::chrono::steady_clock::time_point{} : now;
                last_click_x = m.x;
                last_click_y = m.y;
            }

            // Was it on a cover / thumbnail (as drawn last frame)?
            bool on_cover = false;
            for (const Box& b : ui::cover_hit_boxes) if (in(b)) { on_cover = true; break; }
            auto has_current = [&] { std::lock_guard lock(player.state_mutex); return player.state.current != nullptr; };

            /* The second press of a double-click plays what the first one
            selected: re-dispatching it could select something else if the
            page moved in between.
            */
            if(double_click && !search_bar->Focused()) {
                ui::ContentEntry* e = focused_entry();
                const bool self_acting = e && (e->category.rfind("__error__", 0) == 0 || e->category.rfind("__playlist__", 0) == 0);
                // Only what the first click selected (not, say, the row that was
                // selected before a click on empty space).
                if(e && !self_acting && !sidebar->Focused() && last_click_target && e->component == last_click_target) {
                    double_click_played_at = std::chrono::steady_clock::now();
                    const auto before = current_state;
                    layout->OnEvent(Event::Return);
                    // On the cover: the track in full screen, as before. On the title: it just plays.
                    if (on_cover && current_state == before && current_state != ui::State::NOWPLAYING && has_current()) {
                        ui->OnEvent(Config::keyEvent(cfg.KEY_NOW_PLAYING));
                    }
                    return true;
                }
            }

            // A click selects in place: the view follows the selection again.
            if(click) page_view.follow = true;

            const bool handled = layout->OnEvent(event);

            if(click) {
                ui::ContentEntry* e = handled ? focused_entry() : nullptr;
                last_click_target = e ? e->component : nullptr;
            }

            /* Click model, like a desktop music app:
            - single click selects; on navigation (sidebar, library filters) it
              also opens, since there is nothing else to do with a selection
            - double-click plays
            - buttons that act by themselves (Play, Retry) need nothing more
            */
            if(click && handled && !search_bar->Focused()) {
                bool navigation = sidebar->Focused();
                if(ui::ContentEntry* e = focused_entry()) {
                    navigation = navigation || e->category.rfind("__library_filters__", 0) == 0 || e->category == "__explore_tabs__"
                              || e->category.rfind("__search_filters__", 0) == 0;
                    // What opens a page (album, artist, playlist, podcast) opens on one
                    // click, like a link; what plays (a song, a video) on a double-click.
                    const music::ApiResult* r = (!e->carousel_data.results.empty() && e->selected >= 0 && static_cast<size_t>(e->selected) < e->carousel_data.results.size())
                        ? &e->carousel_data.results[e->selected]
                        : (!e->grid_data.results.empty() && e->selected >= 0 && static_cast<size_t>(e->selected) < e->grid_data.results.size())
                            ? &e->grid_data.results[e->selected] : &e->result;
                    // (Not grids: their selection follows the focus only once drawn.)
                    if (e->grid_data.results.empty() && e->category.rfind("__track__", 0) != 0 && current_state != ui::State::QUEUE
                        && (std::holds_alternative<music::AlbumRef>(r->data) || std::holds_alternative<music::ArtistRef>(r->data)
                            || std::holds_alternative<music::PlaylistRef>(r->data) || std::holds_alternative<music::PodcastRef>(r->data)))
                        navigation = true;
                    const bool self_acting = e->category.rfind("__error__", 0) == 0 || e->category.rfind("__playlist__", 0) == 0;
                    if(self_acting) navigation = false, double_click = false;
                }
                if(double_click) double_click_played_at = std::chrono::steady_clock::now();
                const auto before = current_state;
                if(navigation || double_click) layout->OnEvent(Event::Return);
                if (double_click && !navigation && on_cover && current_state == before && current_state != ui::State::NOWPLAYING && has_current()) {
                    ui->OnEvent(Config::keyEvent(cfg.KEY_NOW_PLAYING));
                }
            }

            return true;
        }

        // Keys move the selection: the view follows it again.
        if(event != Event::Custom) page_view.follow = true;

        // An open prompt takes the keyboard.
        if(prompt.open) {
            if(event == Event::Escape) { prompt.open = false; return true; }
            if(event == Event::Return) {
                std::string v = prompt.value;
                while (!v.empty() && v.back() == ' ') v.pop_back();
                while (!v.empty() && v.front() == ' ') v.erase(v.begin());
                prompt.open = false;
                if (!v.empty() && prompt.done) prompt.done(v);
                return true;
            }
            if(event == Event::Backspace) {
                // Whole UTF-8 characters.
                while (!prompt.value.empty() && (static_cast<unsigned char>(prompt.value.back()) & 0xC0) == 0x80) prompt.value.pop_back();
                if (!prompt.value.empty()) prompt.value.pop_back();
                return true;
            }
            if(event.is_character() && prompt.value.size() < 150) { prompt.value += event.character(); return true; }
            return true;
        }

        if(ctx.open) {
            if(event == Event::Escape)    { ctx.open = false; return true; }
            if(event == Event::ArrowDown) { ctx.hover = (ctx.hover + 1) % static_cast<int>(ctx.items.size()); return true; }
            if(event == Event::ArrowUp)   { ctx.hover = (ctx.hover + static_cast<int>(ctx.items.size()) - 1) % static_cast<int>(ctx.items.size()); return true; }
            if(event == Event::Return)    { ctx.open = false; const auto run = ctx.items[ctx.hover].run; run(); return true; }
            return true;
        }

        /* Hold h: the cheatsheet, for as long as h is held (key repeat keeps
        coming). A tap still seeks back; the seeks made before the hold was
        recognised are undone.
        */
        if(Config::isKey(event, cfg.KEY_SEEK_BACKWARD) && !search_bar->Focused()) {
            static int streak = 0, seeks = 0;
            static std::chrono::steady_clock::time_point burst_start{};
            const auto now = std::chrono::steady_clock::now();
            const auto gap = now - last_h;
            if (gap > std::chrono::milliseconds(700)) { streak = 0; seeks = 0; burst_start = now; }
            else if (gap < std::chrono::milliseconds(200)) streak++;
            last_h = now;

            if (help_held) return true;
            if (streak >= 2) {
                help_held = true;
                show_help = true;
                if (seeks > 0) player.seekBy(static_cast<int64_t>(seeks) * static_cast<int64_t>(cfg.SEEK_SECONDS) * 1000000LL);
                seeks = 0;
                return true;
            }
            if (show_help) return true;
            seeks++;
            player.seekBy(-static_cast<int64_t>(cfg.SEEK_SECONDS) * 1000000LL);
            return true;
        }

        if(volume_pop.open && event == Event::Escape) { volume_pop.open = false; return true; }

        // The help sheet is modal: it only listens for its own close keys.
        if(show_help) {
            if(event == Event::Escape || Config::isKey(event, cfg.KEY_HELP) || Config::isKey(event, cfg.KEY_QUIT)) {
                show_help = false;
            }
            return !event.is_mouse();
        }

        if(search_bar->Focused()) {
            // Tab with suggestions showing fills the picked one in (the bar's own key).
            const bool suggesting = !search_bar_data.suggestions.empty() && search_bar_data.suggestions_for == search_bar_data.value;
            if (event == Event::Tab && suggesting) return false;
            // Esc (or Tab) leaves the search bar for the page below.
            if(event == Event::Escape || event == Event::Tab || event == Event::TabReverse) {
                close_search();
                return true;
            }
            return false;
        }

        /* Arrows for the music
        - Shift or Option + left / right: previous / next track; + up / down:
          volume (plain arrows stay for moving around)
        - In Now Playing, where nothing else uses them, plain arrows do it
        */
        {
            const std::string& raw = event.input();
            auto is = [&](std::initializer_list<const char*> seqs) {
                for (const char* q : seqs) if (raw == q) return true;
                return false;
            };
            const bool np = current_state == ui::State::NOWPLAYING;
            // < > work in every terminal (Shift/Option arrows depend on it: `moroder keys`).
            if (is({"\x1b[1;2D", "\x1b[1;3D", "\x1b[1;9D", "\x1b" "b", "<"}) || (np && event == Event::ArrowLeft))  { player.skipBackward(); return true; }
            if (is({"\x1b[1;2C", "\x1b[1;3C", "\x1b[1;9C", "\x1b" "f", ">"}) || (np && event == Event::ArrowRight)) { player.skipForward();  return true; }
            if (is({"\x1b[1;2A", "\x1b[1;3A", "\x1b[1;9A"}) || (np && event == Event::ArrowUp))   { player.changeVolume(cfg.VOLUME_STEP);  return true; }
            if (is({"\x1b[1;2B", "\x1b[1;3B", "\x1b[1;9B"}) || (np && event == Event::ArrowDown)) { player.changeVolume(-cfg.VOLUME_STEP); return true; }
        }

        // Tab / Shift+Tab: the next / previous section of the page (a shelf, a
        // grid, the tabs), rather than row after row.
        if ((event == Event::Tab || event == Event::TabReverse) && !search_bar->Focused()
            && current_state != ui::State::NOWPLAYING && current_state != ui::State::QUEUE) {
            const int n = static_cast<int>(main_content->ChildCount());
            int at = 0;
            for (int i = 0; i < n; i++) if (main_content->ChildAt(i) == main_content->ActiveChild()) { at = i; break; }
            const int step = event == Event::Tab ? 1 : -1;
            for (int i = at + step; i >= 0 && i < n; i += step) {
                if (!main_content->ChildAt(i)->Focusable()) continue;
                main_content->SetActiveChild(main_content->ChildAt(i));
                if (!main_content->Focused()) main_content->TakeFocus();
                page_view.follow = true;
                break;
            }
            return true;
        }

        // Now Playing: , and . shift the lyrics earlier / later by half a second
        // (for lyrics timed on another cut of the song).
        if (current_state == ui::State::NOWPLAYING && (event == Event::Character(",") || event == Event::Character("."))) {
            if (auto song = current_song()) {
                // From what is shown: your shift, else a video's suggested one.
                if (!ui::lyrics_offset.count(song->ref.id)) {
                    std::lock_guard lock(player.state_mutex);
                    ui::lyrics_offset[song->ref.id] = player.state.lyrics && player.state.lyrics_id == song->ref.id ? player.state.lyrics->shift : 0.0;
                }
                double& off = ui::lyrics_offset[song->ref.id];
                off += event == Event::Character(".") ? 0.5 : -0.5;
                if (std::abs(off) < 0.01) off = 0;
                save_offsets();
            }
            return true;
        }

        // m (KEY_MUTE): mute / unmute.
        if(Config::isKey(event, cfg.KEY_MUTE)) {
            toggle_mute();
            return true;
        }

        // t (KEY_SLEEP_TIMER): sleep timer, off -> 15 -> 30 -> 60 min -> end of track -> off.
        if(Config::isKey(event, cfg.KEY_SLEEP_TIMER)) {
            static int step = 0;
            const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            if (sleep_at_ms == 0 && !sleep_end_of_track) step = 0;     // it went off meanwhile
            step = (step + 1) % 5;
            const int minutes[] = {0, 15, 30, 60, 0};
            sleep_end_of_track = step == 4;
            sleep_at_ms = minutes[step] ? now_ms + minutes[step] * 60000LL : 0;
            std::lock_guard lock(player.state_mutex);
            player.state.notice = step == 0 ? "Sleep timer off"
                                : step == 4 ? "Sleep timer: at the end of this track"
                                : "Sleep timer: " + std::to_string(minutes[step]) + " min";
            player.state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            return true;
        }

        // v: full-screen Now Playing, and back.
        if(Config::isKey(event, cfg.KEY_NOW_PLAYING) || (event == Event::Escape && current_state == ui::State::NOWPLAYING)) {
            /* Now Playing goes over the page, which stays built underneath
            (hidden from keys and clicks, see main_content_gate): leaving it
            finds the page where it was, same row, same scroll. */
            if (current_state == ui::State::NOWPLAYING) {
                current_state = before_now_playing;
            } else if (event != Event::Escape) {
                before_now_playing = current_state;
                current_state = ui::State::NOWPLAYING;
            }
            return true;
        }

        // A preview ends on Esc before Esc means "back".
        if(player.previewing() && event == Event::Escape) {
            player.stopPreview(true);
            return true;
        }

        // Esc / Backspace / [ : back (Home when there is no history); ] : forward.
        if((event == Event::Escape || event == Event::Backspace || event == Event::Character("[")) && current_state != ui::State::NOWPLAYING) {
            if(go_back()) return true;
            if(current_state != ui::State::HOME) { sidebar_data.onHome(); return true; }
            return event != Event::Escape;
        }
        if(event == Event::Character("]")) {
            go_forward();
            return true;
        }

        if(Config::isKey(event, cfg.KEY_HELP)) {
            show_help = true;
            return true;
        }

        if(Config::isKey(event, cfg.KEY_SEEK_FORWARD)) {
            player.seekBy(static_cast<int64_t>(cfg.SEEK_SECONDS) * 1000000LL);
            return true;
        }

        if(Config::isKey(event, cfg.KEY_VOLUME_DOWN)) {
            player.changeVolume(-cfg.VOLUME_STEP);
            return true;
        }

        if(Config::isKey(event, cfg.KEY_VOLUME_UP)) {
            player.changeVolume(cfg.VOLUME_STEP);
            return true;
        }

        // A preview ends on Esc / Space / the preview key again (Space also
        // brings back the paused track, like a normal play/pause).
        if(player.previewing() && (event == Event::Escape || Config::isKey(event, cfg.KEY_TOGGLE_PAUSE) || Config::isKey(event, cfg.KEY_PREVIEW))) {
            player.stopPreview(true);
            return true;
        }

        if(Config::isKey(event, cfg.KEY_PREVIEW)) {
            if (auto song = focused_song()) player.preview(*song);
            return true;
        }

        if(Config::isKey(event, cfg.KEY_PLAY_NEXT)) {
            if (auto song = focused_song()) {
                auto s = std::make_shared<music::Song>();
                s->setRef(*song);
                player.playNext(std::static_pointer_cast<music::IStreamable>(s));
            }
            return true;
        }

        if(Config::isKey(event, cfg.KEY_SHUFFLE)) {
            player.shuffleUpcoming();
            return true;
        }

        if(Config::isKey(event, cfg.KEY_LIKE)) {
            player.toggleLikeCurrent();
            return true;
        }

        if(Config::isKey(event, cfg.KEY_QUIT)) {
            screen.Exit();
            return true;
        }


        // The queue, while something is loaded (paused included).
        if(Config::isKey(event, cfg.KEY_QUEUE_VIEW) && (audio_playing || playback_bar_data.duration > 0 || has_track)) {
            if (current_state != ui::State::QUEUE) before_queue = current_state;
            current_state = ui::State::QUEUE;
            return true;
        }

        if(Config::isKey(event, cfg.KEY_TOGGLE_SIDEBAR)) {
            sidebar_hidden = !sidebar_hidden;

            if(sidebar_hidden) {
                const bool had_focus = sidebar->Focused();
                sidebar->Detach();
                if (had_focus && main_content->Focusable()) main_content->TakeFocus();   // not left with nothing focused
            } else {
                sidebar_container->Add(sidebar);
            }

            return true;
        }

        // "/" is the usual search key in terminal apps; f stays configurable.
        if(Config::isKey(event, cfg.KEY_FOCUS_SEARCH) || event == Event::Character("/")) {
            open_search();
            return true;
        }

        // n / p: the usual next / previous of terminal players, besides x / z.
        if(event == Event::Character("n")) { player.skipForward();  return true; }
        if(event == Event::Character("p")) { player.skipBackward(); return true; }

        if(Config::isKey(event, cfg.KEY_SKIP_BACKWARD)) {
            player.skipBackward();
            return true;
        }

        if(Config::isKey(event, cfg.KEY_SKIP_FORWARD)) {
            player.skipForward();
            return true;
        }

        if(Config::isKey(event, cfg.KEY_TOGGLE_PAUSE)) {
            player.togglePause();
            return true;
        }

        // The playback bar reads the mode out of the state copy it takes each
        // render, so returning true is all the redraw this needs.
        if(Config::isKey(event, cfg.KEY_TOGGLE_LOOP)) {
            player.cycleLoop();
            return true;
        }

        return false;
    });

    /* Help sheet
    - Every action and its current key, so bindings are discoverable and stay
      right when the config remaps them
    */
    auto help_sheet = [&cfg] {
        // Calm: keys in bold white, what they do in grey, headings over a thin rule.
        auto key = [](const std::string& k) {
            return text(k.empty() ? "-" : k) | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | size(WIDTH, EQUAL, 14);
        };
        auto row = [&](const std::string& k, const std::string& what) {
            return hbox({ text("  "), key(k), text(what) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)), text("  ") });
        };
        auto heading = [](const std::string& t) {
            return vbox({
                hbox({ text("  "), text(t) | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) }),
                hbox({ text("  "), separatorLight() | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)) | flex, text("  ") }),
            });
        };

        Element left = vbox({
            heading("Playback"),
            row(cfg.KEY_TOGGLE_PAUSE,  "play / pause"),
            row("\u21e7\u2192 > " + cfg.KEY_SKIP_FORWARD + " n",  "next track"),
            row("\u21e7\u2190 < " + cfg.KEY_SKIP_BACKWARD + " p", "previous / restart"),
            row(cfg.KEY_SEEK_FORWARD,  "forward " + std::to_string(cfg.SEEK_SECONDS) + " s"),
            row(cfg.KEY_SEEK_BACKWARD, "back " + std::to_string(cfg.SEEK_SECONDS) + " s (hold: this sheet)"),
            row("\u21e7\u2191 " + cfg.KEY_VOLUME_UP,     "volume up"),
            row("\u21e7\u2193 " + cfg.KEY_VOLUME_DOWN,   "volume down"),
            row(cfg.KEY_MUTE,          "mute / unmute"),
            row(cfg.KEY_TOGGLE_LOOP,   "loop: off, queue, track"),
            row(cfg.KEY_SLEEP_TIMER,   "sleep timer: 15, 30, 60 min, end"),
            row(cfg.KEY_SHUFFLE,       "shuffle what comes next"),
            row(cfg.KEY_LIKE,          "like / unlike"),
            row(cfg.KEY_NOW_PLAYING,   "now playing (arrows: skip, volume)"),
            text(""),
            heading("On the selected item"),
            row(cfg.KEY_PLAY_NOW,          "play now (starts a radio)"),
            row(cfg.KEY_ADD_TO_QUEUE,      "add to queue"),
            row(cfg.KEY_PLAY_NEXT,         "play next"),
            row(cfg.KEY_PREVIEW,           "preview 15 s (middle click)"),
            row(cfg.KEY_REMOVE_FROM_QUEUE, "remove from queue"),
            row(cfg.KEY_MOVE_UP + " " + cfg.KEY_MOVE_DOWN, "move up / down (queue)"),
        });
        Element right = vbox({
            heading("Navigation"),
            row("arrows",                 "move around"),
            row(cfg.KEY_FOCUS_SEARCH + " /",  "search (\u2191: past searches)"),
            row("esc \u232b [",           "back (previous page)"),
            row("]",                      "forward"),
            row(cfg.KEY_QUEUE_VIEW,       "queue"),
            row(cfg.KEY_TOGGLE_SIDEBAR,   "toggle sidebar"),
            row(cfg.KEY_HELP,             "this sheet"),
            row(cfg.KEY_QUIT,             "quit"),
            text(""),
            heading("Mouse"),
            row("click",                  "select (sidebar: open)"),
            row("dbl-click",              "play (stays on the page)"),
            row("right-click",            "menu: play next, radio..."),
            row("middle",                 "preview 15 s"),
            row("wheel",                  "scroll by lines"),
            row("\uf053 \uf054 / swipe",     "back / forward (swipe: not in Warp)"),
            row("swipe cover",            "now playing: skip tracks"),
            row("volume",                 "click: slider, wheel: level"),
            row("bar",                    "click or drag the line to seek"),
            row("title",                  "now playing (dbl-click a cover too)"),
            row("\uf03a",                     "queue (click again: back)"),
            row("home, library",          "click again: refresh"),
            row("lyrics",                 "click a line: jump there"),
            row(", .",                    "now playing: lyrics earlier / later"),
            row("queue",                  "drag a row to move it"),
            text(""),
#ifdef _WIN32
            hbox({ text("  "), text("\u21e7 = shift or alt") | dim }),
#else
            hbox({ text("  "), text("\u21e7 = shift or \u2325 option") | dim }),
#endif
        });

        return vbox({
            text(""),
            hbox({ filler(), text("Keys") | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)), filler() }),
            text(""),
            hbox({ left, text("   "), right }),
            text(""),
        }) | borderRounded | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY));
    };

    ui = Renderer(ui, [ui, &show_help, help_sheet, &ctx, &screen, &volume_pop, &playback_bar_data, &prompt, &search_bar_data, search_bar] {
        // The held-h cheatsheet goes away once h is let go (repeats stop).
        if (help_held && std::chrono::steady_clock::now() - last_h > std::chrono::milliseconds(450)) {
            help_held = false;
            show_help = false;
        }

        Element base = ui->Render();

        // A notice ("Link copied", "Refreshing Home"...) normally shows in the
        // playback bar; with nothing playing there is no bar: a small toast at
        // the top right instead.
        if (!playback_bar_data.notice.empty() && playback_bar_data.box_progress.y_min < 0) {
            Element toast = hbox({ text(" "), text(playback_bar_data.notice) | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)), text(" ") })
                | borderStyled(BorderStyle::ROUNDED, ui::GetColor(ui::MColor::ACCENT_PRIMARY)) | ui::seeThrough;
            base = dbox({ base, vbox({ filler() | size(HEIGHT, EQUAL, 4), hbox({ filler(), toast, text("  ") }), filler() }) });
        }

        // Search suggestions, under the bar.
        auto& sb = search_bar_data;
        if (search_bar->Focused() && !sb.suggestions.empty() && sb.suggestions_for == sb.value && !sb.value.empty()) {
            /* A dropdown under the search bar, lined up with it: its icons
            under the bar's icon, the words under what you type. The page's
            own background (no block of colour over the terminal's), the
            picked row marked by the accent bar and bold, the rest of each
            suggestion (what it adds to what you typed) brighter. */
            const Color accent = ui::GetColor(ui::MColor::ACCENT_PRIMARY);
            const Color typed = ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY), dimc = ui::GetColor(ui::MColor::SEPARATOR_PRIMARY);
            auto lower = [](std::string t) { for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return t; };
            const std::string q = lower(sb.value);
            const int bar_w = sb.box.x_max - sb.box.x_min + 1;
            size_t longest = 0;
            for (const auto& t : sb.suggestions) longest = std::max(longest, t.size());
            const int W = std::clamp(static_cast<int>(longest) + 10, std::min(48, bar_w - 2), std::max(20, std::min(80, bar_w - 2)));

            sb.suggestion_boxes.assign(sb.suggestions.size(), Box{});
            Elements rows;
            for (size_t i = 0; i < sb.suggestions.size(); i++) {
                const std::string& t = sb.suggestions[i];
                const bool on = static_cast<int>(i) == sb.suggestion_index;
                const bool past = std::find(sb.history.begin(), sb.history.end(), t) != sb.history.end();
                const size_t n = lower(t).rfind(q, 0) == 0 ? q.size() : 0;     // the part you typed
                rows.push_back(hbox({
                    text(on ? "\u258e" : " ") | color(accent),
                    text(past ? "\uf1da" : "\uf002") | color(on ? accent : dimc),
                    text("  "),
                    text(t.substr(0, n)) | color(on ? ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY) : typed) | (on ? bold : nothing),
                    text(t.substr(n)) | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
                    filler(),
                }) | reflect(sb.suggestion_boxes[i]));
            }
            rows.push_back(hbox({
                filler(),
                text("\u2191\u2193 ") | color(typed), text("pick  ") | color(dimc),
                text("tab ") | color(typed), text("fill  ") | color(dimc),
                text("enter ") | color(typed), text("search ") | color(dimc),
            }));
            Element list = vbox(std::move(rows)) | size(WIDTH, EQUAL, W)
                | borderStyled(BorderStyle::ROUNDED, dimc);
            // A blank cell on each side: lines of the page don't join its border.
            list = hbox({ text(" "), list, text(" ") }) | ui::seeThrough;
            base = dbox({
                base,
                vbox({
                    // Right under the line beneath the bar, its border under the bar's start.
                    filler() | size(HEIGHT, EQUAL, std::max(0, sb.box.y_max + 2)),
                    hbox({ filler() | size(WIDTH, EQUAL, std::max(0, sb.box.x_min)), list, filler() }),
                    filler(),
                }),
            });
        }

        if (volume_pop.open && playback_bar_data.box_volume.x_min >= 0) {
            /* A slim slider: the played part in the accent colour, a knob, the
            rest as a thin line; light border, the terminal's own background.
            */
            constexpr int BAR = 22;
            const int v = std::clamp(playback_bar_data.volume, 0, 100);
            const int knob = (v * (BAR - 1) + 50) / 100;
            const Color accent = ui::GetColor(ui::MColor::ACCENT_PRIMARY);
            const std::string icon = v == 0 ? "\U000f075f" : v < 34 ? "\U000f057f" : v < 67 ? "\U000f0580" : "\U000f057e";

            std::string played, rest;
            for (int i = 0; i < knob; i++) played += "\u2501";
            for (int i = knob + 1; i < BAR; i++) rest += "\u2500";

            Element pop = hbox({
                text(" "),
                text(icon) | color(v == 0 ? ui::GetColor(ui::MColor::SEPARATOR_PRIMARY) : ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | reflect(volume_pop.mute),
                text("  "),
                hbox({
                    text(played) | color(accent),
                    text("\u25cf") | color(Color::White),
                    text(rest) | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)),
                }) | reflect(volume_pop.bar),
                text("  "),
                text(std::to_string(v)) | align_right | size(WIDTH, EQUAL, 3) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)),
                text(" "),
            }) | borderStyled(BorderStyle::ROUNDED, ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)) | ui::seeThrough;

            constexpr int W = BAR + 12;
            const Box& vb = playback_bar_data.box_volume;
            const int x = std::max(0, std::min(vb.x_max - W + 2, screen.dimx() - W - 1));
            // Just above the playback bar's progress line, not over it.
            const Box& pb = playback_bar_data.box_progress;
            const int top_of_bar = pb.y_min >= 0 ? std::min(pb.y_min, vb.y_min) : vb.y_min;
            const int y = std::max(0, top_of_bar - 3);
            base = dbox({
                base,
                vbox({
                    filler() | size(HEIGHT, EQUAL, y),
                    hbox({ filler() | size(WIDTH, EQUAL, x), pop, filler() }),
                    filler(),
                }),
            });
        }

        if (!ctx.open) ctx_bar = -1;
        if (ctx.open) {
            // As wide as its longest line, not a fixed block: less of the page
            // hidden behind it.
            int W = 16;
            for (const auto& it : ctx.items) W = std::max(W, static_cast<int>(ftxui::string_width(it.label) + ftxui::string_width(it.hint)) + 11);
            // The title fits the items' width (trimmed), it doesn't widen the menu.
            W = std::min(W, 40);
            /* The app's style: a red bar and red icon on the picked row, its
            label bold white; the title dim on top. Under it, what the menu
            covers shows through, darkened (see see_through.hpp): a cover
            stays a cover, a see-through terminal stays see-through. */
            const Color accent = ui::GetColor(ui::MColor::ACCENT_PRIMARY);
            // The red bar glides to the row picked (the label lights at once).
            double& bar = ctx_bar;
            // A menu just opened: the bar starts on its row.
            if (bar < 0 || ctx.hover < 0) bar = std::max(0, ctx.hover);
            ui::anim::approach(bar, std::max(0, ctx.hover), 0.55, 0.3);
            const int bar_row = ctx.hover < 0 ? -1 : static_cast<int>(std::lround(bar));
            Elements rows;
            if (!ctx.title.empty())
                rows.push_back(hbox({ text("  "), text(utils::trimSuffix(ctx.title, W - 5, "\u2026")) | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)), filler() }));
            for (size_t i = 0; i < ctx.items.size(); i++) {
                const auto& it = ctx.items[i];
                const bool on = static_cast<int>(i) == ctx.hover;
                Element row = hbox({
                    text(static_cast<int>(i) == bar_row ? "\u258e" : " ") | color(accent),
                    text(" "),
                    text(it.icon) | size(WIDTH, EQUAL, 3) | color(on ? accent : ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)),
                    text(utils::trimSuffix(it.label, std::max(4, W - 8 - static_cast<int>(ftxui::string_width(it.hint))), "\u2026"))
                        | (on ? bold : nothing) | color(on ? Color(Color::White) : ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
                    filler(),
                    text(it.hint) | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)),
                    text("  "),
                });
                rows.push_back(row | reflect(ctx.boxes[i]));
            }

            Element menu = ui::seeThrough(vbox(std::move(rows))
                | size(WIDTH, EQUAL, W)
                | borderStyled(BorderStyle::ROUNDED, ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)));

            // Keep it on screen near the pointer.
            const int h = static_cast<int>(ctx.items.size()) + (ctx.title.empty() ? 0 : 1) + 2;
            const int x = std::max(0, std::min(ctx.x, screen.dimx() - W - 3));
            const int y = std::max(0, std::min(ctx.y, screen.dimy() - h - 1));
            base = dbox({
                base,
                vbox({
                    filler() | size(HEIGHT, EQUAL, y),
                    hbox({ filler() | size(WIDTH, EQUAL, x), menu, filler() }),
                    filler(),
                }),
            });
        }

        if (prompt.open) {
            const Color accent = ui::GetColor(ui::MColor::ACCENT_PRIMARY);
            // Title, then an underlined field with a thin cursor; quiet hints.
            Element box = vbox({
                text(""),
                hbox({ text("  "), text(prompt.title) | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)), filler() }),
                text(""),
                hbox({ text("  "), text(prompt.value) | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)), text("\u258f") | color(accent), filler() }),
                hbox({ text("  "), separatorLight() | color(accent) | flex, text("  ") }),
                text(""),
                hbox({ filler(), text("enter") | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)), text(" ok   ") | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)),
                       text("esc") | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)), text(" cancel  ") | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)) }),
            }) | size(WIDTH, EQUAL, 56)
               | borderStyled(BorderStyle::ROUNDED, ui::GetColor(ui::MColor::SEPARATOR_PRIMARY))
               | ui::seeThrough | center;
            base = dbox({ base, box });
        }

        if (!show_help) return base;
        return dbox({ base, help_sheet() | ui::seeThrough | center });
    });

    /* Quitting must be instant and clean
    - The terminal is restored once the loop returns
    - Stop the sound, flush the logs, and leave without running destructors:
      background work (a playlist's worth of queued tracks, cover downloads on
      detached threads) would otherwise keep the process alive with music
      playing, or touch mutexes torn down during exit and abort
    */
    auto finish = [&] {
        // The tab on screen at quit: where it was, for the next launch.
        if (is_tab(nav_current.state)) {
            std::string sub;
            if (nav_current.state == ui::State::LIBRARY) sub = library_filter();
            if (nav_current.state == ui::State::EXPLORE) { std::lock_guard lock(player.state_mutex); sub = player.state.explore_tab + "|" + player.state.explore_title; }
            tab_views[static_cast<int>(nav_current.state)] = { nav_current.view.anchor, nav_current.view.line, nav_current.active, sub };
            save_views();
        }
        player.saveSession();
        player.detachUI();
        player.pause();
        spdlog::shutdown();
        std::_Exit(0);
    };

    // Nerd Font glyphs swapped for plain symbols where the font lacks them (ICONS).
    ui = Renderer(ui, [inner = ui] { return ui::withIcons(inner->Render()); });

    // moroder search <words>: start on those results.
    if (!start_query.empty()) {
        search_bar_data.value = start_query;
        search_bar_data.onSearch(start_query);
    }

#ifdef __APPLE__
    /* The UI gets its own thread and blocks on input like on Linux; the main
    thread parks in its run loop, where macOS delivers media keys and Control
    Center commands. Before, one thread polled both every 16 ms, which added
    up to 16 ms to every keypress and woke the CPU 60 times a second.
    */
    std::thread([&] {
        screen.Loop(ui);
        finish();
    }).detach();

    services::runMainLoop();
#else
    screen.Loop(ui);
    finish();
#endif
}

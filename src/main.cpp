#include "../include/ui/components/playback_bar.hpp"
#include "../include/ui/components/search_bar.hpp"
#include "../include/ui/components/sidebar.hpp"
#include "../include/ui/components/content.hpp"
#include "../include/ui/constants/states.hpp"
#include "../include/ui/constants/colors.hpp"

#include "../include/services/player.hpp"
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
#include <sys/stat.h>
#include <filesystem>
#include <algorithm>
#include <iostream>
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

int main(int argc, char *argv[]) {
    Config::app_name = APP_NAME;

    utils::ensure_dir(utils::resolve_path(MORODER_LOG_PATH));

    auto logger = spdlog::basic_logger_mt(APP_NAME, utils::resolve_path(MORODER_LOG_PATH).string() + "/" + APP_NAME + ".log", true);
    logger->flush_on(spdlog::level::info);

    spdlog::set_default_logger(logger);

    /* Subcommands run before anything else is built.
    - setup has to work on a machine with no config and no session, so it must
      not go through Player, which needs both
    */
    const std::string command = argc > 1 ? argv[1] : "";

    if (command == "setup") {
        return setup::run(argc, argv);
    }

    /* Anonymous mode
    - Everything except the account works without a session: search, radio and
      playback all go out unauthenticated
    - The library and the home page have nothing to fill themselves with, and
      every request is slower, so this is opt-in rather than a silent fallback
    */
    const bool anonymous = command == "--anonymous" || command == "-a";
    Config::anonymous = anonymous;

    if (command == "--help" || command == "-h") {
        std::cout << "usage: " << APP_NAME << " [command]\n\n"
                  << "  setup         sign in to YouTube Music and fill in the config\n"
                  << "  --anonymous   run without an account, -a for short\n"
                  << "  --help        this text\n\n"
                  << "Run with no arguments to start the player.\n";
        return 0;
    }

    if (!command.empty() && !anonymous) {
        std::cerr << "unknown command: " << command << "\n"
                  << "try `" << APP_NAME << " --help`\n";
        return 2;
    }

    /* A missing browser.json means the user has not run setup, and every
    library request would fail with an auth error they cannot act on. Say so
    once, here, rather than letting the library page explain it later.
    */
    if (!anonymous && !fs::exists(Config::get().YTM_COOKIES_PATH / "browser.json")) {
        std::cerr << "No YouTube Music session found.\n"
                  << "Run `" << APP_NAME << " setup` to sign in, or `" << APP_NAME
                  << " --anonymous` to run without an account.\n";
        return 1;
    }

    if (anonymous) {
        spdlog::info("MAIN: starting without an account");
    }

    services::Player player(APP_NAME, APP_NAME_HUMAN, 1481401025964540125);
    if(!player.isInitialized()) {
        return 1;
    }

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


    player.isLoggedIn();
    player.getHome();
    player.getLibraryPlaylists();
    player.getLibraryAlbums();
    player.getLibraryArtists();
    player.getLibraryPodcasts();
    player.getLibrarySongs();

    int spinner_frame = 0;
    int sidebar_selected = 0;

    bool sidebar_hidden  = false;
    bool sidebar_focused = false;
    bool audio_playing = false;

    auto current_state = ui::State::HOME;

    ui::SearchBarData search_bar_data = {};
    auto search_bar = ui::SearchBar(&search_bar_data);

    // Search is entered on purpose and left after Enter / Esc (see FocusGate).
    bool search_active = false;
    int content_column = 1;     // 0 = search bar, 1 = page content
    auto search_gate = Make<FocusGate>(search_bar, &search_active);

    ui::PlaybackData playback_bar_data = {};
    auto playback_bar = ui::PlaybackBar(&playback_bar_data);

    player.setOnPositionTick([&screen, &playback_bar_data](uint64_t position, uint64_t duration) {
        playback_bar_data.position = position / 1000000ULL;
        playback_bar_data.duration = duration / 1000000ULL;

        // Remove two seconds to account for UI refresh period
        // This is just a fix so that the UI progress bar actually reaches song end.
        // duration is unsigned: guard before subtracting, or it wraps around.
        if (duration <= services::Player::LOOP_REWIND_EPSILON) return;
        duration -= services::Player::LOOP_REWIND_EPSILON;

        int progress = (position * 100) / duration;
        progress = std::clamp(progress, 0, 100);

        playback_bar_data.progress = progress;
        screen.PostEvent(Event::Custom);
    });

    player.setOnStreamStart([&screen, &playback_bar_data] {
        playback_bar_data.progress = 0;
        playback_bar_data.position = 0;
        playback_bar_data.duration = 0;
        screen.PostEvent(Event::Custom);
    });

    player.setOnStreamEnd([&screen, &playback_bar_data] {
        playback_bar_data.progress = 0;
        playback_bar_data.position = 0;
        playback_bar_data.duration = 0;
        screen.PostEvent(Event::Custom);
    });

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
                auto container = make_streamable(music::Album{});
                player.queue(container, !should_queue, !should_queue);
                queued = true;

            } else if constexpr (std::is_same_v<T, music::EpisodeRef>) {
                auto streamable = make_streamable(music::Episode{});
                player.queue(streamable, !should_queue, !should_queue);
                queued = true;

            } else if constexpr (std::is_same_v<T, music::PlaylistRef>) {
                auto container = make_streamable(music::Playlist{});
                player.queue(container, !should_queue, !should_queue);
                queued = true;
            }

        }, result.data);

        if (!queued) {
            spdlog::info("ITEM: nothing queued for result type, " + result.resultType);
            return true;
        }

        current_state = ui::State::QUEUE;

        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));

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
            main_content
        }, &content_column),
    });

    auto layout = Container::Vertical({
        main_content_container,
        playback_bar
    });

    auto open_search = [&] {
        search_active = true;
        content_column = 0;
        search_bar->TakeFocus();
    };

    // Back to the page; if it has nothing focusable yet (results loading),
    // focus lands on it as soon as it does.
    auto close_search = [&] {
        search_active = false;
        content_column = 1;
        if (main_content->Focusable()) main_content->TakeFocus();
    };

    sidebar_data.onHome = [&current_state, &main_content_items, &main_content, &screen] {
        spdlog::info("SIDEBAR: Home pressed");

        current_state = ui::State::HOME;

        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));

        screen.PostEvent(Event::Custom);
    };

    sidebar_data.onLibrary = [&current_state, &main_content_items, &main_content, &screen] {
        spdlog::info("SIDEBAR: Library pressed");
        current_state = ui::State::LIBRARY;

        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));

        screen.PostEvent(Event::Custom);
    };

    search_bar_data.onSearch = [&current_state, &player, &main_content_items, &main_content, &screen, &close_search](const std::string& value) {
        spdlog::info("SEARCHBAR: enter pressed with value, " + value);
        close_search();
        current_state = ui::State::SEARCH;

        player.search(value);

        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));

        screen.PostEvent(Event::Custom);
    };

    auto ui = Renderer(layout, [&] {
        sidebar_focused = sidebar->Focused();

        services::Player::PlayerState state_copy;
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

            audio_playing = state_copy.flags["audio"] == services::Player::Flags::Ongoing;
        }

        spinner_frame++;

        ui::getSidebarData(&state_copy, &sidebar_data);
        ui::getPlaybackData(&state_copy, &playback_bar_data, screen.dimx());

        if(current_state == ui::State::HOME) {
            ui::buildHome(&state_copy, main_content_items, main_content, on_item_press, [&player, &main_content, &main_content_items, &screen]{
                player.getHome();

                if(player.state.flags["library_playlists"] != services::Player::Flags::Done) {
                    player.getLibraryPlaylists();
                }

                main_content_items.clear();
                main_content->DetachAllChildren();
                main_content->Add(Renderer([] { return emptyElement(); }));

                screen.PostEvent(Event::Custom);
            });

        } else if(current_state == ui::State::SEARCH && state_copy.flags["search"] == services::Player::Flags::Done) {
            ui::buildSearch(&state_copy, main_content_items, main_content, on_item_press, [&player, &search_bar_data, &main_content, &main_content_items, &screen] {
                player.search(search_bar_data.value);

                main_content_items.clear();
                main_content->DetachAllChildren();
                main_content->Add(Renderer([] { return emptyElement(); }));

                screen.PostEvent(Event::Custom);
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

        } else if(current_state == ui::State::LIBRARY) {
            int library_rows = std::max(1, (screen.dimy() - 12) / ui::GRID_TILE_HEIGHT);

            ui::buildLibrary(&state_copy, main_content_items, main_content, library_rows, search_bar_data.value, on_chip_press, on_item_press,
                [&player, &screen] {
                    player.retryLibrary();
                    screen.PostEvent(Event::Custom);
                });
        }

        return vbox({
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

                        sidebar->Render() | yframe
                    }) | size(WIDTH, EQUAL, 30)
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
                        text(" "),
                        [&] {
                            const std::string error_key =
                                  current_state == ui::State::HOME    ? "home"
                                : current_state == ui::State::SEARCH  ? "search"
                                : current_state == ui::State::LIBRARY ? "library"
                                : current_state == ui::State::PLAYLIST ? "open_playlist"
                                                                      : "";

                            const bool has_error = !error_key.empty() && state_copy.errors.count(error_key) > 0;

                            /* The signed-out notice on home is an ErrorBox too,
                            and wants the same centred, flexed layout even though
                            there is no entry in errors for it.
                            */
                            const bool notice = current_state == ui::State::HOME
                                && state_copy.flags["is_logged_in"] == services::Player::Flags::Done
                                && !state_copy.is_logged_in;

                            return (has_error || notice)
                                ? main_content->Render() | yframe | flex
                                : current_state != ui::State::QUEUE
                                    ? main_content->Render() | yframe | yflex
                                    : main_content->Render() | xflex;
                        }(),
                    }) | yflex,

                    ((main_content_items.empty() || sidebar_data.is_loading) && state_copy.is_logged_in) ?
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

            (playback_bar_data.is_playing  && state_copy.flags["paused"] == services::Player::Flags::False) ||
            (!playback_bar_data.is_playing && state_copy.flags["paused"] == services::Player::Flags::True)
                ? playback_bar->Render()
                : emptyElement(),

            Config::get().EXTRA_BOTTOM_PADDING ? text("") : emptyElement()
        });
    });

    bool show_help = false;

    /* Context menu (right-click)
    - Actions for whatever the pointer was over; each action replays the key
      that does the same thing, so mouse and keyboard can never disagree
    */
    struct ContextMenu {
        bool open = false;
        int x = 0, y = 0;
        int hover = 0;
        std::vector<std::pair<std::string, std::function<void()>>> items;
        std::vector<Box> boxes;
    } ctx;

    // Double-click detection.
    auto last_click = std::chrono::steady_clock::time_point{};
    int last_click_x = -1, last_click_y = -1;

    // Content entry that has focus (a row, a carousel, the filters...), if any.
    auto focused_entry = [&]() -> ui::ContentEntry* {
        for (auto& item : main_content_items)
            if (item.component && item.component->Focused()) return &item;
        return nullptr;
    };

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

            auto in = [&](const Box& b) { return b.Contain(m.x, m.y); };

            if(wheel && in(playback_bar_data.box_volume)) {
                player.changeVolume(m.button == Mouse::WheelUp ? cfg.VOLUME_STEP : -cfg.VOLUME_STEP);
                return true;
            }

            if(click) {
                const Box& p = playback_bar_data.box_progress;
                if(in(p) && p.x_max > p.x_min) {
                    player.seekToFraction(double(m.x - p.x_min) / double(p.x_max - p.x_min));
                    return true;
                }
                if(in(playback_bar_data.box_prev))   { player.skipBackward();      return true; }
                if(in(playback_bar_data.box_play))   { player.togglePause();       return true; }
                if(in(playback_bar_data.box_next))   { player.skipForward();       return true; }
                if(in(playback_bar_data.box_like))   { player.toggleLikeCurrent(); return true; }
                if(in(playback_bar_data.box_loop))   { player.cycleLoop();         return true; }
                if(in(playback_bar_data.box_volume)) {
                    // Mute, and unmute back to the level it had.
                    static int before_mute = 100;
                    const int now = playback_bar_data.volume;
                    if (now > 0) { before_mute = now; player.changeVolume(-now); }
                    else         { player.changeVolume(before_mute); }
                    return true;
                }
            }

            // An open context menu takes the mouse first.
            if(ctx.open) {
                if(m.motion == Mouse::Released || m.button == Mouse::None) {
                    for(size_t i = 0; i < ctx.boxes.size(); i++) if(in(ctx.boxes[i])) ctx.hover = static_cast<int>(i);
                    return true;
                }
                if(click) {
                    for(size_t i = 0; i < ctx.boxes.size(); i++) {
                        if(in(ctx.boxes[i])) {
                            ctx.open = false;
                            ctx.items[i].second();
                            return true;
                        }
                    }
                }
                ctx.open = false;     // click elsewhere closes it
                return true;
            }

            const bool right_click = m.button == Mouse::Right && m.motion == Mouse::Pressed;

            if(right_click) {
                // Focus what is under the pointer, as a left press would.
                Event left = Event::Mouse(event.input(), m);
                left.mouse().button = Mouse::Left;
                if(!layout->OnEvent(left) || search_bar->Focused()) return true;

                ctx.items.clear();
                auto key = [&](const std::string& bind) { return [&, bind] { layout->OnEvent(Config::keyEvent(bind)); }; };

                if(sidebar->Focused()) {
                    const int sel = sidebar_selected;
                    if(sel > 2 && static_cast<size_t>(sel - 3) < sidebar_data.results.size()) {
                        const music::Playlist pl = sidebar_data.results[sel - 3];
                        ctx.items.push_back({"\uf07c  Open", key(cfg.KEY_PLAY_NOW)});
                        ctx.items.push_back({"\uf04b  Play", [&player, pl] {
                            player.queue(std::static_pointer_cast<music::IStreamableContainer>(std::make_shared<music::Playlist>(pl)), true, true);
                        }});
                        ctx.items.push_back({"\uf067  Add to queue", key(cfg.KEY_ADD_TO_QUEUE)});
                    }
                } else if(ui::ContentEntry* e = focused_entry()) {
                    const std::string& c = e->category;
                    const bool special = c.rfind("__library_filters__", 0) == 0 || c.rfind("__error__", 0) == 0 || c.rfind("__playlist__", 0) == 0;
                    if(!special) {
                        const bool track = c.rfind("__track__", 0) == 0;
                        ctx.items.push_back({track ? "\uf04b  Play from here" : "\uf04b  Play", key(cfg.KEY_PLAY_NOW)});
                        if(current_state == ui::State::QUEUE) {
                            ctx.items.push_back({"\uf00d  Remove from queue", key(cfg.KEY_REMOVE_FROM_QUEUE)});
                        } else {
                            ctx.items.push_back({"\uf067  Add to queue", key(cfg.KEY_ADD_TO_QUEUE)});
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

            const bool handled = layout->OnEvent(event);

            /* Click model, like a desktop music app:
            - single click selects; on navigation (sidebar, library filters) it
              also opens, since there is nothing else to do with a selection
            - double-click plays
            - buttons that act by themselves (Play, Retry) need nothing more
            */
            if(click && handled && !search_bar->Focused()) {
                bool navigation = sidebar->Focused();
                if(ui::ContentEntry* e = focused_entry()) {
                    navigation = navigation || e->category.rfind("__library_filters__", 0) == 0;
                    const bool self_acting = e->category.rfind("__error__", 0) == 0 || e->category.rfind("__playlist__", 0) == 0;
                    if(self_acting) navigation = false, double_click = false;
                }
                if(navigation || double_click) layout->OnEvent(Event::Return);
            }

            // Wheel over something that doesn't scroll itself: move the focus.
            if(wheel && !handled) {
                layout->OnEvent(m.button == Mouse::WheelUp ? Event::ArrowUp : Event::ArrowDown);
            }

            return true;
        }

        if(ctx.open) {
            if(event == Event::Escape)    { ctx.open = false; return true; }
            if(event == Event::ArrowDown) { ctx.hover = (ctx.hover + 1) % static_cast<int>(ctx.items.size()); return true; }
            if(event == Event::ArrowUp)   { ctx.hover = (ctx.hover + static_cast<int>(ctx.items.size()) - 1) % static_cast<int>(ctx.items.size()); return true; }
            if(event == Event::Return)    { ctx.open = false; ctx.items[ctx.hover].second(); return true; }
            return true;
        }

        // The help sheet is modal: it only listens for its own close keys.
        if(show_help) {
            if(event == Event::Escape || Config::isKey(event, cfg.KEY_HELP) || Config::isKey(event, cfg.KEY_QUIT)) {
                show_help = false;
            }
            return !event.is_mouse();
        }

        if(search_bar->Focused()) {
            // Esc leaves the search bar for the page below.
            if(event == Event::Escape) {
                close_search();
                return true;
            }
            return false;
        }

        // Esc elsewhere goes back to Home, like a browser's back.
        if(event == Event::Escape && current_state != ui::State::HOME) {
            sidebar_data.onHome();
            return true;
        }

        if(Config::isKey(event, cfg.KEY_HELP)) {
            show_help = true;
            return true;
        }

        if(Config::isKey(event, cfg.KEY_SEEK_BACKWARD)) {
            player.seekBy(-static_cast<int64_t>(cfg.SEEK_SECONDS) * 1000000LL);
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

        if(Config::isKey(event, cfg.KEY_LIKE)) {
            player.toggleLikeCurrent();
            return true;
        }

        if(Config::isKey(event, cfg.KEY_QUIT)) {
            screen.Exit();
            return true;
        }


        if(Config::isKey(event, cfg.KEY_QUEUE_VIEW) && audio_playing) {
            current_state = ui::State::QUEUE;
            return true;
        }

        if(Config::isKey(event, cfg.KEY_TOGGLE_SIDEBAR)) {
            sidebar_hidden = !sidebar_hidden;

            if(sidebar_hidden) {
                sidebar->Detach();
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
        auto key = [](const std::string& k) {
            return text(k.empty() ? "-" : k) | bold | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)) | size(WIDTH, EQUAL, 9);
        };
        auto row = [&](const std::string& k, const std::string& what) {
            return hbox({ text("  "), key(k), text(what) | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)), text("  ") });
        };
        auto heading = [](const std::string& t) {
            return hbox({ text("  "), text(t) | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)) });
        };

        return vbox({
            text(""),
            hbox({ filler(), text("Keybinds") | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)), filler() }),
            text(""),
            heading("Playback"),
            row(cfg.KEY_TOGGLE_PAUSE,  "play / pause"),
            row(cfg.KEY_SKIP_FORWARD,  "next track"),
            row(cfg.KEY_SKIP_BACKWARD, "previous track"),
            row(cfg.KEY_SEEK_FORWARD,  "forward " + std::to_string(cfg.SEEK_SECONDS) + " s"),
            row(cfg.KEY_SEEK_BACKWARD, "back " + std::to_string(cfg.SEEK_SECONDS) + " s"),
            row(cfg.KEY_VOLUME_UP,     "volume up"),
            row(cfg.KEY_VOLUME_DOWN,   "volume down"),
            row(cfg.KEY_TOGGLE_LOOP,   "loop: off, queue, track"),
            row(cfg.KEY_LIKE,          "like / unlike current track"),
            text(""),
            heading("On the selected item"),
            row(cfg.KEY_PLAY_NOW,          "play now (starts a radio)"),
            row(cfg.KEY_ADD_TO_QUEUE,      "add to queue"),
            row(cfg.KEY_REMOVE_FROM_QUEUE, "remove from queue"),
            text(""),
            heading("Navigation"),
            row("arrows",                 "move around"),
            row(cfg.KEY_FOCUS_SEARCH + " /",  "search"),
            row("esc",                    "leave search / back to home"),
            row("click",                  "select (sidebar: open)"),
            row("dbl-click",              "play"),
            row("right-click",            "menu: play, queue, open"),
            row(cfg.KEY_QUEUE_VIEW,       "queue"),
            row(cfg.KEY_TOGGLE_SIDEBAR,   "toggle sidebar"),
            row(cfg.KEY_HELP,             "this help"),
            row(cfg.KEY_QUIT,             "quit"),
            text(""),
        }) | borderRounded | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY));
    };

    ui = Renderer(ui, [ui, &show_help, help_sheet, &ctx, &screen] {
        Element base = ui->Render();

        if (ctx.open) {
            Elements rows;
            for (size_t i = 0; i < ctx.items.size(); i++) {
                Element row = text(" " + ctx.items[i].first + "  ");
                row = static_cast<int>(i) == ctx.hover
                    ? row | bold | inverted
                    : row | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY));
                rows.push_back(row | reflect(ctx.boxes[i]));
            }
            Element menu = vbox(std::move(rows)) | borderRounded | clear_under;

            // Keep it on screen near the pointer.
            const int x = std::max(0, std::min(ctx.x, screen.dimx() - 28));
            const int y = std::max(0, std::min(ctx.y, screen.dimy() - static_cast<int>(ctx.items.size()) - 3));
            base = dbox({
                base,
                vbox({
                    filler() | size(HEIGHT, EQUAL, y),
                    hbox({ filler() | size(WIDTH, EQUAL, x), menu, filler() }),
                    filler(),
                }),
            });
        }

        if (!show_help) return base;
        return dbox({ base, help_sheet() | clear_under | center });
    });

#ifdef __APPLE__
    {
        /* Media keys and Control Center commands arrive on the main thread's run
        loop, so the UI loop yields to it between frames instead of blocking.
        Scoped so the terminal is restored as soon as the UI quits.
        */
        ftxui::Loop loop(&screen, ui);
        while (!loop.HasQuitted()) {
            loop.RunOnce();
            services::pumpMainThread(0.016);
        }
    }
#else
    screen.Loop(ui);
#endif

    /* Quitting must be instant and clean
    - The terminal is already restored (the loop's scope ended)
    - Stop the sound, flush the logs, and leave without running destructors:
      background work (a playlist's worth of queued tracks, cover downloads on
      detached threads) would otherwise keep the process alive with music
      playing, or touch mutexes torn down during exit and abort
    */
    player.detachUI();
    player.pause();
    spdlog::shutdown();
    std::_Exit(0);
}

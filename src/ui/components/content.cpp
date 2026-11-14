#include "../../../include/ui/components/content.hpp"
#include "../../../include/ui/components/error.hpp"
#include "../../../include/ui/constants/colors.hpp"

#include "../../../include/config/config.hpp"

#include "spdlog/spdlog.h"
#include "image_view.hpp"
#include "../../../include/ui/cover_cache.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <type_traits>
#include <string>

namespace {

/* homeCategoryRank
- Shelves named in HOME_ORDER come first, in that order; everything else
  follows in the order YouTube Music sent it, as the config documents
*/
int homeCategoryRank(const std::vector<std::string>& sent_order, const std::string& category) {
    const auto& order = Config::get().HOME_ORDER;
    const std::string key = utils::lower(category);

    for (size_t i = 0; i < order.size(); i++) {
        if (order[i] == key) return static_cast<int>(i);
    }

    for (size_t i = 0; i < sent_order.size(); i++) {
        if (sent_order[i] == category) return static_cast<int>(order.size() + i);
    }

    return std::numeric_limits<int>::max();
}

/* activeLibraryFilter
- Chips are exclusive, so at most one is ever enabled
- Empty means "no filter", which shows everything except songs
*/
std::string activeLibraryFilter(ui::ChipsData* chips) {
    for (const auto& chip : chips->entries) {
        if (chip.enabled) return chip.id;
    }

    return "";
}

/* librarySignature
- Identifies what the library grid is currently showing
- Rebuilding the Grid component is only needed when this changes, which is
  the user picking a filter, typing in the search bar, or an async library
  fetch landing
*/
std::string librarySignature(services::Player::PlayerState* state, const std::string& filter, const std::string& query) {
    return "__library_grid__:" + filter
        + ":" + query
        + ":" + std::to_string(state->library_playlists.size())
        + ":" + std::to_string(state->library_albums.size())
        + ":" + std::to_string(state->library_songs.size())
        + ":" + std::to_string(state->library_artists.size())
        + ":" + std::to_string(state->library_podcasts.size());
}

}

using namespace ftxui;

int ui::anim_frame = 0;
bool ui::suppress_click = false;

namespace {
class MouseGateBase : public ftxui::ComponentBase {
public:
    MouseGateBase(ftxui::Component child, std::shared_ptr<bool> shown) : shown_(std::move(shown)) { Add(std::move(child)); }
    bool OnEvent(ftxui::Event e) override {
        if (e.is_mouse() && !*shown_) return false;
        return ChildAt(0)->OnEvent(e);
    }
private:
    std::shared_ptr<bool> shown_;
};
}

ftxui::Component ui::MouseGate(ftxui::Component child, std::shared_ptr<bool> shown) {
    return ftxui::Make<MouseGateBase>(std::move(child), std::move(shown));
}

// Video id of the track playing now, for the "now playing" marks in lists.
static std::string playing_id;

static void updatePlayingId(services::Player::PlayerState* state) {
    playing_id.clear();
    if (auto s = std::dynamic_pointer_cast<music::Song>(state->current)) playing_id = s->ref.id;
}

namespace {

bool buildError(services::Player::PlayerState* state, const std::string& key, std::deque<ui::ContentEntry>& main_content_items, ftxui::Component main_content, std::function<void()> on_retry) {
    auto error = state->errors.find(key);

    if (error == state->errors.end()) {
        if (!main_content_items.empty() && main_content_items.front().category.rfind("__error__", 0) == 0) {
            spdlog::info("CONTENT: {} error cleared, rebuilding the page", key);

            main_content_items.clear();
            main_content->DetachAllChildren();
            main_content->Add(Renderer([] { return emptyElement(); }));
        }

        return false;
    }

    const std::string id = "__error__" + error->second.message;

    if (main_content_items.size() == 1 && main_content_items.front().category == id) {
        return true;
    }

    /* Guarded by the identity check above, so this fires when the box appears
    or its message changes, not on every render
    */
    spdlog::warn("CONTENT: showing {} error, {} (retryable: {})", key, error->second.message, error->second.retryable);

    main_content_items.clear();
    main_content->DetachAllChildren();
    main_content->Add(Renderer([] { return emptyElement(); }));

    main_content_items.push_back(ui::ContentEntry{});
    ui::ContentEntry& item = main_content_items.back();

    item.category = id;
    item.selected = -1;
    item.focused = false;
    item.component = ui::ErrorBox(error->second.message, error->second.retryable ? on_retry : nullptr);

    main_content->DetachAllChildren();
    main_content->Add(item.component);
    item.component->TakeFocus();

    return true;
}

}

/* buildSignedOut
- Home is the only page that mentions being signed out: it is the one built
  entirely from the account, so without one there is nothing to show
- Gated on the login check having finished. is_logged_in starts false, so
  without that gate the notice flashes up on every start before the check lands
- No retry button, because signing in happens in a terminal, not here
*/
bool buildSignedOut(services::Player::PlayerState* state, std::deque<ui::ContentEntry>& main_content_items, ftxui::Component main_content) {
    static const std::string id = "__signed_out__";

    const bool checked = state->flags["is_logged_in"] == services::Player::Flags::Done;

    if (!checked || state->is_logged_in) {
        if (!main_content_items.empty() && main_content_items.front().category == id) {
            spdlog::info("CONTENT: signed in, rebuilding home");

            main_content_items.clear();
            main_content->DetachAllChildren();
            main_content->Add(Renderer([] { return emptyElement(); }));
        }

        return false;
    }

    if (main_content_items.size() == 1 && main_content_items.front().category == id) {
        return true;
    }

    spdlog::info("CONTENT: home is showing the signed out notice");

    main_content_items.clear();
    main_content_items.push_back(ui::ContentEntry{});

    ui::ContentEntry& item = main_content_items.back();

    item.category = id;
    item.selected = -1;
    item.focused = false;

    /* Deliberately not focused: the box has no button, and taking focus would
    pull it away from the sidebar and the search bar, which both still work.
    */
    item.component = ui::ErrorBox(
        "Search and playback still work, but every request has to go out "
        "anonymously, which makes them slower. "
        "Run 'moroder setup' in a terminal to sign in.",
        nullptr,
        "Not signed in..."
    );

    main_content->DetachAllChildren();
    main_content->Add(item.component);

    return true;
}

void ui::buildHome(services::Player::PlayerState* state, std::deque<ContentEntry>& main_content_items, ftxui::Component main_content, std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_press, std::function<void()> on_retry) {
    if (buildError(state, "home", main_content_items, main_content, on_retry)) {
        return;
    }

    // After buildError, so a genuine home failure still gets its retry button.
    if (buildSignedOut(state, main_content_items, main_content)) {
        return;
    }

    /* A refreshed page (after the cached one) may have dropped some shelves.
    Components hold raw pointers into their ContentEntry, and erasing from the
    middle of a deque moves the others, so rebuild everything instead.
    */
    const bool stale = std::any_of(main_content_items.begin(), main_content_items.end(), [&](const ContentEntry& e) {
        return !e.category.empty() && e.category.rfind("__", 0) != 0 && !state->home.count(e.category);
    });
    if (stale) {
        main_content_items.clear();
        main_content->DetachAllChildren();
        main_content->Add(Renderer([] { return emptyElement(); }));
    }

    for (auto& [category, _] : state->home) {
        const std::string& category_ref = category;

        auto already_exists = std::any_of(
            main_content_items.begin(), main_content_items.end(),
            [&](const ContentEntry& item) {
                return item.category == category_ref;
            }
        );

        if (already_exists) continue;

        main_content_items.push_back(ContentEntry{});
        ContentEntry& item = main_content_items.back();

        item.category = category;
        item.selected = 0;
        item.focused = false;

        if (utils::lower(category) == "quick picks") {
            ui::getGridData(state, category, &item.grid_data);
            item.component = MouseGate(Grid(&item.grid_data, &item.selected, &item.focused, 4, on_press), item.shown);
        } else {
            item.component = MouseGate(Carousel(&item.carousel_data, &item.selected, &item.focused, on_press), item.shown);
        }
    }

    std::vector<std::pair<int, ContentEntry*>> ordered;
    ordered.reserve(main_content_items.size());

    for (auto& item : main_content_items) {
        ordered.push_back({ homeCategoryRank(state->home_order, item.category), &item });
    }

    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    bool matches = (main_content->ChildCount() == ordered.size());
    if (matches) {
        for (size_t i = 0; i < ordered.size(); i++) {
            if (main_content->ChildAt(i) != ordered[i].second->component) { matches = false; break; }
        }
    }

    if (!matches && !ordered.empty()) {
        Component focused_child;
        for (auto& [rank, item] : ordered) {
            if (item->component->Focused()) focused_child = item->component;
        }

        main_content->DetachAllChildren();
        for (auto& [rank, item] : ordered) main_content->Add(item->component);

        // Nothing focused yet: start at the top. A stale index here made the
        // windowed render (main.cpp) begin a few shelves down, hiding the first.
        if (focused_child) main_content->SetActiveChild(focused_child);
        else main_content->SetActiveChild(ordered.front().second->component);
    }

    for (auto& item : main_content_items) {
        ui::getCarouselData(state, item.category, &item.carousel_data);
        item.focused = item.component->Focused();
    }
}

void ui::buildSearch(services::Player::PlayerState* state, std::deque<ContentEntry>& main_content_items, ftxui::Component main_content, std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_press, std::function<void()> on_retry) {
    if (buildError(state, "search", main_content_items, main_content, on_retry)) {
        return;
    }

    updatePlayingId(state);

    std::vector<std::string> incoming_categories;

    for (auto& search_result : state->search_results) {
        std::string category;
        std::visit([&](auto&& result) {
            using T = std::decay_t<decltype(result)>;
            if constexpr (std::is_same_v<T, music::SongRef>)          category = result.id;
            else if constexpr (std::is_same_v<T, music::AlbumRef>)    category = result.id;
            else if constexpr (std::is_same_v<T, music::ArtistRef>)   category = result.id + result.name;
            else if constexpr (std::is_same_v<T, music::PlaylistRef>) category = result.id;
            else if constexpr (std::is_same_v<T, music::PodcastRef>)  category = result.id + result.name;
            else if constexpr (std::is_same_v<T, music::EpisodeRef>)  category = result.id;
        }, search_result.data);
        incoming_categories.push_back(category);
    }

    bool same = (main_content_items.size() == incoming_categories.size());
    if (same) {
        for (size_t i = 0; i < main_content_items.size(); i++) {
            if (main_content_items[i].category != incoming_categories[i]) { same = false; break; }
        }
    }
    if (same) return;

    main_content_items.clear();
    main_content->DetachAllChildren();

    for (auto& search_result : state->search_results) {
        std::string category, top, bottom, url;

        std::visit([&](auto&& result) {
            using T = std::decay_t<decltype(result)>;
            if constexpr (std::is_same_v<T, music::SongRef>) {
                category = result.id;
                top      = result.title;
                bottom   = !result.artists.empty() ? result.artists[0].name : "";
                url      = result.thumbnail_small;

            } else if constexpr (std::is_same_v<T, music::AlbumRef>) {
                category = result.id;
                top      = result.title;
                bottom   = !result.artists.empty() ? result.artists[0].name : "";
                url      = result.thumbnail_small;

            } else if constexpr (std::is_same_v<T, music::ArtistRef>) {
                category = result.id + result.name;
                top      = result.name;
                bottom   = "Artist";
                url      = result.thumbnail_small;

            } else if constexpr (std::is_same_v<T, music::PlaylistRef>) {
                category = result.id;
                top      = result.title;
                bottom   = result.author;
                url      = result.thumbnail_small;

            } else if constexpr (std::is_same_v<T, music::PodcastRef>) {
                category = result.id + result.name;
                top      = result.name;
                bottom   = "Podcast";
                url      = result.thumbnail_small;

            } else if constexpr (std::is_same_v<T, music::EpisodeRef>) {
                category = result.id;
                top      = result.title;
                bottom   = result.podcast.name;
                url      = result.thumbnail_small;
            }
        }, search_result.data);

        main_content_items.push_back(ContentEntry{});
        ContentEntry& item = main_content_items.back();

        item.selected = -1;
        item.category = category;
        item.focused  = false;
        item.result   = search_result;

        ButtonOption opt;
        opt.transform = [top_cap = top, bottom_cap = bottom, url_cap = url, id_cap = category](const EntryState& s) {
            const bool now = !id_cap.empty() && id_cap == playing_id;
            Element thumb = url_cap.empty()
                ? filler() | size(WIDTH, EQUAL, 4) | size(HEIGHT, EQUAL, 2)
                : image_view(ui::cover(url_cap)) | size(WIDTH, EQUAL, 4) | size(HEIGHT, EQUAL, 2);

            auto row = hbox({
                thumb,
                text("  "),
                vbox({
                    hbox({
                        now ? text("\uf028  ") | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)) : emptyElement(),
                        text(top_cap) | (now ? color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)) | bold
                                       : s.focused ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | bold
                                       : color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY))),
                    }),
                    text(bottom_cap) | (s.focused
                        ? color(ui::GetColor(ui::MColor::TEXT_BOTTOM_PRIMARY))
                        : color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY))),
                }) | flex,
            }) | size(HEIGHT, EQUAL, 2);

            return vbox({
                text(" "),
                hbox({
                    text(" "),
                    row
                })
            });
        };

        auto button = Button("", [] {}, opt);

        item.component = CatchEvent(button, [on_press, result = search_result](ftxui::Event event) {
            return on_press(event, result);
        });

        item.focused = false;
        main_content->Add(item.component);
    }

    for (auto& item : main_content_items) {
        item.focused = item.component->Focused();
    }
}

void ui::buildQueue(services::Player::PlayerState* state, std::deque<ContentEntry>& main_content_items, ftxui::Component main_content, std::function<bool(const ftxui::Event&, const int)> on_queue_press) {
    static std::string last_current_id = "";
    static int focus_override = -1;

    std::string current_id;

    if (state->current) {
        if (auto song = std::dynamic_pointer_cast<music::Song>(state->current))
            current_id = song->ref.id;
        else if (auto ep = std::dynamic_pointer_cast<music::Episode>(state->current))
            current_id = ep->ref.id;
    }

    std::deque<ui::QueueEntry> incoming_rows;

    if (!state->user_queue.empty()) {
        incoming_rows.push_back({ "__divider_user__", "", "", "", true, "Next in queue" });
    }

    /* A queued 1,000-track playlist made the queue view build and draw every
    row each frame. Show the next QUEUE_ROWS; the rest still plays in order.
    Rows keep their queue index, so skip / remove stay exact.
    */
    constexpr size_t QUEUE_ROWS = 200;
    size_t shown_user = 0;

    for (auto& item : state->user_queue) {
        if (shown_user == QUEUE_ROWS) break;
        QueueEntry row;
        if (auto song = std::dynamic_pointer_cast<music::Song>(item)) {
            row.id     = song->ref.id;
            row.top    = song->ref.title;
            row.bottom = !song->ref.artists.empty() ? song->ref.artists[0].name : "";
            row.url    = song->ref.thumbnail_small;

        } else if (auto ep = std::dynamic_pointer_cast<music::Episode>(item)) {
            row.id     = ep->ref.id;
            row.top    = ep->ref.title;
            row.bottom = ep->ref.podcast.name;
            row.url    = ep->ref.thumbnail_small;

        } else {
            continue;
        }

        incoming_rows.push_back(row);
        shown_user++;
    }

    const bool truncated = state->user_queue.size() > QUEUE_ROWS;
    if (truncated) {
        incoming_rows.push_back({ "__divider_more__", "", "", "", true,
            "+ " + std::to_string(state->user_queue.size() - QUEUE_ROWS) + " more in queue" });
    }

    // Autoplay follows the whole user queue; past a truncation its rows would
    // not map to real queue indexes, and it is far away anyway.
    if (!truncated && state->autoplay && !state->radio_queue.empty()) {
        incoming_rows.push_back({ "__divider_radio__", "", "", "", true, "Autoplay is on" });

        for (auto& item : state->radio_queue) {
            QueueEntry row;
            if (auto song = std::dynamic_pointer_cast<music::Song>(item)) {
                row.id     = song->ref.id;
                row.top    = song->ref.title;
                row.bottom = !song->ref.artists.empty() ? song->ref.artists[0].name : "";
                row.url    = song->ref.thumbnail_small;

            } else if (auto ep = std::dynamic_pointer_cast<music::Episode>(item)) {
                row.id     = ep->ref.id;
                row.top    = ep->ref.title;
                row.bottom = ep->ref.podcast.name;
                row.url    = ep->ref.thumbnail_small;
            } else {
                continue;
            }

            incoming_rows.push_back(row);
        }
    }

    bool same = (main_content_items.size() == incoming_rows.size());
    if (same) {
        for (size_t i = 0; i < main_content_items.size(); i++) {
            if (main_content_items[i].category != incoming_rows[i].id) { same = false; break; }
        }
    }

    if (same && current_id == last_current_id) {
        return;
    }

    last_current_id = current_id;

    std::string thumbnail_url;
    if (state->current) {
        if (auto song = std::dynamic_pointer_cast<music::Song>(state->current))
            thumbnail_url = song->ref.thumbnail_large;
        else if (auto ep = std::dynamic_pointer_cast<music::Episode>(state->current))
            thumbnail_url = ep->ref.thumbnail_large;
    }

    main_content_items.clear();
    main_content->DetachAllChildren();

    int  queue_index = 0;
    auto queue_container = Container::Vertical({});
    bool first_divider = true;

    for (auto& row : incoming_rows) {
        main_content_items.push_back(ContentEntry{});
        ContentEntry& entry = main_content_items.back();

        entry.category = row.id;
        entry.selected = -1;
        entry.focused  = false;

        if (row.is_divider) {
            std::string label = row.divider_label;

            entry.component = Renderer([label, first_divider] {
                return vbox({
                    first_divider ? emptyElement() : text(" "),
                    first_divider ? emptyElement() : text(" "),
                    text(label) | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
                    separator() | color(ui::GetColor(ui::MColor::SEPARATOR_SECONDARY)),
                });
            });

            first_divider = false;

        } else {
            ButtonOption opt;
            opt.transform = [top_cap = row.top, bottom_cap = row.bottom, url_cap = row.url](const EntryState& s) {

                Element thumb = url_cap.empty()
                    ? filler() | size(WIDTH, EQUAL, 4) | size(HEIGHT, EQUAL, 2)
                    : image_view(ui::cover(url_cap)) | size(WIDTH, EQUAL, 4) | size(HEIGHT, EQUAL, 2);

                auto row_el = hbox({
                    thumb,
                    text("  "),
                    vbox({
                        text(utils::trimSuffix(top_cap, 20, "...")) | (s.focused
                            ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | bold
                            : color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY))),

                        text(utils::trimSuffix(bottom_cap, 20, "...")) | (s.focused
                            ? color(ui::GetColor(ui::MColor::TEXT_BOTTOM_PRIMARY))
                            : color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY))),
                    }) | flex,
                }) | size(HEIGHT, EQUAL, 2);

                return vbox({
                    text(" "),
                    row_el
                });
            };

            auto button = Button(opt);
            button = CatchEvent(button, [on_queue_press, queue_index](const ftxui::Event& event) {
                const Config& cfg = Config::get();

                if (Config::isKey(event, cfg.KEY_REMOVE_FROM_QUEUE)) {
                    focus_override = queue_index;
                }

                return on_queue_press(event, queue_index);
            });

            entry.component = button;
            queue_index++;
        }

        queue_container->Add(entry.component);
    }

    if (focus_override >= 0) {
        int child_index = std::min(focus_override + 1, (int)queue_container->ChildCount() - 1);

        if(state->user_queue.size() >= child_index) {
            child_index -=1;
        }

        if(child_index == state->user_queue.size() + 1) {
            child_index -=1;
        }

        if(child_index <= 0) {
            child_index = 1;
        }

        queue_container->SetActiveChild(queue_container->ChildAt(child_index));
        focus_override = -1;

    } else if (queue_container->ChildCount() > state->queue_position + 1) {
        queue_container->SetActiveChild(queue_container->ChildAt(state->queue_position + 1));
    }

    auto queue_wrapper = Renderer(queue_container, [thumbnail_url, queue_container] {
        return hbox({
            filler(),
            vbox({
                filler(),
                image_view(ui::cover(thumbnail_url)) | size(WIDTH, EQUAL, 60) | size(HEIGHT, EQUAL, 30),
                filler()
            }),
            filler(),
            vbox({
                text(" "),
                text(" "),
                queue_container->Render() | yframe | yflex | size(WIDTH, ftxui::GREATER_THAN, 30),
            }) | yflex,
        }) | yflex;
    });

    main_content->Add(queue_wrapper);

    for (auto& entry : main_content_items) {
        entry.focused = entry.component->Focused();
    }
}

void ui::buildLibrary(services::Player::PlayerState* state, std::deque<ContentEntry>& main_content_items, ftxui::Component main_content, int rows, const std::string& query, std::function<bool(const ftxui::Event&, const ui::ChipEntry&)> on_chip_press, std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_item_press, std::function<void()> on_retry) {
    if (buildError(state, "library", main_content_items, main_content, on_retry)) {
        return;
    }

    if (main_content_items.empty()) {
        main_content_items.push_back(ContentEntry{});
        ContentEntry& filters = main_content_items.back();

        filters.category = "__library_filters__";
        filters.selected = 0;
        filters.focused = false;

        filters.chips_data.exclusive = true;

        ui::setChipsData(&filters.chips_data, {
            {"playlists", "Playlists"},
            {"albums", "Albums"},
            {"songs", "Songs"},
            {"artists", "Artists"},
            {"podcasts", "Podcasts"},
        });

        filters.component = Chips(&filters.chips_data, &filters.selected, &filters.focused, on_chip_press);

        main_content_items.push_back(ContentEntry{});
        ContentEntry& grid = main_content_items.back();

        grid.category = "";
        grid.selected = 0;
        grid.focused  = false;
        grid.component = Renderer([] { return emptyElement(); });

        main_content->DetachAllChildren();
        main_content->Add(filters.component);
        main_content->Add(grid.component);
    }

    ContentEntry& filters = main_content_items[0];
    ContentEntry& grid = main_content_items[1];

    const std::string filter = activeLibraryFilter(&filters.chips_data);
    const std::string signature = librarySignature(state, filter, query);

    if (grid.category != signature) {
        grid.category = signature;
        grid.selected = 0;

        ui::getLibraryGridData(state, filter, query, &grid.grid_data);

        // Signature-gated, so this is one line per filter change or keystroke
        // rather than one per render.
        spdlog::info("CONTENT: library grid rebuilt, filter '{}', query '{}', {} items", filter, query, grid.grid_data.entries.size());

        /* Grid() renders nothing at all when it has no entries, which reads as
        a broken page rather than an empty filter, so say so instead.
        */
        if (grid.grid_data.entries.empty() && !query.empty()) {
            const std::string message = "Nothing in your library matches \"" + query + "\"";

            grid.component = Renderer([message] {
                return vbox({
                    text(""),
                    text(""),
                    hbox({
                        text(" "),
                        text(message) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)),
                    }),
                });
            });

        } else {
            grid.component = Grid(&grid.grid_data, &grid.selected, &grid.focused, rows, on_item_press, ui::GridStyle::Tile);
        }

        main_content->DetachAllChildren();
        main_content->Add(filters.component);
        main_content->Add(grid.component);
    }

    for (auto& entry : main_content_items) {
        entry.focused = entry.component->Focused();
    }
}

void ui::buildPlaylist(services::Player::PlayerState* state, std::deque<ContentEntry>& main_content_items, ftxui::Component main_content,
                       std::function<bool(const ftxui::Event&, int)> on_track, std::function<void()> on_play_all,
                       std::function<void()> on_queue_all, std::function<void()> on_retry) {
    if (buildError(state, "open_playlist", main_content_items, main_content, on_retry)) {
        return;
    }

    auto playlist = state->open_playlist;
    if (!playlist) return;

    updatePlayingId(state);

    const bool loading = state->flags["open_playlist"] == services::Player::Flags::Ongoing;
    const auto tracks = playlist->getStreamables();

    /* Paging
    - Only PAGE rows exist at first and another page is added as the focus
      nears the end: rendering 1,000+ rows (and fetching their covers) every
      frame kept a core busy
    */
    constexpr size_t PAGE = 100;
    static std::string paged_id;
    static size_t shown = PAGE;
    if (paged_id != playlist->ref.id) { paged_id = playlist->ref.id; shown = PAGE; }

    int focused_track = -1;
    for (auto& item : main_content_items) {
        if (item.component && item.component->Focused() && item.category.rfind("__track__", 0) == 0) {
            focused_track = std::stoi(item.category.substr(9)) - 1;
        }
    }
    if (focused_track >= 0 && static_cast<size_t>(focused_track) + 10 >= shown && shown < tracks.size()) {
        shown += PAGE;
    }
    const size_t visible = std::min(shown, tracks.size());

    // Rebuild only when what is shown changes.
    const std::string signature = "__playlist__:" + playlist->ref.id + ":" + std::to_string(tracks.size()) + ":" + (loading ? "1" : "0") + ":" + std::to_string(visible) + ":" + state->open_kind;
    if (!main_content_items.empty() && main_content_items.front().category == signature) {
        for (auto& item : main_content_items) item.focused = item.component->Focused();
        return;
    }

    main_content_items.clear();
    main_content->DetachAllChildren();

    // Header: title, author, count, and the two actions.
    {
        main_content_items.push_back(ContentEntry{});
        ContentEntry& head = main_content_items.back();
        head.category = signature;

        auto pill = [](const std::string& label, std::function<void()> fn) {
            ButtonOption opt;
            opt.on_click = [fn = std::move(fn)] { if (!ui::suppress_click) fn(); };
            opt.transform = [label](const EntryState& s) {
                auto e = text(" " + label + " ") | borderRounded;
                return s.focused ? e | bold | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY))
                                 : e | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY));
            };
            return Button(opt);
        };

        auto actions = Container::Horizontal({
            pill("\uf04b  Play", on_play_all),
            Renderer([] { return text("  "); }),
            pill("\uf067  Add to queue", on_queue_all),
        });

        const std::string title = playlist->ref.title;
        const std::string sub = playlist->ref.author.empty() ? "" : playlist->ref.author + "  \u2022  ";
        const std::string count = loading ? "loading tracks..." : std::to_string(tracks.size()) + " tracks";
        const std::string thumb = playlist->ref.thumbnail_large.empty() ? playlist->ref.thumbnail_small : playlist->ref.thumbnail_large;

        std::string kind = state->open_kind;
        for (auto& ch : kind) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        head.component = Renderer(actions, [actions, title, sub, count, thumb, loading, kind] {
            Element cover = thumb.empty()
                ? filler() | size(WIDTH, EQUAL, 14) | size(HEIGHT, EQUAL, 7)
                : image_view(ui::cover(thumb)) | size(WIDTH, EQUAL, 14) | size(HEIGHT, EQUAL, 7);
            return vbox({
                text(" "),
                hbox({
                    text(" "),
                    cover,
                    text("   "),
                    vbox({
                        text(kind) | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)),
                        text(title) | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
                        hbox({
                            text(sub + count) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)),
                            loading ? hbox({ text("  "), spinner(15, ui::anim_frame) | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)) }) : emptyElement(),
                        }),
                        filler(),
                        actions->Render(),
                    }),
                }),
                text(" "),
            });
        });

        main_content->Add(head.component);
    }

    for (size_t i = 0; i < visible; i++) {
        auto song = std::dynamic_pointer_cast<music::Song>(tracks[i]);
        if (!song) continue;

        const std::string top = song->ref.title;
        const std::string bottom = song->ref.artists.empty() ? "" : song->ref.artists[0].name;
        const std::string url = song->ref.thumbnail_small;
        const std::string number = std::to_string(i + 1);

        main_content_items.push_back(ContentEntry{});
        ContentEntry& item = main_content_items.back();
        item.category = "__track__" + number;
        item.result.resultType = "song";
        item.result.data = song->ref;

        ButtonOption opt;
        const std::string id = song->ref.id;
        opt.transform = [top, bottom, url, number, id](const EntryState& s) {
            Element thumb = url.empty()
                ? filler() | size(WIDTH, EQUAL, 4) | size(HEIGHT, EQUAL, 2)
                : image_view(ui::cover(url)) | size(WIDTH, EQUAL, 4) | size(HEIGHT, EQUAL, 2);
            const bool now = !id.empty() && id == playing_id;
            return hbox({
                // The playing track shows a speaker in the accent colour.
                (now ? text("\uf028") | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)) | bold
                     : text(number) | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY))) | size(WIDTH, EQUAL, 6) | align_right,
                text("  "),
                thumb,
                text("  "),
                vbox({
                    text(top) | (now ? color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)) | bold
                               : s.focused ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | bold
                                           : color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY))),
                    text(bottom) | color(s.focused ? ui::GetColor(ui::MColor::TEXT_BOTTOM_PRIMARY)
                                                   : ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)),
                }) | flex,
            }) | size(HEIGHT, EQUAL, 2);
        };

        const int index = static_cast<int>(i);
        item.component = CatchEvent(Button(opt), [on_track, index](const Event& e) { return on_track(e, index); });
        main_content->Add(item.component);
    }

    // Opening a playlist lands on its first track, not on the header's
    // buttons (where left/right only flipped between Play and Add to queue).
    if (focused_track < 0 && !loading && main_content_items.size() > 1) {
        main_content_items[1].component->TakeFocus();
    }

    // Keep the focus on the row it was on when a page was added.
    if (focused_track >= 0) {
        const std::string want = "__track__" + std::to_string(focused_track + 1);
        for (auto& item : main_content_items) {
            if (item.category == want) { item.component->TakeFocus(); break; }
        }
    }

    for (auto& item : main_content_items) item.focused = item.component->Focused();
}

void ui::buildArtist(services::Player::PlayerState* state, std::deque<ContentEntry>& main_content_items, ftxui::Component main_content,
                     std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_press, std::function<void()> on_retry) {
    if (buildError(state, "open_artist", main_content_items, main_content, on_retry)) {
        return;
    }

    auto page = state->open_artist;
    if (!page) return;

    const bool loading = state->flags["open_artist"] == services::Player::Flags::Ongoing;
    const std::string signature = "__artist__:" + page->ref.id + ":" + std::to_string(page->order.size()) + ":" + (loading ? "1" : "0");

    if (main_content_items.empty() || main_content_items.front().category != signature) {
        main_content_items.clear();
        main_content->DetachAllChildren();

        // Header: picture and name.
        main_content_items.push_back(ContentEntry{});
        ContentEntry& head = main_content_items.back();
        head.category = signature;

        const std::string name = page->ref.name;
        const std::string thumb = page->ref.thumbnail_large.empty() ? page->ref.thumbnail_small : page->ref.thumbnail_large;
        head.component = Renderer([name, thumb, loading] {
            Element picture = thumb.empty()
                ? filler() | size(WIDTH, EQUAL, 14) | size(HEIGHT, EQUAL, 7)
                : image_view(ui::cover(thumb)) | size(WIDTH, EQUAL, 14) | size(HEIGHT, EQUAL, 7);
            return vbox({
                text(" "),
                hbox({
                    text(" "), picture, text("   "),
                    vbox({
                        text("ARTIST") | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)),
                        text(name) | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
                        loading ? spinner(15, ui::anim_frame) | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)) : emptyElement(),
                    }),
                }),
            });
        });
        main_content->Add(head.component);

        // One carousel per section, in YouTube Music's order.
        for (const std::string& category : page->order) {
            main_content_items.push_back(ContentEntry{});
            ContentEntry& item = main_content_items.back();
            item.category = category;
            item.selected = 0;
            item.component = MouseGate(Carousel(&item.carousel_data, &item.selected, &item.focused, on_press), item.shown);
            main_content->Add(item.component);
        }

        // Land on the first section (Top songs), not the header.
        if (main_content_items.size() > 1) main_content_items[1].component->TakeFocus();
    }

    for (auto& item : main_content_items) {
        if (item.category.rfind("__artist__", 0) == 0) continue;
        ui::getCarouselDataFrom(page->shelves, item.category, &item.carousel_data);
        item.focused = item.component->Focused();
    }
}

#include "../../../include/ui/components/carousel.hpp"
#include "../../../include/ui/constants/colors.hpp"
#include "../../../include/ui/components/content.hpp"
#include "image_view.hpp"
#include "../../../include/ui/cover_cache.hpp"

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

using namespace ftxui;

void ui::getCarouselData(services::Player::PlayerState* state, std::string category, CarouselData* data) {
    getCarouselDataFrom(state->home, category, data);
    data->is_loading = state->flags["home"] == services::Player::Flags::Ongoing;

}

void ui::getCarouselDataFrom(const std::unordered_map<std::string, std::vector<music::ApiResult>>& shelves, std::string category, CarouselData* data) {
    data->entries_spoof.clear();
    data->entries.clear();
    data->results.clear();      // rebuilt each frame; it used to grow forever and go stale

    auto it = shelves.find(category);
    if (it == shelves.end())
        return;

    if (utils::lower(category) == "listen again") {
        category = "  " + category;

    } else if(utils::lower(category) == "forgotten favorites") {
        category = "  " + category;

    } else if(utils::lower(category) == "morning sunshine") {
        category = "  " + category;

    } else if(utils::lower(category) == "from your library") {
        category = "  " + category;
    }

    for (auto& r : it->second) {
        std::string url;
        std::string top;
        std::string bottom;

        std::visit([&](auto&& item) {
            using T = std::decay_t<decltype(item)>;

            if constexpr (std::is_same_v<T, music::SongRef>) {
                top    = item.title;
                bottom = item.artists.empty() ? "" : item.artists[0].name;
                url    = item.thumbnail_small;

            } else if constexpr (std::is_same_v<T, music::AlbumRef>) {
                top    = item.title;
                bottom = item.artists.empty() ? "" : item.artists[0].name;
                url    = item.thumbnail_small;

            } else if constexpr (std::is_same_v<T, music::ArtistRef>) {
                top    = item.name;
                bottom = "Artist";
                url    = item.thumbnail_small;

            } else if constexpr (std::is_same_v<T, music::PlaylistRef>) {
                top    = item.title;
                bottom = item.author;
                url    = item.thumbnail_small;

            } else if constexpr (std::is_same_v<T, music::PodcastRef>) {
                top    = item.name;
                bottom = "Podcast";
                url    = item.thumbnail_small;

            } else if constexpr (std::is_same_v<T, music::EpisodeRef>) {
                top    = item.title;
                bottom = item.podcast.name;
                url    = item.thumbnail_small;
            }
        }, r.data);

        data->results.push_back(r);
        data->entries_spoof.push_back(url);
        data->entries.push_back({
            url,
            top,
            bottom
        });
    }

    data->category_name = category;
}

ftxui::Component ui::Carousel(CarouselData* data, int* selected, bool* focused, std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_press) {
    MenuOption options;
    options.direction = Direction::Right;
    options.Horizontal();

    options.entries_option.transform = [data, focused](const EntryState& state) {
        const auto& item = data->entries[state.index];

        ftxui::Element thumb;

        if (item.image_url.empty()) {
            thumb = ui::noCover(20, 10);
        } else {
            try {
                thumb = image_view(ui::cover(item.image_url))
                    | size(WIDTH, EQUAL, 20)
                    | size(HEIGHT, EQUAL, 10)
                    | borderStyled(BorderStyle::EMPTY);
                // thumb = ui::noCover(20, 10);

            } catch (...) {
                thumb = ui::noCover(20, 10);
            }
        }

        return hbox({
            vbox({
                ui::coverHit(thumb),
                hbox({
                    text(" "),
                    vbox({
                        text(utils::trimSuffix(item.top, 20, "...")) | (state.active && *focused ?  color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | bold : color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY))),
                        text(utils::trimSuffix(item.bottom, 20, "...")) | (state.active && *focused ? color(ui::GetColor(ui::MColor::TEXT_BOTTOM_PRIMARY)) | bold : color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY))),
                    }),
                }),
            }),
            text(" "),
        });
    };

    options.on_enter = [data, selected] () {
        // spdlog::info("CAROUSEL: enter pressed on item, " + data->entries[*selected].top + " " + data->entries[*selected].bottom + " " + data->results[*selected].resultType);
    };

    auto menu = Menu(&data->entries_spoof, selected, options);

    menu = CatchEvent(menu, [data, selected, on_press](const ftxui::Event& event) {
        // Keys follow the selection again (not the app's own refresh events).
        if (!event.is_mouse() && event != ftxui::Event::Custom) data->hscroll.offset = -1;
        if (*selected < 0 || static_cast<size_t>(*selected) >= data->results.size()) return false;
        return on_press(event, data->results[*selected]);
    });

    return Renderer(menu, [menu, data] {
        return vbox({
            text(""),
            text(""),
            hbox({
                text(" "),
                text(data->category_name),
            }),
            text(""),
            ui::hscrolled(menu->Render(), &data->hscroll),
        });
    });
}

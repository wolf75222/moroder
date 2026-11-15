#include "../../../include/ui/components/sidebar.hpp"

#include "../../../include/ui/constants/colors.hpp"
#include "../../../include/ui/structs/entries.hpp"
#include "../../../include/ui/components/content.hpp"
#include "spdlog/spdlog.h"

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>

using namespace ftxui;

void ui::getSidebarData(services::Player::PlayerState* state, ui::SidebarData* data) {
    data->entries.clear();
    data->entries_spoof.clear();
    data->results.clear();      // rebuilt each frame; it used to grow forever and go stale

    // A spinner beside Home / Library while it (re)loads: clicking it again
    // refreshes it, and this shows it is on its way.
    static const char* frames[] = {"\u280b", "\u2819", "\u2839", "\u2838", "\u283c", "\u2834", "\u2826", "\u2827", "\u2807", "\u280f"};
    const std::string spin = std::string("  ") + frames[(ui::anim_frame / 2) % 10];
    auto busy = [&](const char* f) { auto it = state->flags.find(f); return it != state->flags.end() && it->second == services::Player::Flags::Ongoing; };
    data->entries = {
        {std::string("\uf4e2  Home") + (busy("home") ? spin : ""), ""},
        {std::string("\uf02e  Library") + (busy("library_playlists") || busy("library_albums") ? spin : ""), ""},
    };

    data->entries_spoof = {
        "01",
        "02",
    };

    if(state->is_logged_in) {
        data->entries.push_back({"  New playlist", ""});
    } else {
        data->entries.push_back({"  Sign in", ""});
    }

    data->entries_spoof.push_back("03");

    for (auto& playlist : state->library_playlists) {
        data->results.emplace_back(playlist);
        data->entries_spoof.emplace_back(playlist.ref.id);
        data->entries.emplace_back(ui::SimpleEntry{ playlist.ref.title, playlist.ref.author });
    }

    data->is_loading = state->flags["library_playlists"] == services::Player::Flags::Ongoing;
}

Component ui::Sidebar(ui::SidebarData* data, int* selected, bool* focused, std::function<bool(const ftxui::Event&, const music::Playlist&)> on_press) {
    MenuOption options;

    options.entries_option.animated_colors.foreground = AnimatedColorOption {
        .inactive = ui::GetColor(ui::MColor::WHITE),
        .active = ui::GetColor(ui::MColor::DEFAULT)
    };

    options.entries_option.transform = [data, focused](const EntryState& entry_state) {
        const auto& item = data->entries[entry_state.index];

        if(entry_state.index == 2) {
            return vbox({
                separator() | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)),
                hbox({
                    text("  "),
                    vbox({
                        text(""),
                        text(item.top) | (entry_state.active && *focused ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | bold : color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY))),
                        text(""),
                    })
                })
            }) | (entry_state.active && *focused ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | bold : color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)));
        }

        if(entry_state.index > 2) {
            return hbox({
                text("  "),
                vbox({
                    text(""),
                    text(item.top) | (entry_state.active && *focused ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | bold : color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY))),
                    text(item.bottom) | (entry_state.active && *focused ? color(ui::GetColor(ui::MColor::TEXT_BOTTOM_PRIMARY)) | bold : color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY))),
                })
            });
        }

        return hbox({
            text("  "),
            vbox({
                text(item.top) | (entry_state.active && *focused ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | bold : color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY))),
                text("")
            }) | (entry_state.active && *focused ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | bold : color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)))
        });
    };

    options.on_enter = [data, selected] {
        if(data->entries_spoof[*selected] == "01") {
            data->onHome();

        } else if(data->entries_spoof[*selected] == "02") {
            data->onLibrary();

        } else if(data->entries_spoof[*selected] == "03") {
            if (data->onThird) data->onThird();

        } else if (*selected > 2 && static_cast<size_t>(*selected - 3) < data->results.size()) {
            spdlog::info("SIDEBAR: pressed enter on playlist, " + data->results[*selected - 3].ref.title);
        }
    };

    auto menu = Menu(&data->entries_spoof, selected, options);

    return CatchEvent(menu, [data, selected, on_press](const ftxui::Event& event) {
        if (*selected > 2 && static_cast<size_t>(*selected - 3) < data->results.size()) {
            return on_press(event, data->results[(*selected - 3)]);
        }
        return false;
    });
}

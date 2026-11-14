#include "../../../include/ui/components/playback_bar.hpp"
#include "../../../include/ui/constants/colors.hpp"

#include <ftxui/dom/elements.hpp>
#include <algorithm>

#include "image_view.hpp"

using namespace ftxui;

namespace {

std::string clock(uint64_t seconds) {
    const uint64_t h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
    char buf[16];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu", (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
    else       std::snprintf(buf, sizeof(buf), "%llu:%02llu", (unsigned long long)m, (unsigned long long)s);
    return buf;
}

}

void ui::getPlaybackData(services::Player::PlayerState* state, PlaybackData* data, int dimx) {
    data->is_playing = state->flags["audio"] == services::Player::Flags::Ongoing;
    data->can_skip_forwards  = state->flags["can_skip_forwards"]  == services::Player::Flags::True;
    data->can_skip_backwards = state->flags["can_skip_backwards"] == services::Player::Flags::True;
    data->loop_mode = state->loop_mode;
    data->dimx = dimx;
    data->volume = state->volume;

    if (auto song_ptr = std::dynamic_pointer_cast<music::Song>(state->current)) {
        data->title = song_ptr->ref.title;
        data->artist = !song_ptr->ref.artists.empty() ? song_ptr->ref.artists[0].name : "";
        data->image_url = song_ptr->ref.thumbnail_small;

    } else if (auto episode_ptr = std::dynamic_pointer_cast<music::Episode>(state->current)) {
        data->title = episode_ptr->ref.title;
        data->artist = episode_ptr->ref.podcast.name;
        data->image_url = episode_ptr->ref.thumbnail_small;
    }
}

Component ui::PlaybackBar(PlaybackData* data) {
    return Renderer([data] {
        int percent = std::clamp(data->progress, 0, 100);

        int width = data->dimx;
        std::string bar_char = "▁";

        Elements progress_line;
        progress_line.reserve(width);

        int filled = (width * percent) / 100;

        for (int i = 0; i < width; ++i) {
            if (i < filled) {
                progress_line.push_back(
                    text(bar_char) |
                    color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)) |
                    bold
                );
            } else {
                progress_line.push_back(
                    text(bar_char) |
                    color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY))
                );
            }
        }

        Element left_controls = hbox({
            text("  "),
            text("") | ((data->can_skip_backwards) ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) : color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY))),
            text("  "),
            text(data->is_playing ? "" : "") | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
            text("  "),
            text("") | ((data->can_skip_forwards)  ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) : color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)))
        });

        Element track_info;

        if (data->image_url.empty()) {
            track_info = hbox({
                text(data->title) | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
                text(" - ") | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)),
                text(data->artist) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY))
            });
        } else {
            track_info = hbox({
                image_view(data->image_url)
                    | size(WIDTH, EQUAL, 2)
                    | size(HEIGHT, EQUAL, 1),

                text("  "),

                text(utils::trimSuffix(data->title, 50, "..."))  | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
                data->artist.empty() ? emptyElement() : text(" - ") | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)),
                text(data->artist) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)),
            });
        }

        /* Loop icon
        - Off is dimmed the same way an unavailable skip arrow is
        - This icon set has no repeat-one glyph, so a track loop is the same
          icon in the accent colour rather than a different one
        */
        const ftxui::Color loop_color =
              data->loop_mode == services::Player::LoopMode::Track ? ui::GetColor(ui::MColor::ACCENT_PRIMARY)
            : data->loop_mode == services::Player::LoopMode::Queue ? ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)
            :                                                        ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY);

        // Elapsed / total, dimmed while the stream is still loading.
        Element time = data->duration > 0
            ? hbox({
                text(clock(data->position)) | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
                text(" / ") | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)),
                text(clock(data->duration)) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)),
              })
            : text("--:-- / --:--") | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY));

        // Volume: icon follows the level (off / down / up).
        const char* volume_icon = data->volume == 0 ? "\uf026" : data->volume < 50 ? "\uf027" : "\uf028";

        Element right_controls = hbox({
            time,
            text("    "),
            text(volume_icon) | color(data->volume == 0 ? ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY) : ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
            text(" " + std::to_string(data->volume) + "%") | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)) | size(WIDTH, EQUAL, 5),
            text("   "),
            text("") | color(loop_color),
            text("  "),
        });

        return vbox({
            hbox(std::move(progress_line)),
            text(" "),
            hbox({
                left_controls,
                filler(),
                track_info,
                filler(),
                right_controls,
            }) | size(HEIGHT, EQUAL, 1)
        }) | size(HEIGHT, EQUAL, 3);
    });
}

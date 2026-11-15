#include "../../../include/ui/components/playback_bar.hpp"
#include "../../../include/ui/constants/colors.hpp"

#include <ftxui/dom/elements.hpp>
#include <algorithm>
#include <chrono>

#include "image_view.hpp"
#include "../../../include/ui/cover_cache.hpp"

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
    data->can_like = false;
    data->liked = false;
    data->notice = std::chrono::steady_clock::now() < state->notice_until ? state->notice : "";

    if (auto song_ptr = std::dynamic_pointer_cast<music::Song>(state->current)) {
        data->title = song_ptr->ref.title;
        data->artist = !song_ptr->ref.artists.empty() ? song_ptr->ref.artists[0].name : "";
        data->image_url = song_ptr->ref.thumbnail_small;

        data->can_like = !song_ptr->ref.id.empty();
        auto it = state->liked.find(song_ptr->ref.id);
        data->liked = it != state->liked.end() && it->second;

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

        // Two runs of the bar character (played / to come), not one element per column.
        const int filled = std::clamp((width * percent) / 100, 0, std::max(0, width));
        std::string played, rest;
        played.reserve(static_cast<size_t>(filled) * 3);
        rest.reserve(static_cast<size_t>(std::max(0, width - filled)) * 3);
        for (int i = 0; i < filled; ++i) played += bar_char;
        for (int i = filled; i < width; ++i) rest += bar_char;
        Elements progress_line = {
            text(played) | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)) | bold,
            text(rest) | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)),
        };

        // Level with the sidebar's icons; a little more air between them.
        Element left_controls = hbox({
            text("  "),
            text("") | ((data->can_skip_backwards) ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) : color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY))) | reflect(data->box_prev),
            text("   "),
            text(data->is_playing ? "" : "") | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | reflect(data->box_play),
            text("   "),
            text("") | ((data->can_skip_forwards)  ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) : color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY))) | reflect(data->box_next)
        });

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
            : hbox({
                // Stream still resolving (yt-dlp + first bytes): say so, animated.
                spinner(15, data->frame) | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)),
                text(" loading") | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)),
              });

        // Volume: icon follows the level (off / down / up).
        const char* volume_icon = data->volume == 0 ? "\uf026" : data->volume < 50 ? "\uf027" : "\uf028";

        Element right_controls = hbox({
            time,
            text("     "),
            data->can_like
                ? (data->liked
                    ? text("\uf004") | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)) | reflect(data->box_like)
                    : text("\uf08a") | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)) | reflect(data->box_like))
                : emptyElement(),
            data->can_like ? text("   ") : emptyElement(),
            data->sleep_label.empty() ? emptyElement()
                : hbox({ text("\U000f04b2 " + data->sleep_label) | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)), text("   ") }),
            hbox({
                text(volume_icon) | color(data->volume == 0 ? ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY) : ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
                text(" " + std::to_string(data->volume) + "%") | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)) | size(WIDTH, EQUAL, 5),
            }) | reflect(data->box_volume),
            text("   "),
            text("") | color(loop_color) | reflect(data->box_loop),
            text("   "),
            text("\uf074") | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)) | reflect(data->box_shuffle),
            text("   "),
            // The queue view, one click away (a key: a).
            text("\uf03a") | color(data->queue_open ? ui::GetColor(ui::MColor::ACCENT_PRIMARY) : ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)) | reflect(data->box_queue),
            text("   "),
        });

        // Both sides as wide as the wider one: the title in the screen's middle.
        left_controls->ComputeRequirement();
        right_controls->ComputeRequirement();
        const int side = std::max(left_controls->requirement().min_x, right_controls->requirement().min_x);

        // Title · artist in what is left between them (a long one used to push
        // the icons off the right edge). (A 2-cell cover here read as a smudge.)
        const int room = std::max(8, width - 2 * side - 4);
        const int artist_w = data->artist.empty() ? 0 : std::min(static_cast<int>(utils::utf8Length(data->artist)), std::max(0, room / 3));
        const int title_w = std::max(4, room - (artist_w ? artist_w + 5 : 0));
        Element track_info = hbox({
            text(utils::trimSuffix(data->title, title_w, "\u2026")) | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
            artist_w ? text("  \u00b7  ") | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)) : emptyElement(),
            artist_w ? text(utils::trimSuffix(data->artist, artist_w, "\u2026")) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)) : emptyElement(),
        });

        return vbox({
            hbox(std::move(progress_line)) | reflect(data->box_progress),
            text(" "),
            hbox({
                hbox({ left_controls, filler() }) | size(WIDTH, EQUAL, side),
                filler(),
                // A transient notice (like result, errors) briefly takes the
                // track's place, where the eye already is.
                data->notice.empty()
                    ? track_info | reflect(data->box_track)
                    : text(data->notice) | bold | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)),
                filler(),
                hbox({ filler(), right_controls }) | size(WIDTH, EQUAL, side),
            }) | size(HEIGHT, EQUAL, 1)
        }) | size(HEIGHT, EQUAL, 3);
    });
}

#include "../../../include/ui/components/now_playing.hpp"
#include "../../../include/ui/constants/colors.hpp"
#include "../../../include/ui/cover_cache.hpp"
#include "../../../include/utils/utils.hpp"
#include "../../../include/ui/components/content.hpp"
#include "../../../include/config/config.hpp"

#include "image_view.hpp"

#include <algorithm>
#include <cstdio>

using namespace ftxui;

namespace {

std::string clock(double s) {
    if (s < 0) s = 0;
    const int t = static_cast<int>(s);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
    return buf;
}

}

Element ui::renderNowPlaying(services::Player::PlayerState* state, double position, double duration, bool playing, int width, int height) {
    auto song = std::dynamic_pointer_cast<music::Song>(state->current);
    if (!song) {
        return text("Nothing is playing. Press v or Esc to go back.") | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)) | center | flex;
    }

    const auto& ref = song->ref;
    const std::string artist = ref.artists.empty() ? "" : ref.artists[0].name;
    const std::string art = ref.thumbnail_large.empty() ? ref.thumbnail_small : ref.thumbnail_large;

    // Cover: as large as the page allows, square in terminal cells (2:1).
    const int cover_h = std::clamp(height - 12, 8, 26);
    const int cover_w = cover_h * 2;

    Element cover = art.empty()
        ? filler() | size(WIDTH, EQUAL, cover_w) | size(HEIGHT, EQUAL, cover_h) | border
        : image_view(ui::cover(art)) | size(WIDTH, EQUAL, cover_w) | size(HEIGHT, EQUAL, cover_h);

    const float ratio = duration > 0 ? static_cast<float>(std::clamp(position / duration, 0.0, 1.0)) : 0.f;

    Element info = vbox({
        text(""),
        text(utils::trimSuffix(ref.title, cover_w, "...")) | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
        text(artist) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)),
        text(song->album.title) | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)),
        text(""),
        gauge(ratio) | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)) | size(WIDTH, EQUAL, cover_w),
        hbox({
            text(clock(position)) | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
            filler(),
            text(playing ? "" : "paused") | dim,
            filler(),
            text(clock(duration)) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)),
        }) | size(WIDTH, EQUAL, cover_w),
    });

    // Lyrics: synced lines around the current one, or the plain text.
    Elements lines;
    const bool ours = state->lyrics && state->lyrics_id == ref.id;
    const auto lyrics_flag = state->flags.count("lyrics") ? state->flags["lyrics"] : services::Player::Flags::Done;

    if (!Config::get().ENABLE_LYRICS) {
        lines.push_back(text("Lyrics are off (ENABLE_LYRICS)") | dim);
    } else if (!ours && lyrics_flag == services::Player::Flags::Ongoing) {
        lines.push_back(hbox({ spinner(15, ui::anim_frame) | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)), text(" looking for lyrics") | dim }));
    } else if (!ours || !state->lyrics->found) {
        lines.push_back(text("No lyrics found for this track") | dim);
    } else if (!state->lyrics->lines.empty()) {
        const auto& ls = state->lyrics->lines;
        int cur = -1;
        for (size_t i = 0; i < ls.size(); i++) if (ls[i].first <= position + 0.2) cur = static_cast<int>(i);

        const int visible = std::max(5, height - 6);
        const int before = visible / 3;
        const int start = std::clamp(cur - before, 0, std::max(0, static_cast<int>(ls.size()) - visible));
        for (int i = start; i < std::min<int>(ls.size(), start + visible); i++) {
            const std::string& t = ls[i].second.empty() ? std::string("♪") : ls[i].second;
            Element e = paragraph(t);
            if (i == cur) e = e | bold | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY));
            else if (i < cur) e = e | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY));
            else e = e | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY));
            lines.push_back(e);
        }
    } else {
        for (const auto& t : state->lyrics->plain) lines.push_back(paragraph(t.empty() ? " " : t) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)));
    }

    Element lyrics = vbox({
        text(""),
        text("Lyrics") | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)),
        text(""),
        vbox(std::move(lines)) | yframe | flex,
    }) | flex;

    return hbox({
        text("  "),
        vbox({ cover, info }),
        text("    "),
        lyrics,
        text("  "),
    }) | flex;
}

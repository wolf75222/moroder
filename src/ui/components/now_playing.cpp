#include "../../../include/ui/components/now_playing.hpp"
#include "../../../include/ui/constants/colors.hpp"
#include "../../../include/ui/cover_cache.hpp"
#include "../../../include/utils/utils.hpp"
#include "../../../include/ui/components/content.hpp"
#include "../../../include/config/config.hpp"

#include "image_view.hpp"
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace ftxui;

namespace {

// Words of a line, wrapped to `width` columns: rows of word indices.
std::vector<std::vector<int>> wrapWords(const std::vector<std::string>& words, int width) {
    std::vector<std::vector<int>> rows(1);
    int used = 0;
    for (int k = 0; k < static_cast<int>(words.size()); k++) {
        const int w = ftxui::string_width(words[k]) + 1;
        if (used > 0 && used + w > width) { rows.emplace_back(); used = 0; }
        rows.back().push_back(k);
        used += w;
    }
    return rows;
}

std::vector<std::string> splitWords(const std::string& line) {
    std::vector<std::string> words;
    std::string w;
    for (char ch : line) {
        if (ch == ' ') { if (!w.empty()) words.push_back(w); w.clear(); }
        else w += ch;
    }
    if (!w.empty()) words.push_back(w);
    return words;
}

std::string clock(double s) {
    if (s < 0) s = 0;
    const int t = static_cast<int>(s);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
    return buf;
}

}

std::vector<std::pair<std::shared_ptr<Box>, double>> ui::lyric_boxes;
Box ui::now_playing_cover_box{-1, -1, -1, -1};
Box ui::now_playing_title_box{-1, -1, -1, -1};
Box ui::now_playing_artist_box{-1, -1, -1, -1};
Box ui::now_playing_album_box{-1, -1, -1, -1};

Element ui::renderNowPlaying(services::Player::PlayerState* state, double position, double duration, bool playing, int width, int height) {
    ui::lyric_boxes.clear();
    ui::now_playing_cover_box = Box{-1, -1, -1, -1};
    ui::now_playing_title_box = ui::now_playing_artist_box = ui::now_playing_album_box = Box{-1, -1, -1, -1};
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

    // Room for the lyrics: the page minus sidebar, cover and margins.
    const int lyrics_w = std::max(20, width - 33 - cover_w - 10);

    Element cover = art.empty()
        ? filler() | size(WIDTH, EQUAL, cover_w) | size(HEIGHT, EQUAL, cover_h) | border
        : image_view(ui::cover(art)) | size(WIDTH, EQUAL, cover_w) | size(HEIGHT, EQUAL, cover_h);
    cover = cover | reflect(ui::now_playing_cover_box);

    const float ratio = duration > 0 ? static_cast<float>(std::clamp(position / duration, 0.0, 1.0)) : 0.f;

    Element info = vbox({
        text(""),
        text(utils::trimSuffix(ref.title, cover_w, "...")) | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | reflect(ui::now_playing_title_box),
        // Click the artist or the album to open it; right-click for the track's menu.
        hbox({ text(artist) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)) | reflect(ui::now_playing_artist_box), filler() }),
        hbox({ text(song->album.title) | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)) | reflect(ui::now_playing_album_box), filler() }),
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
    ui::lyric_boxes.clear();
    const bool ours = state->lyrics && state->lyrics_id == ref.id;
    const auto lyrics_flag = state->flags.count("lyrics") ? state->flags["lyrics"] : services::Player::Flags::Done;

    if (!Config::get().ENABLE_LYRICS) {
        lines.push_back(text("Lyrics are off (ENABLE_LYRICS)") | dim);
    } else if (!ours && lyrics_flag == services::Player::Flags::Ongoing) {
        lines.push_back(hbox({ spinner(15, ui::anim_frame) | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)), text(" looking for lyrics") | dim }));
    } else if (!ours || !state->lyrics->found) {
        lines.push_back(text("No lyrics found for this track") | dim);
    } else if (!state->lyrics->lines.empty()) {
        /* Synced lyrics, sung along
        - The current line fills word by word as it is sung; sung lines fade
          to grey, the next ones wait in a lighter grey
        - A change of line eases in (the old line fades out, the new one in)
          instead of jumping
        - Instrumental gaps show three dots that fill until the voice is back
        */
        const auto& ls = state->lyrics->lines;
        int cur = -1;
        for (size_t i = 0; i < ls.size(); i++) if (ls[i].first <= position + 0.15) cur = static_cast<int>(i);

        static std::string last_song;
        static int last_cur = -2;
        static std::chrono::steady_clock::time_point changed_at{};
        const auto now = std::chrono::steady_clock::now();
        if (last_song != ref.id) { last_song = ref.id; last_cur = cur; changed_at = now - std::chrono::seconds(1); }
        if (cur != last_cur) { last_cur = cur; changed_at = now; }
        const float ease = std::clamp(std::chrono::duration<float>(now - changed_at).count() / 0.35f, 0.f, 1.f);

        const Color sung   = ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY);
        const Color ahead  = ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY);
        const Color bright = Color::RGB(255, 255, 255);
        const Color accent = ui::GetColor(ui::MColor::ACCENT_PRIMARY);

        const int visible = std::max(5, height - 6);     // rows
        const int before = visible / 3;

        // Rows each line takes once wrapped, to keep the current line a third
        // of the way down whatever the line lengths.
        auto rows_of = [&](int i) {
            if (ls[i].second.empty()) return 1;
            return static_cast<int>(wrapWords(splitWords(ls[i].second), lyrics_w).size());
        };
        int start = std::max(0, cur);
        for (int used = 0; start > 0 && used + rows_of(start - 1) <= before; ) used += rows_of(--start);

        int rows_left = visible;
        for (int i = start; i < static_cast<int>(ls.size()) && rows_left > 0; i++) {
            const double t0 = ls[i].first;
            const double t1 = i + 1 < static_cast<int>(ls.size()) ? ls[i + 1].first : std::max(duration, t0 + 4.0);
            const float sungness = static_cast<float>(std::clamp((position - t0) / std::max(0.5, (t1 - t0) * 0.85), 0.0, 1.0));

            if (ls[i].second.empty()) {
                // Instrumental: three dots filling over the gap.
                if (i == cur) {
                    Elements dots;
                    for (int d = 0; d < 3; d++) {
                        const bool on = sungness * 3.f > static_cast<float>(d);
                        dots.push_back(text(on ? "\u25cf " : "\u25cb ") | color(on ? bright : ahead));
                    }
                    lines.push_back(hbox(std::move(dots)));
                } else {
                    auto b = std::make_shared<Box>();
                    ui::lyric_boxes.emplace_back(b, t0);
                    lines.push_back(text("\u2022 \u2022 \u2022") | color(i < cur ? sung : ahead) | reflect(*b));
                }
                rows_left--;
                continue;
            }

            const std::vector<std::string> words = splitWords(ls[i].second);
            const auto rows = wrapWords(words, lyrics_w);
            const int filled = i == cur ? static_cast<int>(std::ceil(sungness * static_cast<float>(words.size()))) : 0;

            for (const auto& row : rows) {
                if (rows_left-- <= 0) break;
                Elements ws;
                for (int k : row) {
                    Color c;
                    if (i == cur) {
                        c = k < filled ? bright : ahead;
                        if (ease < 1.f) c = Color::Interpolate(ease, ahead, c);         // fades in
                    } else {
                        c = i < cur ? sung : ahead;
                        if (i == cur - 1 && ease < 1.f) c = Color::Interpolate(ease, bright, sung);   // fades out
                    }
                    Element w = text(words[k] + " ") | color(c);
                    ws.push_back(i == cur ? w | bold : w);
                }
                auto b = std::make_shared<Box>();
                ui::lyric_boxes.emplace_back(b, t0);
                lines.push_back(hbox(std::move(ws)) | reflect(*b));
            }
        }
    } else {
        for (const auto& t : state->lyrics->plain) lines.push_back(paragraph(t.empty() ? " " : t) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)));
    }

    Element lyrics = vbox({
        text(""),
        text("Lyrics") | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)),
        text(""),
        vbox(std::move(lines)) | flex,
    }) | flex;

    return hbox({
        text("  "),
        vbox({ cover, info }),
        text("    "),
        lyrics,
        text("  "),
    }) | flex;
}

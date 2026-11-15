#include "../../../include/ui/components/now_playing.hpp"
#include "../../../include/ui/constants/colors.hpp"
#include "../../../include/ui/cover_cache.hpp"
#include "../../../include/utils/utils.hpp"
#include "../../../include/ui/components/content.hpp"
#include "../../../include/config/config.hpp"

#include "image_view.hpp"
#include <ftxui/screen/string.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include "../../../include/ui/anim.hpp"
#include "../../../include/ui/fade.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace ftxui;

namespace {

int swipe_dir = 0;                                   // the last swipe on the cover (1: next)
ui::anim::Clock::time_point swipe_at{};

// Its child drawn `skip` rows higher, clipped to its box: the lyrics column
// glides a row at a time instead of jumping a whole line.
class ShiftUp : public ftxui::Node {
public:
    ShiftUp(ftxui::Element child, int skip) : Node(ftxui::Elements{std::move(child)}), skip_(skip) {}
    void ComputeRequirement() override {
        children_[0]->ComputeRequirement();
        requirement_ = children_[0]->requirement();
        requirement_.min_y = std::max(0, requirement_.min_y - skip_);
    }
    void SetBox(ftxui::Box box) override {
        Node::SetBox(box);
        ftxui::Box b = box;
        b.y_min -= skip_;
        children_[0]->SetBox(b);
    }
    void Render(ftxui::Screen& screen) override {
        const ftxui::Box saved = screen.stencil;
        screen.stencil = ftxui::Box::Intersection(box_, screen.stencil);
        children_[0]->Render(screen);
        screen.stencil = saved;
    }
private:
    int skip_;
};

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
std::vector<ui::UpNextRow> ui::now_playing_upnext;
std::vector<std::pair<Box, int>> ui::now_playing_upnext_boxes;
Box ui::now_playing_upnext_area{-1, -1, -1, -1};
std::unordered_map<std::string, double> ui::lyrics_offset;
Box ui::now_playing_progress_box{-1, -1, -1, -1};
int ui::now_playing_upnext_scroll = 0;

Element ui::renderNowPlaying(services::Player::PlayerState* state, double position, double duration, bool playing, int width, int height) {
    ui::lyric_boxes.clear();
    ui::now_playing_cover_box = Box{-1, -1, -1, -1};
    ui::now_playing_title_box = ui::now_playing_artist_box = ui::now_playing_album_box = Box{-1, -1, -1, -1};
    ui::now_playing_upnext_boxes.clear();
    ui::now_playing_upnext_area = Box{-1, -1, -1, -1};
    ui::now_playing_progress_box = Box{-1, -1, -1, -1};
    auto song = std::dynamic_pointer_cast<music::Song>(state->current);
    if (!song) {
        return text("Nothing is playing. Press v or Esc to go back.") | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)) | center | flex;
    }

    const auto& ref = song->ref;
    const std::string artist = ref.artists.empty() ? "" : ref.artists[0].name;
    const std::string art = ref.thumbnail_large.empty() ? ref.thumbnail_small : ref.thumbnail_large;

    // No lyrics to show (none found, or turned off): the cover and the track
    // take the page on their own, centred and larger.
    const bool ours = state->lyrics && state->lyrics_id == ref.id;
    // Only once the lookup has answered (it starts with the stream): before
    // that the layout would jump from centred to two columns.
    // Words without timings don't follow the song: shown as a wall of text,
    // they were more in the way than useful. The cover takes the page instead.
    const bool unsynced = ours && state->lyrics->found && state->lyrics->lines.empty();
    const bool no_lyrics = !Config::get().ENABLE_LYRICS || (ours && !state->lyrics->found) || unsynced;

    // Cover: as large as the page allows, square in terminal cells (2:1).
    const int room_w = std::max(16, width - (ui::sidebarWidth(width) + 7) - 8);     // the page minus sidebar and margins
    // With lyrics, "up next" gets a column when the page is wide enough,
    // the cover giving up a little room for it (down to 12 rows).
    constexpr int UP_W = 34;
    const int page_w = width - (ui::sidebarWidth(width) + 7);                       // the page: the screen minus the sidebar
    const int fixed = 3 + 6 + 30 + 4 + UP_W + 2;         // margins, lyrics' minimum, the column
    const bool room_for_next = !no_lyrics && !ui::now_playing_upnext.empty() && page_w - fixed >= 24;
    int cover_h = no_lyrics ? std::clamp(std::min(height - 9, room_w / 2), 8, 40)
                            : std::clamp(height - 12, 8, 26);
    if (room_for_next) cover_h = std::min(cover_h, std::max(8, (page_w - fixed) / 2));
    const int cover_w = cover_h * 2;

    // Room for the lyrics: the page minus sidebar, cover and margins.
    int lyrics_w = std::max(20, width - (ui::sidebarWidth(width) + 7) - cover_w - 13);

    Element cover = art.empty()
        ? filler() | size(WIDTH, EQUAL, cover_w) | size(HEIGHT, EQUAL, cover_h) | border
        : ui::coverArt(art, cover_w, cover_h);
    cover = cover | reflect(ui::now_playing_cover_box);

    /* A new track: its cover fades in, and after a swipe it also slides in
    from that side (3 columns, the lyrics staying put). */
    static std::string cover_song;
    static ui::anim::Clock::time_point cover_at{};
    if (cover_song != ref.id) { cover_song = ref.id; cover_at = ui::anim::Clock::now(); }
    const double cover_p = ui::anim::easeOut(ui::anim::progress(cover_at, 260));
    cover = ui::fadeIn(cover, cover_p);
    int slide = 0;
    if (swipe_dir != 0) {
        const double sp = ui::anim::progress(swipe_at, 260);
        slide = static_cast<int>(std::lround(swipe_dir * 3 * (1.0 - ui::anim::easeOut(sp))));
        if (sp >= 1.0) swipe_dir = 0;
    }

    const float ratio = duration > 0 ? static_cast<float>(std::clamp(position / duration, 0.0, 1.0)) : 0.f;

    /* The track under the cover
    - Title on up to two lines, then "artist · album" (click to open them)
    - A thin progress line (click / drag to seek), the times under its ends
    */
    auto line = [&](Element e, bool centre) {
        return centre ? hbox({ filler(), std::move(e), filler() }) : hbox({ std::move(e), filler() });
    };
    auto track_block = [&](bool centre) {
        Elements title_rows;
        const std::vector<std::string> words = splitWords(ref.title);
        const auto rows = wrapWords(words, cover_w);
        for (size_t r = 0; r < rows.size() && r < 2; r++) {
            std::string t;
            for (int k : rows[r]) t += (t.empty() ? "" : " ") + words[k];
            if (r == 1 && rows.size() > 2) t = utils::trimSuffix(t + " ...", cover_w - 3, "...");
            title_rows.push_back(line(text(utils::trimSuffix(t, cover_w, "...")) | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)), centre));
        }
        if (title_rows.empty()) title_rows.push_back(text(""));

        // Artist and album share one line, the album dimmer.
        const std::string alb = song->album.title;
        const int art_w = static_cast<int>(utils::utf8Length(artist));
        Element who = hbox({
            text(utils::trimSuffix(artist, cover_w, "\u2026")) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)) | reflect(ui::now_playing_artist_box),
            alb.empty() || art_w + 6 >= cover_w ? emptyElement() : hbox({
                text(" \u00b7 ") | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)),
                text(utils::trimSuffix(alb, cover_w - art_w - 3, "\u2026")) | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)) | reflect(ui::now_playing_album_box),
            }),
        });

        // A thin progress line as wide as the cover, the times under its ends.
        // In half cells: the head moves twice as often ("╸", half a line).
        const int halves = std::clamp(static_cast<int>(ratio * static_cast<float>(cover_w) * 2.f), 0, cover_w * 2);
        const int full = halves / 2, half = halves % 2;
        std::string done, rest;
        for (int i = 0; i < full; i++) done += "\u2501";
        if (half) done += "\u2578";
        for (int i = full + half; i < cover_w; i++) rest += "\u2501";
        return vbox({
            text(""),
            vbox(std::move(title_rows)) | reflect(ui::now_playing_title_box),
            line(std::move(who), centre),
            text(""),
            hbox({
                text(done) | color(ui::GetColor(ui::MColor::ACCENT_PRIMARY)),
                text(rest) | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)),
            }) | reflect(ui::now_playing_progress_box),
            hbox({
                text(clock(position)) | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
                filler(),
                text(playing ? "" : "paused") | dim,
                filler(),
                text(clock(duration)) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)),
            }),
        }) | size(WIDTH, EQUAL, cover_w);
    };

    /* Up next: the queue, then autoplay, as in the queue view; a column of
    its own when the page is wide enough. */
    auto up_next = [&](int rows_room) -> Element {
        if (ui::now_playing_upnext.empty()) return emptyElement();
        ui::now_playing_upnext_boxes.reserve(ui::now_playing_upnext.size());
        Elements col;
        bool in_auto = false, first = true;
        int used = 0;
        const int total = static_cast<int>(ui::now_playing_upnext.size());
        ui::now_playing_upnext_scroll = std::clamp(ui::now_playing_upnext_scroll, 0, std::max(0, total - 1));
        bool more = false;
        for (int k = ui::now_playing_upnext_scroll; k < total; k++) {
            const auto& r = ui::now_playing_upnext[k];
            if (first || r.autoplay != in_auto) {
                if (used + 6 > rows_room) break;      // a heading needs a row under it
                if (!first) { col.push_back(text("")); used++; }
                col.push_back(text(r.autoplay ? "Autoplay" : "Next in queue") | bold | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)));
                col.push_back(separatorLight() | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)));
                used += 2;
                in_auto = r.autoplay; first = false;
            }
            if (used + 3 > rows_room - 1) { more = true; break; }
            Element thumb = r.thumb.empty() ? ui::noCover(4, 2)
                                            : ui::coverArt(r.thumb, 4, 2);
            ui::now_playing_upnext_boxes.emplace_back(Box{-1, -1, -1, -1}, r.index);
            Box& b = ui::now_playing_upnext_boxes.back().first;
            col.push_back(text(""));
            col.push_back(hbox({
                thumb, text("  "),
                vbox({
                    text(utils::trimSuffix(r.title, UP_W - 7, "...")) | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)),
                    text(utils::trimSuffix(r.artist, UP_W - 7, "...")) | color(ui::GetColor(ui::MColor::TEXT_BOTTOM_SECONDARY)),
                }),
                filler(),
            }) | reflect(b));
            used += 3;
        }
        // Rows above or below: say so (the wheel scrolls the column).
        Element above = ui::now_playing_upnext_scroll > 0
            ? text("\u2191 " + std::to_string(ui::now_playing_upnext_scroll) + " more") | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY))
            : text("");
        Element below = more ? text("\u2193 more") | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)) : text("");
        return vbox({ above, vbox(std::move(col)), text(""), below, filler() }) | size(WIDTH, EQUAL, UP_W) | reflect(ui::now_playing_upnext_area);
    };
    if (room_for_next) lyrics_w = std::max(20, lyrics_w - UP_W - 2);


    if (no_lyrics) {
        Element centre_block = vbox({
            cover, track_block(true),
            unsynced ? vbox({ text(""), line(text("No synced lyrics for this track") | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)), true) }) : emptyElement(),
        });
        if (page_w >= cover_w + 8 + UP_W + 6)
            centre_block = hbox({ std::move(centre_block), text("      "), up_next(height - 2) });
        return vbox({ filler(), line(std::move(centre_block), true), filler() }) | flex;
    }

    // Lyrics: synced lines around the current one, or the plain text.
    Elements lines;
    int lyric_skip = 0;       // rows of the first line drawn above the column (gliding)
    bool shift_badge = false; // the "+8.0 s" row sits first in lines

    if (!Config::get().ENABLE_LYRICS) {
        lines.push_back(text("Lyrics are off (ENABLE_LYRICS)") | dim);
    } else if (!ours) {
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
        // A hand shift: positive = the words come later.
        // Your own shift, else the one suggested for a music video.
        const double shift = ui::lyrics_offset.count(ref.id) ? ui::lyrics_offset[ref.id] : state->lyrics->shift;
        const double lpos = position - shift;   // the lyrics' clock, not the progress line's
        int cur = -1;
        for (size_t i = 0; i < ls.size(); i++) if (ls[i].first <= lpos + 0.15) cur = static_cast<int>(i);

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

        const int visible = std::max(5, height - 2);     // rows
        const int before = visible * 2 / 5;
        // A blank row between lines when there's room: easier to follow.
        const int gap = height >= 22 ? 1 : 0;

        // Rows each line takes once wrapped, to keep the current line a third
        // of the way down whatever the line lengths.
        auto rows_of = [&](int i) {
            if (ls[i].second.empty()) return 1 + gap;
            return static_cast<int>(wrapWords(splitWords(ls[i].second), lyrics_w).size()) + gap;
        };
        int start = std::max(0, cur);
        for (int used = 0; start > 0 && used + rows_of(start - 1) <= before; ) used += rows_of(--start);

        /* The column glides to its new place (eased, a row at a time) when
        the current line moves on, instead of jumping by a whole line. In
        rows from the top of the song; a new song or a seek far away jumps. */
        {
            static double shown_row = -1;
            static std::string glide_song;
            int target_row = 0;
            for (int i = 0; i < start; i++) target_row += rows_of(i);
            if (glide_song != ref.id || shown_row < 0 || std::abs(shown_row - target_row) > visible) { glide_song = ref.id; shown_row = target_row; }
            ui::anim::approach(shown_row, target_row, 0.3, 0.3);
            const int top_row = static_cast<int>(std::lround(shown_row));
            int s2 = 0, before_s2 = 0;
            while (s2 < static_cast<int>(ls.size()) && before_s2 + rows_of(s2) <= top_row) before_s2 += rows_of(s2++);
            if (s2 != start || top_row != target_row) {
                start = std::min(s2, static_cast<int>(ls.size()) - 1);
                lyric_skip = std::max(0, top_row - before_s2);
            }
        }

        int rows_left = visible + lyric_skip;
        // The shift badge takes a row at the top.
        if (shift != 0.0) rows_left--;

        // Before the first line (the intro): three dots that fill until the
        // voice comes in, instead of a column of grey lines and nothing lit.
        if (cur < 0 && !ls.empty() && ls[0].first > 1.0) {
            const float intro = static_cast<float>(std::clamp(lpos / ls[0].first, 0.0, 1.0));
            Elements dots;
            for (int d = 0; d < 3; d++) {
                const bool on = intro * 3.f > static_cast<float>(d);
                dots.push_back(text(on ? "\u25cf " : "\u25cb ") | color(on ? bright : ahead));
            }
            lines.push_back(hbox(std::move(dots)));
            rows_left--;
            if (gap && rows_left > 0) { lines.push_back(text("")); rows_left--; }
        }

        for (int i = start; i < static_cast<int>(ls.size()) && rows_left > 0; i++) {
            const double t0 = ls[i].first;
            const double t1 = i + 1 < static_cast<int>(ls.size()) ? ls[i + 1].first : std::max(duration, t0 + 4.0);
            const float sungness = static_cast<float>(std::clamp((lpos - t0) / std::max(0.5, (t1 - t0) * 0.85), 0.0, 1.0));

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
                    ui::lyric_boxes.emplace_back(b, t0 + shift);
                    lines.push_back(text("\u2022 \u2022 \u2022") | color(i < cur ? sung : ahead) | reflect(*b));
                }
                rows_left--;
                if (gap && rows_left > 0) { lines.push_back(text("")); rows_left--; }
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
                        // Further back, fainter too.
                        if (i < cur - 2) c = Color::Interpolate(std::min(0.6f, 0.2f * static_cast<float>(cur - 2 - i)), sung, Color::RGB(40, 40, 44));
                        // Further ahead, fainter: the eye stays near the current line.
                        if (i > cur + 2) c = Color::Interpolate(std::min(0.6f, 0.15f * static_cast<float>(i - cur - 2)), ahead, sung);
                        if (i == cur - 1 && ease < 1.f) c = Color::Interpolate(ease, bright, sung);   // fades out
                    }
                    Element w = text(words[k] + " ") | color(c);
                    ws.push_back(i == cur ? w | bold : w);
                }
                auto b = std::make_shared<Box>();
                ui::lyric_boxes.emplace_back(b, t0 + shift);
                lines.push_back(hbox(std::move(ws)) | reflect(*b));
            }
            if (gap && rows_left > 0) { lines.push_back(text("")); rows_left--; }
        }
    } else {
        for (const auto& t : state->lyrics->plain) lines.push_back(paragraph(t.empty() ? " " : t) | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)));
    }

    // Synced lyrics fill the column; a message or plain text sits level with the cover.
    const bool synced = ours && state->lyrics->found && !state->lyrics->lines.empty();
    const bool fill = synced || lines.size() > 2;     // plain text, top down
    const double shown_shift = !ours || !state->lyrics ? 0.0 : ui::lyrics_offset.count(ref.id) ? ui::lyrics_offset[ref.id] : state->lyrics->shift;
    if (synced && shown_shift != 0.0) {
        char badge[32];
        std::snprintf(badge, sizeof badge, "%+.1f s", shown_shift);
        lines.insert(lines.begin(), text(badge) | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY)));
        shift_badge = true;
    }
    // Synced lines centred in their column: balanced between the cover and
    // up next, like a lyrics view, rather than flush left with a gap after.
    if (synced) for (auto& l : lines) l = hbox({ filler(), std::move(l), filler() });
    if (synced && lyric_skip > 0) {
        // The badge (if any) stays put; the words glide.
        const bool badge = shift_badge;
        Elements words(lines.begin() + (badge ? 1 : 0), lines.end());
        Element col = std::make_shared<ShiftUp>(vbox(std::move(words)), lyric_skip);
        Elements kept;
        if (badge) kept.push_back(lines.front());
        kept.push_back(col);
        lines = std::move(kept);
    }
    Element lyrics = fill
        ? vbox({ text(""), vbox(std::move(lines)) | flex }) | flex
        : vbox({ filler(), vbox(std::move(lines)), filler() }) | flex;

    return hbox({
        text(std::string(static_cast<size_t>(3 + slide), ' ')),
        vbox({ filler(), cover, track_block(true), filler() }),     // centred under the cover
        text(std::string(static_cast<size_t>(6 - slide), ' ')),
        lyrics,
        room_for_next ? hbox({ text("  "), up_next(height - 2) }) : emptyElement(),
        text("  "),
    }) | flex;
}

void ui::nowPlayingSwiped(int dir) { swipe_dir = dir > 0 ? 1 : -1; swipe_at = ui::anim::Clock::now(); }

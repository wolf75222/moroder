#include <algorithm>
#include "../../../include/ui/components/search_bar.hpp"
#include "../../../include/ui/constants/colors.hpp"

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

using namespace ftxui;

ftxui::Component ui::SearchBar(SearchBarData* data) {
    InputOption option;

    option.placeholder = "Search songs, albums, artists, podcasts";
    option.multiline = false;

    option.transform = [data](const InputState& state) {
        Element e = state.element;

        if (!data->value.empty())
            return e | color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) | bold;

        return e | color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY));
    };

    option.on_enter = [data]() {
        // A highlighted suggestion is what gets searched.
        if (data->suggestion_index >= 0 && data->suggestion_index < static_cast<int>(data->suggestions.size()) && data->suggestions_for == data->value) {
            data->value = data->suggestions[data->suggestion_index];
            data->cursor = static_cast<int>(data->value.size());
        }
        data->suggestions.clear();
        data->suggestion_index = -1;
        // Remember it (newest first, no duplicates, 30 at most).
        if (!data->value.empty()) {
            auto& h = data->history;
            h.erase(std::remove(h.begin(), h.end(), data->value), h.end());
            h.insert(h.begin(), data->value);
            if (h.size() > 30) h.resize(30);
        }
        data->history_index = -1;
        data->onSearch(data->value);
    };
    option.cursor_position = &data->cursor;

    auto input = Input(&data->value, option);

    input = CatchEvent(input, [data](Event event){
        // With suggestions showing, Up / Down move through them; Tab fills one in.
        const bool listing = !data->value.empty() && !data->suggestions.empty() && data->suggestions_for == data->value;
        if (listing && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            const int n = static_cast<int>(data->suggestions.size());
            int i = data->suggestion_index + (event == Event::ArrowDown ? 1 : -1);
            data->suggestion_index = std::clamp(i, -1, n - 1);
            return true;
        }
        if (listing && event == Event::Tab) {
            const int i = std::max(0, data->suggestion_index);
            data->value = data->suggestions[i];
            data->cursor = static_cast<int>(data->value.size());
            data->suggestion_index = -1;
            return true;
        }

        // Up / Down: previous searches, like a shell history.
        if(event == Event::ArrowUp || event == Event::ArrowDown) {
            const int n = static_cast<int>(data->history.size());
            if (n == 0) return true;
            // Down with no history step taken: leave what is typed alone.
            if (event == Event::ArrowDown && data->history_index < 0) return true;
            int i = data->history_index + (event == Event::ArrowUp ? 1 : -1);
            if (i >= n) i = n - 1;
            if (i < 0) { data->history_index = -1; data->value.clear(); data->cursor = 0; return true; }
            data->history_index = i;
            data->value = data->history[i];
            data->cursor = static_cast<int>(data->value.size());
            return true;
        }

        return false;
    });

    return Renderer(input, [input, data] {
        // Typing changed the text: new suggestions (the caller fetches them).
        static std::string last;
        if (data->value != last) {
            last = data->value;
            data->suggestion_index = -1;
            if (data->onTyped && input->Focused()) data->onTyped(data->value);
        }
        return vbox({
            text(" "),
            hbox({
                text("  "),
                text("") | (input->Focused() ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY)) : color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY))),
                text("  "),
                input->Render(),
            }),
        }) | reflect(data->box);
    });
}

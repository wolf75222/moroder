#include "../../../include/ui/components/chips.hpp"

#include "../../../include/ui/constants/colors.hpp"
#include "../../../include/utils/utils.hpp"

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>

using namespace ftxui;

void ui::setChipsData(ChipsData* data, const std::vector<ChipEntry>& entries) {
    bool ids_changed = data->entries.size() != entries.size();

    if (!ids_changed) {
        for (size_t i = 0; i < entries.size(); i++) {
            if (data->entries[i].id != entries[i].id) { ids_changed = true; break; }
        }
    }

    data->entries = entries;

    if (ids_changed) {
        data->entries_spoof.clear();
        for (auto& chip : data->entries) {
            data->entries_spoof.push_back(chip.id);
        }
    }
}

ftxui::Component ui::Chips(ChipsData* data, int* selected, bool* focused, std::function<bool(const ftxui::Event&, const ChipEntry&)> on_press) {
    MenuOption options;
    options.direction = Direction::Right;
    options.Horizontal();

    options.entries_option.transform = [data, focused](const EntryState& state) {
        if (state.index < 0 || state.index >= (int)data->entries.size()) {
            return emptyElement();
        }

        const auto& chip = data->entries[state.index];
        bool active = state.active && *focused;

        /* Tabs, not boxed pills: the chosen one is white with an accent
        underline, the one under the keyboard is white, the others grey.
        */
        const bool narrow = ftxui::Terminal::Size().dimx < 100;
        const std::string name = utils::trimSuffix(narrow && !chip.short_label.empty() ? chip.short_label : chip.label, 24, "\u2026");
        Element label = text(name) | (chip.enabled || active ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY))
                                                            : color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)));
        if (chip.enabled || active) label = label | bold;
        std::string under;
        for (size_t i = 0; i < utils::utf8Length(name); i++) under += chip.enabled ? "\u2501" : "\u2500";
        Element line = (chip.enabled || active)
            ? text(under) | color(chip.enabled ? ui::GetColor(ui::MColor::ACCENT_PRIMARY) : ui::GetColor(ui::MColor::SEPARATOR_PRIMARY))
            : text(std::string(utils::utf8Length(name), ' '));
        // A narrow window: less air between tabs, so Explore's four still fit
        // at 80 columns (the last one went past the edge, out of the mouse's reach).
        const std::string pad = ftxui::Terminal::Size().dimx < 120 ? " " : "  ";
        return hbox({ text(pad), vbox({ text(""), label, line }), text(pad) });
    };

    auto menu = Menu(&data->entries_spoof, selected, options);

    menu = CatchEvent(menu, [data, selected, on_press](const ftxui::Event& event) {
        if (data->entries.empty()) return false;

        int index = std::clamp(*selected, 0, (int)data->entries.size() - 1);
        return on_press(event, data->entries[index]);
    });

    return Renderer(menu, [menu] {
        return vbox({
            hbox({
                menu->Render() | xframe,
            }),
        });
    });
}

void ui::toggleChip(ChipsData* data, const std::string& id) {
    for (auto& entry : data->entries) {
        if (entry.id == id) {
            entry.enabled = !entry.enabled;

        } else if (data->exclusive) {
            entry.enabled = false;
        }
    }
}

#include "../../../include/ui/components/chips.hpp"

#include "../../../include/ui/constants/colors.hpp"
#include "../../../include/utils/utils.hpp"

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include "../../../include/ui/anim.hpp"

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

namespace {
// The chosen tab's underline, at absolute columns on the row's third line.
class Underline : public ftxui::Node {
public:
    Underline(int x, int w) : x_(x), w_(w) {}
    void ComputeRequirement() override { requirement_ = ftxui::Requirement{}; }
    void Render(ftxui::Screen& screen) override {
        const int y = box_.y_min + 2;
        if (y > box_.y_max) return;
        for (int x = std::max(x_, box_.x_min); x < x_ + w_ && x <= box_.x_max; x++) {
            auto& c = screen.CellAt(x, y);
            c.character = "\u2501";
            c.foreground_color = ui::GetColor(ui::MColor::ACCENT_PRIMARY);
        }
    }
private:
    int x_, w_;
};
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
        const std::string name = utils::trimSuffix(chip.label, 24, "\u2026");
        Element label = text(name) | (chip.enabled || active ? color(ui::GetColor(ui::MColor::TEXT_TOP_PRIMARY))
                                                            : color(ui::GetColor(ui::MColor::TEXT_TOP_SECONDARY)));
        if (chip.enabled || active) label = label | bold;
        // The chosen tab's red underline is drawn by the row (it slides from
        // tab to tab); the one under the keyboard gets a quiet line here.
        std::string under;
        for (size_t i = 0; i < utils::utf8Length(name); i++) under += "\u2500";
        Element line = (active && !chip.enabled)
            ? text(under) | color(ui::GetColor(ui::MColor::SEPARATOR_PRIMARY))
            : text(std::string(utils::utf8Length(name), ' '));
        if (data->label_boxes.size() < data->entries.size()) data->label_boxes.resize(data->entries.size());
        label = label | reflect(data->label_boxes[state.index]);
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

    return Renderer(menu, [menu, data] {
        // Where the underline goes: under the chosen tab, as laid out last frame.
        int target = -1;
        for (size_t i = 0; i < data->entries.size() && i < data->label_boxes.size(); i++)
            if (data->entries[i].enabled && data->label_boxes[i].x_max >= data->label_boxes[i].x_min) target = static_cast<int>(i);
        Element under = emptyElement();
        // A chosen tab not laid out yet (a rebuilt row): one more frame places it.
        if (target < 0) for (const auto& e : data->entries) if (e.enabled) { ui::anim::more(); break; }
        if (target >= 0) {
            const Box& b = data->label_boxes[target];
            const double tx = b.x_min, tw = b.x_max - b.x_min + 1;
            if (data->under_x < 0) { data->under_x = tx; data->under_w = tw; }
            ui::anim::approach(data->under_x, tx, 0.4, 0.4);
            ui::anim::approach(data->under_w, tw, 0.4, 0.4);
            under = std::make_shared<Underline>(static_cast<int>(std::lround(data->under_x)), static_cast<int>(std::lround(data->under_w)));
        }
        return dbox({ menu->Render() | xframe, under });
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

#pragma once

#include <ftxui/screen/box.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include <functional>
#include <string>
#include <vector>

namespace ui {

struct ChipEntry {
    std::string id;
    std::string label;

    bool enabled = false;
};

struct ChipsData {
    std::vector<ChipEntry> entries;
    std::vector<std::string> entries_spoof;
    bool exclusive = false;

    // The chosen tab's underline slides between tabs: each label's box as
    // drawn last frame, and where the underline is now (x, width).
    std::vector<ftxui::Box> label_boxes;
    double under_x = -1, under_w = 0;
};

void setChipsData(ChipsData* data, const std::vector<ChipEntry>& entries);
void toggleChip(ChipsData *data, const std::string &id);

ftxui::Component Chips(ChipsData* data, int* selected, bool* focused, std::function<bool(const ftxui::Event&, const ChipEntry&)> on_press);

}

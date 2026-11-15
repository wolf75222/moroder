#pragma once

#include <string>

namespace ui {

struct SimpleEntry {
    std::string top;
    std::string bottom;
};

struct ImageEntry {
    std::string image_url;

    std::string top;
    std::string bottom;

    bool wide = false;      // a 16:9 picture (a music video)
};

}

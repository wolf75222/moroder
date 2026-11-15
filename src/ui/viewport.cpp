#include "../../include/ui/viewport.hpp"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <algorithm>

namespace ui {

namespace {

class ViewportNode : public ftxui::Node {
public:
    ViewportNode(ftxui::Elements children, std::vector<int> indices, int active, ViewportState* state)
        : Node(std::move(children)), indices_(std::move(indices)), active_(active), state_(state) {}

    void ComputeRequirement() override {
        requirement_ = {};
        for (auto& c : children_) {
            c->ComputeRequirement();
            requirement_.min_x = std::max(requirement_.min_x, c->requirement().min_x);
        }
        requirement_.min_y = 1;
        requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_y = 1;
        requirement_.flex_grow_x = 1;
        requirement_.flex_shrink_x = 1;
    }

    void SetBox(ftxui::Box box) override {
        Node::SetBox(box);
        const int view = box.y_max - box.y_min + 1;
        const int n = static_cast<int>(children_.size());

        // Content position of each child.
        std::vector<int> top(n + 1, 0);
        for (int i = 0; i < n; i++) top[i + 1] = top[i] + std::max(0, children_[i]->requirement().min_y);
        const int total = top[n];

        auto slot = [&](int page_index) {
            for (int i = 0; i < n; i++) if (indices_[i] == page_index) return i;
            return -1;
        };

        // Where the view was, in this window's coordinates.
        int y;
        const int a = slot(state_->anchor);
        if (a >= 0) {
            y = top[a] + state_->line;
        } else if (n > 0 && state_->anchor < indices_.front()) {
            y = 0;
        } else {
            y = total;
        }

        y += state_->pending;
        state_->pending = 0;

        // Keep the selection in view, moving no further than needed.
        const int s = slot(active_);
        if (state_->follow && s >= 0) {
            int from = top[s], to = top[s + 1];
            // Taller than the view (the library's grid): follow what is
            // selected inside it, not the top of the whole block.
            const auto& f = children_[s]->requirement().focused;
            if (to - from > view && f.enabled && f.box.y_max >= f.box.y_min) {
                from = top[s] + f.box.y_min;
                to = std::min(top[s + 1], top[s] + f.box.y_max + 1);
            }
            const int h = to - from;
            if (from < y) y = from;
            else if (to > y + view) y = h > view ? from : to - view;
        }

        y = std::max(0, std::min(y, std::max(0, total - view)));

        // Back to page index + line.
        state_->anchor = n > 0 ? indices_.front() : 0;
        state_->line = y;
        for (int i = 0; i < n; i++) {
            if (top[i + 1] > y) { state_->anchor = indices_[i]; state_->line = y - top[i]; break; }
        }

        state_->height = view;
        state_->first_visible = state_->last_visible = -1;
        for (int i = 0; i < n; i++) {
            if (top[i] >= y && top[i + 1] <= y + view) {
                if (state_->first_visible < 0) state_->first_visible = indices_[i];
                state_->last_visible = indices_[i];
            }
        }
        state_->at_top = y <= 0;
        state_->at_bottom = y + view >= total;

        boxes_.assign(n, ftxui::Box{});
        state_->drawn.clear();
        for (int i = 0; i < n; i++) {
            ftxui::Box& b = boxes_[i];
            b = box;
            b.y_min = box.y_min + top[i] - y;
            b.y_max = box.y_min + top[i + 1] - y - 1;
            if (b.y_max < box.y_min || b.y_min > box.y_max) {
                // Not drawn, so its click areas are never clipped: park them
                // far off screen, where no pointer can reach them.
                b.y_min -= 100000; b.y_max -= 100000;
            } else {
                state_->drawn.emplace_back(indices_[i], ftxui::Box::Intersection(b, box));
            }
            children_[i]->SetBox(b);
        }
    }

    void Render(ftxui::Screen& screen) override {
        const ftxui::Box saved = screen.stencil;
        screen.stencil = ftxui::Box::Intersection(box_, screen.stencil);
        for (size_t i = 0; i < children_.size() && i < boxes_.size(); i++) {
            const ftxui::Box& b = boxes_[i];
            if (b.y_max < box_.y_min || b.y_min > box_.y_max) continue;   // off screen (parked)
            children_[i]->Render(screen);
        }
        screen.stencil = saved;
    }

private:
    std::vector<int> indices_;
    std::vector<ftxui::Box> boxes_;
    int active_;
    ViewportState* state_;
};

}

ftxui::Element viewport(ftxui::Elements children, std::vector<int> indices, int active, ViewportState* state) {
    return std::make_shared<ViewportNode>(std::move(children), std::move(indices), active, state);
}

}

#include "ui/focus.h"

#include <cmath>

namespace ui {

void FocusGroup::Clear() {
    items_.clear();
    index_ = -1;
}

void FocusGroup::Add(Widget* w) {
    items_.push_back(w);
    if (index_ < 0 && Selectable(w)) index_ = static_cast<int>(items_.size()) - 1;
}

Widget* FocusGroup::Current() const {
    if (index_ < 0 || index_ >= static_cast<int>(items_.size())) return nullptr;
    return items_[static_cast<std::size_t>(index_)];
}

void FocusGroup::SetIndex(int i) {
    if (i >= 0 && i < static_cast<int>(items_.size()) && Selectable(items_[static_cast<std::size_t>(i)])) {
        index_ = i;
    }
}

bool FocusGroup::Move(app::Dir dir) {
    Widget* cur = Current();
    if (!cur || dir == app::Dir::None) return false;
    const Vec2 c = cur->rect.Center();

    int best = -1;
    float bestScore = 0.0f;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (i == index_) continue;
        const Widget* w = items_[static_cast<std::size_t>(i)];
        if (!Selectable(w)) continue;
        const Vec2 p = w->rect.Center();
        const float dx = p.x - c.x;
        const float dy = p.y - c.y;
        // Only candidates in the half-plane of the requested direction qualify.
        float along = 0.0f;
        float across = 0.0f;
        switch (dir) {
            case app::Dir::Up:    along = -dy; across = std::fabs(dx); break;
            case app::Dir::Down:  along = dy;  across = std::fabs(dx); break;
            case app::Dir::Left:  along = -dx; across = std::fabs(dy); break;
            case app::Dir::Right: along = dx;  across = std::fabs(dy); break;
            case app::Dir::None:  continue;
        }
        if (along <= 4.0f) continue;
        // Cross-axis offset costs more than distance along the axis, so rows and columns hold.
        const float score = along + 1.6f * across;
        if (best < 0 || score < bestScore) {
            best = i;
            bestScore = score;
        }
    }
    if (best < 0) return false;
    index_ = best;
    return true;
}

int FocusGroup::HitTest(float x, float y) const {
    // Later widgets are drawn on top, so they win ties.
    for (int i = static_cast<int>(items_.size()) - 1; i >= 0; --i) {
        const Widget* w = items_[static_cast<std::size_t>(i)];
        if (Selectable(w) && w->rect.Contains(x, y)) return i;
    }
    return -1;
}

void FocusGroup::EnsureValid() {
    if (index_ >= 0 && index_ < static_cast<int>(items_.size()) && Selectable(items_[static_cast<std::size_t>(index_)])) {
        return;
    }
    index_ = -1;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (Selectable(items_[static_cast<std::size_t>(i)])) {
            index_ = i;
            break;
        }
    }
}

}  // namespace ui

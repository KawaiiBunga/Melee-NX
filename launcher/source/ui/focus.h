// Focus navigation over a screen's widgets: directional moves picked by geometry, and touch hits.
#pragma once

#include <vector>

#include "app/input.h"
#include "ui/geometry.h"
#include "ui/widgets.h"

namespace ui {

class FocusGroup {
public:
    // Widgets are not owned. Add them in reading order: that order picks the first focus.
    void Clear();
    void Add(Widget* w);
    const std::vector<Widget*>& Items() const { return items_; }

    Widget* Current() const;
    int Index() const { return index_; }
    void SetIndex(int i);

    // Moves to the nearest selectable widget in `dir`. Returns true when focus changed.
    bool Move(app::Dir dir);

    // Index of the selectable widget under (x, y), or -1.
    int HitTest(float x, float y) const;

    // Re-picks focus if the current widget became unselectable (hidden, disabled, or removed).
    void EnsureValid();

private:
    static bool Selectable(const Widget* w) { return w->visible && w->focusable && w->enabled; }

    std::vector<Widget*> items_;
    int index_ = -1;
};

}  // namespace ui

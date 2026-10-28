#pragma once
#ifndef VOLCANO_GUI_ITEM_SLOT_VIEW_H
#define VOLCANO_GUI_ITEM_SLOT_VIEW_H

#include <cstdint>
#include <Pincel/Element.hpp>
#include "inventory/ItemStack.hpp"

namespace Volcano::GUI {

// One inventory slot's contents as Pincel elements: the item's icon as an
// Image, and its count when above 1. The icon is an ordinary Image, so a slot
// is just a square that takes the mouse and reports clicks.
class ItemSlotView {
public:
    // Adds a `size` pixel square at (x, y) inside `parent`, placed against its padding box.
    // `fontPixels` is the size of the count.
    static ItemSlotView Add(Pincel::Element parent, int x, int y, int size, int fontPixels);

    // Shows `item`, or nothing for an empty stack. Cheap when it is what is already shown.
    void Set(const ItemStack& item);

    // The square itself, for hover and clicks.
    Pincel::Element Slot() const { return slot; }

    // Forgets the icons made for each size, after the textures or the GUI scale changed.
    static void ClearCache();

private:
    Pincel::Element slot;
    Pincel::Element image;
    Pincel::Element count;
    int size = 0;
    int32_t shownId = -1;
    int shownCount = -1;
};

} // namespace Volcano::GUI

#endif

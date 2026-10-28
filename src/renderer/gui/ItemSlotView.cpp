#include "ItemSlotView.hpp"
#include <algorithm>
#include <string>
#include <unordered_map>
#include "GUIController.hpp"
#include "PincelSprite.hpp"

namespace Volcano::GUI {

namespace {

using Pincel::Color;

// Icons already fitted to a slot size, by item and size.
std::unordered_map<uint64_t, Sprite>& IconCache()
{
    static std::unordered_map<uint64_t, Sprite> cache;
    return cache;
}

// The item's icon at exactly `size` pixels square: pixel art is enlarged in whole steps and kept crisp,
// and the big rendered icons of blocks are shrunk once, here, rather than every time Pincel draws.
const Sprite* FittedIcon(const ItemStack& item, int size)
{
    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(item.itemId)) << 16) | static_cast<uint64_t>(size);
    auto& cache = IconCache();
    auto found = cache.find(key);
    if (found == cache.end())
    {
        Sprite fitted;
        if (const Sprite* source = GUIController::GetItemSprite(item))
        {
            fitted = size % source->width == 0 && size % source->height == 0 && source->width == source->height
                ? source->ScaledNearest(size / source->width)
                : source->Resized(size, size);
        }
        found = cache.emplace(key, std::move(fitted)).first;
    }
    return found->second.IsEmpty() ? nullptr : &found->second;
}

} // namespace

ItemSlotView ItemSlotView::Add(Pincel::Element parent, int x, int y, int size, int fontPixels)
{
    ItemSlotView view;
    view.size = size;

    Pincel::Insets corner;
    corner.left = x;
    corner.top = y;
    view.slot = parent.AddStack(Pincel::Orientation::Vertical);
    view.slot.SetPosition(Pincel::Position::Absolute)
        .SetInsets(corner)
        .SetSize(static_cast<uint16_t>(size), static_cast<uint16_t>(size));

    // The image has to exist to be filled later, so it starts out clear.
    Sprite blank;
    blank.width = size;
    blank.height = size;
    blank.rgba.assign(static_cast<size_t>(size) * size * 4, 0);
    if (auto image = blank.AddTo(view.slot))
    {
        view.image = *image;
        Pincel::Insets origin;
        origin.left = 0;
        origin.top = 0;
        view.image.SetPosition(Pincel::Position::Absolute)
            .SetInsets(origin)
            .SetPointerEvents(Pincel::PointerEvents::None)
            .SetVisible(false);
    }

    // Right-aligned to the slot's bottom-right, a pixel over, with a gray shadow.
    int offset = std::max(1, size / 16);
    Pincel::Insets bottomRight;
    bottomRight.right = -offset;
    bottomRight.bottom = -offset;
    view.count = view.slot.AddText("");
    view.count.SetPosition(Pincel::Position::Absolute)
        .SetInsets(bottomRight)
        .SetFontSize(static_cast<uint16_t>(fontPixels))
        .SetForeground(Color{ 255, 255, 255 })
        .SetTextShadow(Color{ 63, 63, 63 }, offset, offset)
        .SetPointerEvents(Pincel::PointerEvents::None)
        .SetVisible(false);
    return view;
}

void ItemSlotView::Set(const ItemStack& item)
{
    int32_t id = item.IsEmpty() ? 0 : item.itemId;
    int amount = item.IsEmpty() ? 0 : item.count;
    if (id == shownId && amount == shownCount) return;

    if (id != shownId)
    {
        const Sprite* icon = id != 0 ? FittedIcon(item, size) : nullptr;
        if (icon != nullptr) icon->SetOn(image);
        image.SetVisible(icon != nullptr);
    }
    if (amount > 1) count.SetText(std::to_string(amount));
    count.SetVisible(amount > 1);

    shownId = id;
    shownCount = amount;
}

void ItemSlotView::ClearCache()
{
    IconCache().clear();
}

} // namespace Volcano::GUI

#pragma once
#ifndef VOLCANO_SPLASH_SCREEN_H
#define VOLCANO_SPLASH_SCREEN_H

namespace Volcano::SplashScreen {

// Loads resources/splash-screen.jpeg and presents ONE frame showing it
// (letterboxed to the window's current aspect ratio), then tears down every
// Vulkan object it created before returning — none of that needs to
// outlive the present, since the swapchain image it wrote to just sits on
// screen (held by the compositor) regardless of what happens to the
// resources that drew into it.
//
// Call once, synchronously, right after VulkanInit::Init() (needs the
// device/swapchain/render pass/command pool it creates) and before the
// slow synchronous startup work — BlockRegistry::Init(), EntityRegistry::
// Init(), TextureManager::LoadResourcePack() — that otherwise leaves the
// window sitting there undrawn for the entire multi-second load.
//
// Never throws: a missing/unreadable splash image, or any Vulkan call
// along the way failing, is logged and simply skipped — a missing splash
// screen isn't worth failing startup over.
void Show();

} // namespace Volcano::SplashScreen

#endif

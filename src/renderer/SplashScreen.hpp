#pragma once
#ifndef VOLCANO_SPLASH_SCREEN_H
#define VOLCANO_SPLASH_SCREEN_H

namespace Volcano::SplashScreen {

// Loads resources/splash-screen.png (transparent background expected) and
// presents it fit to the window and centered, with an empty progress bar.
// Its Vulkan objects stay alive until Hide() so SetProgress() can redraw.
//
// Call once, synchronously, right after VulkanInit::Init() (needs the
// device/swapchain/render pass/command pool it creates) and before the
// slow synchronous startup work.
//
// Never throws: a missing/unreadable splash image, or any Vulkan call
// along the way failing, is logged and simply skipped — a missing splash
// screen isn't worth failing startup over.
void Show();

// Pumps the GLFW event queue (keeps the window from going "Not Responding")
// and redraws the progress bar. progress is 0..1. Cheap enough to call from
// inside loops; redraws are rate-limited. Safe to call with no splash up.
void SetProgress(float progress);

// Destroys everything Show() created. Safe to call if Show() failed or was
// never called. Call before the real frame loop starts.
void Hide();

} // namespace Volcano::SplashScreen

#endif

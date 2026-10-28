#pragma once
#ifndef VOLCANO_RESOURCES_RESOURCE_RELOADER_H
#define VOLCANO_RESOURCES_RESOURCE_RELOADER_H

namespace Volcano {

struct GlobalState;
class TextureManager;
class MeshingThread;

// Synchronous resource-pack reload: re-reads Graphics.ResourcePacks and
// rebuilds every cache that holds pack data (block visuals, atlas, item
// registry/models, GUI icons, entity skins, sky/cloud art, sounds, lang),
// then remeshes the loaded world.
namespace ResourceReloader {

// Tells the reloader which long-lived objects to rebuild. Call once from
// main() after both exist (GlobalState has no slot for them: adding one
// would change its layout and break mods).
void Attach(TextureManager* textureManager, MeshingThread* meshingThread);

// Entry point for F3+T and the future settings GUI (call it after changing
// Graphics.ResourcePacks). Render thread only, and only between frames: not
// while an ImGui frame or command buffer is being recorded (RenderThread
// runs it at the top of DrawFrame). Blocks for seconds, showing the splash
// screen with a progress bar. No-op if Attach() hasn't run.
void Reload(GlobalState* state);

// Asks for a reload at the next safe point (RenderThread's DrawFrame). Safe from any thread, such as the pack screen.
void Request();

// True once per Request(); the render thread polls it.
bool ConsumeRequest();

} // namespace ResourceReloader

} // namespace Volcano

#endif

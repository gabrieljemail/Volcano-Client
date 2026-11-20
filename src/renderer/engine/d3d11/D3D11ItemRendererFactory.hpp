#pragma once
#ifndef VOLCANO_ENGINE_D3D11_ITEM_RENDERER_FACTORY_H
#define VOLCANO_ENGINE_D3D11_ITEM_RENDERER_FACTORY_H

// Creating the Direct3D 11 item renderer without including <d3d11.h>: for
// translation units (VolcanoClient.cpp) whose own `using` declarations clash
// with windows.h. See D3D11ItemRenderer.hpp for the class.

namespace Volcano {
class TextureManager;
namespace Engine {
class RenderEngine;
namespace D3D11 {

class D3D11ItemRenderer;

// Builds and initializes the renderer for `engine` (which must be the Direct3D
// 11 one, with its texture array attached). Null, with the reason logged, if
// it can't be built: items then fall back to flat sprites and cubes.
D3D11ItemRenderer* CreateItemRenderer(RenderEngine* engine, const TextureManager* textures);
// Releases the GPU objects; the renderer stays valid (passes may still hold it
// until the engine shuts down). Follow with DestroyItemRenderer.
void ShutdownItemRenderer(D3D11ItemRenderer* renderer);
void DestroyItemRenderer(D3D11ItemRenderer* renderer);

} // namespace D3D11
} // namespace Engine
} // namespace Volcano

#endif

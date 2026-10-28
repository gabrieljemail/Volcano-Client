#include "ResourceReloader.hpp"
#include "ResourceManager.hpp"
#include "../GlobalState.hpp"
#include "../Logger.hpp"
#include "../ModLoader.hpp"
#include "../audio/SoundRegistry.hpp"
#include "../inventory/ItemRegistry.hpp"
#include "../network/TextComponent.hpp"
#include "../renderer/SplashScreen.hpp"
#include "../renderer/TextureManager.hpp"
#include "../renderer/engine/RenderEngine.hpp"
#include "../renderer/entity/EntityRenderer.hpp"
#include "../renderer/gui/GUIController.hpp"
#include "../renderer/misc/ItemModel.hpp"
#include "../renderer/misc/ItemRenderer.hpp"
#include "../renderer/misc/ModelLoader.hpp"
#include "../renderer/terrain/MeshingThread.hpp"
#include "../renderer/terrain/models/BlockRegistry.hpp"
#include "../renderer/terrain/models/World.hpp"
#include <atomic>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace Volcano::ResourceReloader {

namespace {

TextureManager* attachedTextures = nullptr;
MeshingThread* attachedMeshing = nullptr;
bool running = false;
std::atomic<bool> requested{false};

// Runs one rebuild step; a failure is logged and the reload carries on.
template<typename Fn>
void Step(const char* what, Fn&& fn)
{
    try
    {
        fn();
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[RESOURCES] Reload step failed (") + what + "): " + e.what());
    }
}

// Marks every loaded section dirty so MeshingThread remeshes it against the
// new layer indices and visual ids.
size_t RemeshWorld(GlobalState* state)
{
    if (state->world == nullptr) return 0;

    std::vector<SectionKey> sections = state->world->NonEmptySections();
    std::lock_guard<std::mutex> lock(state->networkInbox.mutex);
    for (const SectionKey& key : sections) state->networkInbox.dirtySections.push_back(key);
    return sections.size();
}

} // namespace

void Attach(TextureManager* textureManager, MeshingThread* meshingThread)
{
    attachedTextures = textureManager;
    attachedMeshing = meshingThread;
}

void Reload(GlobalState* state)
{
    if (running || attachedTextures == nullptr || state == nullptr || state->renderEngine == nullptr) return;
    running = true;

    Log::Info("[RESOURCES] Reloading resources...");
    TextureManager& textures = *attachedTextures;
    Engine::RenderEngine* engine = state->renderEngine;

    // Nothing may be in flight on the GPU before images start being destroyed.
    engine->WaitIdle();

    SplashScreen::Show();
    SplashScreen::SetProgress(0.02f);

    // The mesher reads layer indices and block visuals, so park it first.
    if (attachedMeshing != nullptr) attachedMeshing->Pause();
    SplashScreen::SetProgress(0.05f);

    Step("ResourceManager", [&] { ResourceManager::Instance().Reload(); });
    ModelLoader::ClearCache();
    ItemModel::ClearCache();
    SplashScreen::SetProgress(0.08f);

    // Re-resolve block visuals, then translate the ids already stored in
    // loaded chunks (they hold Block structs, not state ids).
    Step("BlockRegistry", [&] {
        BlockRegistry::VisualRemap remap = BlockRegistry::Reload();
        if (state->world != nullptr)
        {
            state->world->TransformBlocks([&remap](const Block& block) { return remap.Apply(block); });
        }
    });
    SplashScreen::SetProgress(0.15f);

    Step("TextureManager", [&] {
        textures.Reload([](float fraction) { SplashScreen::SetProgress(0.15f + fraction * 0.60f); });
    });
    SplashScreen::SetProgress(0.75f);

    Step("ItemRegistry", [&] { ItemRegistry::Init(textures); });

    // Point everything at the new atlas.
    if (textures.array.view != VK_NULL_HANDLE) engine->AttachTextures(textures);
    Step("Passes", [&] { engine->ReloadPassTextures(); });
    SplashScreen::SetProgress(0.80f);

    Step("ItemRenderer", [&] {
        if (state->itemRenderer != nullptr) state->itemRenderer->ReloadTextures();
    });
    Step("GUI", [&] { GUIController::ReloadTextures(&textures); });
    SplashScreen::SetProgress(0.88f);

    Step("EntityRenderer", [&] { EntityRenderer::ReloadTextures(); });
    SplashScreen::SetProgress(0.92f);

    Step("Lang", [&] { ReloadLang(); });
    Step("Sounds", [&] {
        if (state->soundRegistry != nullptr) state->soundRegistry->IndexSounds();
    });
    SplashScreen::SetProgress(0.95f);

    if (state->modLoader != nullptr) state->modLoader->Fire(EventType::TEXTURES_LOADED);

    size_t sections = RemeshWorld(state);
    if (attachedMeshing != nullptr) attachedMeshing->Resume();

    SplashScreen::SetProgress(1.0f);
    SplashScreen::Hide();
    Log::Info("[RESOURCES] Reload complete; remeshing " + std::to_string(sections) + " sections.");
    running = false;
}

void Request() { requested.store(true); }

bool ConsumeRequest() { return requested.exchange(false); }

} // namespace Volcano::ResourceReloader

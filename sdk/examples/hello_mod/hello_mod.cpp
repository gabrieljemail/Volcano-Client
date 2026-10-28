// A small tour of the mod API. Build it with -DVOLCANO_BUILD_EXAMPLES=ON; it
// lands in mods/ as hello_mod.dll (.so on Linux).
//
// It shows, in order: the mod entry points, event handlers (with and without
// user data), a HUD widget that draws the hotbar with the client's own item
// icons, a packet tap, a sound tap, a config value and a render pass. Every
// one of these is optional; a mod can use just the part it needs.
#include <atomic>
#include <memory>
#include <string>
#include <imgui.h>

#include "mods/VolcanoModAPI.h"
#include "GlobalState.hpp"
#include "Config.hpp"
#include "Logger.hpp"
#include "audio/AudioManager.hpp"
#include "inventory/InventoryManager.hpp"
#include "network/models/PacketEvent.hpp"
#include "renderer/engine/RenderEngine.hpp"
#include "renderer/engine/RenderPass.hpp"
#include "renderer/gui/GUIController.hpp"

using namespace Volcano;

namespace {

// Counters the HUD widget shows. Atomics because packets arrive on the
// network thread and ticks on the tick thread, while the HUD reads on the
// render thread.
std::atomic<uint64_t> ticks{ 0 };
std::atomic<uint64_t> clientboundPackets{ 0 };
std::atomic<uint64_t> sounds{ 0 };
std::atomic<uint64_t> frames{ 0 };
uint32_t packetListenerId = 0;
uint32_t soundListenerId = 0;
std::string greeting;

// A render pass in a mod: derive from RenderPass, give it a unique name and
// hand it to the engine. This one only counts frames; a real one would
// record draw calls into frame.commandBuffer (see docs/RenderingEngineRework.md).
class FrameCounterPass : public Engine::RenderPass {
public:
    FrameCounterPass() : RenderPass("HelloModFrameCounter") {}
    void Record(Engine::FrameContext&) override { frames++; }
};

// Extended handlers receive the event payload and a user-data pointer.
void OnChat(GlobalState*, const VC_EventData* data, void*)
{
    Log::Info(std::string("[hello_mod] chat: ") + (data->text != nullptr ? data->text : ""));
}

// A HUD widget: the hotbar mirrored in the top-left corner. TakeSnapshot()
// copies the inventory under its lock, so drawing needs no locking at all.
void DrawHud(GlobalState* state)
{
    InventoryManager::Snapshot inventory = state->inventory.TakeSnapshot();

    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
    ImGui::Begin("##hello_mod", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing);

    ImGui::Text("%s  ticks %llu  packets %llu  sounds %llu  frames %llu", greeting.c_str(),
        static_cast<unsigned long long>(ticks.load()), static_cast<unsigned long long>(clientboundPackets.load()),
        static_cast<unsigned long long>(sounds.load()), static_cast<unsigned long long>(frames.load()));

    // One 18-unit slot per hotbar entry, laid out under the text.
    const float slot = 18.0f * GUIController::GetArtScale();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < inventory.hotbar.size(); i++)
    {
        ImVec2 corner(origin.x + slot * static_cast<float>(i), origin.y);
        ImU32 fill = i == inventory.selectedHotbarSlot ? IM_COL32(255, 255, 255, 110) : IM_COL32(0, 0, 0, 110);
        drawList->AddRectFilled(corner, ImVec2(corner.x + slot, corner.y + slot), fill);
        GUIController::DrawItem(drawList, ImVec2(corner.x + slot * 0.055f, corner.y + slot * 0.055f), inventory.hotbar[i]);
    }
    ImGui::Dummy(ImVec2(slot * static_cast<float>(inventory.hotbar.size()), slot));
    ImGui::End();
}

} // namespace

VC_MOD_EXPORT const VC_ModInfo* VC_GetModInfo()
{
    static const VC_ModInfo info{ VC_API_VERSION_MAJOR, VC_API_VERSION_MINOR, "hello_mod", "Hello Mod", "1.0" };
    return &info;
}

VC_MOD_EXPORT void VC_InitMod(GlobalState* state)
{
    // Config values are plain typed keys in settings.json.
    greeting = std::get<std::string>(state->config->Get("HelloMod.Greeting", std::string("Hello from a mod")));
    Log::Info("[hello_mod] loaded: " + greeting);

    // Plain C-style handlers have no payload; the Ex form adds payload and user data.
    VC_EventHandlerEx(static_cast<uint8_t>(EventType::CHAT_RECEIVED), OnChat, nullptr);
    VC_EventHandlerEx(static_cast<uint8_t>(EventType::TICK),
        [](GlobalState*, const VC_EventData*, void* counter) { ++*static_cast<std::atomic<uint64_t>*>(counter); }, &ticks);

    // Things that need the engine exist only after the matching event.
    VC_EventHandler(static_cast<uint8_t>(EventType::RENDER_START), [](GlobalState* game)
    {
        game->renderEngine->InsertPassBefore("GUI", std::make_unique<FrameCounterPass>());
    });
    VC_EventHandler(static_cast<uint8_t>(EventType::AUDIO_LOADED), [](GlobalState* game)
    {
        soundListenerId = game->audioManager->AddSoundListener([](SoundEvent&) { sounds++; });
    });

    // The packet tap and the HUD element need no particular moment.
    packetListenerId = PacketEvents::AddListener([](PacketEvent& event)
    {
        if (event.direction == PacketEvent::Direction::CLIENTBOUND) clientboundPackets++;
    });
    GUIController::AddHudElement("hello_mod", [state]() { DrawHud(state); });
}

// Undo everything that points into this DLL before it is unloaded.
VC_MOD_EXPORT void VC_ShutdownMod(GlobalState* state)
{
    GUIController::RemoveHudElement("hello_mod");
    PacketEvents::RemoveListener(packetListenerId);
    if (state->audioManager != nullptr) state->audioManager->RemoveSoundListener(soundListenerId);
    if (state->renderEngine != nullptr) state->renderEngine->RemovePass("HelloModFrameCounter");
}

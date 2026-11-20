// Partition Volcano:Application - the Application struct and the small types
// declared beside it in Application.hpp.
//
// Application.hpp stays the single source of truth and a plain header: mods
// (sdk/) include it textually and read its fields, and a dozen headers
// forward-declare `struct Application;` from outside any module. Including it
// in the global module fragment keeps every one of those declarations in the
// global module, so mangled names, the mod ABI and those forward declarations
// are unchanged; the export block below is what makes the names visible to
// `import Volcano;`.
module;

#include "../Application.hpp"

export module Volcano:Application;

export namespace Volcano {

using Volcano::WindowMode;
using Volcano::Resolution;
using Volcano::PendingBlockEdit;
using Volcano::NetworkInbox;
using Volcano::ConnectionPhase;
using Volcano::LocalMotionEvent;
using Volcano::LocalMotionInbox;
using Volcano::releaseMeshSlabsHook;
using Volcano::Application;

// Application.hpp forward-declares these so its pointer members compile; code
// that named them through that header can still do so.
using Volcano::TickLoop;
using Volcano::Connection;
using Volcano::InteractionManager;
using Volcano::AudioManager;
using Volcano::MusicController;
using Volcano::ModLoader;
using Volcano::SoundRegistry;
using Volcano::BlockSounds;
using Volcano::EntitySounds;
using Volcano::CombatSounds;
using Volcano::RideController;
using Volcano::LightEngine;
using Volcano::ItemRenderer;
using Volcano::ParticleSystem;
using Volcano::ResourceManager;
using Volcano::MSAccountManager;
using Volcano::SkinFetcher;

namespace Engine {
using Volcano::Engine::RenderEngine;
namespace D3D11 {
using Volcano::Engine::D3D11::D3D11ItemRenderer;
}
}

}

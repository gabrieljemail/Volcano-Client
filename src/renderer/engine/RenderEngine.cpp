#include <chrono>
#include <exception>
#include <memory>
#include <sstream>
#include <stdexcept>
#include "RenderEngine.hpp"
#include "../../ApplicationDeps.hpp"
import Volcano;
#include "../../Logger.hpp"
#include "vulkan/VulkanRenderEngine.hpp"
#ifdef _WIN32
#include "d3d11/D3D11RenderEngine.hpp"
#endif

namespace Volcano::Engine {

namespace {

constexpr size_t NOT_FOUND = static_cast<size_t>(-1);

} // namespace

std::unique_ptr<RenderEngine> CreateAvailableRenderEngine(Application* state,
    const std::string& preferredBackend)
{
    std::vector<std::unique_ptr<RenderEngine>> candidates;
    auto addVulkan = [&]() {
        candidates.push_back(std::make_unique<Vulkan::VulkanRenderEngine>(state));
    };
#ifdef _WIN32
    auto addD3D11 = [&]() {
        candidates.push_back(std::make_unique<D3D11::D3D11RenderEngine>(state));
    };
    if (preferredBackend == "Direct3D 11" || preferredBackend == "DirectX11")
    {
        addD3D11();
        addVulkan();
    }
    else
    {
        if (preferredBackend != "Vulkan")
        {
            Log::Error("[WARN] Unknown Graphics.PreferredRenderer \"" + preferredBackend
                + "\"; trying Vulkan first.");
        }
        addVulkan();
        addD3D11();
    }
#else
    if (preferredBackend != "Vulkan")
    {
        Log::Error("[WARN] Direct3D 11 is only available on Windows; trying Vulkan.");
    }
    addVulkan();
#endif

    std::ostringstream failures;
    bool firstFailure = true;
    for (auto& candidate : candidates)
    {
        state->renderEngine = candidate.get();
        try
        {
            candidate->Init();
            return std::move(candidate);
        }
        catch (const std::exception& error)
        {
            std::string message = std::string(candidate->GetBackendName()) + " initialization failed: " + error.what();
            Log::Error("[WARN] " + message);
            if (!firstFailure) failures << '\n';
            failures << message;
            firstFailure = false;
            try
            {
                candidate->Shutdown();
            }
            catch (const std::exception& cleanupError)
            {
                Log::Error("[ERROR] Renderer cleanup failed: " + std::string(cleanupError.what()));
            }
            state->renderEngine = nullptr;
        }
    }

    throw std::runtime_error("[ERROR] No rendering backend could be initialized.\n" + failures.str());
}

RenderPass* RenderEngine::AddPass(std::unique_ptr<RenderPass> pass)
{
    return InsertAt(passes.size(), std::move(pass));
}

RenderPass* RenderEngine::InsertPassBefore(const std::string& existingPass, std::unique_ptr<RenderPass> pass)
{
    size_t index = IndexOf(existingPass);
    if (index == NOT_FOUND)
    {
        Log::Error("[ERROR] InsertPassBefore: no render pass named \"" + existingPass + "\".");
        return nullptr;
    }
    return InsertAt(index, std::move(pass));
}

RenderPass* RenderEngine::InsertPassAfter(const std::string& existingPass, std::unique_ptr<RenderPass> pass)
{
    size_t index = IndexOf(existingPass);
    if (index == NOT_FOUND)
    {
        Log::Error("[ERROR] InsertPassAfter: no render pass named \"" + existingPass + "\".");
        return nullptr;
    }
    return InsertAt(index + 1, std::move(pass));
}

bool RenderEngine::RemovePass(const std::string& passName)
{
    size_t index = IndexOf(passName);
    if (index == NOT_FOUND) return false;

    // An in-flight frame may still reference the pass's pipelines.
    if (passesCreated)
    {
        WaitIdle();
        passes[index]->Destroy();
    }
    passes.erase(passes.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

RenderPass* RenderEngine::FindPass(const std::string& passName) const
{
    size_t index = IndexOf(passName);
    return index == NOT_FOUND ? nullptr : passes[index].get();
}

std::vector<std::string> RenderEngine::GetPassNames() const
{
    std::vector<std::string> names;
    names.reserve(passes.size());
    for (const auto& pass : passes) names.push_back(pass->GetName());
    return names;
}

std::vector<std::pair<std::string, float>> RenderEngine::GetPassRecordTimes() const
{
    std::vector<std::pair<std::string, float>> times;
    times.reserve(passes.size());
    for (const auto& pass : passes) times.emplace_back(pass->GetName(), pass->GetRecordMs());
    return times;
}

void RenderEngine::SetShaderPack(const std::string& packName)
{
    shaders.SetActivePack(packName);

    // Persist what actually took effect
    state->config->Set("Graphics.ShaderPack", shaders.GetActivePack());
    state->config->Save();

    RequestShaderReload();
}

void RenderEngine::CreatePasses()
{
    try
    {
        for (auto& pass : passes) pass->Create(shaders);
    }
    catch (const std::exception& e)
    {
        if (!shaders.HasActivePack()) throw;

        Log::Error("[ERROR] Shader pack \"" + shaders.GetActivePack() + "\" failed to build (" + e.what()
            + ") - falling back to built-in shaders.");
        DestroyPasses();
        shaders.SetActivePack("");
        for (auto& pass : passes) pass->Create(shaders);
    }
    passesCreated = true;
}

void RenderEngine::DestroyPasses()
{
    for (auto it = passes.rbegin(); it != passes.rend(); ++it) (*it)->Destroy();
    passesCreated = false;
}

void RenderEngine::ReloadPassShaders()
{
    if (!passesCreated) return;

    WaitIdle();
    Log::Info("Reloading shaders" + (shaders.HasActivePack() ? " (pack \"" + shaders.GetActivePack() + "\")" : std::string()) + "...");

    try
    {
        for (auto& pass : passes) pass->ReloadShaders(shaders);
    }
    catch (const std::exception& e)
    {
        if (!shaders.HasActivePack()) throw;

        Log::Error("[ERROR] Shader pack \"" + shaders.GetActivePack() + "\" failed to build (" + e.what()
            + ") - falling back to built-in shaders.");
        shaders.SetActivePack("");
        for (auto& pass : passes) pass->ReloadShaders(shaders);
    }

    Log::Info("Shader reload complete.");
}

void RenderEngine::ReloadPassTextures()
{
    if (!passesCreated) return;

    WaitIdle();
    for (auto& pass : passes)
    {
        try
        {
            pass->ReloadTextures(shaders);
        }
        catch (const std::exception& e)
        {
            Log::Error("[ERROR] Pass \"" + pass->GetName() + "\" failed to reload textures: " + e.what());
        }
    }
}

bool RenderEngine::HasEnabledPass(PassStage stage) const
{
    for (const auto& pass : passes)
    {
        if (pass->IsEnabled() && pass->GetStage() == stage) return true;
    }
    return false;
}

void RenderEngine::RecordPasses(FrameContext& frame, PassStage stage)
{
    // See Application::trianglesDrawn's own comment
    if (stage == PassStage::PRE_MAIN && frame.state && frame.state->debugHudVisible)
    {
        for (uint64_t& count : frame.state->trianglesDrawn) count = 0;
        for (float& ms : frame.state->chunkPhaseFrameMs) ms = 0.0f;
    }

    const bool profile = frame.state && frame.state->debugHudVisible;
    frame.stage = stage;
    for (auto& pass : passes)
    {
        if (!pass->IsEnabled() || pass->GetStage() != stage) continue;
        if (!profile)
        {
            pass->Record(frame);
            continue;
        }

        auto start = std::chrono::steady_clock::now();
        pass->Record(frame);
        float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        pass->SetRecordMs(pass->GetRecordMs() + (ms - pass->GetRecordMs()) * 0.05f);
    }

    if (profile && stage == PassStage::OVERLAY)
    {
        Application* s = frame.state;
        for (int i = 0; i < Application::CHUNK_PHASE_COUNT; i++)
        {
            s->chunkPhaseMs[i] += (s->chunkPhaseFrameMs[i] - s->chunkPhaseMs[i]) * 0.05f;
        }
    }
}

RenderPass* RenderEngine::InsertAt(size_t index, std::unique_ptr<RenderPass> pass)
{
    if (!pass) return nullptr;

    if (IndexOf(pass->GetName()) != NOT_FOUND)
    {
        Log::Error("[ERROR] A render pass named \"" + pass->GetName() + "\" already exists.");
        return nullptr;
    }

    // Late additions (a mod registering after startup) are created on the
    // spot so they're ready by the next Record().
    if (passesCreated)
    {
        try
        {
            pass->Create(shaders);
        }
        catch (const std::exception& e)
        {
            Log::Error("[ERROR] Render pass \"" + pass->GetName() + "\" failed to create: " + e.what());
            pass->Destroy();
            return nullptr;
        }
    }

    RenderPass* raw = pass.get();
    passes.insert(passes.begin() + static_cast<std::ptrdiff_t>(index), std::move(pass));
    return raw;
}

size_t RenderEngine::IndexOf(const std::string& passName) const
{
    for (size_t i = 0; i < passes.size(); i++)
    {
        if (passes[i]->GetName() == passName) return i;
    }
    return NOT_FOUND;
}

} // namespace Volcano::Engine

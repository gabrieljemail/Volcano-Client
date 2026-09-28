#include <exception>
#include "RenderEngine.hpp"
#include "../../GlobalState.hpp"
#include "../../Logger.hpp"

namespace Volcano::Engine {

namespace {

constexpr size_t NOT_FOUND = static_cast<size_t>(-1);

} // namespace

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

void RenderEngine::SetShaderPack(const std::string& packName)
{
    shaders.SetActivePack(packName);

    // Persist what actually took effect — a pack that doesn't exist falls
    // back to "" rather than being saved and failing again next launch.
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
            + ") — falling back to built-in shaders.");
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
    Log::Info("[INFO] Reloading shaders" + (shaders.HasActivePack() ? " (pack \"" + shaders.GetActivePack() + "\")" : std::string()) + "...");

    try
    {
        for (auto& pass : passes) pass->ReloadShaders(shaders);
    }
    catch (const std::exception& e)
    {
        if (!shaders.HasActivePack()) throw;

        Log::Error("[ERROR] Shader pack \"" + shaders.GetActivePack() + "\" failed to build (" + e.what()
            + ") — falling back to built-in shaders.");
        shaders.SetActivePack("");
        for (auto& pass : passes) pass->ReloadShaders(shaders);
    }

    Log::Info("[INFO] Shader reload complete.");
}

void RenderEngine::RecordPasses(FrameContext& frame)
{
    for (auto& pass : passes)
    {
        if (pass->IsEnabled()) pass->Record(frame);
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

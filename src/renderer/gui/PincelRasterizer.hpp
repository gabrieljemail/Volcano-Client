#pragma once
#ifndef VOLCANO_GUI_PINCEL_RASTERIZER_H
#define VOLCANO_GUI_PINCEL_RASTERIZER_H

#include <cstdint>
#include <memory>
#include <Pincel/DrawData.hpp>
#include "../engine/FrameContext.hpp"

namespace Volcano::Engine {
class RenderEngine;
}

namespace Volcano::GUI {

class PincelRasterizer {
public:
    virtual ~PincelRasterizer() = default;

    PincelRasterizer(const PincelRasterizer&) = delete;
    PincelRasterizer& operator=(const PincelRasterizer&) = delete;

    virtual bool Create(Engine::RenderEngine& engine) = 0;
    virtual void Destroy() = 0;
    virtual void Upload(const Pincel::DrawData& data) = 0;
    virtual void Record(Engine::FrameContext& frame) = 0;
    virtual uint64_t UploadedVersion() const = 0;

protected:
    PincelRasterizer() = default;
};

std::unique_ptr<PincelRasterizer> CreatePincelRasterizer(Engine::RenderEngine& engine);

} // namespace Volcano::GUI

#endif

#pragma once
#ifndef VOLCANO_CLOUD_SCENE_H
#define VOLCANO_CLOUD_SCENE_H

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include "../engine/SceneView.hpp"

namespace Volcano {

class Config;

// The CPU half of the clouds, shared by the Vulkan CloudPass and the Direct3D 11
// one: vanilla's cloud cells (textures/environment/clouds.png) laid out in
// 12-block squares drifting slowly, 4 blocks thick (Fancy) or one flat layer
// (Fast); the camera-relative grid mesh; and the per-frame placement, color and
// transform. Each backend only owns the vertex buffer and draws it.
// Settings: Graphics.Clouds (0 off, 1 fast, 2 fancy), Graphics.CloudHeight.
class CloudScene {
public:
    static constexpr int TEXTURE_SIZE = 256;
    static constexpr int RADIUS_CELLS = 28;
    static constexpr float CELL_SIZE = 12.0f;
    static constexpr float THICKNESS = 4.0f;
    static constexpr float DRIFT_BLOCKS_PER_SECOND = 0.6f;
    static constexpr uint32_t MAX_VERTICES = 6u * 6u * (2 * RADIUS_CELLS) * (2 * RADIUS_CELLS);

    // Matches clouds.vert's Vertex inputs: camera-relative position and a face shade.
    struct Vertex {
        float x, y, z;
        float shade;
    };

    // Matches clouds.vert's Push block (push constants on Vulkan, cbuffer b0 on D3D11).
    struct Push {
        glm::mat4 mvp;
        glm::vec4 color;  // rgb, a = overall opacity
        glm::vec4 offset; // xyz: grid offset from the camera, w = fade end distance
    };

    // What to draw this frame: the grid's center cell and style (1 fast, 2 fancy;
    // rebuild the mesh when either changes) and the shader constants.
    struct Draw {
        int centerX = 0;
        int centerZ = 0;
        int mode = 0;
        Push push{};
    };

    // Reads the cloud cells from the resource pack. False if the texture is
    // missing or not 256x256.
    bool LoadCells();
    bool Loaded() const { return !cells.empty(); }

    // False when nothing should be drawn (clouds off, or the title panorama).
    bool Plan(const Engine::SceneView& scene, Config* config, Draw& out) const;

    // Writes the grid's vertices (up to MAX_VERTICES) into `out`; returns how many.
    uint32_t BuildMesh(Vertex* out, int centerX, int centerZ, int mode) const;

private:
    bool CellAt(int x, int z) const;

    std::vector<bool> cells; // TEXTURE_SIZE^2, true where a cloud is
};

} // namespace Volcano

#endif

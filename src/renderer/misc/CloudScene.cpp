#include "CloudScene.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <variant>
#include <stb_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include "../../Config.hpp"
#include "../../Logger.hpp"
#include "../../resources/ImageLoad.hpp"

namespace Volcano {

namespace {

constexpr const char* CLOUD_TEXTURE = "assets/minecraft/textures/environment/clouds.png"; // Logical pack path.

// Vanilla's cloud base is 192.33 blocks up in the overworld.
constexpr uint32_t DEFAULT_CLOUD_HEIGHT = 192;

// Face brightness, top lightest.
constexpr float SHADE_TOP = 1.0f;
constexpr float SHADE_BOTTOM = 0.7f;
constexpr float SHADE_NORTH_SOUTH = 0.8f;
constexpr float SHADE_EAST_WEST = 0.9f;

} // namespace

bool CloudScene::LoadCells()
{
    int width = 0, height = 0;
    stbi_uc* pixels = LoadImageRGBA(CLOUD_TEXTURE, width, height);
    if (pixels == nullptr || width != TEXTURE_SIZE || height != TEXTURE_SIZE)
    {
        Log::Error(std::string("[ERROR] Clouds: couldn't use ") + CLOUD_TEXTURE + " (missing, or not 256x256).");
        if (pixels != nullptr) stbi_image_free(pixels);
        return false;
    }

    cells.assign(static_cast<size_t>(TEXTURE_SIZE) * TEXTURE_SIZE, false);
    for (size_t i = 0; i < cells.size(); i++) cells[i] = pixels[i * 4 + 3] > 0;
    stbi_image_free(pixels);
    return true;
}

bool CloudScene::CellAt(int x, int z) const
{
    int px = ((x % TEXTURE_SIZE) + TEXTURE_SIZE) % TEXTURE_SIZE;
    int pz = ((z % TEXTURE_SIZE) + TEXTURE_SIZE) % TEXTURE_SIZE;
    return cells[static_cast<size_t>(pz) * TEXTURE_SIZE + px];
}

uint32_t CloudScene::BuildMesh(Vertex* out, int centerX, int centerZ, int mode) const
{
    uint32_t count = 0;

    // Corners must be counter-clockwise seen from outside (the terrain's
    // front-face convention); two triangles per quad.
    auto quad = [&](float shade, const float (&c)[4][3])
    {
        if (count + 6 > MAX_VERTICES) return;
        static const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int index : order) out[count++] = { c[index][0], c[index][1], c[index][2], shade };
    };

    const bool fancy = mode == 2;
    const float y0 = 0.0f;
    const float y1 = fancy ? THICKNESS : 0.0f;

    for (int dz = -RADIUS_CELLS; dz < RADIUS_CELLS; dz++)
    {
        for (int dx = -RADIUS_CELLS; dx < RADIUS_CELLS; dx++)
        {
            int cellX = centerX + dx, cellZ = centerZ + dz;
            if (!CellAt(cellX, cellZ)) continue;

            float x0 = dx * CELL_SIZE, x1 = x0 + CELL_SIZE;
            float z0 = dz * CELL_SIZE, z1 = z0 + CELL_SIZE;

            quad(SHADE_TOP, { { x0, y1, z0 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 } });
            quad(SHADE_BOTTOM, { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 } });
            if (!fancy) continue;

            if (!CellAt(cellX + 1, cellZ))
                quad(SHADE_EAST_WEST, { { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x1, y0, z1 } });
            if (!CellAt(cellX - 1, cellZ))
                quad(SHADE_EAST_WEST, { { x0, y0, z0 }, { x0, y0, z1 }, { x0, y1, z1 }, { x0, y1, z0 } });
            if (!CellAt(cellX, cellZ + 1))
                quad(SHADE_NORTH_SOUTH, { { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 } });
            if (!CellAt(cellX, cellZ - 1))
                quad(SHADE_NORTH_SOUTH, { { x0, y0, z0 }, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y0, z0 } });
        }
    }
    return count;
}

bool CloudScene::Plan(const Engine::SceneView& scene, Config* config, Draw& out) const
{
    if (!Loaded() || scene.menuBackground) return false;

    int mode = static_cast<int>(std::min<uint32_t>(std::get<uint32_t>(config->Get("Graphics.Clouds", uint32_t{2})), 2));
    if (mode == 0) return false;
    float height = static_cast<float>(std::get<uint32_t>(config->Get("Graphics.CloudHeight", DEFAULT_CLOUD_HEIGHT)));

    // The grid is anchored to the drifting cloud field, so its centre cell is
    // whichever one the camera is over now.
    float drift = scene.time * DRIFT_BLOCKS_PER_SECOND;
    glm::vec3 camera = scene.cameraPosition;
    out.centerX = static_cast<int>(std::floor((camera.x - drift) / CELL_SIZE));
    out.centerZ = static_cast<int>(std::floor(camera.z / CELL_SIZE));
    out.mode = mode;

    glm::vec3 offset(out.centerX * CELL_SIZE + drift - camera.x, height - camera.y, out.centerZ * CELL_SIZE - camera.z);

    // White by day, a dim blue-grey at night (linear, like the sky colors).
    float daylight = glm::smoothstep(0.15f, 0.65f, scene.dayBrightness);
    glm::vec3 color = glm::mix(glm::vec3(0.012f, 0.014f, 0.03f), glm::vec3(1.0f), daylight);

    out.push.mvp = scene.proj * glm::mat4(glm::mat3(scene.view)) * glm::translate(glm::mat4(1.0f), offset);
    out.push.color = glm::vec4(color, 0.8f);
    out.push.offset = glm::vec4(offset, RADIUS_CELLS * CELL_SIZE);
    return true;
}

} // namespace Volcano

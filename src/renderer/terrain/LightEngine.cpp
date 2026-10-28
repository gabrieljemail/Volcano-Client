#include "LightEngine.hpp"
#include <algorithm>
#include <queue>
#include <unordered_set>

namespace Volcano {

namespace {

constexpr int MAX_VISITS = 40000; // Per edit; the server's own update fixes anything left over.
const glm::ivec3 DIRECTIONS[6] = { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1} };

int FloorDiv16(int value)
{
    return value >= 0 ? value / 16 : -((-value + 15) / 16);
}

} // namespace

std::vector<SectionKey> LightEngine::OnBlockChanged(glm::ivec3 position, const Block& oldBlock, const Block& newBlock)
{
    std::lock_guard<std::mutex> lock(mutex);
    changed.clear();
    visits = 0;

    bool lightMatters = oldBlock.lightEmission != 0 || newBlock.lightEmission != 0
        || oldBlock.LightOpacity() != newBlock.LightOpacity();
    if (!lightMatters) return {};

    Relight(false, position, newBlock);
    Relight(true, position, newBlock);

    // Every changed cell and its neighbors: faces sample the light next to them.
    std::unordered_set<int64_t> seen;
    std::vector<SectionKey> dirty;
    auto addSection = [&](glm::ivec3 cell)
    {
        int sectionY = (cell.y - WORLD_MIN_Y) / SECTION_SIZE_Y;
        if (cell.y < WORLD_MIN_Y || sectionY >= SECTIONS_PER_CHUNK) return;
        SectionKey key{ FloorDiv16(cell.x), FloorDiv16(cell.z), sectionY };
        int64_t id = (static_cast<int64_t>(key.chunkX) << 40) ^ (static_cast<int64_t>(key.chunkZ) << 16) ^ key.sectionY;
        if (seen.insert(id).second && world->HasChunk(key.chunkX, key.chunkZ)) dirty.push_back(key);
    };
    for (const glm::ivec3& cell : changed)
    {
        addSection(cell);
        for (const glm::ivec3& direction : DIRECTIONS) addSection(cell + direction);
    }
    return dirty;
}

void LightEngine::Relight(bool sky, glm::ivec3 position, const Block& newBlock)
{
    std::queue<Cell> removal;
    std::queue<Cell> spread;

    // Clear the edited cell and everything that was lit through it.
    uint8_t previous = Get(sky, position);
    if (previous > 0)
    {
        Set(sky, position, 0);
        removal.push({ position, previous });
    }
    while (!removal.empty() && visits < MAX_VISITS)
    {
        Cell cell = removal.front();
        removal.pop();
        for (const glm::ivec3& direction : DIRECTIONS)
        {
            glm::ivec3 next = cell.position + direction;
            if (!Loaded(next)) continue;
            uint8_t level = Get(sky, next);
            if (level == 0) continue;

            visits++;
            bool litThroughHere = level < cell.level || (sky && direction.y == -1 && cell.level == 15 && level == 15);
            if (litThroughHere)
            {
                Set(sky, next, 0);
                removal.push({ next, level });
            }
            else
            {
                spread.push({ next, level }); // Lit by something else; re-spread from it below.
            }
        }
    }

    // Light flowing into the cell from its neighbors, plus its own emission.
    for (const glm::ivec3& direction : DIRECTIONS)
    {
        glm::ivec3 next = position + direction;
        if (!Loaded(next)) continue;
        uint8_t level = Get(sky, next);
        if (level > 0) spread.push({ next, level });
    }
    if (!sky && newBlock.lightEmission > 0)
    {
        Set(false, position, newBlock.lightEmission);
        spread.push({ position, newBlock.lightEmission });
    }

    while (!spread.empty() && visits < MAX_VISITS)
    {
        Cell cell = spread.front();
        spread.pop();
        for (const glm::ivec3& direction : DIRECTIONS)
        {
            glm::ivec3 next = cell.position + direction;
            if (!Loaded(next)) continue;

            int opacity = world->GetBlock(next.x, next.y, next.z).LightOpacity();
            if (opacity >= 15) continue;

            int level = cell.level - std::max(1, opacity);
            if (sky && direction.y == -1 && cell.level == 15 && opacity == 0) level = 15; // Sky light falls without fading.
            if (level > Get(sky, next))
            {
                visits++;
                Set(sky, next, static_cast<uint8_t>(level));
                spread.push({ next, static_cast<uint8_t>(level) });
            }
        }
    }
}

uint8_t LightEngine::Get(bool sky, glm::ivec3 position) const
{
    LightSample sample = world->GetLightSample(position.x, position.y, position.z);
    return sky ? sample.sky : sample.block;
}

void LightEngine::Set(bool sky, glm::ivec3 position, uint8_t value)
{
    if (world->SetLight(position.x, position.y, position.z, sky, value)) changed.push_back(position);
}

bool LightEngine::Loaded(glm::ivec3 position)
{
    if (position.y < WORLD_MIN_Y || position.y >= WORLD_MIN_Y + CHUNK_SIZE_Y) return false;
    return world->HasChunk(FloorDiv16(position.x), FloorDiv16(position.z));
}

} // namespace Volcano

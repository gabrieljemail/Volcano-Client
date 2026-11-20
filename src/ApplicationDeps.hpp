#pragma once
#ifndef VOLCANO_APPLICATION_DEPS_H
#define VOLCANO_APPLICATION_DEPS_H

// Everything Application.hpp includes. Files that `import Volcano;` (the
// Application module, see modules/Volcano.Application.cppm) still include this:
// an import makes the module's declarations reachable but does not bring its
// textual includes (std containers, GLFW, glm, the model headers) along, and
// these files used to get them through Application.hpp. Trim per file later.
#include "models/GUIWindow.hpp"
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include "renderer/models/Mesh.hpp"
#include "renderer/entity/models/Entity.hpp"
#include "renderer/entity/models/PlayerEntity.hpp"
#include "renderer/gui/models/GUIWindow.hpp"
#include "renderer/terrain/models/Chunk.hpp"
#include "renderer/terrain/models/ChunkSection.hpp"
#include "renderer/terrain/models/World.hpp"
#include "InputHandler.hpp"
#include "Player.hpp"
#include "PlayerAttributes.hpp"
#include "Config.hpp"
#include "SystemInfo.hpp"
#include "inventory/InventoryManager.hpp"
#include "network/CommandTree.hpp"
#include "network/TextComponent.hpp"

#endif

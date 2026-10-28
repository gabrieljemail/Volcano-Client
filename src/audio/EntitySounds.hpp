#pragma once
#ifndef VOLCANO_ENTITY_SOUNDS_H
#define VOLCANO_ENTITY_SOUNDS_H

#include <string>
#include "../GlobalState.hpp"

namespace Volcano {

// Entity voice sounds. The registry indexes raw .ogg files, so an entity's
// hurt sound is found by folder: damage/hit for players, mob/<name>/hurt for mobs.
class EntitySounds {
public:
    explicit EntitySounds(GlobalState* state) : state(state) {}

    // `entityName` is the EntityRegistry name ("player", "zombie", ...).
    // `distance` is from the listener in blocks; sounds fade out by 16.
    void PlayHurt(const std::string& entityName, float distance);

private:
    GlobalState* state;
};

} // namespace Volcano

#endif

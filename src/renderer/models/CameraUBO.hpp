#pragma once
#ifndef CAMERA_UBO_H
#define CAMERA_UBO_H

#include <cstdint>
#include <glm/glm.hpp>

namespace Volcano {

struct CameraUBO {
    glm::mat4 view;
    glm::mat4 proj;
};

} // namespace Volcano

#endif

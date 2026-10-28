#pragma once
#ifndef VOLCANO_HAND_MODEL_H
#define VOLCANO_HAND_MODEL_H

#include <cstdint>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include "../engine/FrameContext.hpp"
#include "../engine/ShaderLibrary.hpp"

namespace Volcano {

// The first-person hands: the bare arm when the main hand is empty, and the
// held items otherwise (drawn through ItemRenderer). Follows vanilla's
// ItemInHandRenderer for placement and the swing / eat animations.
//
// Settings, read every frame:
//   Graphics.HandScale   (uint32, percent) scales the arm and the item.
//   Graphics.LeftHanded  (bool) swaps which side the main hand is on.
//
// Render thread only. Needs EntityRenderer initialized first (it borrows its
// default skin) and GlobalState::itemRenderer with its world pipeline.
class HandModel {
public:
    void Create(const Engine::ShaderLibrary& shaders);
    void Destroy();
    // Records into the main render pass, on top of everything drawn so far.
    void Record(Engine::FrameContext& frame);

private:
    struct GpuBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
    };
    struct ArmMesh {
        GpuBuffer vertices;
        GpuBuffer indices;
        uint32_t indexCount = 0;
    };

    bool ready = false;
    float mainHandHeight = 1.0f; // 1 = raised; drops after a swing.
    int64_t lastRecordNs = 0;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ArmMesh rightArmMesh;
    ArmMesh leftArmMesh;

    void BuildArm(ArmMesh& mesh, const glm::vec3& boxMin, const glm::vec2& uv);
    void CreatePipeline(const Engine::ShaderLibrary& shaders);
    void DestroyPipeline();
    GpuBuffer Upload(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    void DestroyBuffer(GpuBuffer& buffer);

    void DrawArm(VkCommandBuffer cmd, const ArmMesh& mesh, const glm::mat4& mvp);
};

} // namespace Volcano

#endif

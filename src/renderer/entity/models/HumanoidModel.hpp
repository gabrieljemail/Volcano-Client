#pragma once
#ifndef VOLCANO_HUMANOID_MODEL_H
#define VOLCANO_HUMANOID_MODEL_H

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace Volcano {

// Static bind-pose mesh for the shape vanilla's HumanoidModel.java describes
// (resources/minecraft-code/net/minecraft/client/model/HumanoidModel.java,
// createMesh()) — the same skeleton player/zombie/mannequin all share in
// vanilla too (only the bound skin texture differs; see EntityRenderer).
// This is geometry only, no animation: box positions are the fixed
// createMesh() pivots/dimensions (yOffset=0, no CubeDeformation inflation),
// baked directly into vertex positions rather than kept as a runtime bone
// transform. The "hat" overlay layer (a second, slightly inflated copy of
// the head box vanilla uses for the hat-brim visual) isn't modeled — this
// pass is the plain body layer only.
//
// A future pass that adds real animation (walk cycle, head tracking, ...)
// should sample whatever pose function it introduces from EntityRenderer::
// RecordDraw (the render/main thread, once per frame) — NOT from TickLoop's
// fixed 20Hz tick. Driving visible motion off a 20Hz source reads as low-
// framerate/stepped no matter how smooth the underlying math is, the same
// problem TickLoop's own sneak eye-height easing already has; don't repeat
// it here once bones actually move.
namespace HumanoidModel {

struct Vertex {
    glm::vec3 position; // World-local units (1.0 = 1 block), feet at the origin — see BuildMesh's own comment.
    glm::vec3 normal;
    glm::vec2 uv; // 0..1, against a 64x64 skin sheet (TEXTURE_SHEET_SIZE) — matches both player and zombie skins.
};

constexpr float TEXTURE_SHEET_SIZE = 64.0f;

// One box, straight from HumanoidModel.createMesh()'s literal values
// (CubeDeformation.NONE, yOffset=0.0F): `origin`/`size` are the box's
// addBox(x,y,z, w,h,d) corner+dimensions, `uv` is its texOffs(u,v), `pivot`
// is its PartPose.offset — all still in vanilla's own model-space units
// (Y increasing DOWNWARD from a y=0 reference at the neck/shoulder line,
// 16 units = 1 block) exactly as the Java source declares them; BuildMesh
// converts to this engine's Y-up, feet-at-origin, 1-unit-per-block space.
struct BoxDef {
    glm::vec3 origin;
    glm::vec3 size;
    glm::vec2 uv;
    bool mirror; // Flips U for this box's faces — vanilla's left_arm/left_leg reuse the right side's texOffs mirrored.
    glm::vec3 pivot;
};

constexpr std::array<BoxDef, 6> BOXES = {{
    // head
    BoxDef{ {-4.0f, -8.0f, -4.0f}, {8.0f, 8.0f, 8.0f}, {0.0f, 0.0f}, false, {0.0f, 0.0f, 0.0f} },
    // body
    BoxDef{ {-4.0f, 0.0f, -2.0f}, {8.0f, 12.0f, 4.0f}, {16.0f, 16.0f}, false, {0.0f, 0.0f, 0.0f} },
    // right_arm
    BoxDef{ {-3.0f, -2.0f, -2.0f}, {4.0f, 12.0f, 4.0f}, {40.0f, 16.0f}, false, {-5.0f, 2.0f, 0.0f} },
    // left_arm (mirrored)
    BoxDef{ {-1.0f, -2.0f, -2.0f}, {4.0f, 12.0f, 4.0f}, {40.0f, 16.0f}, true, {5.0f, 2.0f, 0.0f} },
    // right_leg
    BoxDef{ {-2.0f, 0.0f, -2.0f}, {4.0f, 12.0f, 4.0f}, {0.0f, 16.0f}, false, {-1.9f, 12.0f, 0.0f} },
    // left_leg (mirrored)
    BoxDef{ {-2.0f, 0.0f, -2.0f}, {4.0f, 12.0f, 4.0f}, {0.0f, 16.0f}, true, {1.9f, 12.0f, 0.0f} },
}};

namespace detail {

// One face of a box, in the box's own pre-transform (Y-down) model space —
// normal/u/v chosen so u x v == normal throughout (matching
// EntityRenderer::BuildCubeMesh's own winding rule: corners taken in order
// center -u-v, +u-v, +u+v, -u+v are then CCW seen from outside, which is
// what VulkanInit's VK_FRONT_FACE_CLOCKWISE + VK_CULL_MODE_BACK_BIT state
// needs to cull the right side). Named by vanilla's own face terms (up/
// down/north/south/east/west) for BoxDef::uv's sake, even though "up" here
// is a -Y normal — Y is still model-down/world-up at this point.
struct FaceDef { glm::vec3 normal, u, v; const char* name; };

constexpr std::array<FaceDef, 6> FACES = {{
    { {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, "up" },
    { {0.0f, 1.0f, 0.0f},  {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, "down" },
    { {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, "north" },
    { {0.0f, 0.0f, 1.0f},  {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, "south" },
    { {1.0f, 0.0f, 0.0f},  {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, "east" },
    { {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, "west" },
}};

// Standard Minecraft entity-model box UV unwrap — unchanged since Java
// Edition's earliest model format, reproduced identically by every third-
// party model tool. Given texOffs (u,v) and box dims (w=x, h=y, d=z):
//
//       +---d---+---w---+
//       |  up   | down  |
//  +----+---+---+---+---+
//  |west|north|east|south|
//  +----+-----+----+-----+
//
// (top strip is `d` tall; bottom strip is `h` tall). Returns the rect for
// one named face as (u0, v0, u1, v1) in texel space — all six checked
// directly against vanilla's real ModelPart.Cube constructor (resources/
// minecraft-code/net/minecraft/client/model/geom/ModelPart.java), not just
// the ASCII layout above: every rect here is exactly the (u0..u4, v0..v2)
// span that Java's Cube ctor hands each named Direction's Polygon. up/down
// have NO flip in vanilla (an earlier version of this comment claimed one,
// from a stale mental model of a pre-PoseStack ModelPart — checked against
// the actual bundled source this time); the real per-face rotation bug
// vanilla's vertex order encodes is handled by FaceUVCorners below instead.
inline glm::vec4 FaceUVRect(const char* faceName, glm::vec2 uv, glm::vec3 size)
{
    float w = size.x, h = size.y, d = size.z;
    std::string_view name(faceName);
    if (name == "up")    return { uv.x + d,         uv.y,     uv.x + d + w,         uv.y + d };
    if (name == "down")  return { uv.x + d + w,      uv.y,     uv.x + d + w + w,     uv.y + d };
    if (name == "west")  return { uv.x,              uv.y + d, uv.x + d,             uv.y + d + h };
    if (name == "north") return { uv.x + d,          uv.y + d, uv.x + d + w,         uv.y + d + h };
    if (name == "east")  return { uv.x + d + w,       uv.y + d, uv.x + d + w + d,     uv.y + d + h };
    /* south */          return { uv.x + d + w + d,   uv.y + d, uv.x + d + w + d + w, uv.y + d + h };
}

// Which of the 4 geometric corners (in BuildMesh's fixed (-u,-v),(+u,-v),
// (+u,+v),(-u,+v) winding order — see FaceDef's own comment on why that
// order can't change) gets which corner of the rect FaceUVRect returns.
//
// The "obvious" pairing — (u0,v0),(u1,v0),(u1,v1),(u0,v1), i.e. u tracks
// face.u and v tracks face.v — is only right when face.u/face.v (see FACES)
// happen to point along the same world axes FaceUVRect's own u/v texel axes
// assume (true for south, and for west/north/east's V axis). For down,
// north and east, face.u/face.v point along the OTHER pair of axes instead
// (e.g. north's face.u is model Y, not X) — a real 90-degree texture
// rotation, not fixable by reversing a range, and NOT fixable by swapping
// face.u/face.v themselves either: that would flip the sign of face.u x
// face.v, handing the face the wrong winding (invisible under backface
// culling) for every box where width != depth. So it's fixed here instead,
// by handing those three faces the transposed corner order. up/west/south
// need a plain range reversal (mirror), not a transpose. All of this is
// derived (not guessed) from vanilla's actual per-face vertex-to-UV remap
// in ModelPart.Polygon's constructor — see this file's own git history for
// the full derivation if this ever needs re-deriving.
inline std::array<glm::vec2, 4> FaceUVCorners(const char* faceName, float u0, float v0, float u1, float v1)
{
    std::string_view name(faceName);
    if (name == "up")
        return { glm::vec2(u0, v1), glm::vec2(u1, v1), glm::vec2(u1, v0), glm::vec2(u0, v0) };
    if (name == "west" || name == "south")
        return { glm::vec2(u1, v0), glm::vec2(u0, v0), glm::vec2(u0, v1), glm::vec2(u1, v1) };
    // down, north, east.
    return { glm::vec2(u0, v0), glm::vec2(u0, v1), glm::vec2(u1, v1), glm::vec2(u1, v0) };
}

// Converts a point from HumanoidModel.java's own coordinate space (Y-down,
// 16 units/block, y=0 at the neck/shoulder line — see BOXES' own comment)
// to this engine's world-local space (Y-up, 1 unit/block, y=0 at the
// feet) — matches Entity::position's "feet/base" convention. 24.0 is the
// model-space Y of the feet (right_leg/left_leg's pivot.y + their box's
// own bottom edge), the same constant vanilla's own model proportions use.
inline glm::vec3 ToWorldLocal(glm::vec3 modelSpace)
{
    constexpr float FEET_Y = 24.0f;
    constexpr float UNITS_PER_BLOCK = 16.0f;
    return glm::vec3(modelSpace.x, FEET_Y - modelSpace.y, modelSpace.z) / UNITS_PER_BLOCK;
}

} // namespace detail

// Appends the full 6-box static mesh (in world-local space, feet at the
// origin — see detail::ToWorldLocal) to `vertices`/`indices`. Call once;
// the result is a fixed shared mesh, same as EntityRenderer's own placeholder
// cube — there's nothing per-entity or per-frame in it.
inline void BuildMesh(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
{
    for (const BoxDef& box : BOXES)
    {
        glm::vec3 modelCenter = box.pivot + box.origin + box.size * 0.5f;

        for (const detail::FaceDef& face : detail::FACES)
        {
            glm::vec3 faceCenterModel = modelCenter + face.normal * (glm::dot(box.size, glm::abs(face.normal)) * 0.5f);
            float halfU = glm::dot(box.size, glm::abs(face.u)) * 0.5f;
            float halfV = glm::dot(box.size, glm::abs(face.v)) * 0.5f;

            glm::vec4 rect = detail::FaceUVRect(face.name, box.uv, box.size);
            float u0 = rect.x, v0 = rect.y, u1 = rect.z, v1 = rect.w;
            if (box.mirror) std::swap(u0, u1);

            // Corners in (-u,-v), (+u,-v), (+u,+v), (-u,+v) order, matching
            // BuildCubeMesh's own winding — see FaceDef's own comment. Which
            // rect corner lands on which of these four is face-specific —
            // see FaceUVCorners.
            std::array<glm::vec3, 4> cornersModel = {
                faceCenterModel - halfU * face.u - halfV * face.v,
                faceCenterModel + halfU * face.u - halfV * face.v,
                faceCenterModel + halfU * face.u + halfV * face.v,
                faceCenterModel - halfU * face.u + halfV * face.v,
            };
            std::array<glm::vec2, 4> rawUV = detail::FaceUVCorners(face.name, u0, v0, u1, v1);
            std::array<glm::vec2, 4> cornersUV = {
                rawUV[0] / TEXTURE_SHEET_SIZE,
                rawUV[1] / TEXTURE_SHEET_SIZE,
                rawUV[2] / TEXTURE_SHEET_SIZE,
                rawUV[3] / TEXTURE_SHEET_SIZE,
            };

            // Normal flips the same way position does (a pure axis
            // reflection is its own correct normal transform — no
            // inverse-transpose needed).
            glm::vec3 worldNormal(face.normal.x, -face.normal.y, face.normal.z);

            uint32_t base = static_cast<uint32_t>(vertices.size());
            for (int i = 0; i < 4; i++)
            {
                glm::vec3 worldPos = detail::ToWorldLocal(cornersModel[static_cast<size_t>(i)]);
                glm::vec3 normal = worldNormal;
                // This mesh's local "front" (the north face's eyes/face
                // texture — see BOXES' head entry) sits on local -Z, per
                // vanilla's own vertex layout (see FaceUVCorners' comment).
                // But this engine's own yaw convention — TickLoop's
                // `forward{ sin(yaw), 0, cos(yaw) }`, used for the local
                // player's own movement and proven correct there — treats
                // +Z as forward. Rather than touch the shared yaw-rotation
                // matrix both entity shaders use (risking the untextured
                // placeholder-cube path, where a sign flip is currently
                // invisible but would stop being so later), turn the mesh
                // itself 180 degrees around the vertical axis here so its
                // face ends up on the +Z side the rotation math expects.
                worldPos.x = -worldPos.x;
                worldPos.z = -worldPos.z;
                normal.x = -normal.x;
                normal.z = -normal.z;
                vertices.push_back({ worldPos, normal, cornersUV[static_cast<size_t>(i)] });
            }
            indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
    }
}

} // namespace HumanoidModel
} // namespace Volcano

#endif

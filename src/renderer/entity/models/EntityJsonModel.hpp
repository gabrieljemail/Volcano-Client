#pragma once
#ifndef VOLCANO_ENTITY_JSON_MODEL_H
#define VOLCANO_ENTITY_JSON_MODEL_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

namespace Volcano::EntityJsonModel {

// Data-driven replacement for what used to be HumanoidModel.hpp's hardcoded
// BOXES array - one JSON file (resources/volcano/models/entity/*.json) per
// species now supplies the box list instead of a new .hpp per mob. See
// PigModel.hpp/CowModel.hpp/QuadrupedModel.hpp/GolemModel.hpp for the
// flat-tinted procedural shapes this doesn't replace (yet) - this loader is
// currently only used by the UV-textured humanoid path (player/zombie/
// skeleton), which is the one that actually needs per-species box
// dimensions (see skeleton.json's thinner arms/legs) and a real skin.

struct Vertex {
    glm::vec3 position; // World-local units (1.0 = 1 block), feet at the origin - see detail::ToWorldLocal.
    glm::vec3 normal;
    glm::vec2 uv; // 0..1, against this model's own textureWidth x textureHeight sheet.
};

// One box, straight from a model JSON's `boxes` entry - same fields as
// vanilla's CubeListBuilder.addBox(x,y,z,w,h,d) + texOffs(u,v) + the containing
// part's PartPose.offset, still in vanilla's own model-space units (Y
// increasing DOWNWARD from a y=0 reference at the neck/shoulder line, 16
// units = 1 block) exactly as the Java source declares them - BuildMesh
// converts to this engine's Y-up, feet-at-origin, 1-unit-per-block space.
struct BoxDef {
    glm::vec3 origin;
    glm::vec3 size;
    glm::vec2 uv;
    bool mirror = false; // Flips U for this box's faces - vanilla's left_arm/left_leg reuse the right side's texOffs mirrored.
    glm::vec3 pivot{ 0.0f };
    glm::vec3 rotation{ 0.0f }; // Degrees (vanilla PartPose rotation), applied about pivot in Z,Y,X order.
    float inflate = 0.0f;       // CubeDeformation: grows the box on every side, UVs keep the original size.
    std::string part;           // Animated together; JSON "part", defaulting to "name".
};

struct ModelDef {
    float textureWidth = 64.0f;
    float textureHeight = 64.0f;
    float feetY = 24.0f; // Model-space Y of the ground; vanilla is 24 for nearly everything.
    std::vector<BoxDef> boxes;
};

// Parses one model JSON (see resources/volcano/models/entity/*.json for the
// schema - deliberately close to OptiFine's CEM box format, not glTF: these
// models are axis-aligned boxes with a standard Minecraft UV unwrap, not
// arbitrary meshes, so a full glTF loader (buffers/accessors/node graphs)
// would be solving a harder problem than the one that actually exists here).
inline ModelDef Load(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        throw std::runtime_error("[ERROR] EntityJsonModel: failed to open " + path);
    }

    nlohmann::json j;
    file >> j;

    ModelDef model;
    model.textureWidth = j.value("textureWidth", 64.0f);
    model.textureHeight = j.value("textureHeight", 64.0f);
    model.feetY = j.value("feetY", 24.0f);

    for (const auto& b : j.at("boxes"))
    {
        BoxDef box;
        auto origin = b.at("origin"); box.origin = { origin[0], origin[1], origin[2] };
        auto size = b.at("size");     box.size = { size[0], size[1], size[2] };
        auto uv = b.at("uv");         box.uv = { uv[0], uv[1] };
        box.mirror = b.value("mirror", false);
        if (b.contains("pivot"))
        {
            auto pivot = b.at("pivot");
            box.pivot = { pivot[0], pivot[1], pivot[2] };
        }
        if (b.contains("rotation"))
        {
            auto rotation = b.at("rotation");
            box.rotation = { rotation[0], rotation[1], rotation[2] };
        }
        box.inflate = b.value("inflate", 0.0f);
        box.part = b.value("part", b.value("name", std::string()));
        model.boxes.push_back(box);
    }

    return model;
}

namespace detail {

// One face of a box, in the box's own pre-transform (Y-down) model space -
// normal/u/v chosen so u x v == normal throughout, matching EntityBoxMesh.hpp
// AppendBox's own winding rule (corners taken in order center -u-v, +u-v,
// +u+v, -u+v are then CCW seen from outside). That convention is only valid
// for a proper (non-reflecting) model-to-world transform, though - see
// BuildMesh's own comment on why this file's index order is reversed
// relative to AppendBox's.
struct FaceDef { glm::vec3 normal, u, v; const char* name; };

constexpr std::array<FaceDef, 6> FACES = {{
    { {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, "up" },
    { {0.0f, 1.0f, 0.0f},  {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, "down" },
    { {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, "north" },
    { {0.0f, 0.0f, 1.0f},  {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, "south" },
    { {1.0f, 0.0f, 0.0f},  {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, "east" },
    { {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, "west" },
}};

// Standard Minecraft entity-model box UV unwrap - see HumanoidModel.hpp's
// git history (this file's predecessor) for the full derivation against
// vanilla's real ModelPart.Cube/Polygon constructors.
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

// Converts a point from vanilla's own coordinate space (Y-down, 16
// units/block, y=0 at the neck/shoulder line) to this engine's world-local
// space (Y-up, 1 unit/block, y=0 at the feet) - matches Entity::position's
// "feet/base" convention. 24.0 is the model-space Y of the feet
// (right_leg/left_leg's pivot.y + their box's own bottom edge), the same
// constant vanilla's own model proportions use.
inline glm::vec3 ToWorldLocal(glm::vec3 modelSpace, float feetY = 24.0f)
{
    constexpr float UNITS_PER_BLOCK = 16.0f;
    return glm::vec3(modelSpace.x, feetY - modelSpace.y, modelSpace.z) / UNITS_PER_BLOCK;
}

} // namespace detail

// One animatable part (all boxes sharing a `part`): its index range and the
// point it turns about.
struct PartInfo {
    std::string name;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    glm::vec3 pivot{ 0.0f };      // Mesh-local (world-local) space.
    glm::vec3 pivotModel{ 0.0f }; // Vanilla model units.
};

struct MeshInfo {
    std::vector<PartInfo> parts;
};

// Appends the full static mesh (in world-local space, feet at the origin -
// see detail::ToWorldLocal) to `vertices`/`indices`. Call once per loaded
// ModelDef; the result is a fixed shared mesh - nothing per-entity or
// per-frame in it. Each part's boxes are contiguous so parts can be posed
// separately. `vanillaHanded` is for entities that animate (see above);
// chests keep the legacy mirrored layout their JSON was authored against.
inline MeshInfo BuildMesh(const ModelDef& model, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
    float scale = 1.0f, bool vanillaHanded = false)
{
    MeshInfo info;
    auto emitBox = [&](const BoxDef& box)
    {
        // Geometry size includes the inflate; UVs below keep box.size.
        glm::vec3 geomSize = box.size + glm::vec3(box.inflate * 2.0f);
        glm::vec3 modelCenter = box.pivot + box.origin + box.size * 0.5f;

        // Vanilla's PartPose rotation: Rz * Ry * Rx about the pivot.
        glm::mat3 rot(1.0f);
        bool rotated = box.rotation != glm::vec3(0.0f);
        if (rotated)
        {
            glm::vec3 r = glm::radians(box.rotation);
            rot = glm::mat3(glm::rotate(glm::mat4(1.0f), r.z, glm::vec3(0, 0, 1)))
                * glm::mat3(glm::rotate(glm::mat4(1.0f), r.y, glm::vec3(0, 1, 0)))
                * glm::mat3(glm::rotate(glm::mat4(1.0f), r.x, glm::vec3(1, 0, 0)));
        }

        for (const detail::FaceDef& face : detail::FACES)
        {
            glm::vec3 faceCenterModel = modelCenter + face.normal * (glm::dot(geomSize, glm::abs(face.normal)) * 0.5f);
            float halfU = glm::dot(geomSize, glm::abs(face.u)) * 0.5f;
            float halfV = glm::dot(geomSize, glm::abs(face.v)) * 0.5f;

            // Legacy (chest) models are vanilla's Y-up ChestModel with Y
            // flipped to Y-down, but vanilla's up/down regions did not swap
            // with it: the visible top is the region after the depth strip
            // plus the width (28,0 for the lid), the underside the one right
            // after the depth (14,0). Corner order is already vanilla's.
            const char* uvFace = face.name;
            if (!vanillaHanded)
            {
                if (std::string_view(face.name) == "up") uvFace = "down";
                else if (std::string_view(face.name) == "down") uvFace = "up";
            }
            glm::vec4 rect = detail::FaceUVRect(uvFace, box.uv, box.size);
            float u0 = rect.x, v0 = rect.y, u1 = rect.z, v1 = rect.w;
            if (box.mirror) std::swap(u0, u1);

            std::array<glm::vec3, 4> cornersModel = {
                faceCenterModel - halfU * face.u - halfV * face.v,
                faceCenterModel + halfU * face.u - halfV * face.v,
                faceCenterModel + halfU * face.u + halfV * face.v,
                faceCenterModel - halfU * face.u + halfV * face.v,
            };
            std::array<glm::vec2, 4> rawUV = detail::FaceUVCorners(face.name, u0, v0, u1, v1);
            std::array<glm::vec2, 4> cornersUV = {
                glm::vec2(rawUV[0].x / model.textureWidth, rawUV[0].y / model.textureHeight),
                glm::vec2(rawUV[1].x / model.textureWidth, rawUV[1].y / model.textureHeight),
                glm::vec2(rawUV[2].x / model.textureWidth, rawUV[2].y / model.textureHeight),
                glm::vec2(rawUV[3].x / model.textureWidth, rawUV[3].y / model.textureHeight),
            };

            // Normal flips the same way position does (a pure axis
            // reflection is its own correct normal transform - no
            // inverse-transpose needed).
            glm::vec3 faceNormal = face.normal;
            if (rotated)
            {
                for (glm::vec3& corner : cornersModel) corner = box.pivot + rot * (corner - box.pivot);
                faceNormal = rot * faceNormal;
            }
            glm::vec3 worldNormal(faceNormal.x, -faceNormal.y, faceNormal.z);

            uint32_t base = static_cast<uint32_t>(vertices.size());
            for (int i = 0; i < 4; i++)
            {
                glm::vec3 worldPos = detail::ToWorldLocal(cornersModel[static_cast<size_t>(i)], model.feetY) * scale;
                glm::vec3 normal = worldNormal;
                // This mesh's local "front" (the north face's eyes/face
                // texture - see the head box) sits on local -Z, per
                // vanilla's own vertex layout. This engine's own yaw
                // convention treats +Z as forward, so the mesh is turned
                // 180 degrees around the vertical axis here.
                // vanillaHanded skips the X flip: Y and Z alone are vanilla's
                // own model-to-world rotation, where the extra X flip would
                // leave the whole model mirrored (right arm on the left).
                if (!vanillaHanded) worldPos.x = -worldPos.x;
                worldPos.z = -worldPos.z;
                if (!vanillaHanded) normal.x = -normal.x;
                normal.z = -normal.z;
                vertices.push_back({ worldPos, normal, cornersUV[static_cast<size_t>(i)] });
            }
            // NOT {base, base+1, base+2, base, base+2, base+3} (AppendBox's
            // winding) - ToWorldLocal's Y negation (Y-down to Y-up) composed
            // with the 180-degree X/Z turn just above is an ODD number of
            // axis flips (3: Y, then X and Z), i.e. a net reflection
            // (determinant -1), which inverts the CCW-from-outside winding
            // FaceDef's u/v were chosen to produce. Reversing the two
            // triangles' vertex order here (not touching positions/normals/
            // UVs at all) exactly compensates, restoring correct front-face
            // culling. Without this, every face renders back-to-front -
            // the "inside-out" look this was added to fix. AppendBox itself
            // doesn't need this: its callers author boxes directly in
            // world-local space with no reflecting conversion step.
            if (vanillaHanded) indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
            else indices.insert(indices.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });
        }
    };

    // One contiguous index range per part, in order of first appearance.
    std::vector<std::string> order;
    for (const BoxDef& box : model.boxes)
    {
        if (std::find(order.begin(), order.end(), box.part) == order.end()) order.push_back(box.part);
    }

    for (const std::string& partName : order)
    {
        PartInfo part;
        part.name = partName;
        part.firstIndex = static_cast<uint32_t>(indices.size());
        bool pivotSet = false;
        for (const BoxDef& box : model.boxes)
        {
            if (box.part != partName) continue;
            emitBox(box);
            if (!pivotSet)
            {
                glm::vec3 pivot = detail::ToWorldLocal(box.pivot, model.feetY) * scale;
                part.pivot = vanillaHanded ? glm::vec3(pivot.x, pivot.y, -pivot.z) : glm::vec3(-pivot.x, pivot.y, -pivot.z);
                part.pivotModel = box.pivot;
                pivotSet = true;
            }
        }
        part.indexCount = static_cast<uint32_t>(indices.size()) - part.firstIndex;
        info.parts.push_back(part);
    }
    return info;
}

} // namespace Volcano::EntityJsonModel

#endif

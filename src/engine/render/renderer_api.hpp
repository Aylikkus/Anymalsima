#pragma once
#include <bgfx/bgfx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <variant>
#include <vector>

#include "sky_state.hpp"
#include "text_layout_state.hpp"

enum class RenderCategory : uint8_t {
    TERRAIN_ROCK = 0,     // culled at z > camera_z
    TERRAIN_SOIL = 1,     // culled at z > camera_z
    FLUID = 2,            // culled at z > camera_z
    STRUCTURE_WALL = 3,   // conditionally culled (player can toggle)
    STRUCTURE_FLOOR = 4,  // always visible below camera_z
    VEGETATION = 5,       // NEVER culled — always rendered fully
    FURNITURE = 6,        // never culled
    ENTITY = 7,           // never culled
    ITEM = 8,             // never culled
};

enum class CullingMode {
    FULL,          // show everything (editor/god mode)
    SURFACE_FULL,  // terrain doesn't get hidded by camera_z
    SURFACE,       // terrain hidden above camera_z, trees/entities always shown
    UNDERGROUND,   // like surface but also clips foliage at z
    XRAY_WALLS,    // walls of structures made transparent
};

struct Camera {
    Vec3 position{};
    float yaw = 0;
    float pitch = -.5f;
    float distance = 10;
    float viewDistance = 4096.f;
    bool terrainSky = false;
    bool underwater = false;
    SkyState sky{};
    float nearPlane = 1.f;
    Vec3 direction() const {
        const float flat = std::cos(pitch);
        return {flat * std::sin(yaw), std::sin(pitch), flat * std::cos(yaw)};
    }
    Vec3 eye() const { return position - direction() * distance; }
};

// The world view (view 0) is camera-relative: its eye sits at the origin.
// Every world draw passes this model matrix, which places a mesh whose
// vertex (0,0,0) is at world `origin` relative to the eye. It is computed in
// double, so meshes near the camera stay precise at any world coordinate
// (float world positions near x = 60000 are only exact to ~0.004 tiles,
// which opens gaps between greedy-meshed quads).
inline std::array<float, 16> relativeTransform(const Camera& camera, double ox,
                                               double oy, double oz) {
    const double pitch = camera.pitch, yaw = camera.yaw, d = camera.distance;
    const double ex = camera.position.x - std::cos(pitch) * std::sin(yaw) * d;
    const double ey = camera.position.y - std::sin(pitch) * d;
    const double ez = camera.position.z - std::cos(pitch) * std::cos(yaw) * d;
    std::array<float, 16> m{};
    m[0] = m[5] = m[10] = m[15] = 1;
    m[12] = float(ox - ex);
    m[13] = float(oy - ey);
    m[14] = float(oz - ez);
    return m;
}

struct StaticMesh {
    bgfx::VertexBufferHandle vbh;
    bgfx::IndexBufferHandle ibh;
};

struct DynamicMesh {
    bgfx::DynamicVertexBufferHandle vbh = BGFX_INVALID_HANDLE;
    bgfx::DynamicIndexBufferHandle ibh = BGFX_INVALID_HANDLE;
    uint32_t capacityVerts = 0;
    uint32_t capacityIndices = 0;
    uint32_t activeIndices = 0;
    uint32_t activeVertices = 0;
};

using MeshVariant = std::variant<StaticMesh, DynamicMesh>;

class IRenderer {
   public:
    virtual ~IRenderer() = default;

    template <typename VertexT>
    StaticMesh createStaticMesh(const VertexT* verts, size_t vertCount,
                                const uint16_t* indices, size_t indexCount) {
        bgfx::VertexLayout layout;
        VertexT::init(layout);

        auto vbh = bgfx::createVertexBuffer(
            bgfx::makeRef(verts, sizeof(VertexT) * vertCount), layout);

        auto ibh = bgfx::createIndexBuffer(
            bgfx::makeRef(indices, sizeof(uint16_t) * indexCount));

        return {vbh, ibh};
    };

    template <typename VertexT>
    void updateVertexBuffer(DynamicMesh& mesh, const VertexT* verts,
                            size_t vertCount, const uint32_t* indices,
                            size_t indexCount, bool copy) {
        // Проверяем, нужно ли пересоздать буферы (рост)
        if (vertCount > mesh.capacityVerts ||
            indexCount > mesh.capacityIndices) {
            if (bgfx::isValid(mesh.vbh)) bgfx::destroy(mesh.vbh);
            if (bgfx::isValid(mesh.ibh)) bgfx::destroy(mesh.ibh);

            bgfx::VertexLayout layout;
            VertexT::init(layout);

            mesh.capacityVerts = uint32_t(vertCount);
            mesh.capacityIndices = uint32_t(indexCount);

            mesh.vbh =
                bgfx::createDynamicVertexBuffer(mesh.capacityVerts, layout);
            mesh.ibh = bgfx::createDynamicIndexBuffer(mesh.capacityIndices,
                                                      BGFX_BUFFER_INDEX32);
        }
        // Out of bgfx handles (BGFX_CONFIG_MAX_DYNAMIC_*): drop the mesh
        // instead of updating an invalid handle (a crash in release builds).
        if (!validDynamic(mesh)) {
            releaseDynamic(mesh);
            return;
        }

        const bgfx::Memory* vert_mem;
        const bgfx::Memory* index_mem;

        if (copy) {
            vert_mem = bgfx::copy(verts, sizeof(VertexT) * vertCount);
            index_mem = bgfx::copy(indices, sizeof(uint32_t) * indexCount);
        } else {
            vert_mem = bgfx::makeRef(verts, sizeof(VertexT) * vertCount);
            index_mem = bgfx::makeRef(indices, sizeof(uint32_t) * indexCount);
        }

        // Загружаем данные
        bgfx::update(mesh.vbh, 0, vert_mem);
        bgfx::update(mesh.ibh, 0, index_mem);

        // Запоминаем, сколько реально нужно отрисовать
        mesh.activeIndices = uint32_t(indexCount);
        mesh.activeVertices = uint32_t(vertCount);
    }

    template <typename VertexT>
    DynamicMesh createDynamicMesh(size_t vertCount, size_t indexCount) {
        bgfx::VertexLayout layout;
        VertexT::init(layout);
        DynamicMesh mesh;

        // Создаем с запасом 50%
        mesh.capacityVerts = vertCount;
        mesh.capacityIndices = indexCount;

        mesh.vbh = bgfx::createDynamicVertexBuffer(mesh.capacityVerts, layout);
        mesh.ibh = bgfx::createDynamicIndexBuffer(mesh.capacityIndices,
                                                  BGFX_BUFFER_INDEX32);

        mesh.activeIndices = uint32_t(indexCount);
        mesh.activeVertices = uint32_t(vertCount);
        // Out of handles: an empty mesh; the next update tries again.
        if (!validDynamic(mesh)) releaseDynamic(mesh);

        return mesh;
    }

    // Both buffers exist (creation fails once bgfx runs out of handles).
    static bool validDynamic(const DynamicMesh& mesh) {
        return bgfx::isValid(mesh.vbh) && bgfx::isValid(mesh.ibh);
    }
    // Number of dynamic meshes whose buffers could not be created.
    static inline size_t failedDynamicMeshes = 0;

   private:
    static void releaseDynamic(DynamicMesh& mesh) {
        if (bgfx::isValid(mesh.vbh)) bgfx::destroy(mesh.vbh);
        if (bgfx::isValid(mesh.ibh)) bgfx::destroy(mesh.ibh);
        mesh = {};
        ++failedDynamicMeshes;
    }

   public:
    virtual void scheduleDelete(const MeshVariant& mesh) = 0;

    // Rebuilds rows + glyph positions if dirty.
    // Call once per frame before draw/hit-test.
    virtual void rebuild(TextLayoutState& state) const = 0;

    // Call when one char changes (active text typing)
    // Rebuilds only changed row + offsets other rows
    virtual void rebuildIncrementally(TextLayoutState& state) const = 0;

    // Fast lookups — only valid after rebuild()
    virtual Vec2 caretPosition(TextLayoutState& state, size_t index,
                               bool raw = false) const = 0;
    virtual size_t caretAtPoint(const TextLayoutState& state,
                                Vec2 point) const = 0;
    virtual void buildSelectionRects(TextLayoutState& state, size_t selStart,
                                     size_t selEnd) const = 0;

    void setCamera(const Camera& newCamera) { camera = newCamera; }

    virtual const float* getViewMatrix() = 0;
    virtual const float* getProjMatrix() = 0;

   protected:
    Camera camera{};
};

class MeshHandle {
   public:
    MeshHandle() = default;

    MeshHandle(MeshHandle&& other) noexcept { *this = std::move(other); }

    MeshHandle& operator=(MeshHandle&& other) noexcept {
        if (this != &other) {
            release();

            renderer = other.renderer;
            mesh = std::move(other.mesh);
            valid = other.valid;
            isDynamic = other.isDynamic;

            other.valid = false;
        }
        return *this;
    }

    ~MeshHandle() { release(); }

    const MeshVariant& get() const { return mesh; }
    // Initialised with GPU buffers (creation fails once bgfx runs out of
    // handles).
    bool drawable() const {
        if (!valid) return false;
        if (const auto* d = std::get_if<DynamicMesh>(&mesh))
            return IRenderer::validDynamic(*d);
        const auto& m = std::get<StaticMesh>(mesh);
        return bgfx::isValid(m.vbh) && bgfx::isValid(m.ibh);
    }

    template <typename VertexT>
    void initStatic(IRenderer* r, const VertexT* verts, size_t vertCount,
                    const uint16_t* indices, size_t indexCount) {
        if (valid) release();
        renderer = r;
        isDynamic = false;
        mesh = r->createStaticMesh(verts, vertCount, indices, indexCount);
        valid = true;
    }

    template <typename VertexT>
    void initDynamic(IRenderer* r, size_t vertCount, size_t indexCount) {
        if (valid) release();
        renderer = r;
        isDynamic = true;
        mesh = r->createDynamicMesh<VertexT>(vertCount, indexCount);
        valid = true;
    }

    template <typename VertexT>
    void update(const VertexT* verts, size_t vertCount, const uint32_t* indices,
                size_t indexCount, bool copy = false) {
        if (!isDynamic) return;

        renderer->updateVertexBuffer(std::get<DynamicMesh>(mesh), verts,
                                     vertCount, indices, indexCount, copy);
    }

    void release() {
        if (renderer && valid) {
            renderer->scheduleDelete(mesh);
            valid = false;
        }
    }

   private:
    IRenderer* renderer = nullptr;
    MeshVariant mesh;
    bool valid = false;
    bool isDynamic = false;
};

enum class PropertyType { Invalid, Sampler, Vec4, Vec4Array, Mat4 };

struct ShaderProperty {
    ~ShaderProperty() {
        if (bgfx::isValid(m_handle)) {
            bgfx::destroy(m_handle);
        }
    }

    ShaderProperty() {}
    ShaderProperty(const ShaderProperty&) = delete;
    ShaderProperty& operator=(const ShaderProperty&) = delete;

    // Allow moving (transferring ownership)
    ShaderProperty(ShaderProperty&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = BGFX_INVALID_HANDLE;
        other.type = PropertyType::Invalid;
        other.stage = 0;
    }

    ShaderProperty& operator=(ShaderProperty&& other) noexcept {
        if (this != &other) {
            if (bgfx::isValid(m_handle)) bgfx::destroy(m_handle);
            m_handle = other.m_handle;
            other.m_handle = BGFX_INVALID_HANDLE;
            other.type = PropertyType::Invalid;
            other.stage = 0;
        }
        return *this;
    }

    void initTexture(const char* name, uint8_t stage_,
                     bgfx::TextureHandle tex) {
        if (bgfx::isValid(m_handle)) bgfx::destroy(m_handle);

        m_handle = bgfx::createUniform(name, bgfx::UniformType::Sampler);
        type = PropertyType::Sampler;
        stage = stage_;
        data = tex;
    }

    void initFloat(const char* name, float value) {
        if (bgfx::isValid(m_handle)) bgfx::destroy(m_handle);

        m_handle = bgfx::createUniform(name, bgfx::UniformType::Vec4, 1);
        type = PropertyType::Vec4;
        data = std::array<float, 4>{value, 0.0f, 0.0f, 0.0f};
    }

    void setFloat(float value) {
        std::get<std::array<float, 4>>(data)[0] = value;
    }

    void initVec4(const char* name, const std::array<float, 4>& values) {
        if (bgfx::isValid(m_handle)) bgfx::destroy(m_handle);

        m_handle = bgfx::createUniform(name, bgfx::UniformType::Vec4, 1);
        type = PropertyType::Vec4;
        data = values;
    }

    void setVec4(const std::array<float, 4>& values) { data = values; }

    // A uniform array of `count` vec4 values, all zero initially.
    void initVec4Array(const char* name, uint16_t count) {
        if (bgfx::isValid(m_handle)) bgfx::destroy(m_handle);

        m_handle = bgfx::createUniform(name, bgfx::UniformType::Vec4, count);
        type = PropertyType::Vec4Array;
        data = std::vector<float>(size_t(count) * 4, 0.0f);
    }

    // Copies up to the declared count of vec4 values (4 floats each); the
    // rest keep their previous contents.
    void setVec4Array(const float* values, size_t vec4Count) {
        auto& array = std::get<std::vector<float>>(data);
        const size_t n = std::min(array.size(), vec4Count * 4);
        std::copy(values, values + n, array.begin());
    }

    void initMat4(const char* name, const std::array<float, 16>& values) {
        if (bgfx::isValid(m_handle)) bgfx::destroy(m_handle);

        m_handle = bgfx::createUniform(name, bgfx::UniformType::Mat4, 1);
        type = PropertyType::Mat4;
        data = values;
    }

    void bind() const {
        switch (type) {
            case PropertyType::Sampler:
                bgfx::setTexture(stage, m_handle,
                                 std::get<bgfx::TextureHandle>(data));
                break;
            case PropertyType::Vec4:
                bgfx::setUniform(
                    m_handle, std::get<std::array<float, 4>>(data).data(), 1);
                break;
            case PropertyType::Vec4Array: {
                const auto& array = std::get<std::vector<float>>(data);
                bgfx::setUniform(m_handle, array.data(),
                                 uint16_t(array.size() / 4));
                break;
            }
            case PropertyType::Mat4:
                bgfx::setUniform(
                    m_handle, std::get<std::array<float, 16>>(data).data(), 1);
                break;
            case PropertyType::Invalid:
                break;
        }
    }

   private:
    PropertyType type = PropertyType::Invalid;

    bgfx::UniformHandle m_handle = BGFX_INVALID_HANDLE;
    uint8_t stage;  // Only for Samplers
    std::variant<bgfx::TextureHandle, float, std::array<float, 4>,
                 std::array<float, 16>, std::vector<float>>
        data;
};

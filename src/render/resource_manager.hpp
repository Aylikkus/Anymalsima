#pragma once

#include <bgfx/bgfx.h>
#include <miniaudio.h>
#include <stb_image.h>
#include <stb_truetype.h>

#include <array>
#include <vector>

#include "core/assets_api.hpp"
#include "render/render_commands.hpp"
#include "render/text_layout_state.hpp"
#include "serialization/file_io.hpp"
#include "serialization/serializable_api.hpp"
#include "utils/font_resolver.hpp"

struct TextureResource {
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    uint16_t width;
    uint16_t height;
    std::vector<uint8_t> cpuData;
};

struct Glyph {
    float advance;
    float bearingX;
    float bearingY;
    float width;
    float height;
    float u0, v0, u1, v1;
    int atlasIndex;
};

struct FontAtlas {
    bgfx::TextureHandle atlas;
    uint32_t size;

    int width;
    int height;

    int penX = 0;
    int penY = 0;
    int rowHeight = 0;

    std::unordered_map<int32_t, Glyph> glyphs;
    std::vector<uint8_t> cpuBitmap;

    float lineHeight;
    float ascent;
};

static Glyph dummy;

struct SoundData {
    void* pData;
    ma_uint64 frameCount;
    ma_format format;
    ma_uint32 channels;
    ma_uint32 sampleRate;
};

struct FontResource {
    std::vector<uint8_t> ttfData;  // raw font file
    stbtt_fontinfo fontInfo;

    std::unordered_map<uint32_t, std::vector<FontAtlas>>
        atlases;  // size -> atlas
};

struct MaterialGpuData {
    uint16_t albedoLayer = 0;
    uint16_t normalLayer = 0;
};

class ResourceManager : public IAssets {
   public:
    ResourceManager();
    const SoundData& getSoundData(const std::string& key) const;
    const std::string& getMusicPath(const std::string& key) const;
    bgfx::ProgramHandle getProgram(const std::string& key) const;
    bgfx::TextureHandle getTexture(const std::string& key) const;
    FontAtlas& createNewAtlas(FontRole role, uint32_t size);
    FontAtlas& getCurrentAtlas(FontRole role, uint32_t size);
    FontResource& getFontResource(FontRole role, uint32_t size);
    void warmupGlyphs(FontRole role, uint32_t size,
                      const std::vector<int32_t>& codepoints);
    const Glyph* getGlyph(FontRole role, uint32_t size, int32_t cp) const;
    bool fontHasGlyph(FontRole role, int32_t codepoint);

    uint16_t getMaterialAlbedoLayer(const std::string& materialId) const;
    uint16_t getMaterialNormalLayer(const std::string& materialId) const;

    bgfx::TextureHandle getMaterialAlbedoArray() const;
    bgfx::TextureHandle getMaterialNormalArray() const;

    void clear();

    void loadFromJson(const Json& j);

    void loadFont(FontRole role, const std::string& path);
    void loadTexture(const std::string& key, const std::string& path,
                     bool isMaterial = false);
    void loadSound(const std::string& key, const std::string& path);
    void loadMusic(const std::string& key, const std::string& path);
    void loadShader(const std::string& key, const std::string& path);
    void buildMaterialTextureArrays();

    Vec2u getTextureSize(const std::string& key);

   private:
    bgfx::ShaderHandle loadShaderBin(const std::string& path);
    const bgfx::Memory* loadMemory(const std::string& path);
    std::array<FontId, static_cast<size_t>(FontRole::Count)> fontRoles;

    std::unordered_map<std::string, SoundData> soundBuffers;
    std::unordered_map<std::string, std::string> musicPaths;

    bgfx::TextureHandle materialAlbedoArray = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle materialNormalArray = BGFX_INVALID_HANDLE;
    std::unordered_map<std::string, MaterialGpuData> materialGpu;

    std::unordered_map<std::string, bgfx::ProgramHandle> programs;
    std::unordered_map<std::string, TextureResource> textures;
    std::unordered_map<std::string, TextureResource> materialsTextures;
    std::unordered_map<FontRole, FontResource> fonts;
};

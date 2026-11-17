#include "resource_manager.hpp"

#include <miniaudio.h>

#include <fstream>

#include "items/block.hpp"
#include "items/flora_validation.hpp"
#include "items/foliage_material.hpp"
#include "items/material.hpp"
#include "items/rock.hpp"
#include "items/soil_validation.hpp"
#include "utils/utf8.hpp"

const bgfx::Memory* ResourceManager::loadMemory(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Failed to open shader: " + path);

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    const bgfx::Memory* mem = bgfx::alloc(uint32_t(size + 1));
    file.read((char*)mem->data, size);
    mem->data[mem->size - 1] = '\0';

    return mem;
}

void ResourceManager::loadFont(FontRole role, const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Failed to open font: " + path);

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);

    file.read(reinterpret_cast<char*>(buffer.data()), size);

    FontResource font;
    font.ttfData = std::move(buffer);

    if (!stbtt_InitFont(&font.fontInfo, font.ttfData.data(), 0)) {
        throw std::runtime_error("Failed to init font: " + path);
    }

    fonts[role] = std::move(font);
}

ResourceManager::ResourceManager() {
    fontRoles.fill(UINT32_MAX);

    registerDataType<Material>();
    registerDataType<Rock>();
    registerDataType<Block>();
    registerDataType<Soil>();
    // Flora types (docs/handoff/flora.md). Ground cover ids are saved raw in
    // tiles, so keep new types and files after the existing ones.
    registerDataType<GroundCover>();
    registerDataType<FoliageMaterial>();
    registerDataType<Flora>();
    // Trees (docs/handoff/trees.md); ids saved raw like flora.
    registerDataType<Tree>();
}

inline FontRole matchRole(std::string key) {
    if (key == "regular") {
        return FontRole::Regular;
    }
    if (key == "bold") {
        return FontRole::Bold;
    }
    if (key == "regular_cjk") {
        return FontRole::Regular_CJK;
    }
    if (key == "bold_cjk") {
        return FontRole::Bold_CJK;
    }

    throw std::runtime_error("Incorrect font role: " + key);
}

void ResourceManager::loadFromJson(const Json& j) {
    // Load fonts
    if (j.contains("fonts")) {
        for (auto& [key, value] : j["fonts"].items()) {
            loadFont(matchRole(key), value.get<std::string>());
        }
    }

    // Load textures
    if (j.contains("textures")) {
        for (auto& [key, value] : j["textures"].items()) {
            loadTexture(key, value.get<std::string>());
        }
    }

    // Load materials textures
    if (j.contains("materials")) {
        for (auto& [key, value] : j["materials"].items()) {
            loadTexture(key, value.get<std::string>(), true);
        }
    }

    // Load sounds
    if (j.contains("sounds")) {
        for (auto& [key, value] : j["sounds"].items()) {
            loadSound(key, value.get<std::string>());
        }
    }

    // Load music
    if (j.contains("music")) {
        for (auto& [key, value] : j["music"].items()) {
            loadMusic(key, value.get<std::string>());
        }
    }

    // Load shaders
    if (j.contains("shaders")) {
        for (auto& [key, value] : j["shaders"].items()) {
            loadShader(key, value.get<std::string>());
        }
    }

    if (j.contains("data")) {
        for (const auto& entry : j["data"]) {
            std::string type = entry["type"];

            for (const auto& file : entry["files"]) {
                Json data = FileIO::readJson(file);

                auto it = loaders.find(type);
                if (it == loaders.end()) {
                    throw std::runtime_error("Unknown data type: " + type);
                }

                try {
                    it->second(*this, data);
                } catch (const std::exception& error) {
                    throw std::runtime_error(file.get<std::string>() + ": " +
                                             error.what());
                }
            }
        }
    }
    validateSoilReferences(*this);
    validateFloraReferences(*this);
    validateTreeReferences(*this);
    validateFoliageTextures(*this, [&](const std::string& key) {
        return materialsTextures.contains(key);
    });
    for (const auto& material : getStorage<Material>().items) {
        if (!materialsTextures.contains(material.texture_albedo) ||
            !materialsTextures.contains(material.texture_normal))
            throw std::runtime_error(material.name +
                                     ": missing material texture");
    }
    for (const auto& block : getStorage<Block>().items) {
        if (!getStorage<Material>().indexByName.contains(block.material))
            throw std::runtime_error(block.name + ": missing material " +
                                     block.material);
    }
}

void ResourceManager::clear() {
    for (auto& [_, program] : programs) {
        if (bgfx::isValid(program)) bgfx::destroy(program);
    }

    for (auto& [_, tex] : textures) {
        if (bgfx::isValid(tex.handle)) bgfx::destroy(tex.handle);
    }

    for (auto& [_, tex] : materialsTextures) {
        if (bgfx::isValid(tex.handle)) bgfx::destroy(tex.handle);
    }

    for (auto& [_, f] : fonts) {
        for (auto& [_, atlases] : f.atlases) {
            for (auto& a : atlases) {
                if (bgfx::isValid(a.atlas)) bgfx::destroy(a.atlas);
            }
        }
    }

    for (auto& [key, sd] : soundBuffers) {
        ma_free(sd.pData, nullptr);
    }

    if (bgfx::isValid(materialAlbedoArray)) bgfx::destroy(materialAlbedoArray);
    if (bgfx::isValid(materialNormalArray)) bgfx::destroy(materialNormalArray);
    if (bgfx::isValid(foliageAlbedoArray)) bgfx::destroy(foliageAlbedoArray);
    if (bgfx::isValid(foliageNormalArray)) bgfx::destroy(foliageNormalArray);

    textures.clear();
    fonts.clear();
    programs.clear();
    soundBuffers.clear();
    musicPaths.clear();
}

void ResourceManager::loadTexture(const std::string& key,
                                  const std::string& path, bool isMaterial) {
    int w, h, channels;
    stbi_uc* data = stbi_load(path.c_str(), &w, &h, &channels, 4);

    if (!data) throw std::runtime_error("Failed to load texture: " + path);

    std::vector<uint8_t> cpu(data, data + w * h * 4);

    if (!isMaterial) {
        textures[key] = {BGFX_INVALID_HANDLE, uint16_t(w), uint16_t(h),
                         std::move(cpu)};
        const bgfx::Memory* mem =
            bgfx::makeRef(textures[key].cpuData.data(), w * h * 4);

        textures[key].handle =
            bgfx::createTexture2D(uint16_t(w), uint16_t(h), false, 1,
                                  bgfx::TextureFormat::RGBA8, 0, mem);
    } else {
        materialsTextures[key] = {BGFX_INVALID_HANDLE, uint16_t(w), uint16_t(h),
                                  std::move(cpu)};
    }

    stbi_image_free(data);
}

void ResourceManager::loadSound(const std::string& key,
                                const std::string& path) {
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Missing sound: " + path);
    }

    ma_decoder_config backendConfig =
        ma_decoder_config_init(ma_format_f32, 2, 48000);

    ma_uint64 frameCount;
    void* pRawData;

    ma_result result =
        ma_decode_file(path.c_str(), &backendConfig, &frameCount, &pRawData);
    if (result != MA_SUCCESS) {
        throw std::runtime_error("Can't load sound: " + path);
    }

    soundBuffers.emplace(key,
                         SoundData{.pData = pRawData,
                                   .frameCount = frameCount,
                                   .format = backendConfig.format,
                                   .channels = backendConfig.channels,
                                   .sampleRate = backendConfig.sampleRate});
}

const SoundData& ResourceManager::getSoundData(const std::string& key) const {
    auto it = soundBuffers.find(key);
    if (it == soundBuffers.end()) {
        throw std::runtime_error("Sound not found: " + key);
    }
    return it->second;
}

void ResourceManager::loadMusic(const std::string& key,
                                const std::string& path) {
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Failed to load: " + path);
    }
    musicPaths[key] = path;
}

const std::string& ResourceManager::getMusicPath(const std::string& key) const {
    auto it = musicPaths.find(key);
    if (it == musicPaths.end()) {
        throw std::runtime_error("Music not found: " + key);
    }
    return it->second;
}

bgfx::ShaderHandle ResourceManager::loadShaderBin(const std::string& path) {
    const bgfx::Memory* mem = loadMemory(path);
    bgfx::ShaderHandle handle = bgfx::createShader(mem);

    if (!bgfx::isValid(handle))
        throw std::runtime_error("Invalid shader: " + path);

    return handle;
}

void ResourceManager::loadShader(const std::string& key,
                                 const std::string& path) {
    std::string vsPath = path + ".vs.bin";
    std::string fsPath = path + ".fs.bin";

    bgfx::ShaderHandle vsh = loadShaderBin(vsPath);
    bgfx::ShaderHandle fsh = loadShaderBin(fsPath);

    bgfx::ProgramHandle program = bgfx::createProgram(vsh, fsh, true);

    if (!bgfx::isValid(program))
        throw std::runtime_error("Failed to create program: " + key);

    programs[key] = program;
}

Vec2u ResourceManager::getTextureSize(const std::string& key) {
    auto it = textures.find(key);
    if (it == textures.end())
        throw std::runtime_error("Texture not found: " + key);

    return {it->second.width, it->second.height};
}

bgfx::ProgramHandle ResourceManager::getProgram(const std::string& key) const {
    auto it = programs.find(key);
    if (it == programs.end())
        throw std::runtime_error("Program not found: " + key);

    return it->second;
}

bgfx::TextureHandle ResourceManager::getTexture(const std::string& key) const {
    auto it = textures.find(key);
    if (it == textures.end())
        throw std::runtime_error("Texture not found: " + key);

    return it->second.handle;
}

FontAtlas& ResourceManager::createNewAtlas(FontRole role, uint32_t size) {
    auto& font = fonts.at(role);
    auto& list = font.atlases[size];

    const int atlasW = 512;
    const int atlasH = 512;

    FontAtlas atlas;
    atlas.atlas = bgfx::createTexture2D(
        atlasW, atlasH, false, 1, bgfx::TextureFormat::R8,
        BGFX_TEXTURE_NONE | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
            BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP |
            BGFX_SAMPLER_V_CLAMP,
        nullptr);

    atlas.cpuBitmap.resize(atlasW * atlasH, 0);
    bgfx::updateTexture2D(
        atlas.atlas, 0, 0, 0, 0, atlasW, atlasH,
        bgfx::copy(atlas.cpuBitmap.data(), atlas.cpuBitmap.size()));

    atlas.size = size;
    atlas.width = atlasW;
    atlas.height = atlasH;
    atlas.penX = 0;
    atlas.penY = 0;
    atlas.rowHeight = 0;

    float scale = stbtt_ScaleForPixelHeight(&font.fontInfo, (float)size);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font.fontInfo, &ascent, &descent, &lineGap);
    atlas.lineHeight = (ascent - descent + lineGap) * scale;
    atlas.ascent = ascent * scale;

    list.push_back(std::move(atlas));
    return list.back();
}

// Возвращает последний (текущий) атлас, создаёт если нет
FontAtlas& ResourceManager::getCurrentAtlas(FontRole role, uint32_t size) {
    auto& font = fonts.at(role);
    auto& list = font.atlases[size];
    if (list.empty()) return createNewAtlas(role, size);
    return list.back();
}

FontResource& ResourceManager::getFontResource(FontRole role, uint32_t size) {
    return fonts[role];
}

void ResourceManager::warmupGlyphs(FontRole role, uint32_t size,
                                   const std::vector<int32_t>& codepoints) {
    FontResource& font = fonts[role];
    float scale = stbtt_ScaleForPixelHeight(&font.fontInfo, (float)size);

    std::vector<int32_t> newChars;
    for (int32_t cp : codepoints) {
        if (cp == '\n') continue;
        if (getGlyph(role, size, cp) == nullptr)
            if (std::find(newChars.begin(), newChars.end(), cp) ==
                newChars.end())
                newChars.push_back(cp);
    }
    if (newChars.empty()) return;

    for (int32_t cp : newChars) {
        int w, h, xoff, yoff;
        unsigned char* bitmap = stbtt_GetCodepointBitmap(
            &font.fontInfo, scale, scale, cp, &w, &h, &xoff, &yoff);

        FontAtlas* atlas = &getCurrentAtlas(role, size);

        // Перенос на новую строку
        if (atlas->penX + w >= atlas->width) {
            atlas->penX = 0;
            atlas->penY += atlas->rowHeight;
            atlas->rowHeight = 0;
        }

        // Переполнение — создаём новый атлас
        if (atlas->penY + h >= atlas->height) {
            // Сначала flush текущего
            bgfx::updateTexture2D(
                atlas->atlas, 0, 0, 0, 0, atlas->width, atlas->height,
                bgfx::copy(atlas->cpuBitmap.data(), atlas->cpuBitmap.size()));

            atlas = &createNewAtlas(role, size);
        }

        int atlasIndex = (int)font.atlases[size].size() - 1;

        for (int row = 0; row < h; row++) {
            memcpy(atlas->cpuBitmap.data() +
                       (atlas->penY + row) * atlas->width + atlas->penX,
                   bitmap + row * w, w);
        }

        Glyph g;
        g.u0 = atlas->penX / float(atlas->width);
        g.v0 = atlas->penY / float(atlas->height);
        g.u1 = (atlas->penX + w) / float(atlas->width);
        g.v1 = (atlas->penY + h) / float(atlas->height);
        g.width = w;
        g.height = h;
        g.bearingX = xoff;
        g.bearingY = yoff;
        g.atlasIndex = atlasIndex;
        int advance;
        stbtt_GetCodepointHMetrics(&font.fontInfo, cp, &advance, 0);
        g.advance = advance * scale;

        atlas->penX += w;
        atlas->rowHeight = std::max(atlas->rowHeight, h);
        atlas->glyphs[cp] = g;

        stbtt_FreeBitmap(bitmap, nullptr);
    }

    // Flush последнего атласа
    FontAtlas& last = getCurrentAtlas(role, size);
    bgfx::updateTexture2D(
        last.atlas, 0, 0, 0, 0, last.width, last.height,
        bgfx::copy(last.cpuBitmap.data(), last.cpuBitmap.size()));
}

const Glyph* ResourceManager::getGlyph(FontRole role, uint32_t size,
                                       int32_t cp) const {
    auto& font = fonts.at(role);
    auto it = font.atlases.find(size);
    if (it == font.atlases.end()) return nullptr;

    for (auto& atlas : it->second) {
        auto git = atlas.glyphs.find(cp);
        if (git != atlas.glyphs.end()) return &git->second;
    }
    return nullptr;
}

bool ResourceManager::fontHasGlyph(FontRole role, int32_t codepoint) {
    auto& font = fonts.at(role);
    // stbtt возвращает 0 если глифа нет
    return stbtt_FindGlyphIndex(&font.fontInfo, codepoint) != 0;
}

uint16_t ResourceManager::getMaterialAlbedoLayer(const std::string& id) const {
    auto it = materialGpu.find(id);
    if (it == materialGpu.end())
        throw std::runtime_error("Material not found: " + id);
    return it->second.albedoLayer;
}

uint16_t ResourceManager::getMaterialNormalLayer(const std::string& id) const {
    auto it = materialGpu.find(id);
    if (it == materialGpu.end())
        throw std::runtime_error("Material not found: " + id);
    return it->second.normalLayer;
}

bgfx::TextureHandle ResourceManager::getMaterialAlbedoArray() const {
    return materialAlbedoArray;
}

bgfx::TextureHandle ResourceManager::getMaterialNormalArray() const {
    return materialNormalArray;
}

static std::vector<uint8_t> downsample(const uint8_t* src, uint16_t w,
                                       uint16_t h) {
    uint16_t nw = std::max<uint16_t>(1, w / 2);
    uint16_t nh = std::max<uint16_t>(1, h / 2);
    std::vector<uint8_t> dst(nw * nh * 4);
    for (uint16_t y = 0; y < nh; ++y) {
        for (uint16_t x = 0; x < nw; ++x) {
            for (int c = 0; c < 4; ++c) {
                uint32_t val = src[((2 * y) * w + (2 * x)) * 4 + c] +
                               src[((2 * y) * w + (2 * x + 1)) * 4 + c] +
                               src[((2 * y + 1) * w + (2 * x)) * 4 + c] +
                               src[((2 * y + 1) * w + (2 * x + 1)) * 4 + c];
                dst[(y * nw + x) * 4 + c] = uint8_t(val / 4);
            }
        }
    }
    return dst;
}

void ResourceManager::buildMaterialTextureArrays() {
    const auto& materials_storage = getStorage<Material>();
    const auto& materials = materials_storage.items;

    const uint16_t texSize = 128;
    const bgfx::TextureFormat::Enum format = bgfx::TextureFormat::RGBA8;

    uint16_t layerCount = (uint16_t)materials.size();

    if (layerCount == 0)
        throw std::runtime_error("No materials to build texture array");

    // ---- CREATE ARRAYS ----
    auto mipCount = [](uint16_t size) -> uint16_t {
        uint16_t n = 0;
        while (size > 0) {
            ++n;
            size >>= 1;
        }
        return n;
    };
    const uint16_t numMips = mipCount(texSize);  // 8 levels (128,64,32,...,1)

    materialAlbedoArray =
        bgfx::createTexture2D(texSize, texSize, true, layerCount, format,
                              BGFX_TEXTURE_NONE | BGFX_SAMPLER_MIN_ANISOTROPIC |
                                  BGFX_SAMPLER_MAG_POINT);

    materialNormalArray =
        bgfx::createTexture2D(texSize, texSize, true, layerCount, format,
                              BGFX_TEXTURE_NONE | BGFX_SAMPLER_MIN_ANISOTROPIC |
                                  BGFX_SAMPLER_MAG_POINT);

    if (!bgfx::isValid(materialAlbedoArray))
        throw std::runtime_error("Failed to create albedo array");

    if (!bgfx::isValid(materialNormalArray))
        throw std::runtime_error("Failed to create normal array");

    // Inside the layer loop, replace the two updateTexture2D calls:
    for (uint16_t layer = 0; layer < layerCount; ++layer) {
        const Material& mat = materials[layer];

        auto uploadMips = [&](bgfx::TextureHandle arr,
                              const TextureResource& tex,
                              const std::string& name) {
            if (tex.width != texSize || tex.height != texSize)
                throw std::runtime_error("Invalid material texture size: " +
                                         name);

            // Mip 0 — original data
            std::vector<uint8_t> current(tex.cpuData);  // copy
            uint16_t mw = texSize, mh = texSize;

            for (uint16_t mip = 0; mip < numMips; ++mip) {
                bgfx::updateTexture2D(arr, layer, mip, 0, 0, mw, mh,
                                      bgfx::copy(current.data(), mw * mh * 4));
                if (mw == 1 && mh == 1) break;
                current = downsample(current.data(), mw, mh);
                mw = std::max<uint16_t>(1, mw / 2);
                mh = std::max<uint16_t>(1, mh / 2);
            }
        };

        uploadMips(materialAlbedoArray,
                   materialsTextures.at(mat.texture_albedo),
                   mat.texture_albedo);
        uploadMips(materialNormalArray,
                   materialsTextures.at(mat.texture_normal),
                   mat.texture_normal);

        materialGpu[mat.name] =
            MaterialGpuData{.albedoLayer = layer, .normalLayer = layer};
    }
    buildFoliageTextureArrays();
}

// Mips of a cutout card keep the coverage of the full-size card: plain box
// filtering thins leaves and blades into nothing at a distance.
static void keepCoverage(std::vector<uint8_t>& texels, double coverage) {
    const size_t count = texels.size() / 4;
    if (!count || coverage <= 0) return;
    const auto covered = [&](double scale) {
        size_t n = 0;
        for (size_t i = 0; i < count; ++i)
            if (texels[i * 4 + 3] * scale >= 127.5) ++n;
        return double(n) / double(count);
    };
    double lo = 0.5, hi = 8;
    for (int step = 0; step < 16; ++step) {
        const double mid = (lo + hi) / 2;
        (covered(mid) < coverage ? lo : hi) = mid;
    }
    for (size_t i = 0; i < count; ++i)
        texels[i * 4 + 3] =
            uint8_t(std::min(255.0, std::round(texels[i * 4 + 3] * hi)));
}

void ResourceManager::buildFoliageTextureArrays() {
    if (!hasStorage<FoliageMaterial>()) return;
    const auto& materials = getStorage<FoliageMaterial>().items;
    if (materials.empty()) return;
    constexpr uint16_t size = 256;
    uint16_t mips = 0;
    for (uint16_t s = size; s > 0; s >>= 1) ++mips;
    const auto layers = uint16_t(materials.size());
    const uint64_t flags = BGFX_TEXTURE_NONE | BGFX_SAMPLER_MIN_ANISOTROPIC;
    foliageAlbedoArray = bgfx::createTexture2D(
        size, size, true, layers, bgfx::TextureFormat::RGBA8, flags);
    foliageNormalArray = bgfx::createTexture2D(
        size, size, true, layers, bgfx::TextureFormat::RGBA8, flags);
    if (!bgfx::isValid(foliageAlbedoArray) ||
        !bgfx::isValid(foliageNormalArray))
        throw std::runtime_error("Failed to create foliage texture arrays");
    std::vector<uint8_t> flat(size_t(size) * size * 4);
    for (size_t i = 0; i < flat.size(); i += 4) {
        flat[i] = flat[i + 1] = 128;
        flat[i + 2] = flat[i + 3] = 255;
    }
    const auto upload = [&](bgfx::TextureHandle array, uint16_t layer,
                            const std::vector<uint8_t>& texels,
                            const std::string& name, bool cutout) {
        if (texels.size() != size_t(size) * size * 4)
            throw std::runtime_error("Foliage texture must be 256x256: " +
                                     name);
        std::vector<uint8_t> current(texels);
        size_t opaque = 0;
        for (size_t i = 3; i < current.size(); i += 4)
            opaque += current[i] >= 128;
        const double coverage = double(opaque) / double(size * size);
        uint16_t w = size;
        for (uint16_t mip = 0; mip < mips; ++mip) {
            bgfx::updateTexture2D(
                array, layer, mip, 0, 0, w, w,
                bgfx::copy(current.data(), uint32_t(current.size())));
            if (w == 1) break;
            current = downsample(current.data(), w, w);
            w = std::max<uint16_t>(1, w / 2);
            if (cutout) keepCoverage(current, coverage);
        }
    };
    for (const auto& m : materials) {
        const auto layer = uint16_t(m.id - 1);
        upload(foliageAlbedoArray, layer,
               materialsTextures.at(m.albedo).cpuData, m.albedo, true);
        upload(foliageNormalArray, layer,
               m.normal.empty() ? flat : materialsTextures.at(m.normal).cpuData,
               m.normal, false);
    }
}

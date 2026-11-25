#include "renderer.hpp"

#include <iostream>

#include "utils/font_resolver.hpp"
#include "utils/utf8.hpp"

Renderer::Renderer(Platform &platform)
    : platform(platform), audio_system(resources) {
    bgfx::Init init{};
    init.platformData = platform.getPlatformData();
    init.type = bgfx::RendererType::Vulkan;  // For now
    Vec2 res = platform.getWindowSize();
    init.resolution.width = res.x;
    init.resolution.height = res.y;
    init.resolution.reset = BGFX_RESET_NONE;

    bool ok = bgfx::init(init);
    assert(ok);

    platform.onResize.connect([](Vec2 newSize) {
        bgfx::reset(newSize.x, newSize.y, BGFX_RESET_NONE);
    });

    createRectMesh();
    createRectUVMesh();
    u_color = bgfx::createUniform("u_color", bgfx::UniformType::Vec4);
    u_uvRect = bgfx::createUniform("u_uvRect", bgfx::UniformType::Vec4);
    u_tex = bgfx::createUniform("u_tex", bgfx::UniformType::Sampler);
    skyRenderer = std::make_unique<SkyRenderer>(resources);
}

void Renderer::draw(const RenderTarget &target) {
    //     case CommandType::TextLayout:
    //         drawTextLayout(cmd.layout);
    //         break;
    //     }
    // }
    for (const auto &cmd : target.commands) {
        switch (cmd.type) {
            case CommandType::Rect:
                drawRect(std::get<DrawRectCmd>(cmd.data));
                break;
            case CommandType::Sprite:
                drawSprite(std::get<DrawSpriteCmd>(cmd.data));
                break;
            case CommandType::Text:
                drawText(std::get<DrawTextCmd>(cmd.data));
                break;
            case CommandType::DrawVertexBuffer:
                drawVB(std::get<DrawVertexBufferCmd>(cmd.data));
                break;
            case CommandType::PushClip: {
                auto clip = std::get<ClipRectCmd>(cmd.data).rect;

                if (!clipStack.empty()) clip = clipStack.back().intersect(clip);

                clipStack.push_back(clip);
                break;
            }
            case CommandType::PopClip: {
                if (!clipStack.empty()) clipStack.pop_back();

                break;
            }
        }
    }
}

bool Renderer::isOpen() { return platform.isRunning(); }

void Renderer::beginFrame() {
    Vec2 res = platform.getWindowSize();

    // View 1 = UI
    bgfx::setViewClear(1, BGFX_CLEAR_NONE, 0x000000FF, 1.0f, 0);

    float view[16];
    bx::mtxIdentity(view);

    float proj[16];
    bx::mtxOrtho(proj, 0.0f, res.x, res.y, 0.0f, 0.0f, 100.0f, 0.0f,
                 bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewMode(1, bgfx::ViewMode::Sequential);
    bgfx::setViewTransform(1, view, proj);
    bgfx::setViewRect(1, 0, 0, (uint16_t)res.x, (uint16_t)res.y);
    bgfx::touch(1);

    // View 0 = world
    bgfx::setViewClear(
        0, (camera.terrainSky ? 0 : BGFX_CLEAR_COLOR) | BGFX_CLEAR_DEPTH,
        0x000000FF, 1.0f, 0);

    Vec3 target = camera.position;

    Vec3 offset;
    offset.x = std::cos(camera.pitch) * std::sin(camera.yaw);
    offset.y = std::sin(camera.pitch);
    offset.z = std::cos(camera.pitch) * std::cos(camera.yaw);

    Vec3 eye = target - offset * camera.distance;

    // Camera-relative: the eye is the origin and every world draw supplies
    // relativeTransform(), so large world coordinates never reach the GPU.
    (void)eye;
    bx::mtxLookAt(view_g, {0.0f, 0.0f, 0.0f}, {offset.x, offset.y, offset.z},
                  {0.0f, 1.0f, 0.0f});

    float aspect = float(res.x) / float(res.y);

    bx::mtxProj(proj_g, 60.0f, aspect, 1.0f, camera.viewDistance + 512.f,
                bgfx::getCaps()->homogeneousDepth);

    bgfx::setViewTransform(0, view_g, proj_g);
    bgfx::setViewRect(0, 0, 0, (uint16_t)res.x, (uint16_t)res.y);
    bgfx::touch(0);
    // Glow shares view 0's camera and depth buffer and runs after it, so
    // additive sprites are hidden by the terrain in front of them.
    bgfx::setViewClear(RenderViews::Glow, BGFX_CLEAR_NONE);
    bgfx::setViewTransform(RenderViews::Glow, view_g, proj_g);
    bgfx::setViewRect(RenderViews::Glow, 0, 0, (uint16_t)res.x,
                      (uint16_t)res.y);
    // Shadow views always run first, whether or not they are used.
    RenderViews::applyOrder(RenderViews::MaxShadowViews);
    if (camera.terrainSky) {
        bgfx::setViewRect(RenderViews::Sky, 0, 0, uint16_t(res.x),
                          uint16_t(res.y));
        bgfx::setViewClear(RenderViews::Sky, BGFX_CLEAR_NONE);
        skyRenderer->draw(camera, aspect, std::get<StaticMesh>(rect.get()));
    }
}

void Renderer::endFrame() {
    bgfx::frame();

    for (auto &mesh : pendingDeletes) {
        std::visit(
            [&](auto &&m) {
                using T = std::decay_t<decltype(m)>;

                if constexpr (std::is_same_v<T, StaticMesh>) {
                    if (bgfx::isValid(m.vbh)) bgfx::destroy(m.vbh);
                    if (bgfx::isValid(m.ibh)) bgfx::destroy(m.ibh);
                } else if constexpr (std::is_same_v<T, DynamicMesh>) {
                    if (bgfx::isValid(m.vbh)) bgfx::destroy(m.vbh);
                    if (bgfx::isValid(m.ibh)) bgfx::destroy(m.ibh);
                }
            },
            mesh);
    }
    pendingDeletes.clear();
}

ResourceManager &Renderer::getResources() { return resources; }

void Renderer::clearTextCaches() { wrapCache.clear(); }

void Renderer::drawRect(const DrawRectCmd &cmd) {
    bgfx::ProgramHandle program = resources.getProgram("color");
    auto mesh = std::get<StaticMesh>(rect.get());

    float mtx[16];
    float translate[16], scale[16];
    bx::mtxTranslate(translate, cmd.rect.x, cmd.rect.y, 0.0f);
    bx::mtxScale(scale, cmd.rect.width, cmd.rect.height, 1.0f);
    bx::mtxMul(mtx, scale, translate);

    bgfx::setTransform(mtx);
    bgfx::setVertexBuffer(0, mesh.vbh);
    bgfx::setIndexBuffer(mesh.ibh);

    float color[4] = {cmd.color.r / 255.0f, cmd.color.g / 255.0f,
                      cmd.color.b / 255.0f, cmd.color.a / 255.0f};

    bgfx::setUniform(u_color, color);

    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_DEPTH_TEST_ALWAYS | BGFX_STATE_BLEND_ALPHA);
    applyClip();
    bgfx::submit(1, program);
}

void Renderer::drawSprite(const DrawSpriteCmd &cmd) {
    auto program = resources.getProgram("sprite");
    auto tex = resources.getTexture(*cmd.texture);
    auto mesh = std::get<StaticMesh>(uv_rect.get());

    float mtx[16];
    float translate[16], scale[16];
    bx::mtxTranslate(translate, cmd.rect.x, cmd.rect.y, 0.0f);
    bx::mtxScale(scale, cmd.rect.width, cmd.rect.height, 1.0f);
    bx::mtxMul(mtx, scale, translate);

    bgfx::setTransform(mtx);

    bgfx::setVertexBuffer(0, mesh.vbh);
    bgfx::setIndexBuffer(mesh.ibh);

    bgfx::setTexture(0, u_tex, tex);

    float color[4] = {cmd.tint.r / 255.0f, cmd.tint.g / 255.0f,
                      cmd.tint.b / 255.0f, cmd.tint.a / 255.0f};

    bgfx::setUniform(u_color, color);

    auto texSize = resources.getTextureSize(*cmd.texture);  // need width/height
    float invW = 1.0f / texSize.x;
    float invH = 1.0f / texSize.y;

    float uvRect[4] = {cmd.uv.x * invW, cmd.uv.y * invH, cmd.uv.width * invW,
                       cmd.uv.height * invH};

    bgfx::setUniform(u_uvRect, uvRect);

    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_DEPTH_TEST_ALWAYS | BGFX_STATE_BLEND_ALPHA);
    applyClip();
    bgfx::submit(1, program);
}

void Renderer::drawText(const DrawTextCmd &cmd) {
    auto codepoints = utf8ToCodepoints(cmd.text);

    auto program = resources.getProgram("text");
    auto mesh = std::get<StaticMesh>(uv_rect.get());

    float maxWidth = cmd.rect.width > 0.f ? cmd.rect.width : 0.f;
    auto lines = wrapText(codepoints, maxWidth, cmd.font, cmd.size);

    // Считаем общую высоту для вертикального выравнивания
    const FontAtlas &baseAtlas = resources.getCurrentAtlas(cmd.font, cmd.size);
    float lineHeight = baseAtlas.lineHeight;
    float totalHeight = lines.size() * lineHeight;

    float startY = cmd.rect.y;
    if (cmd.alignV == VerticalAlign::Middle)
        startY = cmd.rect.y + (cmd.rect.height - totalHeight) * 0.5f;
    else if (cmd.alignV == VerticalAlign::Bottom)
        startY = cmd.rect.y + cmd.rect.height - totalHeight;

    float y = startY;

    for (auto &line : lines) {
        float startX;

        if (cmd.alignH == TextAlign::Center)
            startX = cmd.rect.x +
                     (cmd.rect.width - (line.maxX - line.minX)) * 0.5f -
                     line.minX;
        else if (cmd.alignH == TextAlign::Right)
            startX = cmd.rect.x + cmd.rect.width - (line.maxX - line.minX) -
                     line.minX;
        else
            startX = cmd.rect.x - line.minX;

        float x = startX;

        for (int32_t cp : line.codepoints) {
            const Glyph *gPtr = resources.getGlyph(line.font, cmd.size, cp);
            if (!gPtr) {
                continue;
            }
            const Glyph &g = *gPtr;

            float x0 = x + g.bearingX;
            float baselineY = y + baseAtlas.ascent;
            float y0 = baselineY + g.bearingY;

            float mtx[16], translate[16], scale[16];
            bx::mtxTranslate(translate, x0, y0, 0.0f);
            bx::mtxScale(scale, g.width, g.height, 1.0f);
            bx::mtxMul(mtx, scale, translate);
            bgfx::setTransform(mtx);

            float color[4] = {cmd.color.r / 255.0f, cmd.color.g / 255.0f,
                              cmd.color.b / 255.0f, cmd.color.a / 255.0f};

            FontResource &res = resources.getFontResource(line.font, cmd.size);
            bgfx::TextureHandle tex = res.atlases[cmd.size][g.atlasIndex].atlas;

            bgfx::setVertexBuffer(0, mesh.vbh);
            bgfx::setIndexBuffer(mesh.ibh);
            bgfx::setTexture(0, u_tex, tex);
            bgfx::setUniform(u_color, color);

            float uvRect[4] = {g.u0, g.v0, g.u1 - g.u0, g.v1 - g.v0};
            bgfx::setUniform(u_uvRect, uvRect);

            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                           BGFX_STATE_DEPTH_TEST_ALWAYS |
                           BGFX_STATE_BLEND_ALPHA);
            applyClip();
            bgfx::submit(1, program);

            x += g.advance;
        }

        y += lineHeight;
    }
}

void Renderer::drawVB(const DrawVertexBufferCmd &cmd) {
    uint64_t state;
    bgfx::ViewId view;

    switch (cmd.layer) {
        case DrawLayer::UI:
            view = 1;
            state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                    BGFX_STATE_DEPTH_TEST_ALWAYS | BGFX_STATE_BLEND_ALPHA;
            break;
        case DrawLayer::GAME:
            view = 0;
            state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                    BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
            break;
        case DrawLayer::GAME_GLOW:
            view = RenderViews::Glow;
            state = BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS |
                    BGFX_STATE_BLEND_ADD;
            break;
        case DrawLayer::SHADOW:
            view = cmd.view;
            state = BGFX_STATE_WRITE_R | BGFX_STATE_WRITE_Z |
                    BGFX_STATE_DEPTH_TEST_LESS;
            break;
    }

    bool need_clip = cmd.layer == DrawLayer::UI;

    if (cmd.isStaticMesh) {
        // Handle static mesh
        auto mesh = std::get<StaticMesh>(cmd.mesh);

        if (cmd.shader && bgfx::isValid(mesh.vbh) && bgfx::isValid(mesh.ibh)) {
            bgfx::setTransform(cmd.transform.data());
            bgfx::setVertexBuffer(0, mesh.vbh);
            bgfx::setIndexBuffer(mesh.ibh);

            bgfx::ProgramHandle program = resources.getProgram(*cmd.shader);
            if (!bgfx::isValid(program)) {
                throw std::runtime_error("Invalid shader: " + *cmd.shader);
            }

            for (auto *uniform : cmd.uniforms) {
                uniform->bind();
            }
            if (cmd.instances) bgfx::setInstanceDataBuffer(cmd.instances);

            bgfx::setState(state);

            if (need_clip) applyClip();
            bgfx::submit(view, program);
        }
    } else {
        // Handle dynamic mesh
        auto mesh = std::get<DynamicMesh>(cmd.mesh);

        // Not created (out of bgfx handles) or nothing to draw.
        if (cmd.shader && IRenderer::validDynamic(mesh) &&
            mesh.activeIndices > 0) {
            bgfx::setTransform(cmd.transform.data());
            bgfx::setVertexBuffer(0, mesh.vbh, 0, mesh.activeVertices);
            bgfx::setIndexBuffer(mesh.ibh, 0, mesh.activeIndices);

            bgfx::ProgramHandle program = resources.getProgram(*cmd.shader);
            if (!bgfx::isValid(program)) {
                throw std::runtime_error("Invalid shader: " + *cmd.shader);
            }

            for (auto *uniform : cmd.uniforms) {
                uniform->bind();
            }

            bgfx::setState(state);

            if (need_clip) applyClip();
            bgfx::submit(view, program);
        }
    }
}

std::vector<Line> Renderer::wrapText(const std::vector<int32_t> &codepoints,
                                     float maxWidth, FontRole font,
                                     uint32_t size) const {
    WrapKey key{codepoints, font, size, maxWidth};

    auto it = wrapCache.find(key);
    if (it != wrapCache.end()) return it->second;

    auto lines = static_cast<const Renderer *>(this)->wrapText(
        codepoints, maxWidth, font, size);
    return lines;
}

std::vector<Line> Renderer::wrapText(const std::vector<int32_t> &codepoints,
                                     float maxWidth, FontRole font,
                                     uint32_t size) {
    std::vector<Line> lines;
    WrapKey key{codepoints, font, size, maxWidth};
    auto it = wrapCache.find(key);
    if (it != wrapCache.end()) return it->second;

    // Прогреваем все глифы сразу, включая фолбеки
    std::vector<int32_t> toWarmBase, toWarmFallback;
    FontRole fallbackRole = font;
    for (int32_t cp : codepoints) {
        if (cp == '\n') continue;
        if (resources.fontHasGlyph(font, cp))
            toWarmBase.push_back(cp);
        else {
            fallbackRole = FontResolver::fallbackFontFor(cp, font);
            toWarmFallback.push_back(cp);
        }
    }
    resources.warmupGlyphs(font, size, toWarmBase);
    if (!toWarmFallback.empty())
        resources.warmupGlyphs(fallbackRole, size, toWarmFallback);

    auto getFontFor = [&](int32_t cp) -> FontRole {
        return resources.fontHasGlyph(font, cp) ? font : fallbackRole;
    };
    auto getGlyphFor = [&](int32_t cp) -> const Glyph * {
        return resources.getGlyph(getFontFor(cp), size, cp);
    };

    int rawLineIndex = 0;

    // currentStart — индекс в codepoints где начинается текущая строка
    size_t currentStart = 0;
    std::vector<int32_t> current;
    float currentWidth = 0.f;

    auto flushLine = [&](size_t endIdx) {
        FontRole lineFont = font;
        for (int32_t cp : current) {
            if (!resources.fontHasGlyph(font, cp)) {
                lineFont = fallbackRole;
                break;
            }
        }
        float pen = 0.f;
        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();

        for (int32_t cp : current) {
            const Glyph *g = getGlyphFor(cp);
            if (!g) continue;

            float x0 = pen + g->bearingX;
            float x1 = x0 + g->width;

            minX = std::min(minX, x0);
            maxX = std::max(maxX, x1);

            pen += g->advance;
        }

        // Handle empty line safely
        if (minX > maxX) {
            minX = 0.f;
            maxX = 0.f;
        }

        lines.push_back({current, currentStart, endIdx, lineFont, rawLineIndex,
                         minX, maxX});
        current.clear();
        currentStart = endIdx;
    };

    for (size_t i = 0; i < codepoints.size(); i++) {
        int32_t cp = codepoints[i];

        if (cp == '\n') {
            flushLine(i);          // end = позиция '\n', не включая его
            currentStart = i + 1;  // следующая строка начинается после '\n'
            currentWidth = 0.f;
            rawLineIndex++;
            continue;
        }

        const Glyph *g = getGlyphFor(cp);
        float glyphW = g ? g->advance : 0.f;

        if (!current.empty() && maxWidth > 0.f &&
            currentWidth + glyphW > maxWidth) {
            // Ищем последний пробел для переноса по словам
            int splitAt = -1;
            for (int j = (int)current.size() - 1; j >= 0; j--) {
                if (current[j] == ' ') {
                    splitAt = j;
                    break;
                }
            }

            if (splitAt >= 0) {
                // splitAt — локальный индекс в current
                // глобальный индекс пробела в codepoints:
                size_t globalSplit = currentStart + splitAt;

                std::vector<int32_t> nextLine(current.begin() + splitAt + 1,
                                              current.end());
                current.resize(splitAt);
                flushLine(globalSplit);  // end = позиция пробела
                rawLineIndex++;

                current = nextLine;
                currentStart = globalSplit + 1;  // после пробела

                currentWidth = 0.f;
                for (int32_t c2 : current) {
                    const Glyph *g2 = getGlyphFor(c2);
                    if (g2) currentWidth += g2->advance;
                }
            } else {
                flushLine(i);
                rawLineIndex++;
                currentStart = i;
                currentWidth = 0.f;
            }
        }

        current.push_back(cp);
        currentWidth += glyphW;
    }

    flushLine(codepoints.size());  // последняя строка

    if (wrapCache.size() > 1024) {
        wrapCache.clear();
    }

    wrapCache.emplace(key, lines);
    return lines;
}

float Renderer::measureTextWidth(const std::vector<int32_t> &codepoints,
                                 FontRole font, uint32_t size) const {
    float width = 0.f;
    for (int32_t cp : codepoints) {
        if (cp == '\n') break;
        const Glyph *g = resources.getGlyph(font, size, cp);
        if (g) width += g->advance;
    }
    return width;
}

void Renderer::drawTextLayout(const DrawTextLayoutCmd &cmd) {
    // const TextLayoutState &state = cmd.layout;

    // if (state.rows.empty())
    //     return;

    // sf::Text sfText(resources.getFont(state.fontRole));
    // sfText.setCharacterSize(state.fontSize);
    // sfText.setFillColor({cmd.textColor.r,
    //                      cmd.textColor.g,
    //                      cmd.textColor.b,
    //                      cmd.textColor.a});

    // bool usingFallback = false;

    // for (const GlyphRow &row : state.rows)
    // {
    //     if (row.text.empty())
    //         continue;

    //     float y = row.baselineY - state.scroll.y;

    //     if (y > state.rect.y + state.rect.height)
    //         continue;
    //     if (y + state.fontSize < state.rect.y)
    //         continue;

    //     if (state.fontRole != row.font)
    //     {
    //         sfText.setFont(resources.getFont(row.font));
    //         usingFallback = true;
    //     }
    //     else if (usingFallback)
    //     {
    //         sfText.setFont(resources.getFont(state.fontRole));
    //         usingFallback = false;
    //     }
    //     sfText.setString(sf::String::fromUtf16(row.text.begin(),
    //     row.text.end()));

    //     // baselineX/Y are already final screen positions (no bearing tricks)
    //     sfText.setPosition({row.baselineX - state.scroll.x,
    //                         row.baselineY - state.scroll.y});

    //     window.draw(sfText);
    // }
}

void Renderer::rebuild(TextLayoutState &state) const {
    // if (!state.dirty)
    //     return;

    // state.rows.clear();

    // state.lineHeight = state.fontSize * 1.2f;

    // // --- WRAP ---
    // float wrapWidth = state.wrap ? state.rect.width : 1e9f;

    // std::vector<Line> lines = wrapText(
    //     state.text,
    //     wrapWidth,
    //     state.fontRole,
    //     state.fontSize);

    // state.totalHeight = static_cast<float>(lines.size()) * state.lineHeight;

    // // --- VERTICAL ALIGN (TOP SPACE) ---
    // float startY = state.rect.y;

    // if (state.alignV == VerticalAlign::Middle)
    //     startY = state.rect.y + (state.rect.height - state.totalHeight) *
    //     0.5f;
    // else if (state.alignV == VerticalAlign::Bottom)
    //     startY = state.rect.y + (state.rect.height - state.totalHeight);

    // sf::Text sfText(resources.getFont(state.fontRole));
    // sfText.setCharacterSize(state.fontSize);
    // state.maxLineWidth = 0.0f;

    // for (size_t i = 0; i < lines.size(); ++i)
    // {
    //     const Line &ln = lines[i];

    //     GlyphRow row;
    //     row.text = ln.text;
    //     row.startIndex = ln.start;
    //     row.endIndex = ln.end;
    //     row.font = ln.font;
    //     row.raw_line_index = ln.raw_line_index;

    //     // --- MEASURE LINE ---
    //     sf::FloatRect bounds = measureText(
    //         ln.text,
    //         row.font,
    //         state.fontSize);

    //     row.bearingX = bounds.position.x;
    //     row.bearingY = bounds.position.y;
    //     float lineWidth = bounds.size.x;
    //     state.maxLineWidth = std::max(state.maxLineWidth, lineWidth);

    //     // --- HORIZONTAL ALIGN ---
    //     switch (state.alignH)
    //     {
    //     case TextAlign::Center:
    //         row.baselineX =
    //             state.rect.x +
    //             (state.rect.width - bounds.size.x) * 0.5f -
    //             bounds.position.x;
    //         break;

    //     case TextAlign::Right:
    //         row.baselineX =
    //             state.rect.x +
    //             (state.rect.width - bounds.size.x) -
    //             bounds.position.x;
    //         break;

    //     default: // Left
    //         row.baselineX = state.rect.x - bounds.position.x;
    //         break;
    //     }

    //     // --- BASELINE Y (CRITICAL: convert top -> baseline) ---
    //     float topY = startY + static_cast<float>(i) * state.lineHeight;
    //     row.baselineY = topY - bounds.position.y;

    //     sfText.setFont(resources.getFont(row.font));
    //     // --- CARET POSITIONS ---
    //     sfText.setString(sf::String::fromUtf16(
    //         ln.text.begin(), ln.text.end()));

    //     row.caretXPositions.resize(ln.text.size() + 1);

    //     for (size_t j = 0; j <= ln.text.size(); ++j)
    //         row.caretXPositions[j] = sfText.findCharacterPos(j).x;

    //     state.rows.push_back(std::move(row));
    // }

    // state.dirty = false;
    // state.caretDirty = true;
    // state.selDirty = true;
}

void Renderer::rebuildIncrementally(TextLayoutState &state) const {
    // if (!state.incrementDirty || state.lastDelta == 0)
    //     return;

    // state.incrementDirty = false;

    // if (std::abs(state.lastDelta) > 256)
    // {
    //     state.markLayoutDirty();
    //     return;
    // }

    // if (state.rows.empty())
    //     return;

    // if (state.lastDelta == -1 && state.removedNewLine)
    // {
    //     size_t idx = state.incrementIdx;

    //     // 1. найти строку ВЫШЕ (куда склеиваем)
    //     int upperRaw = -1;

    //     for (const auto &row : state.rows)
    //     {
    //         if (idx >= row.startIndex && idx < row.endIndex)
    //         {
    //             upperRaw = row.raw_line_index;
    //             break;
    //         }
    //     }

    //     if (upperRaw < 0)
    //         return;

    //     int lowerRaw = upperRaw + 1;

    //     // 2. найти диапазон rows
    //     size_t firstRow = SIZE_MAX;
    //     size_t lastRow = 0;

    //     for (size_t i = 0; i < state.rows.size(); ++i)
    //     {
    //         int raw = state.rows[i].raw_line_index;

    //         if (raw == upperRaw || raw == lowerRaw)
    //         {
    //             if (firstRow == SIZE_MAX)
    //                 firstRow = i;
    //             lastRow = i;
    //         }
    //         else if (raw > lowerRaw)
    //             break;
    //     }

    //     if (firstRow == SIZE_MAX)
    //         return;

    //     size_t oldRowCount = lastRow - firstRow + 1;

    //     // 3. raw границы
    //     size_t rawStart = state.rows[firstRow].startIndex;
    //     while (rawStart > 0 && state.text[rawStart - 1] != u'\n')
    //         --rawStart;

    //     size_t rawEnd = state.rows[lastRow].endIndex;
    //     while (rawEnd < state.text.size() && state.text[rawEnd] != u'\n')
    //         ++rawEnd;

    //     std::u16string merged = state.text.substr(rawStart, rawEnd -
    //     rawStart);

    //     float wrapWidth = state.wrap ? state.rect.width : 1e9f;

    //     auto newLines = wrapText(
    //         merged,
    //         wrapWidth,
    //         state.fontRole,
    //         state.fontSize);

    //     // 4. индексы
    //     for (auto &ln : newLines)
    //     {
    //         ln.start += rawStart;
    //         ln.end += rawStart;
    //         ln.raw_line_index = upperRaw;
    //     }

    //     // 5. удалить старые
    //     state.rows.erase(
    //         state.rows.begin() + firstRow,
    //         state.rows.begin() + lastRow + 1);

    //     // 6. построить (тот же код GlyphRow)
    //     sf::Text sfText(resources.getFont(state.fontRole));
    //     sfText.setCharacterSize(state.fontSize);
    //     std::vector<GlyphRow> builtRows;
    //     builtRows.reserve(newLines.size());

    //     for (size_t i = 0; i < newLines.size(); ++i)
    //     {
    //         const Line &ln = newLines[i];

    //         GlyphRow row;
    //         row.text = ln.text;
    //         row.startIndex = ln.start;
    //         row.endIndex = ln.end;
    //         row.font = ln.font;
    //         row.raw_line_index = ln.raw_line_index;

    //         sf::FloatRect bounds = measureText(
    //             ln.text,
    //             row.font,
    //             state.fontSize);

    //         row.bearingX = bounds.position.x;
    //         row.bearingY = bounds.position.y;

    //         // baselineX (без изменений логики)
    //         switch (state.alignH)
    //         {
    //         case TextAlign::Center:
    //             row.baselineX =
    //                 state.rect.x +
    //                 (state.rect.width - bounds.size.x) * 0.5f -
    //                 bounds.position.x;
    //             break;

    //         case TextAlign::Right:
    //             row.baselineX =
    //                 state.rect.x +
    //                 (state.rect.width - bounds.size.x) -
    //                 bounds.position.x;
    //             break;

    //         default:
    //             row.baselineX = state.rect.x - bounds.position.x;
    //             break;
    //         }

    //         // baselineY будет выставлен позже

    //         sfText.setFont(resources.getFont(row.font));
    //         sfText.setString(sf::String::fromUtf16(
    //             ln.text.begin(), ln.text.end()));

    //         row.caretXPositions.resize(ln.text.size() + 1);

    //         for (size_t j = 0; j <= ln.text.size(); ++j)
    //             row.caretXPositions[j] = sfText.findCharacterPos(j).x;

    //         builtRows.push_back(std::move(row));
    //     }

    //     // 7. вставить
    //     state.rows.insert(
    //         state.rows.begin() + firstRow,
    //         builtRows.begin(),
    //         builtRows.end());

    //     size_t newRowCount = builtRows.size();

    //     // 8. сдвиг ниже
    //     size_t shiftStart = firstRow + newRowCount;

    //     for (size_t i = shiftStart; i < state.rows.size(); ++i)
    //     {
    //         state.rows[i].startIndex -= 1;
    //         state.rows[i].endIndex -= 1;
    //         state.rows[i].raw_line_index -= 1;
    //     }

    //     // 9. пересчёт Y + height (как у тебя уже есть)
    //     float lineHeight = state.lineHeight;

    //     float baseY;
    //     if (firstRow == 0)
    //         baseY = state.rect.y;
    //     else
    //         baseY = state.rows[firstRow - 1].baselineY + lineHeight;

    //     for (size_t i = firstRow; i < state.rows.size(); ++i)
    //     {
    //         GlyphRow &row = state.rows[i];
    //         float topY = baseY + (i - firstRow) * lineHeight;
    //         row.baselineY = topY - row.bearingY;
    //     }

    //     // --- 11. высота ---
    //     float oldHeight = oldRowCount * lineHeight;
    //     float newHeight = newRowCount * lineHeight;
    //     state.totalHeight += (newHeight - oldHeight);

    //     // --- 12. ширина ---
    //     state.maxLineWidth = 0.0f;
    //     for (const auto &r : state.rows)
    //     {
    //         float w = r.caretXPositions.empty() ? 0.f :
    //         r.caretXPositions.back(); state.maxLineWidth =
    //         std::max(state.maxLineWidth, w);
    //     }

    //     state.caretDirty = true;
    //     state.selDirty = true;
    //     state.removedNewLine = false;
    // }
    // else if (state.lastDelta == 1 || state.lastDelta == -1)
    // {

    //     // --- 1. найти raw_line_index по символу ---
    //     size_t idx = std::min(state.incrementIdx, state.text.size());

    //     int targetRawLine = 0;

    //     for (const GlyphRow &row : state.rows)
    //     {
    //         if (idx >= row.startIndex && idx <= row.endIndex)
    //         {
    //             targetRawLine = row.raw_line_index;
    //             break;
    //         }
    //     }

    //     // --- 2. собрать старые rows этой строки ---
    //     size_t firstRow = SIZE_MAX;
    //     size_t lastRow = 0;

    //     for (size_t i = 0; i < state.rows.size(); ++i)
    //     {
    //         if (state.rows[i].raw_line_index == targetRawLine)
    //         {
    //             if (firstRow == SIZE_MAX)
    //                 firstRow = i;
    //             lastRow = i;
    //         }
    //         if (state.rows[i].raw_line_index > targetRawLine)
    //         {
    //             break;
    //         }
    //     }

    //     if (firstRow == SIZE_MAX)
    //         return;

    //     size_t oldRowCount = lastRow - firstRow + 1;

    //     // --- 3. получить диапазон текста строки ---
    //     size_t rawStart = state.rows[firstRow].startIndex;

    //     // ищем до \n или конца текста
    //     size_t rawEnd = rawStart;
    //     while (rawEnd < state.text.size() && state.text[rawEnd] != u'\n')
    //         ++rawEnd;

    //     std::u16string lineText = state.text.substr(rawStart, rawEnd -
    //     rawStart);

    //     // --- 4. wrap только этой строки ---
    //     float wrapWidth = state.wrap ? state.rect.width : 1e9f;

    //     std::vector<Line> newLines = wrapText(
    //         lineText,
    //         wrapWidth,
    //         state.fontRole,
    //         state.fontSize);

    //     // поправить индексы
    //     for (auto &ln : newLines)
    //     {
    //         ln.start += rawStart;
    //         ln.end += rawStart;
    //         ln.raw_line_index = targetRawLine;
    //     }

    //     size_t newRowCount = newLines.size();

    //     // --- 5. удалить старые rows ---
    //     state.rows.erase(
    //         state.rows.begin() + firstRow,
    //         state.rows.begin() + lastRow + 1);

    //     // --- 6. вставить новые rows ---
    //     sf::Text sfText(resources.getFont(state.fontRole));
    //     sfText.setCharacterSize(state.fontSize);

    //     std::vector<GlyphRow> builtRows;
    //     builtRows.reserve(newRowCount);

    //     for (size_t i = 0; i < newLines.size(); ++i)
    //     {
    //         const Line &ln = newLines[i];

    //         GlyphRow row;
    //         row.text = ln.text;
    //         row.startIndex = ln.start;
    //         row.endIndex = ln.end;
    //         row.font = ln.font;
    //         row.raw_line_index = ln.raw_line_index;

    //         sf::FloatRect bounds = measureText(
    //             ln.text,
    //             row.font,
    //             state.fontSize);

    //         row.bearingX = bounds.position.x;
    //         row.bearingY = bounds.position.y;

    //         // baselineX (без изменений логики)
    //         switch (state.alignH)
    //         {
    //         case TextAlign::Center:
    //             row.baselineX =
    //                 state.rect.x +
    //                 (state.rect.width - bounds.size.x) * 0.5f -
    //                 bounds.position.x;
    //             break;

    //         case TextAlign::Right:
    //             row.baselineX =
    //                 state.rect.x +
    //                 (state.rect.width - bounds.size.x) -
    //                 bounds.position.x;
    //             break;

    //         default:
    //             row.baselineX = state.rect.x - bounds.position.x;
    //             break;
    //         }

    //         // baselineY будет выставлен позже

    //         sfText.setFont(resources.getFont(row.font));
    //         sfText.setString(sf::String::fromUtf16(
    //             ln.text.begin(), ln.text.end()));

    //         row.caretXPositions.resize(ln.text.size() + 1);

    //         for (size_t j = 0; j <= ln.text.size(); ++j)
    //             row.caretXPositions[j] = sfText.findCharacterPos(j).x;

    //         builtRows.push_back(std::move(row));
    //     }

    //     // вставляем
    //     state.rows.insert(
    //         state.rows.begin() + firstRow,
    //         builtRows.begin(),
    //         builtRows.end());

    //     // все строки после изменённого блока
    //     size_t shiftStart = firstRow + newRowCount;

    //     for (size_t i = shiftStart; i < state.rows.size(); ++i)
    //     {
    //         state.rows[i].startIndex += state.lastDelta;
    //         state.rows[i].endIndex += state.lastDelta;
    //     }

    //     // --- 7. пересчитать baselineY только от изменённого места ---
    //     float lineHeight = state.lineHeight;

    //     // найти baselineY первой строки
    //     float baseY;

    //     if (firstRow == 0)
    //     {
    //         float startY = state.rect.y;

    //         if (state.alignV == VerticalAlign::Middle)
    //             startY = state.rect.y + (state.rect.height -
    //             state.totalHeight) * 0.5f;
    //         else if (state.alignV == VerticalAlign::Bottom)
    //             startY = state.rect.y + (state.rect.height -
    //             state.totalHeight);

    //         baseY = startY;
    //     }
    //     else
    //     {
    //         baseY = state.rows[firstRow - 1].baselineY + lineHeight;
    //     }

    //     // пересчитать Y для всех строк ниже
    //     for (size_t i = firstRow; i < state.rows.size(); ++i)
    //     {
    //         GlyphRow &row = state.rows[i];

    //         float topY = baseY + (i - firstRow) * lineHeight;
    //         row.baselineY = topY - row.bearingY;
    //     }

    //     // --- 8. обновить totalHeight ---
    //     float oldHeight = oldRowCount * lineHeight;
    //     float newHeight = newRowCount * lineHeight;

    //     float delta = newHeight - oldHeight;
    //     state.totalHeight += delta;

    //     // --- 9. maxLineWidth (дешёвый вариант — не идеально) ---
    //     state.maxLineWidth = 0.0f;
    //     for (const auto &r : state.rows)
    //     {
    //         float w = r.caretXPositions.empty() ? 0.f :
    //         r.caretXPositions.back(); state.maxLineWidth =
    //         std::max(state.maxLineWidth, w);
    //     }

    //     state.caretDirty = true;
    //     state.selDirty = true;
    // }
    // else if (state.lastDelta < -1 || state.lastDelta > 1)
    // {
    //     size_t changeStart = state.incrementIdx;
    //     size_t changeEnd = state.incrementIdx + std::max(0, state.lastDelta);

    //     // для удаления (delta < 0)
    //     if (state.lastDelta < 0)
    //     {
    //         changeStart = state.incrementIdx;
    //         changeEnd = state.incrementIdx - state.lastDelta; // минус минус
    //     }

    //     changeStart = std::min(changeStart, state.text.size());
    //     changeEnd = std::min(changeEnd, state.text.size());

    //     int firstRaw = -1;
    //     int lastRaw = -1;

    //     for (const GlyphRow &row : state.rows)
    //     {
    //         if (row.endIndex < changeStart)
    //             continue;
    //         if (row.startIndex > changeEnd)
    //             break;

    //         if (firstRaw == -1)
    //             firstRaw = row.raw_line_index;

    //         lastRaw = row.raw_line_index;
    //     }

    //     if (firstRaw == -1)
    //         return;

    //     size_t firstRow = SIZE_MAX;
    //     size_t lastRow = 0;

    //     for (size_t i = 0; i < state.rows.size(); ++i)
    //     {
    //         int raw = state.rows[i].raw_line_index;

    //         if (raw >= firstRaw && raw <= lastRaw)
    //         {
    //             if (firstRow == SIZE_MAX)
    //                 firstRow = i;
    //             lastRow = i;
    //         }
    //         else if (raw > lastRaw)
    //             break;
    //     }

    //     if (firstRow == SIZE_MAX)
    //         return;

    //     size_t oldRowCount = lastRow - firstRow + 1;
    //     size_t rawStart = state.rows[firstRow].startIndex;

    //     // идём вверх до начала строки (\n назад)
    //     while (rawStart > 0 && state.text[rawStart - 1] != u'\n')
    //         --rawStart;

    //     // идём вниз до конца строки
    //     size_t rawEnd = state.rows[lastRow].endIndex;
    //     while (rawEnd < state.text.size() && state.text[rawEnd] != u'\n')
    //         ++rawEnd;

    //     std::u16string blockText = state.text.substr(rawStart, rawEnd -
    //     rawStart); float wrapWidth = state.wrap ? state.rect.width : 1e9f;

    //     std::vector<Line> newLines = wrapText(
    //         blockText,
    //         wrapWidth,
    //         state.fontRole,
    //         state.fontSize);

    //     // назначаем индексы
    //     int rawLineCursor = firstRaw;

    //     for (auto &ln : newLines)
    //     {
    //         ln.start += rawStart;
    //         ln.end += rawStart;

    //         ln.raw_line_index = rawLineCursor;

    //         // если строка заканчивается \n → новая raw строка
    //         if (ln.end < state.text.size() && state.text[ln.end] == u'\n')
    //             rawLineCursor++;
    //     }

    //     state.rows.erase(
    //         state.rows.begin() + firstRow,
    //         state.rows.begin() + lastRow + 1);

    //     sf::Text sfText(resources.getFont(state.fontRole));
    //     sfText.setCharacterSize(state.fontSize);

    //     std::vector<GlyphRow> builtRows;
    //     builtRows.reserve(newLines.size());

    //     for (size_t i = 0; i < newLines.size(); ++i)
    //     {
    //         const Line &ln = newLines[i];

    //         GlyphRow row;
    //         row.text = ln.text;
    //         row.startIndex = ln.start;
    //         row.endIndex = ln.end;
    //         row.font = ln.font;
    //         row.raw_line_index = ln.raw_line_index;

    //         sf::FloatRect bounds = measureText(
    //             ln.text,
    //             row.font,
    //             state.fontSize);

    //         row.bearingX = bounds.position.x;
    //         row.bearingY = bounds.position.y;

    //         // baselineX (без изменений логики)
    //         switch (state.alignH)
    //         {
    //         case TextAlign::Center:
    //             row.baselineX =
    //                 state.rect.x +
    //                 (state.rect.width - bounds.size.x) * 0.5f -
    //                 bounds.position.x;
    //             break;

    //         case TextAlign::Right:
    //             row.baselineX =
    //                 state.rect.x +
    //                 (state.rect.width - bounds.size.x) -
    //                 bounds.position.x;
    //             break;

    //         default:
    //             row.baselineX = state.rect.x - bounds.position.x;
    //             break;
    //         }

    //         // baselineY будет выставлен позже

    //         sfText.setFont(resources.getFont(row.font));
    //         sfText.setString(sf::String::fromUtf16(
    //             ln.text.begin(), ln.text.end()));

    //         row.caretXPositions.resize(ln.text.size() + 1);

    //         for (size_t j = 0; j <= ln.text.size(); ++j)
    //             row.caretXPositions[j] = sfText.findCharacterPos(j).x;

    //         builtRows.push_back(std::move(row));
    //     }

    //     // вставляем
    //     state.rows.insert(
    //         state.rows.begin() + firstRow,
    //         builtRows.begin(),
    //         builtRows.end());

    //     // все строки после изменённого блока
    //     size_t newRowCount = newLines.size();
    //     size_t shiftStart = firstRow + newRowCount;

    //     for (size_t i = shiftStart; i < state.rows.size(); ++i)
    //     {
    //         state.rows[i].startIndex += state.lastDelta;
    //         state.rows[i].endIndex += state.lastDelta;

    //         // корректируем raw_line_index
    //         state.rows[i].raw_line_index += (rawLineCursor - lastRaw - 1);
    //     }

    //     float lineHeight = state.lineHeight;

    //     // найти baselineY первой строки
    //     float baseY;

    //     if (firstRow == 0)
    //     {
    //         float startY = state.rect.y;

    //         if (state.alignV == VerticalAlign::Middle)
    //             startY = state.rect.y + (state.rect.height -
    //             state.totalHeight) * 0.5f;
    //         else if (state.alignV == VerticalAlign::Bottom)
    //             startY = state.rect.y + (state.rect.height -
    //             state.totalHeight);

    //         baseY = startY;
    //     }
    //     else
    //     {
    //         baseY = state.rows[firstRow - 1].baselineY + lineHeight;
    //     }

    //     // пересчитать Y для всех строк ниже
    //     for (size_t i = firstRow; i < state.rows.size(); ++i)
    //     {
    //         GlyphRow &row = state.rows[i];

    //         float topY = baseY + (i - firstRow) * lineHeight;
    //         row.baselineY = topY - row.bearingY;
    //     }

    //     // --- 8. обновить totalHeight ---
    //     float oldHeight = oldRowCount * lineHeight;
    //     float newHeight = newRowCount * lineHeight;

    //     float delta = newHeight - oldHeight;
    //     state.totalHeight += delta;

    //     // --- 9. maxLineWidth (дешёвый вариант — не идеально) ---
    //     state.maxLineWidth = 0.0f;
    //     for (const auto &r : state.rows)
    //     {
    //         float w = r.caretXPositions.empty() ? 0.f :
    //         r.caretXPositions.back(); state.maxLineWidth =
    //         std::max(state.maxLineWidth, w);
    //     }

    //     state.caretDirty = true;
    //     state.selDirty = true;
    // }
}

Vec2 Renderer::caretPosition(TextLayoutState &state, size_t index,
                             bool raw) const {
    if (state.rows.empty()) return {state.rect.x, state.rect.y};

    // Clamp index
    index = std::min(index, state.text.size());

    // Find row containing index
    const GlyphRow *rowPtr = &state.rows.back();

    for (const GlyphRow &row : state.rows) {
        if (index >= row.startIndex && index <= row.endIndex) {
            rowPtr = &row;
            break;
        }
    }

    const GlyphRow &row = *rowPtr;

    size_t local = index - row.startIndex;
    if (local >= row.caretXPositions.size())
        local = row.caretXPositions.size() - 1;

    float x = row.baselineX + row.caretXPositions[local];
    float y = row.baselineY;

    if (raw) {
        return {x, y};
    }

    return {x - state.scroll.x, y - state.scroll.y};
}

size_t Renderer::caretAtPoint(const TextLayoutState &state, Vec2 point) const {
    if (state.rows.empty()) return 0;

    float x = point.x - state.scroll.x;
    float y = point.y + state.scroll.y;

    // Find row by Y
    int rowIndex = 0;
    for (int i = 0; i < (int)state.rows.size(); ++i) {
        float top = state.rows[i].baselineY;
        float bottom = top + state.lineHeight;

        if (y >= top && y < bottom) {
            rowIndex = i;
            break;
        }

        if (y >= bottom) rowIndex = i;
    }

    const GlyphRow &row = state.rows[rowIndex];

    // Convert X into row-local space
    float localX = x - row.baselineX;

    const auto &xp = row.caretXPositions;

    // Binary search nearest caret position
    auto it = std::lower_bound(xp.begin(), xp.end(), localX);
    size_t idx = std::distance(xp.begin(), it);

    if (idx == 0) {
        // nothing
    } else if (idx >= xp.size()) {
        idx = xp.size() - 1;
    } else {
        float dL = localX - xp[idx - 1];
        float dR = xp[idx] - localX;
        if (dL < dR) --idx;
    }

    size_t result = row.startIndex + idx;
    return std::min(result, state.text.size());
}

// ─── buildSelectionRects ────────────────────────────────────────────────────

void Renderer::buildSelectionRects(TextLayoutState &state, size_t selStart,
                                   size_t selEnd) const {
    state.selectionRects.clear();

    if (state.rows.empty()) return;

    if (selStart == selEnd) return;

    if (selStart > selEnd) std::swap(selStart, selEnd);

    selStart = std::min(selStart, state.text.size());
    selEnd = std::min(selEnd, state.text.size());

    for (const GlyphRow &row : state.rows) {
        float y = row.baselineY - state.scroll.y;

        // --- CLIPPING ---
        if (y > state.rect.y + state.rect.height) break;

        if (y + state.fontSize < state.rect.y) continue;

        // IMPORTANT: treat row as [start, end) exclusive range
        size_t rowStart = row.startIndex;
        size_t rowEnd = row.endIndex;

        if (selEnd <= rowStart) continue;
        if (selStart >= rowEnd) continue;

        size_t start = std::max(selStart, rowStart);
        size_t end = std::min(selEnd, rowEnd);

        if (start >= end) continue;

        size_t localStart = start - rowStart;
        size_t localEnd = end - rowStart;

        const auto &xp = row.caretXPositions;

        localStart = std::min(localStart, xp.size() - 1);
        localEnd = std::min(localEnd, xp.size() - 1);

        float x1 = row.baselineX + xp[localStart];
        float x2 = row.baselineX + xp[localEnd];

        if (x2 < x1) std::swap(x1, x2);

        state.selectionRects.push_back({x1 - state.scroll.x,
                                        (row.baselineY) - state.scroll.y,
                                        x2 - x1, state.lineHeight});
    }

    state.selDirty = false;
}

AudioSystem &Renderer::getAudioSystem() { return audio_system; }

void Renderer::clear() {
    skyRenderer.reset();
    if (bgfx::isValid(u_tex)) bgfx::destroy(u_tex);
    if (bgfx::isValid(u_color)) bgfx::destroy(u_color);
    if (bgfx::isValid(u_uvRect)) bgfx::destroy(u_uvRect);

    rect.release();
    uv_rect.release();

    for (auto &mesh : pendingDeletes) {
        std::visit(
            [&](auto &&m) {
                using T = std::decay_t<decltype(m)>;

                if constexpr (std::is_same_v<T, StaticMesh>) {
                    if (bgfx::isValid(m.vbh)) bgfx::destroy(m.vbh);
                    if (bgfx::isValid(m.ibh)) bgfx::destroy(m.ibh);
                } else if constexpr (std::is_same_v<T, DynamicMesh>) {
                    if (bgfx::isValid(m.vbh)) bgfx::destroy(m.vbh);
                    if (bgfx::isValid(m.ibh)) bgfx::destroy(m.ibh);
                }
            },
            mesh);
    }
    pendingDeletes.clear();

    audio_system.close();
    resources.clear();

    bgfx::shutdown();
}

// void Renderer::applyUniforms(sf::Shader& shader, const UniformMap& uniforms)
// {
//     for (const auto& [name, value] : uniforms)
//     {
//         std::visit([&](auto&& v)
//         {
//             using T = std::decay_t<decltype(v)>;
//             if constexpr (std::is_same_v<T, const std::string*>) {
//                 shader.setUniform(name, resources.getTexture(*v));
//             }
//             else {
//                 shader.setUniform(name, v);
//             }
//         }, value);
//     }
// }

void Renderer::scheduleDelete(const MeshVariant &mesh) {
    pendingDeletes.push_back(mesh);
}

void Renderer::createRectMesh() {
    static PosVertex verts[] = {
        {0.f, 0.f, 0.f},
        {1.f, 0.f, 0.f},
        {1.f, 1.f, 0.f},
        {0.f, 1.f, 0.f},
    };

    static uint16_t indices[] = {0, 1, 2, 2, 3, 0};

    rect.initStatic(this, verts, std::size(verts), indices, std::size(indices));
}

void Renderer::createRectUVMesh() {
    static PosUvColorVertex verts[] = {
        {0.f, 0.f, 0.f, 0.f, 0.f, 0xffffffff},
        {1.f, 0.f, 0.f, 1.f, 0.f, 0xffffffff},
        {1.f, 1.f, 0.f, 1.f, 1.f, 0xffffffff},
        {0.f, 1.f, 0.f, 0.f, 1.f, 0xffffffff},
    };

    static uint16_t indices[] = {0, 1, 2, 2, 3, 0};

    uv_rect.initStatic(this, verts, std::size(verts), indices,
                       std::size(indices));
}

void Renderer::applyClip() {
    if (!clipStack.empty()) {
        const Rect &r = clipStack.back();

        if (r.width <= 0.0f || r.height <= 0.0f) {
            bgfx::setScissor(0, 0, 0, 0);
            return;
        }

        bgfx::setScissor(uint16_t(r.x), uint16_t(r.y), uint16_t(r.width),
                         uint16_t(r.height));
    } else {
        bgfx::setScissor(BGFX_INVALID_HANDLE);
    }
}

const float *Renderer::getViewMatrix() { return view_g; }

const float *Renderer::getProjMatrix() { return proj_g; }

#include "world_generator.hpp"

#include <iostream>

#include "core/geometry.hpp"
#include "generated_hydrology.hpp"
#include "generation/perlin_noise.hpp"
#include "height_profile.hpp"
#include "layers_generator.hpp"
#include "serialization/data_storage_api.hpp"

World WorldGenerator::generate(const WorldGenSettings &settings,
                               IAssets *assets,
                               std::function<bool()> shouldCancel) {
    World &world = tempWorld;

    generateHeight(settings, shouldCancel);
    generateHumidity(settings, shouldCancel);
    generateTemperature(settings, shouldCancel);
    generateRivers(settings, shouldCancel);
    generateFeatures(settings, assets, shouldCancel);

    if (shouldCancel()) return {};
    world.genSettings = settings;
    world.hydrology = GeneratedHydrology::build(world, shouldCancel, true, true,
                                                true, true, true, true, true);
    if (!world.hydrology) return {};
    for (int i = 0; i < int(world.regions.size()); ++i) {
        if (world.hydrology->candidateBody(i) > GeneratedHydrology::Sea)
            world.regions[i].addFeature(FeatureMask::LAKE);
    }
    buildRegionMeshes(shouldCancel);

    return std::move(world);
}

void WorldGenerator::generateHeight(const WorldGenSettings &settings,
                                    std::function<bool()> shouldCancel) {
    World &world = tempWorld;
    world.width = settings.width;
    world.height = settings.height;

    world.regions.resize(settings.width * settings.height);

    std::queue<int> waterQ;

    PerlinNoise noise(settings.height_seed);

    for (unsigned int y = 0; y < settings.height; ++y) {
        for (unsigned int x = 0; x < settings.width; ++x) {
            if (shouldCancel()) {
                tempWorld = World();
                return;
            }
            Region &r = world.regions[y * settings.width + x];
            r.coord = {x, y};

            float h = noise.getHeight(x, y, settings.width, settings.height);

            r.height = h;

            if (h < 0.5f) {
                waterQ.push(y * settings.width + x);
                r.water_distance = 0;
            }

            float waterLevel = 0.5f;
            float gap = 0.005f;

            if (r.height < waterLevel) {
                float t = r.height / waterLevel;  // 0..1
                r.height = t * (waterLevel - gap);
            } else {
                float t =
                    (r.height - waterLevel) / (1.0f - waterLevel);  // 0..1
                r.height = (waterLevel + gap) + t * (1.0f - (waterLevel + gap));
            }

            if (r.height < 0.33f)
                r.land = LandType::DEEP_WATER;
            else if (r.height < 0.45f)
                r.land = LandType::WATER;
            else if (r.height < 0.5f)
                r.land = LandType::SHORE;
            else if (r.height < 0.65f)
                r.land = LandType::PLAINS;
            else if (r.height < 0.7f)
                r.land = LandType::HILLS;
            else if (r.height < 0.85f)
                r.land = LandType::MOUNTAINS;
            else
                r.land = LandType::PEAK;

            if (r.height < 0.45f) {
                r.crust = CrustType::OCEANIC;
            } else if (r.height > 0.55f) {
                r.crust = CrustType::CONTINENTAL;
            } else {
                r.crust = CrustType::TRANSITIONAL;
            }

            r.land_height_m = HeightProfile::heightToY(r.height);
            r.land_height_sea_m = r.land_height_m - SEA_LEVEL_Y;
            r.energy = 0.0f;

            r.features = FeatureMask::None;
        }
    }

    computeDistanceToWater(world, waterQ);
}

void WorldGenerator::computeDistanceToWater(World &world,
                                            std::queue<int> &waterQ) {
    int w = world.width;
    int h = world.height;

    // 2. BFS
    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};

    while (!waterQ.empty()) {
        int cur = waterQ.front();
        waterQ.pop();

        int cx = cur % w;
        int cy = cur / w;

        for (int i = 0; i < 4; ++i) {
            int nx = (cx + dx[i] + w) % w;  // wrap X
            int ny = cy + dy[i];

            if (ny < 0 || ny >= h) continue;

            int ni = ny * w + nx;

            Region &ni_reg = world.regions[ni];
            Region &cur_reg = world.regions[cur];

            if (ni_reg.water_distance > cur_reg.water_distance + 1) {
                ni_reg.water_distance = cur_reg.water_distance + 1;
                waterQ.push(ni);
            }
        }
    }
}

void WorldGenerator::generateHumidity(const WorldGenSettings &settings,
                                      std::function<bool()> shouldCancel) {
    World &world = tempWorld;
    const int w = world.width;
    const int h = world.height;

    if (w <= 0 || h <= 0 || world.regions.size() != static_cast<size_t>(w * h))
        return;

    PerlinNoise noise(settings.humidity_seed);

    auto wrapX = [w](int x) -> int {
        x %= w;
        if (x < 0) x += w;
        return x;
    };

    auto heightToWaterMoisture = [](float height) -> float {
        if (height < 0.33f) return 1.00f;  // deep ocean
        if (height < 0.45f) return 0.95f;  // ocean
        if (height < 0.50f) return 0.85f;  // shallow water / coast water
        if (height < 0.51f) return 0.75f;  // beach
        return 0.0f;
    };

    // Диагональный ветер; можно потом менять сезонно или по широте.
    Vec2 windDir = {10.0f, -5.0f};
    const float windLen =
        std::sqrt(windDir.x * windDir.x + windDir.y * windDir.y);
    windDir.x /= windLen;
    windDir.y /= windLen;

    const float maxDist = 256.0f;

    // 1) Базовая влажность: шум + близость к воде
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (shouldCancel()) {
                tempWorld = World();
                return;
            }

            Region &r = world.regions[y * w + x];

            const float nx = static_cast<float>(x) / 120.0f;
            const float ny = static_cast<float>(y) / 120.0f;

            const float baseNoise =
                noise.fbmTileable(nx, ny, w / 120.0f, h / 120.0f);

            if (r.height < 0.5f) r.water_distance = 0.0f;

            const float d = static_cast<float>(r.water_distance);
            const float moistureFromWater = std::exp(-d / maxDist);

            // База: рядом с водой влажно, дальше суше, плюс мелкая
            // вариативность.
            r.humidity = moistureFromWater * 120.0f + baseNoise * 0.15f;

            // Если клетка сама вода — она всегда влажная.
            r.humidity = std::max(r.humidity, heightToWaterMoisture(r.height));
        }
    }

    // 2) Перенос влаги по ветру
    // Идём против ветра, ищем источник влаги, и постепенно ослабляем поток.
    const int MAX_STEPS = 128;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (shouldCancel()) {
                tempWorld = World();
                return;
            }

            float moisture = 0.0f;
            float shadow = 1.0f;

            float fx = static_cast<float>(x);
            float fy = static_cast<float>(y);

            for (int step = 0; step < MAX_STEPS; ++step) {
                fx -= windDir.x;
                fy -= windDir.y;

                // wrap X
                if (fx < 0.0f) fx += w;
                if (fx >= static_cast<float>(w)) fx -= w;

                int sx = static_cast<int>(std::round(fx));
                int sy = static_cast<int>(std::round(fy));

                sx = wrapX(sx);

                // Y не зациклен
                if (sy < 0 || sy >= h) break;

                const Region &src = world.regions[sy * w + sx];

                // Источник влаги
                const float srcWater = heightToWaterMoisture(src.height);
                if (srcWater > 0.0f) moisture = std::max(moisture, srcWater);

                // Горы создают влажностную тень и постепенно выжигают поток
                if (src.height > 0.70f)
                    shadow *= 0.72f;
                else if (src.height > 0.65f)
                    shadow *= 0.86f;
                else
                    shadow *= 0.97f;

                // dot product: насколько направление луча совпадает с ветром
                float dx = static_cast<float>(x) - fx;
                if (dx > w * 0.5f) dx -= w;
                if (dx < -w * 0.5f) dx += w;

                float dy = static_cast<float>(y) - fy;

                float len = std::sqrt(dx * dx + dy * dy);
                if (len > 0.0f) {
                    dx /= len;
                    dy /= len;
                }

                float influence = dx * windDir.x + dy * windDir.y;
                influence = std::max(0.0f, influence);

                moisture *= 0.985f;
                moisture *= shadow;

                world.regions[y * w + x].humidity +=
                    moisture * influence * 0.55f;
            }
        }
    }

    // 3) Диффузия от воды во все стороны
    // Это делает прибрежные зоны влажными даже если ветер туда не дует.
    for (int pass = 0; pass < 10; ++pass) {
        std::vector<float> temp(w * h, 0.0f);

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (shouldCancel()) {
                    tempWorld = World();
                    return;
                }

                const int xm1 = wrapX(x - 1);
                const int xp1 = wrapX(x + 1);

                float sum = 0.0f;
                float weight = 0.0f;

                auto add = [&](int xx, int yy, float wgt) {
                    if (yy < 0 || yy >= h) return;
                    sum += world.regions[yy * w + xx].humidity * wgt;
                    weight += wgt;
                };

                add(x, y, 1.0f);
                add(xm1, y, 0.8f);
                add(xp1, y, 0.8f);
                add(x, y - 1, 0.8f);
                add(x, y + 1, 0.8f);

                temp[y * w + x] = (weight > 0.0f)
                                      ? (sum / weight)
                                      : world.regions[y * w + x].humidity;
            }
        }

        for (int i = 0; i < w * h; ++i) world.regions[i].humidity = temp[i];
    }

    // 4) Ещё немного сгладить, чтобы не было "пятен"
    {
        std::vector<float> temp(w * h, 0.0f);

        for (int y = 1; y < h - 1; ++y) {
            for (int x = 0; x < w; ++x) {
                if (shouldCancel()) {
                    tempWorld = World();
                    return;
                }

                const int xm1 = wrapX(x - 1);
                const int xp1 = wrapX(x + 1);

                const float sum = world.regions[y * w + x].humidity +
                                  world.regions[y * w + xm1].humidity +
                                  world.regions[y * w + xp1].humidity +
                                  world.regions[(y - 1) * w + x].humidity +
                                  world.regions[(y + 1) * w + x].humidity;

                temp[y * w + x] = sum / 5.0f;
            }
        }

        for (int y = 1; y < h - 1; ++y) {
            for (int x = 0; x < w; ++x) {
                if (shouldCancel()) {
                    tempWorld = World();
                    return;
                }

                world.regions[y * w + x].humidity = temp[y * w + x];
            }
        }
    }

    // 5) Нормализация
    float minH = std::numeric_limits<float>::max();
    float maxH = std::numeric_limits<float>::lowest();

    for (const auto &r : world.regions) {
        minH = std::min(minH, r.humidity);
        maxH = std::max(maxH, r.humidity);
    }

    const float range = std::max(maxH - minH, 1e-6f);
    const float inv = 1.0f / range;

    for (auto &r : world.regions) {
        if (shouldCancel()) {
            tempWorld = World();
            return;
        }

        r.humidity = (r.humidity - minH) * inv;

        // Вода всегда влажная
        if (r.height < 0.50f) r.humidity = 1.0f;
    }
}

void WorldGenerator::generateRivers(const WorldGenSettings &settings,
                                    std::function<bool()> shouldCancel) {
    World &world = tempWorld;

    const int w = world.width;
    const int h = world.height;

    if (w <= 0 || h <= 0) return;

    std::vector<int> flowTo(w * h, -1);
    std::vector<float> flow(w * h, 0.0f);
    std::vector<int> order(w * h);

    auto is_water = [](const Region &r) {
        return r.land == LandType::WATER || r.land == LandType::DEEP_WATER ||
               r.land == LandType::SHORE;
    };

    auto wrapX = [w](int x) {
        x %= w;
        if (x < 0) x += w;
        return x;
    };

    // ========================
    // 1. FLOW DIRECTION
    // ========================
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int i = y * w + x;
            Region &r = world.regions[i];

            float bestScore = 1e9f;
            int best = -1;

            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;

                    int nx = wrapX(x + dx);
                    int ny = y + dy;

                    if (ny < 0 || ny >= h) continue;

                    int ni = ny * w + nx;

                    float dh = world.regions[ni].height - r.height;

                    float cost = dh + 0.002f;

                    if (cost < bestScore) {
                        bestScore = cost;
                        best = ni;
                    }
                }

            flowTo[i] = best;

            // итоговый поток
            flow[i] = (0.5f + r.humidity * 2.0f);

            order[i] = i;
        }
    }

    // ========================
    // 2. SORT (high → low)
    // ========================
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return world.regions[a].height > world.regions[b].height;
    });

    // ========================
    // 3. FLOW ACCUMULATION
    // ========================
    for (int i : order) {
        int to = flowTo[i];
        if (to >= 0) {
            flow[to] += flow[i];
            flow[to] = std::min(flow[to], 50.0f);
        }
    }

    // ========================
    // PARAMETERS
    // ========================
    const float SOURCE_THRESHOLD = 3.0f;  // для истоков
    const int MAX_STEPS = 2048;

    world.riverMesh.chunks_x =
        (w + world.riverMesh.chunk_size.x - 1) / world.riverMesh.chunk_size.x;
    world.riverMesh.chunks_y =
        (h + world.riverMesh.chunk_size.y - 1) / world.riverMesh.chunk_size.y;

    world.riverMesh.chunks.clear();
    world.riverMesh.chunks.resize(world.riverMesh.chunks_x *
                                  world.riverMesh.chunks_y);

    for (auto &c : world.riverMesh.chunks) {
        c.vertices.clear();
    }

    // ========================
    // 4. TRACE RIVERS
    // ========================
    for (int i = 0; i < w * h; ++i) {
        if (shouldCancel()) {
            tempWorld = World();
            return;
        }

        Region &src = world.regions[i];

        if (src.temperature_c < -15.0f) continue;

        if (flow[i] < SOURCE_THRESHOLD) continue;

        if (is_water(src)) continue;

        int cur = i;
        int steps = 0;

        bool valid = false;

        // --- проверка: доходит ли до воды
        int test = i;
        int tsteps = 0;

        while (test != -1 && tsteps < MAX_STEPS) {
            if (is_water(world.regions[test])) {
                valid = true;
                break;
            }

            test = flowTo[test];
            tsteps++;
        }

        if (!valid) continue;

        RiverPolyline river;

        // --- трассировка
        while (cur != -1 && steps < MAX_STEPS) {
            auto toFlowDir = [](const Vec2 &d) -> RiverFlowDirection {
                int dx = (int)std::round(d.x);
                int dy = (int)std::round(d.y);

                if (dx == 0 && dy == 0) return RiverFlowDirection::NO;
                if (dx == -1 && dy == -1) return RiverFlowDirection::NW;
                if (dx == 0 && dy == -1) return RiverFlowDirection::N;
                if (dx == 1 && dy == -1) return RiverFlowDirection::NE;
                if (dx == 1 && dy == 0) return RiverFlowDirection::E;
                if (dx == 1 && dy == 1) return RiverFlowDirection::SE;
                if (dx == 0 && dy == 1) return RiverFlowDirection::S;
                if (dx == -1 && dy == 1) return RiverFlowDirection::SW;
                if (dx == -1 && dy == 0) return RiverFlowDirection::W;

                return RiverFlowDirection::NO;
            };

            Region &r = world.regions[cur];

            if (is_water(r)) {
                if (river.path.size() >= 2) {
                    Vec2 pos = {(float)(cur % w), (float)(cur / w)};
                    float strength = flow[cur];

                    river.path.push_back({pos, strength});

                    // direction from previous segment
                    Vec2 prev = river.path[river.path.size() - 2].first;
                    Vec2 dir{pos.x - prev.x, pos.y - prev.y};

                    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                    if (len < 1e-4f) break;

                    dir.x /= len;
                    dir.y /= len;

                    Vec2 curPos = pos;

                    const int MAX_EXTEND_STEPS = 3;  // safety cap
                    int steps = 0;
                    bool candidate_found = false;

                    while (steps < MAX_EXTEND_STEPS) {
                        if (candidate_found) break;
                        int stepsCandidates = 0;

                        Vec2 curPos = pos;

                        auto isBlocked = [&](const Region &r) {
                            // define what counts as obstacle
                            return r.land == LandType::PLAINS ||
                                   r.land == LandType::HILLS ||
                                   r.land == LandType::MOUNTAINS ||
                                   r.land == LandType::PEAK;
                        };

                        auto scoreDir = [&](const Vec2 &candidateDir,
                                            const Vec2 &curPos) {
                            Vec2 next{curPos.x + candidateDir.x,
                                      curPos.y + candidateDir.y};

                            int xi = (int)std::round(next.x);
                            int yi = (int)std::round(next.y);

                            if (xi < 0 || yi < 0 || xi >= w || yi >= h)
                                return -1e9f;

                            Region &rr = world.regions[yi * w + xi];

                            if (isBlocked(rr)) return -1e6f;

                            // prefer forward direction
                            float forwardScore =
                                candidateDir.x * dir.x + candidateDir.y * dir.y;

                            // optional: prefer water proximity
                            float waterBias =
                                (rr.land == LandType::WATER) ? 2.0f : 0.0f;

                            return forwardScore + waterBias;
                        };

                        while (stepsCandidates < MAX_EXTEND_STEPS) {
                            // generate candidate directions (forward + slight
                            // turns)
                            std::array<Vec2, 5> candidates;

                            // forward
                            candidates[0] = dir;

                            // slight left/right
                            candidates[1] = {dir.x * 0.8f - dir.y * 0.6f,
                                             dir.y * 0.8f + dir.x * 0.6f};
                            candidates[2] = {dir.x * 0.8f + dir.y * 0.6f,
                                             dir.y * 0.8f - dir.x * 0.6f};

                            // stronger turns
                            candidates[3] = {-dir.y, dir.x};
                            candidates[4] = {dir.y, -dir.x};

                            // normalize candidatescandidate_found = true;
                            for (auto &c : candidates) {
                                float l = std::sqrt(c.x * c.x + c.y * c.y);
                                if (l > 1e-4f) {
                                    c.x /= l;
                                    c.y /= l;
                                }
                            }

                            // pick best direction
                            float bestScore = -1e9f;
                            Vec2 bestDir = dir;

                            for (const auto &c : candidates) {
                                float s = scoreDir(c, curPos);
                                if (s > bestScore) {
                                    bestScore = s;
                                    bestDir = c;
                                }
                            }

                            if (bestScore < -1e5f) {
                                candidate_found = true;
                                break;
                            }

                            // move
                            curPos.x += bestDir.x;
                            curPos.y += bestDir.y;

                            int xi = (int)std::round(curPos.x);
                            int yi = (int)std::round(curPos.y);

                            if (xi < 0 || yi < 0 || xi >= w || yi >= h) {
                                candidate_found = true;
                                break;
                            }

                            Region &rr = world.regions[yi * w + xi];

                            // stop when reaching valid terrain
                            if (rr.land != LandType::SHORE) {
                                candidate_found = true;
                                break;
                            }

                            river.path.push_back({curPos, strength});
                            // update direction (smooth turning)
                            dir = bestDir;

                            stepsCandidates++;
                        }
                        steps++;
                    }
                }

                break;
            }

            if (r.temperature_c < -15.0f) {
                r.addFeature(FeatureMask::GLACIER);
                break;
            }

            Vec2 pos = {(float)(cur % w), (float)(cur / w)};
            float strength = flow[cur];
            river.path.push_back({pos, strength});

            int next = flowTo[cur];
            if (next != -1) {
                int x0 = cur % w;
                int y0 = cur / w;

                int x1 = next % w;
                int y1 = next / w;

                Vec2 dir{float(x1 - x0), float(y1 - y0)};

                float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (len > 1e-4f) {
                    dir.x /= len;
                    dir.y /= len;
                }

                if (strength > r.river.strength) {
                    r.river.flow = toFlowDir(dir);
                    r.river.strength = strength;
                }
            }

            // дельта
            if (r.water_distance == 1 && flow[cur] > 20.0f)
                r.addFeature(FeatureMask::DELTA);
            cur = flowTo[cur];
            steps++;
        }

        // --- Build river mesh once (world space) ---
        {
            const auto &path = river.path;
            if (path.size() < 2) continue;

            // ── UNWRAP PATH into continuous coordinates ──────────────
            std::vector<std::pair<Vec2, float>> unwrapped;
            unwrapped.reserve(path.size());
            unwrapped.push_back(path[0]);

            for (size_t i = 1; i < path.size(); ++i) {
                Vec2 prev = unwrapped[i - 1].first;
                Vec2 cur = path[i].first;

                float dx = cur.x - prev.x;
                if (dx > w * 0.5f) dx -= w;
                if (dx < -w * 0.5f) dx += w;

                unwrapped.push_back({{prev.x + dx, cur.y}, path[i].second});
            }

            // ── ONE continuous left/right edge — no segment splitting ──
            std::vector<Vec2> leftPts;
            std::vector<Vec2> rightPts;
            leftPts.reserve(unwrapped.size());
            rightPts.reserve(unwrapped.size());

            Vec2 lastNormal{0.f, 0.f};

            for (size_t i = 0; i < unwrapped.size(); ++i) {
                const auto &[p, flowVal] = unwrapped[i];

                Vec2 dir{0.f, 0.f};

                if (i > 0) {
                    Vec2 pp = unwrapped[i - 1].first;
                    dir.x += p.x - pp.x;
                    dir.y += p.y - pp.y;
                }
                if (i + 1 < unwrapped.size()) {
                    Vec2 np = unwrapped[i + 1].first;
                    dir.x += np.x - p.x;
                    dir.y += np.y - p.y;
                }

                float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (len >= 1e-4f) {
                    dir.x /= len;
                    dir.y /= len;
                    lastNormal = {-dir.y, dir.x};
                }

                float flowQ = std::floor(flowVal * 8.0f) / 8.0f;
                float width = flowQ * 0.005f;

                leftPts.push_back(
                    {p.x - lastNormal.x * width, p.y - lastNormal.y * width});
                rightPts.push_back(
                    {p.x + lastNormal.x * width, p.y + lastNormal.y * width});
            }

            // ── build triangles ──────────────────────────────────────
            auto makeVert = [&](const Vec2 &p, float flowVal, bool isLeft,
                                float ht) -> RiverVertex {
                return RiverVertex(p.x, p.y, 0, 0, 0, 255,
                                   std::clamp(flowVal / 50.0f, 0.f, 1.f),
                                   isLeft ? 0.0f : 1.0f, ht);
            };

            for (size_t i = 1; i < leftPts.size(); ++i) {
                float fA = unwrapped[i - 1].second;
                float fB = unwrapped[i].second;

                float hA = sampleHeight(unwrapped[i - 1].first);
                float hB = sampleHeight(unwrapped[i].first);

                RiverVertex vL0 = makeVert(leftPts[i - 1], fA, true, hA);
                RiverVertex vR0 = makeVert(rightPts[i - 1], fA, false, hA);
                RiverVertex vL1 = makeVert(leftPts[i], fB, true, hB);
                RiverVertex vR1 = makeVert(rightPts[i], fB, false, hB);

                river.mesh.push_back(vL0);
                river.mesh.push_back(vR0);
                river.mesh.push_back(vR1);

                river.mesh.push_back(vL0);
                river.mesh.push_back(vR1);
                river.mesh.push_back(vL1);
            }

            // ── bounds from unwrapped coords ─────────────────────────
            float min_x = std::numeric_limits<float>::max();
            float max_x = -std::numeric_limits<float>::max();
            float min_y = std::numeric_limits<float>::max();
            float max_y = -std::numeric_limits<float>::max();

            for (const auto &[p, _] : unwrapped) {
                if (p.x < min_x) min_x = p.x;
                if (p.x > max_x) max_x = p.x;
                if (p.y < min_y) min_y = p.y;
                if (p.y > max_y) max_y = p.y;
            }
            river.bounds = {min_x, min_y, max_x - min_x, max_y - min_y};
        }

        // NO clamp version (WRAPPING SAFE)
        auto getChunkCoord = [&](const Vec2 &p, int &cx, int &cy) {
            float x = std::fmod(p.x, float(w));
            if (x < 0) x += w;

            float y = std::fmod(p.y, float(h));
            if (y < 0) y += h;

            cx = int(x / world.riverMesh.chunk_size.x);
            cy = int(y / world.riverMesh.chunk_size.y);
        };

        for (size_t i = 0; i + 2 < river.mesh.size(); i += 3) {
            const RiverVertex &a = river.mesh[i + 0];
            const RiverVertex &b = river.mesh[i + 1];
            const RiverVertex &c = river.mesh[i + 2];

            Vec2 centroid{(a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f};

            int cx, cy;
            getChunkCoord(centroid, cx, cy);

            auto &chunk = world.riverMesh.getChunk(cx, cy);

            chunk.vertices.push_back(a);
            chunk.vertices.push_back(b);
            chunk.vertices.push_back(c);
        }

        world.rivers.push_back(river);
    }

    for (auto &chunk : world.riverMesh.chunks) {
        if (chunk.vertices.empty()) continue;

        float min_x = std::numeric_limits<float>::max();
        float max_x = -std::numeric_limits<float>::max();
        float min_y = std::numeric_limits<float>::max();
        float max_y = -std::numeric_limits<float>::max();

        for (const auto &v : chunk.vertices) {
            min_x = std::min(min_x, v.x);
            max_x = std::max(max_x, v.x);
            min_y = std::min(min_y, v.y);
            max_y = std::max(max_y, v.y);
        }

        chunk.bounds = {min_x, min_y, max_x - min_x, max_y - min_y};
    }
}

void WorldGenerator::generateTemperature(const WorldGenSettings &settings,
                                         std::function<bool()> shouldCancel) {
    World &world = tempWorld;

    world.min_temp_c = settings.min_temp_c;
    world.max_temp_c = settings.max_temp_c;

    const int w = world.width;
    const int h = world.height;

    if (w <= 0 || h <= 0) return;

    PerlinNoise noise(settings.temperature_seed);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (shouldCancel()) {
                tempWorld = World();
                return;
            }

            Region &r = world.regions[y * w + x];

            // --- 1. широта ---
            float latitude = (float)y / h;
            float lat = 1.0f - std::abs(latitude * 2.0f - 1.0f);
            lat = std::pow(lat, 1.2f);

            // --- 2. высота ---
            float altitudeCooling = std::pow(r.height, 2.0f);

            // --- 3. шум ---
            float nx = (float)x / 200.0f;
            float ny = (float)y / 200.0f;
            float n = noise.fbmTileable(nx, ny, w / 200.0f, h / 200.0f);

            // --- 4. влажность ---
            float humidityEffect = r.humidity * 0.1f;

            // --- итог ---
            float temp = lat * 1.2f  // широта
                         - altitudeCooling * 0.8f + n * 0.15f + humidityEffect;

            r.temperature = temp;
        }
    }

    // --- нормализация ---
    float minT = std::numeric_limits<float>::max();
    float maxT = std::numeric_limits<float>::lowest();

    for (const auto &r : world.regions) {
        minT = std::min(minT, r.temperature);
        maxT = std::max(maxT, r.temperature);
    }

    float inv = 1.0f / std::max(maxT - minT, 1e-6f);

    for (auto &r : world.regions) {
        r.temperature = (r.temperature - minT) * inv;
        r.temperature_c =
            settings.min_temp_c +
            (settings.max_temp_c - settings.min_temp_c) * r.temperature;

        float t = r.temperature_c;

        if (t < -10)
            r.biome = BiomeType::POLAR;
        else if (t < 0)
            r.biome = BiomeType::TUNDRA;
        else if (t < 15)
            r.biome = BiomeType::TEMPERATE;
        else if (t < 25)
            r.biome = BiomeType::SUBTROPICAL;
        else if (t < 35)
            r.biome = BiomeType::TROPICAL;
        else
            r.biome = BiomeType::SCORCHING;
    }
}

void WorldGenerator::generateFeatures(const WorldGenSettings &settings,
                                      IAssets *assets,
                                      std::function<bool()> shouldCancel) {
    World &world = tempWorld;

    const int w = world.width;
    const int h = world.height;

    if (w <= 0 || h <= 0) return;

    PerlinNoise noise(settings.feature_seed);
    std::mt19937 gen(settings.feature_seed);
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);

    LayersGenerator lg(settings, assets);

    auto idx = [&w](int x, int y) { return y * w + x; };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (shouldCancel()) {
                tempWorld = World();
                return;
            }

            Region &r = world.regions[idx(x, y)];

            // =========================
            // 3. VEGETATION
            // =========================

            auto is_water = [](const Region &reg) {
                return reg.land == LandType::WATER ||
                       reg.land == LandType::DEEP_WATER ||
                       reg.land == LandType::SHORE;
            };

            float hmd = r.humidity;

            if (is_water(r) || r.temperature_c < -10.0f || hmd < 0.2f) {
                r.vegetation = VegetationType::NONE;
            } else if (hmd < 0.4f) {
                r.vegetation = VegetationType::GRASS;
            } else if (hmd < 0.6f) {
                r.vegetation = VegetationType::SHRUB;
            } else if (hmd < 0.8f || r.temperature_c < 20.0f) {
                r.vegetation = VegetationType::FOREST;
            } else {
                r.vegetation = VegetationType::RAINFOREST;
            }

            // =========================
            // 4. FEATURES
            // =========================

            float nx = (float)x / 80.0f;
            float ny = (float)y / 80.0f;
            float n = noise.fbm(nx, ny, w, h);

            // --- ICE / COLD ---
            if (is_water(r) && r.temperature_c < -5)
                r.addFeature(FeatureMask::SEA_ICE);

            if (is_water(r) && r.temperature_c < -15)
                r.addFeature(FeatureMask::ICE_SHEET);

            if (r.land == LandType::MOUNTAINS && r.temperature_c < -5)
                r.addFeature(FeatureMask::GLACIER);

            // --- PERMAFROST ---
            if (!is_water(r) && r.temperature_c < -5 && r.humidity < 0.6f) {
                r.addFeature(FeatureMask::PERMAFROST);
            }

            // --- BEACH ---
            if (r.land == LandType::PLAINS && r.water_distance == 1 &&
                r.temperature_c > -5) {
                r.addFeature(FeatureMask::BEACH);
            }

            // --- ROCKY SHORE ---
            if (r.land == LandType::HILLS && r.water_distance == 1) {
                r.addFeature(FeatureMask::ROCKY_SHORE);
            }

            // --- FLOODPLAIN ---
            if (r.river.flow != RiverFlowDirection::NO && r.humidity > 0.6f &&
                r.land == LandType::PLAINS) {
                r.addFeature(FeatureMask::FLOODPLAIN);
            }

            // --- MANGROVE ---
            if (r.river.flow != RiverFlowDirection::NO &&
                r.water_distance == 1 && r.temperature_c > 20 &&
                r.humidity > 0.7f) {
                r.addFeature(FeatureMask::MANGROVE);
            }

            // --- DESERT DUNES
            float sx = x + settings.feature_seed * 13.37f;
            float sy = y + settings.feature_seed * 7.91f;

            // ── крупные пустынные регионы (мягче и крупнее) ──
            float nx_big = sx / 320.0f;
            float ny_big = sy / 320.0f;

            float desertRegion = noise.fbm(nx_big, ny_big, w, h);
            if (desertRegion < 0.0f) desertRegion = 0.0f;
            if (desertRegion > 1.0f) desertRegion = 1.0f;

            // ── финал (ослаблены все условия) ──
            if (r.temperature_c >= 18.0f && r.humidity < 0.3f &&
                (r.land == LandType::PLAINS || r.land == LandType::HILLS) &&
                desertRegion > 0.35f) {
                r.addFeature(FeatureMask::DESERT_DUNES);
                r.vegetation = VegetationType::NONE;
            }

            // --- SALT FLATS ---
            if (r.biome == BiomeType::SCORCHING && r.humidity < 0.15f &&
                r.water_distance <= 3 && n > 0.6f) {
                r.addFeature(FeatureMask::SALT_FLATS);
            }

            // --- BADLANDS ---
            if (r.humidity < 0.25f && r.land == LandType::HILLS && n > 0.5f) {
                r.addFeature(FeatureMask::BADLANDS);
            }

            // --- PLATEAU ---
            if (r.land == LandType::HILLS && r.height > 0.6f &&
                r.height < 0.7f && n > 0.55f) {
                r.addFeature(FeatureMask::PLATEAU);
            }

            // --- RAVINE ---
            if (!is_water(r) && r.land != LandType::PLAINS) {
                float minNeighbor = r.height;

                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = (x + dx + w) % w;
                        int ny = y + dy;
                        if (ny < 0 || ny >= h) continue;

                        float h2 = world.regions[idx(nx, ny)].height;
                        minNeighbor = std::min(minNeighbor, h2);
                    }

                if (r.height - minNeighbor > 0.15f)
                    r.addFeature(FeatureMask::RAVINE);
            }

            // --- VOLCANO ---

            float volcano = dis(gen);

            if (r.crust == CrustType::CONTINENTAL && volcano > 0.98f) {
                r.addFeature(FeatureMask::VOLCANO);
            }

            // --- CRATER ---
            if (r.land == LandType::PLAINS && n > 0.70f && n < 0.95f) {
                r.addFeature(FeatureMask::CRATER);
            }

            // --- FOREST CLEARING ---
            if (r.vegetation == VegetationType::FOREST && n > 0.6f &&
                n < 0.7f) {
                r.addFeature(FeatureMask::CLEARING);
            }

            // --- BURNED AREA ---
            if (r.temperature_c > 30 && r.humidity < 0.3f &&
                r.vegetation != VegetationType::NONE && n > 0.7f) {
                r.addFeature(FeatureMask::BURNED_AREA);
            }

            // --- CORAL REEF ---
            if (is_water(r) && r.temperature_c > 20 &&
                r.height > 0.45f &&  // мелководье
                r.height < 0.5f && n > 0.5f) {
                r.addFeature(FeatureMask::CORAL_REEF);
            }

            // --- CLIFF ---
            if (!is_water(r)) {
                float maxDiff = 0.0f;

                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = (x + dx + w) % w;
                        int ny = y + dy;
                        if (ny < 0 || ny >= h) continue;

                        float d = std::abs(r.height -
                                           world.regions[idx(nx, ny)].height);
                        maxDiff = std::max(maxDiff, d);
                    }

                if (maxDiff > 0.25f) r.addFeature(FeatureMask::CLIFF);
            }

            // --- FOREST DENSITY ---
            if (r.vegetation == VegetationType::FOREST && r.humidity > 0.7f) {
                r.addFeature(FeatureMask::DENSE_FOREST);
            }

            // --- DEAD ZONE ---
            if (r.temperature_c > 40 && r.humidity < 0.1f) {
                r.addFeature(FeatureMask::DEAD_ZONE);
            }

            // OASIS, SWAMP, MARSH

            if (r.land == LandType::DEEP_WATER || r.land == LandType::WATER ||
                r.land == LandType::SHORE) {
                continue;
            }

            std::vector<RegionLayer> layers = lg.get(r, 20);

            bool shouldHaveOasis = false;
            bool shouldHaveSwamp = false;
            bool shouldHaveMarsh = false;

            for (auto &layer : layers) {
                if (r.hasFeature(FeatureMask::DESERT_DUNES) &&
                    layer.aquifer_saturation > 0.25f) {
                    shouldHaveOasis = true;
                } else if (layer.aquifer_saturation > 0.4f) {
                    shouldHaveSwamp = true;
                } else if (layer.aquifer_saturation > 0.25f) {
                    shouldHaveMarsh = true;
                }
            }

            if (shouldHaveOasis) {
                r.addFeature(FeatureMask::OASIS);
            } else if (shouldHaveSwamp) {
                r.addFeature(FeatureMask::SWAMP);
            } else if (shouldHaveMarsh) {
                r.addFeature(FeatureMask::MARSH);
            }
        }
    }
}

void WorldGenerator::buildRegionMeshes(std::function<bool()> shouldCancel) {
    World &world = tempWorld;
    const int w = world.width;
    const int h = world.height;

    world.regionMeshes.clear();
    world.regionMeshes.resize(static_cast<int>(LayerType::COUNT));

    // ─────────────────────────────────────────────
    // Marching Squares lookup table
    // Each cell corner: TL=8, TR=4, BR=2, BL=1
    // 16 cases → list of edge pairs (each edge = two Vec2 endpoints)
    // Edges are on the unit cell [0,1]x[0,1]
    // ─────────────────────────────────────────────
    //
    //  TL---top---TR
    //  |           |
    // left       right
    //  |           |
    //  BL--bottom--BR
    //
    // midpoints: top=(0.5,0), right=(1,0.5), bottom=(0.5,1), left=(0,0.5)

    struct Edge {
        Vec2 a, b;
    };

    // msEdges[config] = list of edges (0, 2 or 4 endpoints)
    // We store them as flat pairs: {a0,b0, a1,b1, ...}
    const Vec2 TOP = {0.5f, 0.0f};
    const Vec2 RIGHT = {1.0f, 0.5f};
    const Vec2 BOTTOM = {0.5f, 1.0f};
    const Vec2 LEFT = {0.0f, 0.5f};

    // 16 marching-squares cases, each gives 0, 1, or 2 edges
    // Winding: edges go so that "inside" (filled) is to the left
    const std::array<std::vector<Edge>, 16> msEdges = {{
        {},                              // 0000 – nothing
        {{LEFT, BOTTOM}},                // 0001 – BL only
        {{BOTTOM, RIGHT}},               // 0010 – BR only
        {{LEFT, RIGHT}},                 // 0011 – bottom row
        {{RIGHT, TOP}},                  // 0100 – TR only
        {{LEFT, TOP}, {BOTTOM, RIGHT}},  // 0101 – saddle (alt)
        {{BOTTOM, TOP}},                 // 0110 – right col
        {{LEFT, TOP}},                   // 0111 – all except TL
        {{TOP, LEFT}},                   // 1000 – TL only
        {{TOP, BOTTOM}},                 // 1001 – left col
        {{TOP, RIGHT}, {BOTTOM, LEFT}},  // 1010 – saddle (alt)
        {{TOP, RIGHT}},                  // 1011 – all except TR
        {{RIGHT, LEFT}},                 // 1100 – top row
        {{RIGHT, BOTTOM}},               // 1101 – all except BR
        {{BOTTOM, LEFT}},                // 1110 – all except BL
        {},                              // 1111 – fully inside, no edge
    }};

    // ─────────────────────────────────────────────
    // Helper: Douglas-Peucker polyline simplification
    // Used to build LOD1 / LOD2 from LOD0 contours
    // ─────────────────────────────────────────────
    std::function<void(const std::vector<Vec2> &, float, std::vector<Vec2> &)>
        douglasPeucker = [&](const std::vector<Vec2> &pts, float eps,
                             std::vector<Vec2> &out) {
            if (pts.size() < 2) {
                out = pts;
                return;
            }

            // Find point with max distance from line pts.front()–pts.back()
            float maxDist = 0.0f;
            size_t maxIdx = 0;

            Vec2 p1 = pts.front(), p2 = pts.back();
            float dx = p2.x - p1.x, dy = p2.y - p1.y;
            float len = std::sqrt(dx * dx + dy * dy);

            for (size_t i = 1; i + 1 < pts.size(); ++i) {
                float d;
                if (len < 1e-6f) {
                    float ex = pts[i].x - p1.x, ey = pts[i].y - p1.y;
                    d = std::sqrt(ex * ex + ey * ey);
                } else {
                    // Perpendicular distance
                    d = std::abs((pts[i].x - p1.x) * dy -
                                 (pts[i].y - p1.y) * dx) /
                        len;
                }
                if (d > maxDist) {
                    maxDist = d;
                    maxIdx = i;
                }
            }

            if (maxDist > eps) {
                std::vector<Vec2> left(pts.begin(), pts.begin() + maxIdx + 1);
                std::vector<Vec2> right(pts.begin() + maxIdx, pts.end());
                std::vector<Vec2> l2, r2;
                douglasPeucker(left, eps, l2);
                douglasPeucker(right, eps, r2);
                out = l2;
                out.insert(out.end(), r2.begin() + 1, r2.end());
            } else {
                out = {pts.front(), pts.back()};
            }
        };

    // ─────────────────────────────────────────────
    // MAIN LOOP: one pass per layer
    // ─────────────────────────────────────────────
    for (int ly = 0; ly < static_cast<int>(LayerType::COUNT); ++ly) {
        if (shouldCancel()) return;

        LayerType layer = static_cast<LayerType>(ly);
        RegionMesh mesh;

        // ── Step 1: scalar field grid (padded) ──
        // Instead of binary, store the actual height value per cell.
        // The isosurface threshold is the boundary value for each layer.
        const int gw = w + 2;
        const int gh = h + 2;
        std::vector<uint8_t> grid(gw * gh, 0);

        auto sample = [&](int x, int y) -> uint8_t {
            x = (x % w + w) % w;          // wrap X
            y = std::clamp(y, 0, h - 1);  // no Y wrap
            const bool water = world.hydrology->candidateBody(y * w + x) != 0;
            return layer == LayerType::WATER ? water : !water;
        };

        for (int gy = 0; gy < gh; ++gy) {
            for (int gx = 0; gx < gw; ++gx) {
                int wx = gx - 1;
                int wy = gy - 1;

                grid[gy * gw + gx] = sample(wx, wy);
            }
        }

        mesh.chunks_x = (w + mesh.chunk_size.x - 1) / mesh.chunk_size.x;
        mesh.chunks_y = (h + mesh.chunk_size.y - 1) / mesh.chunk_size.y;

        mesh.chunks.resize(mesh.chunks_x * mesh.chunks_y);

        for (int cy = 0; cy < mesh.chunks_y; ++cy) {
            for (int cx = 0; cx < mesh.chunks_x; ++cx) {
                MeshChunk &c = mesh.getChunk(cx, cy);

                float x = cx * mesh.chunk_size.x;
                float y = cy * mesh.chunk_size.y;
                float w = mesh.chunk_size.x;
                float h = mesh.chunk_size.y;

                c.bounds = {x, y, w, h};
            }
        }

        auto emitTri = [&](Vec2 a, Vec2 b, Vec2 c, int lod) {
            Vec2 center{(a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f};

            int cx = int(center.x) / mesh.chunk_size.x;
            int cy = int(center.y) / mesh.chunk_size.y;

            auto wrapCX = [&](int x) -> int {
                x %= mesh.chunks_x;
                if (x < 0) x += mesh.chunks_x;
                return x;
            };

            cx = wrapCX(cx);

            if (cx < 0 || cy < 0 || cx >= mesh.chunks_x || cy >= mesh.chunks_y)
                return;

            auto &chunk = mesh.getChunk(cx, cy);
            std::vector<TerrainVertex> *buf;

            if (lod == 0)
                buf = &chunk.lod0;
            else if (lod == 1)
                buf = &chunk.lod1;
            else if (lod == 2)
                buf = &chunk.lod2;
            else if (lod == 3)
                buf = &chunk.lod3;
            else if (lod == 4)
                buf = &chunk.lod4;
            else if (lod == 5)
                buf = &chunk.lod5;
            else if (lod == 6)
                buf = &chunk.lod6;
            else
                buf = &chunk.lod7;

            for (const auto p : {a, b, c}) {
                const int sx = int(WorldWrap::mod(int(std::floor(p.x)), w));
                const int sy = std::clamp(int(std::floor(p.y)), 0, h - 1);
                const float height = sampleHeight(p);

                const Region &r = world.regions[sy * w + sx];
                auto f = encodeFeatures(r);

                TerrainVertex v;
                v.x = p.x;
                v.y = p.y;
                v.z = 0.0f;

                v.biome = f.biome / 255.0f;
                v.moisture = f.moisture / 255.0f;
                v.temperature = f.temperature / 255.0f;
                // The preview shader uses 0.5 to select its water palette.
                v.height = layer == LayerType::WATER ? std::min(height, .499f)
                                                     : std::max(height, .5f);

                buf->push_back(v);
            }
        };

        auto fanTriangulateEmit = [&](const std::vector<Vec2> &poly,
                                      int lod = 0) {
            if (poly.size() < 3) return;

            for (size_t i = 1; i + 1 < poly.size(); ++i) {
                emitTri(poly[0], poly[i], poly[i + 1], lod);
            }
        };

        // ── Step 2: Marching Squares with interpolated midpoints ──
        std::vector<Edge> rawEdges;
        rawEdges.reserve(w * h / 4);

        // Interpolate crossing point between two field values along a unit
        // edge. Returns t in [0,1] where the zero crossing is.
        auto lerpT = [](float va, float vb) -> float {
            if (std::abs(vb - va) < 1e-6f) return 0.5f;
            // crossing at va + t*(vb-va) = 0  →  t = -va / (vb-va)
            float t = -va / (vb - va);
            return std::clamp(t, 0.0f, 1.0f);
        };

        for (int gy = 0; gy < gh - 1; ++gy) {
            for (int gx = 0; gx < gw - 1; ++gx) {
                uint8_t tl = grid[(gy)*gw + (gx)];
                uint8_t tr = grid[(gy)*gw + (gx + 1)];
                uint8_t br = grid[(gy + 1) * gw + (gx + 1)];
                uint8_t bl = grid[(gy + 1) * gw + (gx)];

                int config = (tl << 3) | (tr << 2) | (br << 1) | bl;

                float ox = static_cast<float>(gx - 1);
                float oy = static_cast<float>(gy - 1);

                if (config == 15) {
                    Vec2 a{ox, oy};
                    Vec2 b{ox + 1.f, oy};
                    Vec2 c{ox + 1.f, oy + 1.f};
                    Vec2 d{ox, oy + 1.f};
                    emitTri(a, b, c, 0);
                    emitTri(a, c, d, 0);
                    continue;
                }
                if (config == 0) continue;
                const auto &edgePairs = msEdges[config];
                for (const Edge &e : edgePairs) {
                    rawEdges.push_back(
                        {{e.a.x + ox, e.a.y + oy}, {e.b.x + ox, e.b.y + oy}});
                }

                // Also fill the inside portion of partial cells
                // Build the polygon of "inside" corners + edge midpoints
                std::vector<Vec2> cellPoly;
                cellPoly.reserve(6);

                // Add inside corners in order (TL→TR→BR→BL)
                struct Corner {
                    Vec2 pos;
                    bool inside;
                };
                Corner corners[4] = {
                    {{ox, oy}, tl == 1},
                    {{ox + 1.f, oy}, tr == 1},
                    {{ox + 1.f, oy + 1.f}, br == 1},
                    {{ox, oy + 1.f}, bl == 1},
                };

                // Walk corners; insert midpoint wherever inside/outside flips
                Vec2 mids[4] = {
                    {ox + 0.5f, oy},        // top
                    {ox + 1.f, oy + 0.5f},  // right
                    {ox + 0.5f, oy + 1.f},  // bottom
                    {ox, oy + 0.5f},        // left
                };

                if (layer == LayerType::WATER &&
                    (config == 5 || config == 10)) {
                    for (int i = 0; i < 4; ++i)
                        if (corners[i].inside)
                            emitTri(corners[i].pos, mids[i], mids[(i + 3) % 4],
                                    0);
                    continue;
                }

                for (int ci = 0; ci < 4; ++ci) {
                    int ni = (ci + 1) % 4;
                    if (corners[ci].inside) cellPoly.push_back(corners[ci].pos);
                    if (corners[ci].inside != corners[ni].inside)
                        cellPoly.push_back(mids[ci]);
                }

                fanTriangulateEmit(cellPoly, 0);
            }
        }

        // ── Step 4: build LOD1 / LOD2 via simplified contours ──
        for (int lodLevel = 1; lodLevel <= 7; ++lodLevel) {
            const int step = lodLevel;

            // Build a downsampled grid by averaging blocks.
            // A block is "in" if more than half its cells belong to the layer.
            // We still pad by 1 so MS has full boundary cells.
            const int dw = (w + step - 1) / step;  // downsampled dims
            const int dh = (h + step - 1) / step;
            const int dgw = dw + 2;  // padded
            const int dgh = dh + 2;
            std::vector<uint8_t> dgrid(dgw * dgh, 0);

            for (int dy = 0; dy < dh; ++dy) {
                for (int dx = 0; dx < dw; ++dx) {
                    int count = 0, total = 0;
                    for (int sy = 0; sy < step; ++sy) {
                        for (int sx = 0; sx < step; ++sx) {
                            int wx = dx * step + sx;
                            int wy = dy * step + sy;

                            // wrap X instead of skipping
                            wx = wx % w;

                            if (wy >= h) continue;

                            ++total;
                            if (sample(wx, wy)) ++count;
                        }
                    }
                    dgrid[(dy + 1) * dgw + (dx + 1)] =
                        (total > 0 && count * 2 >= total) ? 1 : 0;
                }
            }

            // Fill left border (gx=0) from the rightmost interior column
            // (gx=dgw-2) and right border (gx=dgw-1) from the leftmost interior
            // column (gx=1)
            for (int gy = 0; gy < dgh; ++gy) {
                dgrid[gy * dgw + 0] =
                    dgrid[gy * dgw + (dgw - 2)];  // left pad = right edge
                dgrid[gy * dgw + (dgw - 1)] =
                    dgrid[gy * dgw + 1];  // right pad = left edge
            }

            // Run the same MS pass on the downsampled grid.
            // Coordinates are in downsampled space; scale back up by step
            // so they align with the world-space LOD0 mesh.
            std::vector<Vec2> lodTris;
            lodTris.reserve(dw * dh * 2);

            for (int gy = 0; gy < dgh - 1; ++gy) {
                for (int gx = 0; gx < dgw - 1; ++gx) {
                    uint8_t tl = dgrid[(gy)*dgw + (gx)];
                    uint8_t tr = dgrid[(gy)*dgw + (gx + 1)];
                    uint8_t br = dgrid[(gy + 1) * dgw + (gx + 1)];
                    uint8_t bl = dgrid[(gy + 1) * dgw + (gx)];

                    int config = (tl << 3) | (tr << 2) | (br << 1) | bl;
                    if (config == 0) continue;

                    float ox = static_cast<float>((gx - 1) * step);
                    float oy = static_cast<float>((gy - 1) * step);
                    float fs = static_cast<float>(step);

                    if (config == 15) {
                        Vec2 a{ox, oy};
                        Vec2 b{ox + fs, oy};
                        Vec2 c{ox + fs, oy + fs};
                        Vec2 d{ox, oy + fs};

                        emitTri(a, b, c, lodLevel);
                        emitTri(a, c, d, lodLevel);
                        continue;
                    }

                    struct Corner {
                        Vec2 pos;
                        bool inside;
                    };
                    Corner corners[4] = {
                        {{ox, oy}, tl == 1},
                        {{ox + fs, oy}, tr == 1},
                        {{ox + fs, oy + fs}, br == 1},
                        {{ox, oy + fs}, bl == 1},
                    };

                    // Midpoints scaled to world space
                    Vec2 mids[4] = {
                        {ox + fs * 0.5f, oy},       // top
                        {ox + fs, oy + fs * 0.5f},  // right
                        {ox + fs * 0.5f, oy + fs},  // bottom
                        {ox, oy + fs * 0.5f},       // left
                    };

                    std::vector<Vec2> cellPoly;
                    if (layer == LayerType::WATER &&
                        (config == 5 || config == 10)) {
                        for (int i = 0; i < 4; ++i)
                            if (corners[i].inside)
                                emitTri(corners[i].pos, mids[i],
                                        mids[(i + 3) % 4], lodLevel);
                        continue;
                    }

                    cellPoly.reserve(6);
                    for (int ci = 0; ci < 4; ++ci) {
                        int ni = (ci + 1) % 4;
                        if (corners[ci].inside)
                            cellPoly.push_back(corners[ci].pos);
                        if (corners[ci].inside != corners[ni].inside)
                            cellPoly.push_back(mids[ci]);
                    }

                    fanTriangulateEmit(cellPoly, lodLevel);
                }
            }
        }

        world.regionMeshes[ly] = std::move(mesh);
    }
}

float WorldGenerator::sampleHeight(const Vec2 &p) {
    int x = (int)std::floor(p.x);
    int y = (int)std::floor(p.y);

    x = ((x % tempWorld.width) + tempWorld.width) % tempWorld.width;
    int xp1 = (x + 1) % tempWorld.width;
    y = std::clamp(y, 0, tempWorld.height - 2);

    // ── THIS is what was missing ──────────────────────────────
    // Bring p into the same local cell space as the corners.
    // Without this, v2 = p - a is computed across the seam.
    Vec2 pLocal = {float(x) + (p.x - std::floor(p.x)), p.y};
    // ─────────────────────────────────────────────────────────

    Vec2 p00 = {float(x), float(y)};
    Vec2 p10 = {float(xp1), float(y)};
    Vec2 p01 = {float(x), float(y + 1)};
    Vec2 p11 = {float(xp1), float(y + 1)};

    auto getH = [&](int ix, int iy) {
        return tempWorld.regions[iy * tempWorld.width + ix].height;
    };

    float h00 = getH(x, y);
    float h10 = getH(xp1, y);
    float h01 = getH(x, y + 1);
    float h11 = getH(xp1, y + 1);

    // use pLocal instead of p for all vector arithmetic
    float fx = pLocal.x - float(x);  // fractional part, always in [0,1)
    float fy = pLocal.y - float(y);

    if (fx > fy) {
        // triangle: p00, p10, p11
        Vec2 a = p00, b = p10, c = p11;
        float ha = h00, hb = h10, hc = h11;

        Vec2 v0 = b - a;
        Vec2 v1 = c - a;
        Vec2 v2 = pLocal - a;  // ← pLocal, not p

        float d00 = v0.x * v0.x + v0.y * v0.y;
        float d01 = v0.x * v1.x + v0.y * v1.y;
        float d11 = v1.x * v1.x + v1.y * v1.y;
        float d20 = v2.x * v0.x + v2.y * v0.y;
        float d21 = v2.x * v1.x + v2.y * v1.y;

        float denom = d00 * d11 - d01 * d01;
        float v = (d11 * d20 - d01 * d21) / denom;
        float w = (d00 * d21 - d01 * d20) / denom;
        float u = 1.0f - v - w;

        return u * ha + v * hb + w * hc;
    } else {
        // triangle: p00, p11, p01
        Vec2 a = p00, b = p11, c = p01;
        float ha = h00, hb = h11, hc = h01;

        Vec2 v0 = b - a;
        Vec2 v1 = c - a;
        Vec2 v2 = pLocal - a;  // ← pLocal, not p

        float d00 = v0.x * v0.x + v0.y * v0.y;
        float d01 = v0.x * v1.x + v0.y * v1.y;
        float d11 = v1.x * v1.x + v1.y * v1.y;
        float d20 = v2.x * v0.x + v2.y * v0.y;
        float d21 = v2.x * v1.x + v2.y * v1.y;

        float denom = d00 * d11 - d01 * d01;
        float v = (d11 * d20 - d01 * d21) / denom;
        float w = (d00 * d21 - d01 * d20) / denom;
        float u = 1.0f - v - w;

        return u * ha + v * hb + w * hc;
    }
}

FeatureEncoded WorldGenerator::encodeFeatures(const Region &r) {
    FeatureEncoded out{0, 0, 0};

    // ─────────────────────────────
    // BIOME (discrete primary class)
    // ─────────────────────────────
    if (r.hasFeature(FeatureMask::ICE_SHEET))
        out.biome = 250;
    else if (r.hasFeature(FeatureMask::SEA_ICE))
        out.biome = 220;
    else if (r.hasFeature(FeatureMask::DESERT_DUNES))
        out.biome = 100;
    else
        out.biome = 0;

    // ─────────────────────────────
    // MOISTURE (water presence / humidity)
    // ─────────────────────────────
    float m = r.humidity;  // assume 0..1
    out.moisture = uint8_t(m * 255.0f);

    // ─────────────────────────────
    // TEMPERATURE
    // ─────────────────────────────
    float t = r.temperature_c;

    float norm = (t - tempWorld.min_temp_c) /
                 (tempWorld.max_temp_c - tempWorld.min_temp_c);

    norm = std::clamp(norm, 0.0f, 1.0f);

    out.temperature = uint8_t(norm * 255.0f);

    return out;
}

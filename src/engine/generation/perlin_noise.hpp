#pragma once

#include <vector>
#include <numeric>
#include <random>
#include <cmath>
#include <algorithm>

class PerlinNoise
{
public:
    PerlinNoise(uint32_t seed)
    {
        p.resize(256);
        std::iota(p.begin(), p.end(), 0);

        std::default_random_engine engine(seed);
        std::shuffle(p.begin(), p.end(), engine);

        // дублируем
        p.insert(p.end(), p.begin(), p.end());
    }

    float noise(float x, float y) const
    {
        int X = (int)std::floor(x) & 255;
        int Y = (int)std::floor(y) & 255;

        x -= std::floor(x);
        y -= std::floor(y);

        float u = fade(x);
        float v = fade(y);

        int A = p[X] + Y;
        int B = p[X + 1] + Y;

        float res = lerp(v,
                         lerp(u, grad(p[A], x, y),
                              grad(p[B], x - 1, y)),
                         lerp(u, grad(p[A + 1], x, y - 1),
                              grad(p[B + 1], x - 1, y - 1)));

        return (res + 1.0f) / 2.0f; // [0,1]
    }

    float noiseTile(float x, float y, float periodX, float periodY) const
    {
        float fx = x / periodX;
        float fy = y / periodY;

        float s = std::sin(fx * 2.0f * M_PI);
        float c = std::cos(fx * 2.0f * M_PI);
        float u = std::sin(fy * 2.0f * M_PI);
        float v = std::cos(fy * 2.0f * M_PI);

        return noise(c + v, s + u);
    }

    float noiseTileable(float x, float y, float periodX, float periodY)
    {
        float nx = x / periodX;
        float ny = y / periodY;

        float u = nx * 2.0f * M_PI;
        float v = ny * 2.0f * M_PI;

        float x1 = std::cos(u);
        float y1 = std::sin(u);
        float x2 = std::cos(v);
        float y2 = std::sin(v);

        return noise(x1 + x2, y1 + y2);
    }

    float warp(float x, float y)
    {
        float qx = noise(x * 0.5f, y * 0.5f);
        float qy = noise(x * 0.5f + 100.0f, y * 0.5f + 100.0f);

        return noise(x + qx * 2.0f, y + qy * 2.0f);
    }

    float getHeight(float x, float y, float w, float h)
    {
        // --- НОРМАЛИЗАЦИЯ
        float nx = x / w - 0.5f;
        float ny = y / h - 0.5f;

        // ========================
        // 1. CONTINENTS (очень плавно)
        // ========================
        float continents = fbmTileable(x, y, w, h);
        continents = std::pow(continents, 1.5f);

        float warp1 = fbmTileable(x * 0.02f, y * 0.02f, w, h);
        float warp2 = fbmTileable(x * 0.02f + 100.0f, y * 0.02f + 100.0f, w, h);

        float wx = x + (warp1 - 0.5f) * 100.0f;
        float wy = y + (warp2 - 0.5f) * 100.0f;

        float mountains = fbmTileable(wx * 6.0f, wy * 6.0f, w, h);
        mountains = std::pow(mountains, 2.0f);

        float detail = fbmTileable(x * 20.0f, y * 20.0f, w, h) * 0.1f;

        // ========================
        // FINAL
        // ========================
        float height =
            continents * 1.0f +
            mountains * continents * 0.5f + // горы только на суше
            detail;

        return std::clamp(height, 0.0f, 1.0f);
    }

    float fbm(float x, float y, float w, float h)
    {
        float total = 0.0f;
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float maxValue = 0.0f;

        const int octaves = 10;
        const float persistence = 0.5f;

        for (int i = 0; i < octaves; i++)
        {
            total += noiseTile(x * frequency, y * frequency, w, h) * amplitude;

            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= 2.0f;
        }

        return total / maxValue;
    }

    float fbmTileable(float x, float y, float periodX, float periodY)
    {
        float total = 0.0f;
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float maxValue = 0.0f;

        const int octaves = 10;
        const float persistence = 0.5f;

        for (int i = 0; i < octaves; i++)
        {
            float nx = x * frequency;
            float ny = y * frequency;

            total += noiseTileable(nx, ny, periodX, periodY) * amplitude;

            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= 2.0f;
        }

        return total / maxValue;
    }

private:
    std::vector<int> p;

    static float fade(float t)
    {
        return t * t * t * (t * (t * 6 - 15) + 10);
    }

    static float lerp(float t, float a, float b)
    {
        return a + t * (b - a);
    }

    static float grad(int hash, float x, float y)
    {
        int h = hash & 7;
        float u = h < 4 ? x : y;
        float v = h < 4 ? y : x;
        return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
    }
};
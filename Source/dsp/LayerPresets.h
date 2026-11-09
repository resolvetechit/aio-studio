#pragma once
#include <vector>
#include <string>
#include "LayerEngine.h"

namespace aio
{
struct LayerPreset
{
    std::string name;
    float bright = 0.f, motion = 100.f, relScale = 100.f;
    LayerCfg layer[kLayers];
};

// my own sketches of useful layered sounds; the names describe the character, they are not copies of anything
inline const std::vector<LayerPreset>& layerPresets()
{
    static const std::vector<LayerPreset> list = []
    {
        using S = LayerSrc;
        auto L = [] (S src, float lvl, float pan, float coarse, float fine, bool track, float atk, float rel, float cut, float res, float mot, float drv, float lofi)
        {
            LayerCfg c; c.on = true; c.src = (int) src; c.levelDb = lvl; c.pan = pan; c.coarse = coarse; c.fine = fine; c.track = track;
            c.attackMs = atk; c.releaseMs = rel; c.cutoffHz = cut; c.reso = res; c.motion = mot; c.drive = drv; c.lofi = lofi;
            return c;
        };
        std::vector<LayerPreset> v;
        auto add = [&] (const char* name, float bright, float motion, float rel, std::initializer_list<LayerCfg> ls)
        {
            LayerPreset p; p.name = name; p.bright = bright; p.motion = motion; p.relScale = rel;
            int i = 0; for (const auto& c : ls) if (i < kLayers) p.layer[i++] = c;
            // loudness trim so that every sound lands in the same range when a chord is played
            static const struct { const char* n; float db; } trims[] = {
                { "Cinematic - Dark Impact", -8.f }, { "Hits - Heavy Boom", -9.f }, { "Drone - Deep Cave", -4.f }, { "Bass - Sub Weight", -3.f },
                { "Choir - Ghost Choir", -2.f }, { "Dark - Night Drive", -1.f }, { "Keys - Glass Bells", 3.f }, { "Keys - Music Box Lo-fi", 4.f },
                { "Cinematic - Tension Rise", 4.f }, { "Ambient - Floating", 2.f }, { "Pad - Dusk Glass", 1.5f }, { "Pad - Slow Bloom", 1.5f },
                { "Drone - Hollow Wind", 1.f }, { "Ethereal - Shimmer", 1.5f } };
            for (const auto& t : trims) if (p.name == t.n) for (auto& c : p.layer) c.levelDb += t.db;
            v.push_back (p);
        };

        add ("Pad - Dusk Glass", 0, 100, 100, {
            L (S::Pad, -9, 0, 0, 0, true, 900, 2600, 5200, 14, 40, 0, 0),
            L (S::Air, -15, 0, 12, 0, true, 1400, 3200, 6500, 8, 55, 0, 0),
            L (S::Bell, -20, 25, 12, 0, true, 5, 3000, 12000, 0, 30, 0, 0),
            L (S::Texture, -26, 0, 12, 0, true, 2500, 4000, 9000, 20, 60, 0, 0) });
        add ("Pad - Slow Bloom", 0, 110, 120, {
            L (S::Pad, -8, 0, 0, 0, true, 2400, 4200, 3200, 18, 55, 0, 0),
            L (S::Pad, -12, 0, -12, 7, true, 3000, 4200, 1800, 12, 40, 0, 0),
            L (S::Air, -18, 0, 0, 0, true, 3800, 4800, 5000, 10, 50, 0, 0) });
        add ("Drone - Deep Cave", -10, 140, 150, {
            L (S::Drone, -7, 0, -12, 0, true, 1800, 5200, 2600, 20, 60, 0, 0),
            L (S::Drone, -12, 0, 0, 5, true, 2600, 5200, 2000, 15, 70, 0, 0),
            L (S::Texture, -22, 0, -12, 0, true, 3200, 5600, 1500, 35, 80, 0, 0),
            L (S::Impact, -16, 0, -24, 0, false, 1, 6000, 500, 10, 0, 10, 0) });
        add ("Drone - Hollow Wind", 10, 160, 130, {
            L (S::Texture, -9, 0, 0, 0, true, 2200, 4600, 7000, 45, 90, 0, 0),
            L (S::Texture, -13, 0, 12, 0, true, 3000, 4600, 8000, 55, 100, 0, 0),
            L (S::Drone, -18, 0, -12, 0, true, 2000, 4600, 1800, 10, 40, 0, 0),
            L (S::Air, -22, 0, 0, 0, true, 3500, 5000, 4000, 8, 70, 0, 0) });
        add ("Cinematic - Tension Rise", 0, 150, 100, {
            L (S::Texture, -8, 0, 0, 0, true, 5200, 2600, 6500, 40, 100, 0, 0),
            L (S::Drone, -10, 0, -12, 0, true, 4600, 3000, 1800, 20, 60, 20, 0),
            L (S::Pad, -16, 0, 0, 0, true, 5600, 2400, 2400, 25, 70, 0, 0),
            L (S::Bell, -22, 20, 19, 0, true, 3000, 3600, 9000, 0, 40, 0, 0) });
        add ("Cinematic - Dark Impact", -15, 100, 100, {
            L (S::Impact, -4, 0, 0, 0, false, 1, 5000, 3200, 10, 0, 25, 0),
            L (S::Impact, -9, 0, 7, 0, false, 1, 2400, 6500, 10, 0, 15, 0),
            L (S::Drone, -14, 0, -12, 0, true, 2, 5500, 1400, 15, 50, 30, 0),
            L (S::Texture, -20, 0, -12, 0, true, 40, 4600, 1800, 30, 70, 0, 0) });
        add ("Hits - Heavy Boom", -5, 100, 100, {
            L (S::Impact, -3, 0, -7, 0, false, 1, 6500, 2600, 10, 0, 35, 0),
            L (S::Impact, -8, 0, 0, 0, false, 1, 1800, 9000, 15, 0, 25, 0),
            L (S::Texture, -22, 0, -12, 0, false, 5, 3800, 2400, 25, 40, 0, 0) });
        add ("Keys - Glass Bells", 15, 80, 100, {
            L (S::Bell, -7, -15, 0, 0, true, 2, 2400, 15000, 0, 20, 0, 0),
            L (S::Bell, -13, 15, 12, 0, true, 2, 2000, 15000, 0, 25, 0, 0),
            L (S::Pad, -22, 0, 0, 0, true, 400, 1600, 3800, 10, 30, 0, 0) });
        add ("Keys - Music Box Lo-fi", 0, 90, 100, {
            L (S::Bell, -6, 0, 12, 0, true, 2, 1600, 7000, 0, 20, 0, 55),
            L (S::Pad, -17, 0, 0, 0, true, 300, 1200, 2400, 10, 30, 0, 40),
            L (S::Texture, -28, 0, 0, 0, false, 200, 1200, 3000, 10, 40, 0, 70) });
        add ("Lo-fi - Dusty Pad", -15, 110, 100, {
            L (S::Pad, -9, 0, 0, 0, true, 600, 2200, 2800, 12, 45, 12, 30),
            L (S::Bell, -17, 0, 12, 0, true, 3, 1800, 6000, 0, 25, 0, 45),
            L (S::Texture, -24, 0, 0, 0, false, 800, 2200, 4000, 30, 40, 0, 65) });
        add ("Choir - Ghost Choir", 0, 120, 110, {
            L (S::Air, -7, -20, 0, 6, true, 1200, 3000, 7000, 8, 55, 0, 0),
            L (S::Air, -9, 20, 12, -6, true, 1600, 3000, 7000, 8, 65, 0, 0),
            L (S::Air, -14, 0, -12, 0, true, 1400, 3400, 4500, 8, 45, 0, 0),
            L (S::Pad, -22, 0, 0, 0, true, 2000, 3000, 2800, 10, 40, 0, 0) });
        add ("Ambient - Floating", 5, 130, 120, {
            L (S::Pad, -10, 0, 0, 0, true, 1500, 3800, 4200, 14, 50, 0, 0),
            L (S::Bell, -15, -20, 12, 0, true, 6, 3400, 11000, 0, 40, 0, 0),
            L (S::Texture, -20, 0, 12, 0, true, 2800, 4200, 8500, 40, 90, 0, 0),
            L (S::Drone, -18, 0, -12, 0, true, 2600, 4200, 1800, 10, 40, 0, 0) });
        add ("Dark - Night Drive", -10, 100, 100, {
            L (S::Pad, -8, 0, -12, 0, true, 40, 1100, 1500, 28, 35, 20, 0),
            L (S::Pad, -12, 0, 0, 9, true, 60, 1100, 2400, 22, 45, 12, 0),
            L (S::Drone, -16, 0, -24, 0, true, 5, 900, 700, 10, 20, 15, 0),
            L (S::Bell, -22, 15, 12, 0, true, 4, 1600, 8000, 0, 30, 0, 0) });
        add ("Ethereal - Shimmer", 25, 120, 110, {
            L (S::Bell, -9, 0, 12, 0, true, 3, 3200, 14000, 0, 30, 0, 0),
            L (S::Bell, -14, 0, 19, 0, true, 3, 2600, 14000, 0, 30, 0, 0),
            L (S::Air, -14, 0, 12, 0, true, 1500, 3000, 9000, 6, 60, 0, 0),
            L (S::Pad, -18, 0, 0, 0, true, 1200, 3400, 5000, 10, 50, 0, 0) });
        add ("Texture - Rain on Metal", 10, 140, 100, {
            L (S::Texture, -8, 0, 0, 0, true, 8, 1800, 12000, 70, 90, 0, 0),
            L (S::Texture, -12, 0, 12, 0, true, 12, 1800, 12000, 80, 100, 0, 0),
            L (S::Bell, -20, 0, 24, 0, true, 3, 1400, 12000, 0, 40, 0, 20) });
        add ("Bass - Sub Weight", -20, 60, 80, {
            L (S::Drone, -5, 0, -12, 0, true, 4, 500, 900, 8, 10, 10, 0),
            L (S::Pad, -12, 0, -12, 0, true, 8, 500, 800, 20, 20, 25, 0),
            L (S::Impact, -14, 0, -24, 0, true, 1, 700, 350, 10, 0, 20, 0) });
        return v;
    }();
    return list;
}
} // namespace aio

#pragma once
#include "Biquad.h"
#include <array>

namespace aio
{
struct EQBand
{
    bool on = false;
    FilterType type = FilterType::Bell;
    float freq = 1000.f, gainDb = 0.f, q = 1.f;

    bool operator== (const EQBand& o) const
    {
        return on == o.on && type == o.type && freq == o.freq && gainDb == o.gainDb && q == o.q;
    }
    bool isActive() const
    {
        if (! on) return false;
        const bool gainBased = (type == FilterType::Bell || type == FilterType::LowShelf || type == FilterType::HighShelf);
        return ! (gainBased && std::abs (gainDb) < 0.02f);
    }
};

// Six-band parametric EQ (bell / shelves / HP / LP / notch).
class ParametricEQ
{
public:
    static constexpr int kBands = 6;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        for (auto& b : cur) b = EQBand{};
        for (auto& b : bq) for (auto& c : b) { c.c = Coeffs{}; c.reset(); }
    }

    void reset() { for (auto& b : bq) for (auto& c : b) c.reset(); }

    void setBands (const std::array<EQBand, kBands>& nb)
    {
        for (int i = 0; i < kBands; ++i)
        {
            if (nb[(size_t) i] == cur[(size_t) i]) continue;
            cur[(size_t) i] = nb[(size_t) i];
            const auto c = Coeffs::make (nb[(size_t) i].type, sr, nb[(size_t) i].freq, nb[(size_t) i].gainDb, nb[(size_t) i].q);
            bq[(size_t) i][0].c = bq[(size_t) i][1].c = c;
        }
    }

    void process (float* l, float* r, int n)
    {
        for (int b = 0; b < kBands; ++b)
        {
            if (! cur[(size_t) b].isActive()) continue;
            auto& fl = bq[(size_t) b][0];
            auto& fr = bq[(size_t) b][1];
            for (int i = 0; i < n; ++i)
            {
                l[i] = fl.tick (l[i]);
                r[i] = fr.tick (r[i]);
            }
        }
    }

    // Combined magnitude response in dB (used by the UI curve).
    static double responseDb (const std::array<EQBand, kBands>& bands, double sr, double f)
    {
        double db = 0;
        for (auto& b : bands)
            if (b.isActive())
                db += gainToDb (Coeffs::make (b.type, sr, b.freq, b.gainDb, b.q).magnitude (f, sr));
        return db;
    }

private:
    double sr = 44100.0;
    std::array<EQBand, kBands> cur;
    Biquad bq[kBands][2];
};
} // namespace aio

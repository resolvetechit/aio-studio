#pragma once
#include <algorithm>
#include <cmath>

namespace aio
{
constexpr double kPi = 3.14159265358979323846;

enum class FilterType { Bell = 0, LowShelf, HighShelf, HighPass, LowPass, Notch, BandPass };   // BandPass: constant 0 dB peak (internal use)

// RBJ cookbook biquad coefficients (normalised, a0 == 1).
struct Coeffs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static Coeffs make (FilterType t, double sr, double f, double gainDb, double q)
    {
        f = std::clamp (f, 10.0, sr * 0.49);
        q = std::max (q, 0.05);
        const double w0 = 2.0 * kPi * f / sr, cw = std::cos (w0), sw = std::sin (w0);
        const double A = std::pow (10.0, gainDb / 40.0);
        const double alpha = sw / (2.0 * q);
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

        switch (t)
        {
            case FilterType::Bell:
                b0 = 1 + alpha * A;  b1 = -2 * cw;  b2 = 1 - alpha * A;
                a0 = 1 + alpha / A;  a1 = -2 * cw;  a2 = 1 - alpha / A;
                break;
            case FilterType::LowShelf:
            {
                const double sq = 2 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1) - (A - 1) * cw + sq);
                b1 = 2 * A * ((A - 1) - (A + 1) * cw);
                b2 = A * ((A + 1) - (A - 1) * cw - sq);
                a0 = (A + 1) + (A - 1) * cw + sq;
                a1 = -2 * ((A - 1) + (A + 1) * cw);
                a2 = (A + 1) + (A - 1) * cw - sq;
                break;
            }
            case FilterType::HighShelf:
            {
                const double sq = 2 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1) + (A - 1) * cw + sq);
                b1 = -2 * A * ((A - 1) + (A + 1) * cw);
                b2 = A * ((A + 1) + (A - 1) * cw - sq);
                a0 = (A + 1) - (A - 1) * cw + sq;
                a1 = 2 * ((A - 1) - (A + 1) * cw);
                a2 = (A + 1) - (A - 1) * cw - sq;
                break;
            }
            case FilterType::HighPass:
                b0 = (1 + cw) / 2;  b1 = -(1 + cw);  b2 = (1 + cw) / 2;
                a0 = 1 + alpha;     a1 = -2 * cw;    a2 = 1 - alpha;
                break;
            case FilterType::LowPass:
                b0 = (1 - cw) / 2;  b1 = 1 - cw;     b2 = (1 - cw) / 2;
                a0 = 1 + alpha;     a1 = -2 * cw;    a2 = 1 - alpha;
                break;
            case FilterType::Notch:
                b0 = 1;             b1 = -2 * cw;    b2 = 1;
                a0 = 1 + alpha;     a1 = -2 * cw;    a2 = 1 - alpha;
                break;
            case FilterType::BandPass:
                b0 = alpha;         b1 = 0;          b2 = -alpha;
                a0 = 1 + alpha;     a1 = -2 * cw;    a2 = 1 - alpha;
                break;
        }
        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    // |H(e^jw)| in linear gain
    double magnitude (double f, double sr) const
    {
        const double w = 2.0 * kPi * f / sr;
        const double c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2 * w), s2 = std::sin (2 * w);
        const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
        const double dr = 1 + a1 * c1 + a2 * c2,  di = -(a1 * s1 + a2 * s2);
        const double num = nr * nr + ni * ni, den = dr * dr + di * di;
        return den > 0 ? std::sqrt (num / den) : 1.0;
    }
};

// Transposed direct form II, double precision state.
struct Biquad
{
    Coeffs c;
    double z1 = 0, z2 = 0;

    void reset() noexcept { z1 = z2 = 0; }

    inline float tick (float xf) noexcept
    {
        const double x = xf;
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return (float) y;
    }
};

inline double gainToDb (double g) { return 20.0 * std::log10 (std::max (g, 1.0e-9)); }
inline float dbToGain (float db)  { return std::pow (10.0f, db * 0.05f); }
} // namespace aio

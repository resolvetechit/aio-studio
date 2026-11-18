#include "Podcast.h"
#include "../dsp/AutoVoice.h"
#include "../dsp/Loudness.h"
#include "../dsp/Limiter.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace aio
{
namespace podcast
{
namespace
{
constexpr double kPiD = 3.14159265358979323846;

inline double toDb (double lin) { return 20.0 * std::log10 (std::max (lin, 1.0e-9)); }

struct Run { size_t a = 0, b = 0; int sign = 1; float level = 1.f; };     // [a, b) of clipped samples

// ---------------------------------------------------------------------------------------------------------------
// Clipping detector: a flat top is a run of >= 2 samples sitting on (nearly) the extreme value of the file. We need
// at least five such runs before believing it, so a single loud peak is never mistaken for clipping.
long long findRuns (const float* x, size_t n, std::vector<Run>* out)
{
    float hi = 0.f, lo = 0.f;
    for (size_t i = 0; i < n; ++i) { hi = std::max (hi, x[i]); lo = std::min (lo, x[i]); }
    long long total = 0;
    for (int sign = 1; sign >= -1; sign -= 2)
    {
        const float level = sign > 0 ? hi : -lo;
        if (level < 0.1f) continue;
        const float thr = level - std::max (level * 0.002f, 2.0e-5f);
        constexpr float flat = 6.0e-5f;           // a clipped top is flat to within a couple of 16-bit steps
        std::vector<Run> runs;
        long long cnt = 0;
        size_t i = 0;
        while (i < n)
        {
            const bool on = sign > 0 ? x[i] >= thr : x[i] <= -thr;
            if (! on) { ++i; continue; }
            size_t j = i + 1;
            while (j < n && (sign > 0 ? x[j] >= thr : x[j] <= -thr) && std::abs (x[j] - x[j - 1]) <= flat) ++j;
            if (j - i >= 2) { runs.push_back ({ i, j, sign, level }); cnt += (long long) (j - i); }
            i = j;
        }
        if (runs.size() < 5) continue;
        total += cnt;
        if (out) out->insert (out->end(), runs.begin(), runs.end());
    }
    if (out) std::sort (out->begin(), out->end(), [] (const Run& p, const Run& q) { return p.a < q.a; });
    return total;
}

// Levinson-Durbin: a[0] = 1, e_t = sum_k a[k] x[t-k]
void levinson (const std::vector<double>& r, int p, std::vector<double>& a)
{
    a.assign ((size_t) p + 1, 0.0);
    a[0] = 1.0;
    double err = r[0];
    std::vector<double> tmp ((size_t) p + 1);
    for (int i = 1; i <= p; ++i)
    {
        double acc = r[(size_t) i];
        for (int j = 1; j < i; ++j) acc += a[(size_t) j] * r[(size_t) (i - j)];
        const double k = -acc / std::max (err, 1.0e-30);
        tmp = a;
        for (int j = 1; j < i; ++j) a[(size_t) j] = tmp[(size_t) j] + k * tmp[(size_t) (i - j)];
        a[(size_t) i] = k;
        err *= (1.0 - k * k);
        if (err <= 1.0e-30) break;
    }
}

// Rebuilds the samples in `unk` (window-relative indices) by choosing the values that minimise the prediction-error
// energy of an autoregressive model fitted to the surrounding audio (Janssen's least-squares interpolation).
bool solveUnknowns (std::vector<double>& y, const std::vector<int>& unk, const std::vector<double>& a, int p)
{
    const int m = (int) unk.size();
    const int W = (int) y.size();
    // y holds the current estimate with unknowns set to 0; e[t] = sum a[k] y[t-k]
    std::vector<double> e ((size_t) W, 0.0);
    for (int t = p; t < W; ++t)
    {
        double s = 0.0;
        for (int k = 0; k <= p; ++k) s += a[(size_t) k] * y[(size_t) (t - k)];
        e[(size_t) t] = s;
    }
    std::vector<double> ra ((size_t) p + 1, 0.0);
    for (int d = 0; d <= p; ++d)
        for (int k = 0; k + d <= p; ++k) ra[(size_t) d] += a[(size_t) k] * a[(size_t) (k + d)];

    std::vector<double> G ((size_t) m * (size_t) m, 0.0), b ((size_t) m, 0.0);
    for (int i = 0; i < m; ++i)
    {
        const int ui = unk[(size_t) i];
        double s = 0.0;
        for (int t = ui; t <= ui + p && t < W; ++t) s += e[(size_t) t] * a[(size_t) (t - ui)];
        b[(size_t) i] = -s;
        for (int j = 0; j <= i; ++j)
        {
            const int d = std::abs (ui - unk[(size_t) j]);
            G[(size_t) i * (size_t) m + (size_t) j] = d <= p ? ra[(size_t) d] : 0.0;
        }
    }
    // Cholesky (lower triangle)
    const double ridge = 1.0e-9 * std::max (G[0], 1.0e-12);
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j <= i; ++j)
        {
            double s = G[(size_t) i * (size_t) m + (size_t) j];
            for (int k = 0; k < j; ++k) s -= G[(size_t) i * (size_t) m + (size_t) k] * G[(size_t) j * (size_t) m + (size_t) k];
            if (i == j)
            {
                s += ridge;
                if (s <= 1.0e-18) return false;
                G[(size_t) i * (size_t) m + (size_t) i] = std::sqrt (s);
            }
            else G[(size_t) i * (size_t) m + (size_t) j] = s / G[(size_t) j * (size_t) m + (size_t) j];
        }
    }
    std::vector<double> z ((size_t) m);
    for (int i = 0; i < m; ++i)
    {
        double s = b[(size_t) i];
        for (int k = 0; k < i; ++k) s -= G[(size_t) i * (size_t) m + (size_t) k] * z[(size_t) k];
        z[(size_t) i] = s / G[(size_t) i * (size_t) m + (size_t) i];
    }
    for (int i = m - 1; i >= 0; --i)
    {
        double s = z[(size_t) i];
        for (int k = i + 1; k < m; ++k) s -= G[(size_t) k * (size_t) m + (size_t) i] * z[(size_t) k];
        z[(size_t) i] = s / G[(size_t) i * (size_t) m + (size_t) i];
    }
    for (int i = 0; i < m; ++i) y[(size_t) unk[(size_t) i]] = z[(size_t) i];
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
double noiseFloorOf (const float* l, const float* r, size_t n, double sr)
{
    const size_t blk = std::max<size_t> (64, (size_t) (sr * 0.05));
    std::vector<float> v;
    v.reserve (n / blk + 1);
    for (size_t a = 0; a + blk <= n; a += blk)
    {
        double s = 0.0;
        for (size_t i = a; i < a + blk; ++i) { const double m = 0.5 * ((double) l[i] + (double) r[i]); s += m * m; }
        const double rms = std::sqrt (s / (double) blk);
        if (rms > 3.0e-5) v.push_back ((float) rms);        // skip digital silence
    }
    if (v.size() < 4) return -200.0;
    std::sort (v.begin(), v.end());
    return toDb ((double) v[(size_t) ((double) (v.size() - 1) * 0.10)]);
}
} // namespace

long long countClipped (const float* x, size_t n) { return findRuns (x, n, nullptr); }

// ---------------------------------------------------------------------------------------------------------------
long long declipChannel (float* x, size_t n)
{
    std::vector<Run> runs;
    findRuns (x, n, &runs);
    if (runs.empty()) return 0;

    constexpr int p = 32;
    constexpr int ctx = 320;
    constexpr size_t maxRun = 400;          // longer than ~9 ms at 44.1 kHz is not clipping we can rebuild
    constexpr size_t joinGap = 12;
    long long fixed = 0;
    std::vector<double> y, w, a, r ((size_t) p + 1);

    size_t ri = 0;
    while (ri < runs.size())
    {
        // group runs that sit close together into one cluster
        size_t rj = ri + 1;
        while (rj < runs.size() && runs[rj].a - runs[rj - 1].b < joinGap) ++rj;
        const size_t first = runs[ri].a, last = runs[rj - 1].b;
        std::vector<int> unkAbs;
        bool tooLong = false;
        for (size_t k = ri; k < rj; ++k)
        {
            if (runs[k].b - runs[k].a > maxRun) tooLong = true;
            for (size_t i = runs[k].a; i < runs[k].b; ++i) unkAbs.push_back ((int) i);
        }
        const int cs = std::max (0, (int) first - ctx), ce = std::min ((int) n, (int) last + ctx);
        const int W = ce - cs;
        const bool edgeOk = (int) first - cs >= p && ce - (int) last >= p + 1;
        if (tooLong || unkAbs.size() > 600 || ! edgeOk || W < 4 * p) { ri = rj; continue; }

        std::vector<int> unk (unkAbs.size());
        for (size_t i = 0; i < unkAbs.size(); ++i) unk[i] = unkAbs[i] - cs;
        std::vector<bool> isUnk ((size_t) W, false);
        for (int u : unk) isUnk[(size_t) u] = true;

        // start from a straight line across each gap
        y.assign ((size_t) W, 0.0);
        for (int i = 0; i < W; ++i) y[(size_t) i] = x[cs + i];
        for (size_t k = ri; k < rj; ++k)
        {
            const int s0 = (int) runs[k].a - cs - 1, s1 = (int) runs[k].b - cs;
            if (s0 < 0 || s1 >= W) continue;
            for (int i = s0 + 1; i < s1; ++i)
                y[(size_t) i] = y[(size_t) s0] + (y[(size_t) s1] - y[(size_t) s0]) * (double) (i - s0) / (double) (s1 - s0);
        }

        bool ok = true;
        for (int pass = 0; pass < 2 && ok; ++pass)
        {
            // AR model from the (current) window
            w.assign ((size_t) W, 0.0);
            for (int i = 0; i < W; ++i) w[(size_t) i] = y[(size_t) i] * (0.5 - 0.5 * std::cos (2.0 * kPiD * ((double) i + 0.5) / (double) W));
            for (int k = 0; k <= p; ++k)
            {
                double s = 0.0;
                for (int i = k; i < W; ++i) s += w[(size_t) i] * w[(size_t) (i - k)];
                r[(size_t) k] = s;
            }
            if (r[0] < 1.0e-12) { ok = false; break; }
            r[0] *= 1.0 + 1.0e-6;
            levinson (r, p, a);

            std::vector<double> yz = y;
            for (int u : unk) yz[(size_t) u] = 0.0;
            if (! solveUnknowns (yz, unk, a, p)) { ok = false; break; }
            for (size_t k = 0; k < unk.size(); ++k)
            {
                const Run* rr = nullptr;
                for (size_t q = ri; q < rj; ++q) if (unkAbs[k] >= (int) runs[q].a && unkAbs[k] < (int) runs[q].b) { rr = &runs[q]; break; }
                double v = yz[(size_t) unk[k]];
                const double lv = rr != nullptr ? (double) rr->level : 1.0;
                if (rr != nullptr)
                {
                    // the true waveform went at least as far as the flat top, and not absurdly beyond it
                    if (rr->sign > 0) v = std::clamp (v, lv, 3.0 * lv); else v = std::clamp (v, -3.0 * lv, -lv);
                }
                y[(size_t) unk[k]] = v;
            }
        }
        if (ok)
        {
            for (size_t k = 0; k < unk.size(); ++k) x[unkAbs[k]] = (float) y[(size_t) unk[k]];
            fixed += (long long) unk.size();
        }
        ri = rj;
    }
    return fixed;
}

// ---------------------------------------------------------------------------------------------------------------
// Spectral Wiener denoiser. The noise profile is learned from the quietest stretches of the file itself; the gain is a
// decision-directed Wiener filter (smooth, far less "watery" than plain spectral subtraction). Gain is computed from the
// mid signal and applied to both channels so the stereo image can't wobble.
bool denoise (float* l, float* r, size_t n, double sr, float amount, bool& eased, const Progress& prog)
{
    eased = false;
    (void) sr;
    if (amount < 0.01f || n < 8192) return true;
    constexpr int order = 11, N = 1 << order, H = N / 4, B = N / 2 + 1;
    const bool stereo = r != nullptr && r != l;
    juce::dsp::FFT fft (order);

    std::vector<float> win ((size_t) N);
    for (int i = 0; i < N; ++i) win[(size_t) i] = (float) std::sin (kPiD * ((double) i + 0.5) / (double) N);

    const long long frames = ((long long) n + N - 1) / H + 1;       // frame k starts at k*H - (N - H)
    auto startOf = [&] (long long k) { return k * (long long) H - (long long) (N - H); };

    std::vector<float> fx ((size_t) 2 * N), fy ((size_t) 2 * N);
    auto loadFrame = [&] (const float* src, long long s0, std::vector<float>& dst)
    {
        for (int i = 0; i < N; ++i)
        {
            const long long idx = s0 + i;
            dst[(size_t) 2 * (size_t) i] = (idx >= 0 && idx < (long long) n) ? src[idx] * win[(size_t) i] : 0.f;
            dst[(size_t) 2 * (size_t) i + 1] = 0.f;
        }
    };

    // ---- 1. frame energies (mid), pick the quietest ones as the noise profile
    std::vector<float> en ((size_t) frames, 0.f);
    for (long long k = 0; k < frames; ++k)
    {
        const long long s0 = startOf (k);
        double s = 0.0;
        for (int i = 0; i < N; i += 2)
        {
            const long long idx = s0 + i;
            if (idx < 0 || idx >= (long long) n) continue;
            const double m = stereo ? 0.5 * ((double) l[idx] + (double) r[idx]) : (double) l[idx];
            s += m * m;
        }
        en[(size_t) k] = (float) (s / (double) (N / 2));
    }
    std::vector<size_t> cand;
    for (size_t k = 0; k < en.size(); ++k)
    {
        const long long s0 = startOf ((long long) k);
        if (s0 < 0 || s0 + N > (long long) n) continue;                 // edge frames are mostly zero padding
        if (en[k] > 1.0e-9f) cand.push_back (k);                        // skip digital silence
    }
    if (cand.size() < 16) return true;
    std::sort (cand.begin(), cand.end(), [&] (size_t p, size_t q) { return en[p] < en[q]; });
    const size_t nq = std::clamp<size_t> (cand.size() * 12 / 100, 8, 1200);
    const double quietE = en[cand[nq / 2]], loudE = en[cand[std::min (cand.size() - 1, cand.size() * 95 / 100)]];
    const double contrastDb = 10.0 * std::log10 (std::max (loudE, 1.0e-12) / std::max (quietE, 1.0e-12));
    // Without real pauses the "quietest" frames are speech, so the profile would be speech and the filter would eat the
    // voice. Back off smoothly: below 6 dB of contrast do nothing, above 14 dB use the full amount.
    float amt = amount;
    if (contrastDb < 14.0)
    {
        eased = true;
        amt *= (float) std::clamp ((contrastDb - 6.0) / 8.0, 0.0, 1.0);
        if (amt < 0.01f) return true;
    }

    std::vector<double> Pn ((size_t) B, 0.0);
    for (size_t q = 0; q < nq; ++q)
    {
        const long long s0 = startOf ((long long) cand[q]);
        if (stereo) { loadFrame (l, s0, fx); loadFrame (r, s0, fy); for (int i = 0; i < N; ++i) fx[(size_t) 2 * i] = 0.5f * (fx[(size_t) 2 * i] + fy[(size_t) 2 * i]); }
        else loadFrame (l, s0, fx);
        fft.performRealOnlyForwardTransform (fx.data(), true);
        for (int k = 0; k < B; ++k) Pn[(size_t) k] += (double) fx[(size_t) 2 * k] * fx[(size_t) 2 * k] + (double) fx[(size_t) 2 * k + 1] * fx[(size_t) 2 * k + 1];
    }
    for (auto& v : Pn) v /= (double) nq;
    // smooth the profile across frequency so one lucky bin can't punch a hole
    {
        std::vector<double> t = Pn;
        for (int k = 0; k < B; ++k)
        {
            double s = 0.0; int c = 0;
            for (int j = std::max (0, k - 3); j <= std::min (B - 1, k + 3); ++j) { s += t[(size_t) j]; ++c; }
            Pn[(size_t) k] = s / (double) c;
        }
    }

    // ---- 2. filter
    const double over = 1.0 + 1.2 * (double) amt;                              // how much noise we assume is there (1x .. 2.2x)
    const float floorG = (float) std::pow (10.0, -(5.0 + 20.0 * (double) amt) / 20.0);       // max reduction 5 .. 25 dB
    const double beta = 0.97;
    std::vector<float> gPrev ((size_t) B, 1.f), gSm ((size_t) B, 1.f), gPre ((size_t) B, 1.f);
    std::vector<double> gamPrev ((size_t) B, 1.0);
    std::vector<float> outL (n, 0.f), outR (stereo ? n : 0, 0.f);
    const float olaScale = 1.0f;                                                // sine^2 at 75 % overlap sums to 2, and JUCE's real FFT pair has a gain of 0.5

    for (long long k = 0; k < frames; ++k)
    {
        if ((k & 255) == 0 && prog && ! prog ((float) k / (float) frames)) return false;
        const long long s0 = startOf (k);
        loadFrame (l, s0, fx);
        if (stereo) loadFrame (r, s0, fy);
        fft.performRealOnlyForwardTransform (fx.data(), true);
        if (stereo) fft.performRealOnlyForwardTransform (fy.data(), true);

        for (int b = 0; b < B; ++b)
        {
            double re = fx[(size_t) 2 * b], im = fx[(size_t) 2 * b + 1];
            if (stereo) { re = 0.5 * (re + fy[(size_t) 2 * b]); im = 0.5 * (im + fy[(size_t) 2 * b + 1]); }
            const double pn = Pn[(size_t) b] * over + 1.0e-14;
            const double gam = (re * re + im * im) / pn;
            const double xi = beta * (double) gPrev[(size_t) b] * (double) gPrev[(size_t) b] * gamPrev[(size_t) b] + (1.0 - beta) * std::max (gam - 1.0, 0.0);
            const double g = xi / (1.0 + xi);
            gPrev[(size_t) b] = (float) g;
            gamPrev[(size_t) b] = gam;
            gPre[(size_t) b] = (float) g;
        }
        for (int b = 0; b < B; ++b)
        {
            const float a0 = gPre[(size_t) std::max (0, b - 1)], a1 = gPre[(size_t) b], a2 = gPre[(size_t) std::min (B - 1, b + 1)];
            gSm[(size_t) b] = std::max (floorG, 0.25f * a0 + 0.5f * a1 + 0.25f * a2);
        }

        auto finish = [&] (std::vector<float>& d, std::vector<float>& out)
        {
            for (int b = 0; b < B; ++b)
            {
                d[(size_t) 2 * b] *= gSm[(size_t) b]; d[(size_t) 2 * b + 1] *= gSm[(size_t) b];
                if (b > 0 && b < N / 2) { d[(size_t) 2 * (N - b)] = d[(size_t) 2 * b]; d[(size_t) 2 * (N - b) + 1] = -d[(size_t) 2 * b + 1]; }
            }
            d[1] = 0.f; d[(size_t) 2 * (N / 2) + 1] = 0.f;
            fft.performRealOnlyInverseTransform (d.data());
            for (int i = 0; i < N; ++i)
            {
                const long long idx = s0 + i;
                if (idx >= 0 && idx < (long long) n) out[(size_t) idx] += d[(size_t) 2 * i] * win[(size_t) i] * olaScale;
            }
        };
        finish (fx, outL);
        if (stereo) finish (fy, outR);
    }
    std::copy (outL.begin(), outL.end(), l);
    if (stereo) std::copy (outR.begin(), outR.end(), r);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
Stats measure (const float* l, const float* r, size_t n, double sr)
{
    Stats s;
    if (n < 2048) return s;
    LoudnessMeter lm; lm.prepare (sr);
    TruePeak tl, tr;
    constexpr size_t chunk = 1 << 14;
    for (size_t a = 0; a < n; a += chunk)
    {
        const int m = (int) std::min (chunk, n - a);
        lm.push (l + a, r + a, m);
        for (int i = 0; i < m; ++i) { tl.tick (l[a + (size_t) i]); tr.tick (r[a + (size_t) i]); }
    }
    s.lufs = lm.integrated();
    s.truePeakDb = toDb ((double) std::max (tl.truePeak(), tr.truePeak()));
    // spread among the spoken blocks only
    {
        const double gate = lufsToPower (s.lufs - 12.0);
        std::vector<float> v;
        for (float p : lm.blockPowers()) if (p > gate) v.push_back (p);
        if (v.size() > 8)
        {
            std::sort (v.begin(), v.end());
            const auto at = [&] (double q) { return powerToLufs (v[(size_t) (q * (double) (v.size() - 1) + 0.5)]); };
            s.spreadDb = at (0.95) - at (0.10);
        }
    }
    s.noiseFloorDb = noiseFloorOf (l, r, n, sr);
    s.snrDb = s.noiseFloorDb > -150.0 ? std::max (0.0, s.lufs - s.noiseFloorDb) : 0.0;
    s.clipped = std::max (countClipped (l, n), countClipped (r, n));
    s.valid = s.lufs > -69.0;
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
namespace
{
// Runs the limiter over the whole buffer (scaled by `gain`). If `dst` is null this only measures.
void limitPass (const juce::AudioBuffer<float>& src, double sr, double gain, float ceilingDb, juce::AudioBuffer<float>* dst,
                double& lufs, double& tpDb, double& maxGrDb)
{
    const int nch = src.getNumChannels();
    const size_t n = (size_t) src.getNumSamples();
    LookaheadLimiter lim; lim.prepare (sr);
    const int la = lim.getLatency();
    LoudnessMeter lm; lm.prepare (sr);
    TruePeak tl, tr;
    constexpr int chunk = 1 << 13;
    std::vector<float> a ((size_t) chunk), b ((size_t) chunk);
    maxGrDb = 0.0;
    const size_t total = n + (size_t) la;
    for (size_t pos = 0; pos < total; pos += chunk)
    {
        const int m = (int) std::min<size_t> (chunk, total - pos);
        for (int i = 0; i < m; ++i)
        {
            const size_t idx = pos + (size_t) i;
            a[(size_t) i] = idx < n ? (float) (src.getSample (0, (int) idx) * gain) : 0.f;
            b[(size_t) i] = idx < n ? (float) (src.getSample (nch > 1 ? 1 : 0, (int) idx) * gain) : 0.f;
        }
        lim.process (a.data(), b.data(), m, ceilingDb);
        maxGrDb = std::max (maxGrDb, (double) lim.grMeter.load());
        // output sample j of this chunk corresponds to input index pos + j - la
        int j0 = 0;
        if (pos < (size_t) la) j0 = (int) std::min<size_t> ((size_t) m, (size_t) la - pos);
        if (j0 < m)
        {
            lm.push (a.data() + j0, b.data() + j0, m - j0);
            for (int j = j0; j < m; ++j) { tl.tick (a[(size_t) j]); tr.tick (b[(size_t) j]); }
            if (dst != nullptr)
                for (int j = j0; j < m; ++j)
                {
                    const int idx = (int) (pos + (size_t) j) - la;
                    dst->setSample (0, idx, a[(size_t) j]);
                    if (dst->getNumChannels() > 1) dst->setSample (1, idx, b[(size_t) j]);
                }
        }
    }
    lufs = lm.integrated();
    tpDb = toDb ((double) std::max (tl.truePeak(), tr.truePeak()));
}
} // namespace

bool process (juce::AudioBuffer<float>& buf, double sr, const Options& o, Report& rep, const Progress& prog)
{
    rep = {};
    const size_t n = (size_t) buf.getNumSamples();
    const int nch = buf.getNumChannels();
    if (n < 4096 || nch < 1) return false;
    auto stage = [&] (float lo, float hi) { return [&prog, lo, hi] (float x) { return ! prog || prog (lo + (hi - lo) * x); }; };

    auto chanR = [&] { return buf.getWritePointer (nch > 1 ? 1 : 0); };

    // measure what we were given
    {
        std::vector<float> dummy;
        rep.before = measure (buf.getReadPointer (0), buf.getReadPointer (nch > 1 ? 1 : 0), n, sr);
        if (! rep.before.valid) return false;
    }
    if (prog && ! prog (0.04f)) return false;

    // mono fold
    if (o.mono && nch > 1)
    {
        float* l = buf.getWritePointer (0); float* r = buf.getWritePointer (1);
        for (size_t i = 0; i < n; ++i) { const float m = 0.5f * (l[i] + r[i]); l[i] = r[i] = m; }
    }

    // 1. declip
    if (o.declip)
    {
        for (int c = 0; c < nch; ++c) rep.declipped += declipChannel (buf.getWritePointer (c), n);
        if (prog && ! prog (0.10f)) return false;
    }

    // 2. denoise
    if (o.denoise > 0.01f)
    {
        const bool sameCh = o.mono || nch == 1;
        bool eased = false;
        const bool ok = denoise (buf.getWritePointer (0), sameCh ? nullptr : chanR(), n, sr, o.denoise, eased, stage (0.10f, 0.50f));
        if (! ok) return false;
        if (o.mono && nch > 1) buf.copyFrom (1, 0, buf, 0, 0, (int) n);
        rep.denoised = true; rep.denoiseEased = eased;
    }
    if (prog && ! prog (0.50f)) return false;

    // 3. Auto Voice (speech mode): leveler, de-esser, tone, compression
    {
        AutoVoice av; av.prepare (sr);
        VoxSettings s;
        s.on = true; s.mode = VoxSpeech;
        s.level = o.level; s.tone = o.tone; s.deess = o.deess; s.comp = o.comp;
        s.clean = 0.25f * o.denoise; s.targetDb = -20.f;
        constexpr int chunk = 4096;
        std::vector<float> scratch ((size_t) chunk);
        float* l = buf.getWritePointer (0);
        float* r = nch > 1 ? buf.getWritePointer (1) : nullptr;
        for (size_t pos = 0; pos < n; pos += chunk)
        {
            const int m = (int) std::min<size_t> (chunk, n - pos);
            if (r != nullptr) av.process (l + pos, r + pos, m, s);
            else { std::copy (l + pos, l + pos + m, scratch.begin()); av.process (l + pos, scratch.data(), m, s); }
            if (((pos / chunk) & 63) == 0 && prog && ! prog (0.50f + 0.20f * (float) pos / (float) n)) return false;
        }
    }

    // 4. loudness + limiter
    const Stats levelled = measure (buf.getReadPointer (0), buf.getReadPointer (nch > 1 ? 1 : 0), n, sr);
    if (! levelled.valid) return false;
    double gainDb = std::clamp ((double) o.targetLufs - levelled.lufs, -30.0, 40.0);
    float ceilUsed = o.ceilingDb;
    double lufs = 0, tp = 0, gr = 0;
    for (int it = 0; it < 5; ++it)
    {
        limitPass (buf, sr, std::pow (10.0, gainDb / 20.0), ceilUsed, nullptr, lufs, tp, gr);
        if (prog && ! prog (0.70f + 0.04f * (float) (it + 1))) return false;
        const double err = (double) o.targetLufs - lufs;
        const double over = tp - (double) o.ceilingDb;
        if (over > 0.05) ceilUsed -= (float) (over + 0.02);
        if (std::abs (err) < 0.15 && over <= 0.05) break;
        if (std::abs (err) >= 0.15)
        {
            // more gain only helps while the limiter isn't already holding the peaks down; ease the step when it is
            gainDb += err * (gr > 1.0 ? 0.8 : 1.0);
            if (gainDb > 45.0) break;
        }
    }
    limitPass (buf, sr, std::pow (10.0, gainDb / 20.0), ceilUsed, &buf, lufs, tp, gr);
    rep.gainDb = gainDb;
    rep.limiterPeakGrDb = gr;
    if (prog && ! prog (0.94f)) return false;

    rep.after = measure (buf.getReadPointer (0), buf.getReadPointer (nch > 1 ? 1 : 0), n, sr);
    rep.reachedTarget = std::abs (rep.after.lufs - (double) o.targetLufs) < 0.6;
    return true;
}
} // namespace podcast
} // namespace aio

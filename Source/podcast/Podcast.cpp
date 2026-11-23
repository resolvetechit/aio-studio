#include "Podcast.h"
#include "../dsp/AutoVoice.h"
#include "../dsp/Loudness.h"
#include "../dsp/Limiter.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <cstring>

namespace aio
{
namespace podcast
{
bool MemStore::read (float* l, float* r, long long pos, int n)
{
    for (int i = 0; i < n; ++i)
    {
        const long long k = pos + i;
        const bool ok = k >= 0 && k < (long long) L.size();
        l[i] = ok ? L[(size_t) k] : 0.f;
        r[i] = ok ? R[(size_t) k] : 0.f;
    }
    return true;
}

namespace
{
constexpr double kPiD = 3.14159265358979323846;
constexpr float kFlat = 6.0e-5f;            // a clipped top is flat to within a couple of 16-bit steps

inline double toDb (double lin) { return 20.0 * std::log10 (std::max (lin, 1.0e-9)); }

// ---------------------------------------------------------------------------------------------------------------
// Clipping detector: a flat top is a run of >= 2 samples sitting on (nearly) the extreme value of the file. We need
// at least five such runs before believing it, so a single loud peak is never mistaken for clipping.
struct Run { size_t a = 0, b = 0; int sign = 1; float level = 1.f; };     // [a, b) of clipped samples

struct RunCounter
{
    float level = 0.f, thr = 0.f, prev = 0.f;
    int sign = 1;
    bool active = false;
    long long runs = 0, samples = 0;
    size_t len = 0;

    void init (float lv, int sg)
    {
        level = lv; sign = sg; active = lv >= 0.1f;
        thr = lv - std::max (lv * 0.002f, 2.0e-5f);
        runs = samples = 0; len = 0; prev = 0.f;
    }
    void flush() { if (len >= 2) { ++runs; samples += (long long) len; } len = 0; }
    void push (const float* x, int n)
    {
        if (! active) return;
        for (int i = 0; i < n; ++i)
        {
            const float v = x[i];
            const bool on = sign > 0 ? v >= thr : v <= -thr;
            if (on && (len == 0 || std::abs (v - prev) <= kFlat)) ++len;
            else { flush(); if (on) len = 1; }
            prev = v;
        }
    }
};

void decideClip (ClipInfo& ci, float hi, float lo, const RunCounter& rp, const RunCounter& rn)
{
    ci = {};
    ci.pos = rp.active && rp.runs >= 5;
    ci.neg = rn.active && rn.runs >= 5;
    ci.posLevel = hi; ci.negLevel = -lo;
    ci.on = ci.pos || ci.neg;
    ci.clipped = (ci.pos ? rp.samples : 0) + (ci.neg ? rn.samples : 0);
}

ClipInfo clipOfArray (const float* x, size_t n)
{
    float hi = 0.f, lo = 0.f;
    for (size_t i = 0; i < n; ++i) { hi = std::max (hi, x[i]); lo = std::min (lo, x[i]); }
    RunCounter rp, rn;
    rp.init (hi, 1); rn.init (-lo, -1);
    rp.push (x, (int) n); rn.push (x, (int) n);
    rp.flush(); rn.flush();
    ClipInfo ci;
    decideClip (ci, hi, lo, rp, rn);
    return ci;
}

void findRunsFixed (const float* x, size_t from, size_t to, const ClipInfo& ci, std::vector<Run>& out)
{
    out.clear();
    for (int sign = 1; sign >= -1; sign -= 2)
    {
        if (sign > 0 ? ! ci.pos : ! ci.neg) continue;
        const float level = sign > 0 ? ci.posLevel : ci.negLevel;
        const float thr = level - std::max (level * 0.002f, 2.0e-5f);
        size_t i = from;
        while (i < to)
        {
            const bool on = sign > 0 ? x[i] >= thr : x[i] <= -thr;
            if (! on) { ++i; continue; }
            size_t j = i + 1;
            while (j < to && (sign > 0 ? x[j] >= thr : x[j] <= -thr) && std::abs (x[j] - x[j - 1]) <= kFlat) ++j;
            if (j - i >= 2) out.push_back ({ i, j, sign, level });
            i = j;
        }
    }
    std::sort (out.begin(), out.end(), [] (const Run& p, const Run& q) { return p.a < q.a; });
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

constexpr int kAR = 32;               // AR model order
constexpr int kCtx = 320;             // samples of context either side of a cluster
constexpr size_t kMaxRun = 400;       // longer than ~9 ms at 44.1 kHz is not clipping we can rebuild
constexpr size_t kJoinGap = 12;       // runs closer than this are rebuilt together
constexpr size_t kGuard = (size_t) kCtx + kJoinGap + 8;

// Rebuild the clipped samples of one cluster (runs ri..rj-1) inside the buffer x[0, bufLen). Returns how many were rebuilt.
long long repairCluster (float* x, size_t bufLen, const std::vector<Run>& runs, size_t ri, size_t rj)
{
    const int p = kAR;
    const size_t first = runs[ri].a, last = runs[rj - 1].b;
    std::vector<int> unkAbs;
    for (size_t k = ri; k < rj; ++k)
    {
        if (runs[k].b - runs[k].a > kMaxRun) return 0;
        for (size_t i = runs[k].a; i < runs[k].b; ++i) unkAbs.push_back ((int) i);
    }
    const int cs = std::max (0, (int) first - kCtx), ce = std::min ((int) bufLen, (int) last + kCtx);
    const int W = ce - cs;
    const bool edgeOk = (int) first - cs >= p && ce - (int) last >= p + 1;
    if (unkAbs.size() > 600 || ! edgeOk || W < 4 * p) return 0;

    std::vector<int> unk (unkAbs.size());
    for (size_t i = 0; i < unkAbs.size(); ++i) unk[i] = unkAbs[i] - cs;

    std::vector<double> y ((size_t) W), w ((size_t) W), a, r ((size_t) p + 1);
    for (int i = 0; i < W; ++i) y[(size_t) i] = x[cs + i];
    for (size_t k = ri; k < rj; ++k)        // start from a straight line across each gap
    {
        const int s0 = (int) runs[k].a - cs - 1, s1 = (int) runs[k].b - cs;
        if (s0 < 0 || s1 >= W) continue;
        for (int i = s0 + 1; i < s1; ++i)
            y[(size_t) i] = y[(size_t) s0] + (y[(size_t) s1] - y[(size_t) s0]) * (double) (i - s0) / (double) (s1 - s0);
    }
    bool ok = true;
    for (int pass = 0; pass < 2 && ok; ++pass)
    {
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
            if (rr != nullptr)
            {
                const double lv = (double) rr->level;     // the true waveform went at least as far as the flat top, and not absurdly beyond it
                v = rr->sign > 0 ? std::clamp (v, lv, 3.0 * lv) : std::clamp (v, -3.0 * lv, -lv);
            }
            y[(size_t) unk[k]] = v;
        }
    }
    if (! ok) return 0;
    for (size_t k = 0; k < unk.size(); ++k) x[unkAbs[k]] = (float) y[(size_t) unk[k]];
    return (long long) unk.size();
}

// Streaming declipper for one channel. Samples go in with push(); once enough look-ahead has arrived to rebuild a
// cluster of clipped samples it is rebuilt and everything before it becomes "ready" to pop.
class DeclipStream
{
public:
    explicit DeclipStream (const ClipInfo& c) : ci (c) {}

    void push (const float* x, int n) { buf.insert (buf.end(), x, x + n); }
    size_t ready() const { return (size_t) (finalEnd - popped); }

    void scan (bool atEnd)
    {
        const long long bufEnd = bufStart + (long long) buf.size();
        if (! ci.on) { finalEnd = bufEnd; return; }
        const size_t from = (size_t) (finalEnd - bufStart);
        findRunsFixed (buf.data(), from, buf.size(), ci, runs);
        long long newFinal = atEnd ? bufEnd : std::max (finalEnd, bufEnd - (long long) kGuard);
        size_t ri = 0;
        while (ri < runs.size())
        {
            size_t rj = ri + 1;
            while (rj < runs.size() && runs[rj].a - runs[rj - 1].b < kJoinGap) ++rj;
            const size_t first = runs[ri].a, last = runs[rj - 1].b;
            const bool complete = atEnd || last + kGuard <= buf.size();
            if (! complete) { newFinal = std::min (newFinal, bufStart + (long long) first); break; }
            repaired += repairCluster (buf.data(), buf.size(), runs, ri, rj);
            ri = rj;
        }
        finalEnd = std::max (finalEnd, newFinal);
    }

    void pop (float* out, size_t n)
    {
        std::memcpy (out, buf.data() + (size_t) (popped - bufStart), n * sizeof (float));
        popped += (long long) n;
        const long long keepFrom = popped - kCtx - 16;
        if (keepFrom - bufStart > (1 << 16))
        {
            buf.erase (buf.begin(), buf.begin() + (long long) (keepFrom - bufStart));
            bufStart = keepFrom;
        }
    }

    long long repaired = 0;

private:
    ClipInfo ci;
    std::vector<float> buf;
    std::vector<Run> runs;
    long long bufStart = 0, finalEnd = 0, popped = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// Loudness, true peak, level spread and background level, measured in one streaming pass.
struct StatsAcc
{
    LoudnessMeter lm;
    TruePeak tl, tr;
    bool wantTp = true;
    std::vector<float> noise;
    size_t blk = 0, bcnt = 0;
    double bsum = 0.0;

    void prepare (double sr, bool tp)
    {
        lm.prepare (sr);
        wantTp = tp;
        blk = std::max<size_t> (64, (size_t) (sr * 0.05));
        noise.clear(); bsum = 0.0; bcnt = 0;
    }
    void push (const float* l, const float* r, int n)
    {
        lm.push (l, r, n);
        for (int i = 0; i < n; ++i)
        {
            if (wantTp) { tl.tick (l[i]); tr.tick (r[i]); }
            const double m = 0.5 * ((double) l[i] + (double) r[i]);
            bsum += m * m;
            if (++bcnt == blk)
            {
                const double rms = std::sqrt (bsum / (double) blk);
                if (rms > 3.0e-5) noise.push_back ((float) rms);        // skip digital silence
                bsum = 0.0; bcnt = 0;
            }
        }
    }
    Stats finish() const
    {
        Stats s;
        s.lufs = lm.integrated();
        s.truePeakDb = wantTp ? toDb ((double) std::max (tl.truePeak(), tr.truePeak())) : -200.0;
        const double gate = lufsToPower (s.lufs - 12.0);
        std::vector<float> v;
        for (float p : lm.blockPowers()) if (p > gate) v.push_back (p);
        if (v.size() > 8)
        {
            std::sort (v.begin(), v.end());
            const auto at = [&] (double q) { return powerToLufs (v[(size_t) (q * (double) (v.size() - 1) + 0.5)]); };
            s.spreadDb = at (0.95) - at (0.10);
        }
        if (noise.size() >= 4)
        {
            auto nv = noise;
            std::sort (nv.begin(), nv.end());
            s.noiseFloorDb = toDb ((double) nv[(size_t) ((double) (nv.size() - 1) * 0.10)]);
        }
        s.snrDb = s.noiseFloorDb > -150.0 ? std::max (0.0, s.lufs - s.noiseFloorDb) : 0.0;
        s.valid = s.lufs > -69.0;
        return s;
    }
};

// ---------------------------------------------------------------------------------------------------------------
constexpr int kFftOrder = 11, kN = 1 << kFftOrder, kH = kN / 4, kB = kN / 2 + 1;

std::vector<float> sineWindow()
{
    std::vector<float> w ((size_t) kN);
    for (int i = 0; i < kN; ++i) w[(size_t) i] = (float) std::sin (kPiD * ((double) i + 0.5) / (double) kN);
    return w;
}

// Streaming spectral Wiener denoiser. The noise profile comes from the quietest stretches of the file itself; the gain
// is a decision-directed Wiener filter (smooth, far less "watery" than plain spectral subtraction). The gain is computed
// from the mid signal and applied to both channels so the stereo image can't wobble.
class StftDenoiser
{
public:
    StftDenoiser (const NoiseProfile& np, float amount, bool stereoIn, bool& easedOut, float echoAmount = 0.f)
        : fft (kFftOrder), win (sineWindow()), stereo (stereoIn)
    {
        easedOut = false;
        float amt = amount;
        if (np.valid && np.contrastDb < 14.0)
        {
            // Without real pauses the "quietest" frames are speech, so the profile would be speech and the filter would eat the
            // voice. Back off smoothly: below 6 dB of contrast do nothing, above 14 dB use the full amount.
            easedOut = true;
            amt *= (float) std::clamp ((np.contrastDb - 6.0) / 8.0, 0.0, 1.0);
        }
        const bool noiseOn = np.valid && amt >= 0.01f && (int) np.psd.size() == kB;
        gammaLate = 0.55 * (double) std::clamp (echoAmount, 0.f, 1.f);
        on = noiseOn || gammaLate > 0.005;
        if (! on) return;
        if (noiseOn) psd = np.psd; else psd.assign ((size_t) kB, 0.0);
        if (! noiseOn) amt = 0.f;
        over = 1.0 + 1.2 * (double) amt;                                                  // how much noise we assume is there (1x .. 2.2x)
        floorG = (float) std::pow (10.0, -(5.0 + 20.0 * std::max ((double) amt, 0.55 * (double) echoAmount)) / 20.0);   // max reduction 5 .. 25 dB
        ypPast.assign ((size_t) (kLateD + 1) * kB, 0.f); ypPrev.assign ((size_t) kB, 0.f);
        frL.assign ((size_t) kN, 0.f); frR.assign ((size_t) kN, 0.f);
        accL.assign ((size_t) kN, 0.f); accR.assign ((size_t) kN, 0.f);
        fx.assign ((size_t) 2 * kN, 0.f); fy.assign ((size_t) 2 * kN, 0.f);
        gPrev.assign ((size_t) kB, 1.f); gSm.assign ((size_t) kB, 1.f); gPre.assign ((size_t) kB, 1.f);
        gamPrev.assign ((size_t) kB, 1.0);
        fill = kN - kH; skip = kN - kH;
    }

    bool active() const { return on; }

    void process (const float* l, const float* r, int n, std::vector<float>& oL, std::vector<float>& oR)
    {
        for (int i = 0; i < n; ++i) { feed (l[i], stereo ? r[i] : 0.f, oL, oR); ++inCount; }
    }

    void finish (std::vector<float>& oL, std::vector<float>& oR)
    {
        const long long rem = inCount % kH;
        const long long zeros = ((kH - rem) % kH) + 3 * kH;
        for (long long i = 0; i < zeros; ++i) feed (0.f, 0.f, oL, oR);
    }

private:
    void feed (float l, float r, std::vector<float>& oL, std::vector<float>& oR)
    {
        frL[(size_t) fill] = l; frR[(size_t) fill] = r;
        if (++fill < kN) return;
        runFrame (oL, oR);
        std::memmove (frL.data(), frL.data() + kH, (size_t) (kN - kH) * sizeof (float));
        std::memmove (frR.data(), frR.data() + kH, (size_t) (kN - kH) * sizeof (float));
        fill = kN - kH;
    }

    void runFrame (std::vector<float>& oL, std::vector<float>& oR)
    {
        for (int i = 0; i < kN; ++i)
        {
            fx[(size_t) i] = frL[(size_t) i] * win[(size_t) i];           // JUCE wants the N real samples contiguously in the first half
            if (stereo) fy[(size_t) i] = frR[(size_t) i] * win[(size_t) i];
        }
        std::fill (fx.begin() + kN, fx.end(), 0.f);
        if (stereo) std::fill (fy.begin() + kN, fy.end(), 0.f);
        fft.performRealOnlyForwardTransform (fx.data(), true);
        if (stereo) fft.performRealOnlyForwardTransform (fy.data(), true);

        for (int b = 0; b < kB; ++b)
        {
            double re = fx[(size_t) 2 * b], im = fx[(size_t) 2 * b + 1];
            if (stereo) { re = 0.5 * (re + fy[(size_t) 2 * b]); im = 0.5 * (im + fy[(size_t) 2 * b + 1]); }
            // late reverberation: what the room is still ringing with from about 40 ms ago (an exponential tail of the earlier signal)
            const float yp = (float) (re * re + im * im);
            const float ypSm = 0.5f * (yp + ypPrev[(size_t) b]);
            ypPrev[(size_t) b] = yp;
            const double late = gammaLate > 0.0 ? gammaLate * (double) ypPast[(size_t) ((lateHead + 1) % (kLateD + 1)) * kB + (size_t) b] : 0.0;
            ypPast[(size_t) lateHead * kB + (size_t) b] = ypSm;
            const double pn = psd[(size_t) b] * over + late + 1.0e-14;
            const double gam = (re * re + im * im) / pn;
            const double xi = beta * (double) gPrev[(size_t) b] * (double) gPrev[(size_t) b] * gamPrev[(size_t) b] + (1.0 - beta) * std::max (gam - 1.0, 0.0);
            const double g = xi / (1.0 + xi);
            gPrev[(size_t) b] = (float) g;
            gamPrev[(size_t) b] = gam;
            gPre[(size_t) b] = (float) g;
        }
        lateHead = (lateHead + 1) % (kLateD + 1);
        for (int b = 0; b < kB; ++b)
        {
            const float a0 = gPre[(size_t) std::max (0, b - 1)], a1 = gPre[(size_t) b], a2 = gPre[(size_t) std::min (kB - 1, b + 1)];
            gSm[(size_t) b] = std::max (floorG, 0.25f * a0 + 0.5f * a1 + 0.25f * a2);
        }
        auto finishCh = [&] (std::vector<float>& d, std::vector<float>& acc)
        {
            for (int b = 0; b < kB; ++b)
            {
                d[(size_t) 2 * b] *= gSm[(size_t) b]; d[(size_t) 2 * b + 1] *= gSm[(size_t) b];
                if (b > 0 && b < kN / 2) { d[(size_t) 2 * (kN - b)] = d[(size_t) 2 * b]; d[(size_t) 2 * (kN - b) + 1] = -d[(size_t) 2 * b + 1]; }
            }
            d[1] = 0.f; d[(size_t) 2 * (kN / 2) + 1] = 0.f;
            fft.performRealOnlyInverseTransform (d.data());
            for (int i = 0; i < kN; ++i) acc[(size_t) i] += 0.5f * d[(size_t) i] * win[(size_t) i];   // the FFT pair is exact (gain 1); sine^2 at 75 % overlap sums to 2
        };
        finishCh (fx, accL);
        if (stereo) finishCh (fy, accR);

        // the first kH samples of the accumulator are complete now
        if (skip > 0) skip -= kH;
        else
        {
            const long long take = std::min<long long> (kH, inCount - emitted);
            for (long long i = 0; i < take; ++i) { oL.push_back (accL[(size_t) i]); oR.push_back (stereo ? accR[(size_t) i] : accL[(size_t) i]); }
            emitted += std::max<long long> (0, take);
        }
        std::memmove (accL.data(), accL.data() + kH, (size_t) (kN - kH) * sizeof (float));
        std::fill (accL.begin() + (kN - kH), accL.end(), 0.f);
        std::memmove (accR.data(), accR.data() + kH, (size_t) (kN - kH) * sizeof (float));
        std::fill (accR.begin() + (kN - kH), accR.end(), 0.f);
    }

    juce::dsp::FFT fft;
    std::vector<float> win;
    bool stereo, on = false;
    std::vector<double> psd;
    double over = 1.0, beta = 0.97, gammaLate = 0.0;
    static constexpr int kLateD = 4;
    std::vector<float> ypPast, ypPrev;
    int lateHead = 0;
    float floorG = 0.1f;
    std::vector<float> frL, frR, accL, accR, fx, fy, gPrev, gSm, gPre;
    std::vector<double> gamPrev;
    int fill = 0, skip = 0;
    long long inCount = 0, emitted = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// Low cut: a 24 dB/oct high-pass for wind and handling rumble (below the voice, so it takes nothing from it)
struct LowCut
{
    Biquad f[2][2];
    bool on = false;
    void prepare (double sr, float hz)
    {
        on = hz >= 20.f;
        if (! on) return;
        const double fc = std::min ((double) hz, sr * 0.2);
        f[0][0].c = f[1][0].c = Coeffs::make (FilterType::HighPass, sr, fc, 0.0, 0.5412);
        f[0][1].c = f[1][1].c = Coeffs::make (FilterType::HighPass, sr, fc, 0.0, 1.3066);
    }
    void process (float* l, float* r, size_t n)
    {
        if (! on) return;
        for (size_t i = 0; i < n; ++i)
        {
            l[i] = f[0][1].tick (f[0][0].tick (l[i]));
            r[i] = f[1][1].tick (f[1][0].tick (r[i]));
        }
    }
};

// Warmth: a soft, slightly asymmetric tanh curve mixed in (adds a little even and odd harmonics, rounds the peaks). Off at 0.
struct Warmth
{
    double amt = 0.0, drive = 1.0, bias = 0.0, norm = 1.0, dcA = 0.0, dcL = 0.0, dcR = 0.0;
    void prepare (double sr, float a)
    {
        amt = std::clamp ((double) a, 0.0, 1.0);
        drive = 1.0 + 2.5 * amt; bias = 0.12 * amt;
        norm = 1.0 / (drive * (1.0 - std::tanh (bias) * std::tanh (bias)));
        dcA = 1.0 - std::exp (-2.0 * kPiD * 15.0 / sr);
        dcL = dcR = 0.0;
    }
    bool on() const { return amt > 0.005; }
    void process (float* l, float* r, size_t n)
    {
        if (! on()) return;
        const double tb = std::tanh (bias);
        for (size_t i = 0; i < n; ++i)
        {
            const double xl = l[i], xr = r[i];
            double yl = (std::tanh (drive * xl + bias) - tb) * norm, yr = (std::tanh (drive * xr + bias) - tb) * norm;
            yl = xl + amt * (yl - xl); yr = xr + amt * (yr - xr);
            dcL += dcA * (yl - dcL); dcR += dcA * (yr - dcR);
            l[i] = (float) (yl - dcL); r[i] = (float) (yr - dcR);
        }
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Runs the limiter over the whole store (scaled by `gain`), measuring as it goes; writes to `out` if given.
struct LimitResult { Stats st; double grDb = 0.0; };

bool limitPass (Store& pre, long long len, double sr, double gain, float ceilingDb, Store* out, bool wantTp,
                std::vector<float>* waveform, LimitResult& res, const std::function<bool (float)>& prog)
{
    LookaheadLimiter lim; lim.prepare (sr);
    const int la = lim.getLatency();
    StatsAcc acc; acc.prepare (sr, wantTp);
    constexpr int chunk = 1 << 14;
    std::vector<float> a ((size_t) chunk), b ((size_t) chunk);
    double maxGr = 0.0;
    constexpr int cols = 1200;
    if (waveform != nullptr)
    {
        waveform->assign ((size_t) cols * 2, 0.f);
        for (int c = 0; c < cols; ++c) { (*waveform)[(size_t) c * 2] = 1.f; (*waveform)[(size_t) c * 2 + 1] = -1.f; }
    }
    const long long total = len + la;
    for (long long pos = 0; pos < total; pos += chunk)
    {
        if (((pos / chunk) & 31) == 0 && prog && ! prog ((float) pos / (float) total)) return false;
        const int m = (int) std::min<long long> (chunk, total - pos);
        const int real = (int) std::max<long long> (0, std::min<long long> (m, len - pos));
        if (real > 0) { if (! pre.read (a.data(), b.data(), pos, real)) return false; }
        for (int i = 0; i < real; ++i) { a[(size_t) i] = (float) (a[(size_t) i] * gain); b[(size_t) i] = (float) (b[(size_t) i] * gain); }
        for (int i = real; i < m; ++i) { a[(size_t) i] = 0.f; b[(size_t) i] = 0.f; }
        lim.process (a.data(), b.data(), m, ceilingDb);
        maxGr = std::max (maxGr, (double) lim.grMeter.load());
        // output sample j of this chunk corresponds to input index pos + j - la
        int j0 = 0;
        if (pos < (long long) la) j0 = (int) std::min<long long> (m, (long long) la - pos);
        if (j0 < m)
        {
            acc.push (a.data() + j0, b.data() + j0, m - j0);
            if (out != nullptr && ! out->append (a.data() + j0, b.data() + j0, m - j0)) return false;
            if (waveform != nullptr)
                for (int j = j0; j < m; ++j)
                {
                    const long long idx = pos + j - la;
                    const int c = (int) std::min<long long> (cols - 1, idx * cols / std::max<long long> (1, len));
                    const float v = 0.5f * (a[(size_t) j] + b[(size_t) j]);
                    (*waveform)[(size_t) c * 2] = std::min ((*waveform)[(size_t) c * 2], v);
                    (*waveform)[(size_t) c * 2 + 1] = std::max ((*waveform)[(size_t) c * 2 + 1], v);
                }
        }
    }
    if (out != nullptr && ! out->finishWrite()) return false;
    res.st = acc.finish();
    res.grDb = maxGr;
    return true;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------
void scanClipping (const ReadFn& src, long long len, const Progress& prog, ClipInfo out[2])
{
    out[0] = out[1] = {};
    constexpr int C = 1 << 16;
    std::vector<float> l ((size_t) C), r ((size_t) C);
    float hi[2] = { 0.f, 0.f }, lo[2] = { 0.f, 0.f };
    for (long long pos = 0; pos < len; pos += C)
    {
        if (prog && ! prog (0.5f * (float) pos / (float) len)) return;
        const int n = (int) std::min<long long> (C, len - pos);
        if (! src (l.data(), r.data(), pos, n)) return;
        for (int i = 0; i < n; ++i)
        {
            hi[0] = std::max (hi[0], l[(size_t) i]); lo[0] = std::min (lo[0], l[(size_t) i]);
            hi[1] = std::max (hi[1], r[(size_t) i]); lo[1] = std::min (lo[1], r[(size_t) i]);
        }
    }
    RunCounter rp[2], rn[2];
    for (int c = 0; c < 2; ++c) { rp[c].init (hi[c], 1); rn[c].init (-lo[c], -1); }
    for (long long pos = 0; pos < len; pos += C)
    {
        if (prog && ! prog (0.5f + 0.5f * (float) pos / (float) len)) return;
        const int n = (int) std::min<long long> (C, len - pos);
        if (! src (l.data(), r.data(), pos, n)) return;
        rp[0].push (l.data(), n); rn[0].push (l.data(), n);
        rp[1].push (r.data(), n); rn[1].push (r.data(), n);
    }
    for (int c = 0; c < 2; ++c) { rp[c].flush(); rn[c].flush(); decideClip (out[c], hi[c], lo[c], rp[c], rn[c]); }
}

Analysis analyse (const ReadFn& src, long long len, double sr, const Progress& prog)
{
    Analysis a;
    a.sampleRate = sr; a.length = len;
    if (len < 4096) return a;
    constexpr int C = 1 << 16, cols = 1200;
    std::vector<float> l ((size_t) C), r ((size_t) C);
    StatsAcc acc; acc.prepare (sr, true);
    std::vector<double> blockE ((size_t) (len / kH) + 2, 0.0);
    float hi[2] = { 0.f, 0.f }, lo[2] = { 0.f, 0.f };
    a.waveform.assign ((size_t) cols * 2, 0.f);
    for (int c = 0; c < cols; ++c) { a.waveform[(size_t) c * 2] = 1.f; a.waveform[(size_t) c * 2 + 1] = -1.f; }

    // ---- pass 1: loudness, true peak, background level, clip levels, frame energies, waveform
    for (long long pos = 0; pos < len; pos += C)
    {
        if (prog && ! prog (0.28f * (float) pos / (float) len)) return a;
        const int n = (int) std::min<long long> (C, len - pos);
        if (! src (l.data(), r.data(), pos, n)) return a;
        acc.push (l.data(), r.data(), n);
        for (int i = 0; i < n; ++i)
        {
            const float x = l[(size_t) i], y = r[(size_t) i];
            hi[0] = std::max (hi[0], x); lo[0] = std::min (lo[0], x);
            hi[1] = std::max (hi[1], y); lo[1] = std::min (lo[1], y);
            const double m = 0.5 * ((double) x + (double) y);
            const long long k = pos + i;
            blockE[(size_t) (k / kH)] += m * m;
            const int c = (int) std::min<long long> (cols - 1, k * cols / len);
            const float v = (float) m;
            a.waveform[(size_t) c * 2] = std::min (a.waveform[(size_t) c * 2], v);
            a.waveform[(size_t) c * 2 + 1] = std::max (a.waveform[(size_t) c * 2 + 1], v);
        }
    }
    a.stats = acc.finish();
    if (! a.stats.valid) return a;

    // ---- the quietest frames are the noise profile
    const long long nFrames = len / kH;          // frame k covers blocks k-3 .. k
    std::vector<float> en;
    std::vector<size_t> cand;
    en.assign ((size_t) std::max<long long> (nFrames, 0), 0.f);
    for (long long k = 3; k < nFrames; ++k)
    {
        const double e = (blockE[(size_t) k - 3] + blockE[(size_t) k - 2] + blockE[(size_t) k - 1] + blockE[(size_t) k]) / (double) kN;
        en[(size_t) k] = (float) e;
        if (e > 1.0e-9) cand.push_back ((size_t) k);
    }
    std::vector<char> chosen ((size_t) std::max<long long> (nFrames, 0), 0);
    size_t nq = 0;
    if (cand.size() >= 16)
    {
        std::sort (cand.begin(), cand.end(), [&] (size_t p, size_t q) { return en[p] < en[q]; });
        nq = std::clamp<size_t> (cand.size() * 12 / 100, 8, 1200);
        const double quietE = en[cand[nq / 2]], loudE = en[cand[std::min (cand.size() - 1, cand.size() * 95 / 100)]];
        a.noise.contrastDb = 10.0 * std::log10 (std::max (loudE, 1.0e-12) / std::max (quietE, 1.0e-12));
        for (size_t q = 0; q < nq; ++q) chosen[cand[q]] = 1;
    }

    // ---- pass 2: count flat clipped tops, and average the spectrum of the chosen frames
    RunCounter rp[2], rn[2];
    for (int c = 0; c < 2; ++c) { rp[c].init (hi[c], 1); rn[c].init (-lo[c], -1); }
    juce::dsp::FFT fft (kFftOrder);
    const auto win = sineWindow();
    std::vector<double> psd ((size_t) kB, 0.0);
    std::vector<float> hist, fx ((size_t) 2 * kN);
    long long histStart = 0, nextK = 3, used = 0;
    for (long long pos = 0; pos < len; pos += C)
    {
        if (prog && ! prog (0.28f + 0.27f * (float) pos / (float) len)) return a;
        const int n = (int) std::min<long long> (C, len - pos);
        if (! src (l.data(), r.data(), pos, n)) return a;
        rp[0].push (l.data(), n); rn[0].push (l.data(), n);
        rp[1].push (r.data(), n); rn[1].push (r.data(), n);
        if (nq > 0)
        {
            for (int i = 0; i < n; ++i) hist.push_back (0.5f * (l[(size_t) i] + r[(size_t) i]));
            const long long histEnd = histStart + (long long) hist.size();
            while ((nextK + 1) * kH <= histEnd)
            {
                if (nextK < nFrames && chosen[(size_t) nextK])
                {
                    const size_t off = (size_t) ((nextK - 3) * kH - histStart);
                    { for (int i = 0; i < kN; ++i) fx[(size_t) i] = hist[off + (size_t) i] * win[(size_t) i]; std::fill (fx.begin() + kN, fx.end(), 0.f); }
                    fft.performRealOnlyForwardTransform (fx.data(), true);
                    for (int b = 0; b < kB; ++b) psd[(size_t) b] += (double) fx[(size_t) 2 * b] * fx[(size_t) 2 * b] + (double) fx[(size_t) 2 * b + 1] * fx[(size_t) 2 * b + 1];
                    ++used;
                }
                ++nextK;
            }
            const long long keepFrom = (nextK - 3) * kH;
            if (keepFrom > histStart) { hist.erase (hist.begin(), hist.begin() + (keepFrom - histStart)); histStart = keepFrom; }
        }
    }
    for (int c = 0; c < 2; ++c) { rp[c].flush(); rn[c].flush(); decideClip (a.clip[c], hi[c], lo[c], rp[c], rn[c]); }
    a.stats.clipped = std::max (a.clip[0].clipped, a.clip[1].clipped);
    if (used >= 8)
    {
        for (auto& v : psd) v /= (double) used;
        std::vector<double> t = psd;           // smooth across frequency so one lucky bin can't punch a hole
        for (int b = 0; b < kB; ++b)
        {
            double s = 0.0; int c = 0;
            for (int j = std::max (0, b - 3); j <= std::min (kB - 1, b + 3); ++j) { s += t[(size_t) j]; ++c; }
            psd[(size_t) b] = s / (double) c;
        }
        a.noise.psd = std::move (psd);
        a.noise.valid = true;
    }
    // ---- pass 3: who is speaking (a failure here only means "no per-voice processing", never a failed analysis)
    detectSpeakers (src, len, sr, a.stats.noiseFloorDb, a.noise,
                    [&] (float x) { return ! prog || prog (0.55f + 0.45f * x); }, a.speakers);
    a.valid = true;
    return a;
}

// ---------------------------------------------------------------------------------------------------------------
bool processStream (const ReadFn& src, long long len, double sr, bool dual, const Analysis& a, const Options& o,
                    Store& pre, Store& out, Report& rep, std::vector<float>* waveformAfter, const Progress& prog, int chunkFrames)
{
    rep = {};
    if (! a.valid || len < 4096) return false;
    rep.before = a.stats;
    auto P = [&] (float x) { return ! prog || prog (x); };
    auto stage = [&] (float lo, float hi) { return [&prog, lo, hi] (float x) { return ! prog || prog (lo + (hi - lo) * x); }; };

    const bool doDeclip = o.declip && (a.clip[0].on || (dual && a.clip[1].on));
    ClipInfo off; DeclipStream dcL (doDeclip ? a.clip[0] : off), dcR (doDeclip && dual ? a.clip[1] : off);
    bool eased = false;
    const bool stereoDn = dual && ! o.mono;
    StftDenoiser dn (a.noise, o.denoise, stereoDn, eased, o.echo);
    AutoVoice av; av.prepare (sr);
    // per-voice EQ and balance (replaces the single adaptive EQ when the file has two or more voices)
    SpeakerEq spk;
    bool spkActive = false;
    if (o.speakers && a.speakers.valid && a.speakers.count >= 2)
    {
        const auto plan = makeSpeakerPlan (a.speakers, o.tone, o.spkBalance);
        spkActive = plan.active;
        if (spkActive) { spk.prepare (sr, stereoDn); spk.setPlan (plan); }
    }
    LowCut lc; lc.prepare (sr, o.lowCutHz);
    Warmth wm; wm.prepare (sr, o.warmth);
    long long emitPos = 0;
    VoxSettings vs;
    vs.on = true; vs.mode = VoxSpeech;
    vs.level = o.level; vs.tone = spkActive ? 0.f : o.tone; vs.deess = o.deess; vs.comp = o.comp;
    vs.clean = 0.25f * o.denoise; vs.targetDb = -20.f;
    LoudnessMeter lm; lm.prepare (sr);

    std::vector<float> l ((size_t) chunkFrames), r ((size_t) chunkFrames), dL, dR, eL, eR;
    auto runChain = [&] (int n, bool last)
    {
        // 1. declip (each channel on its own)
        if (doDeclip)
        {
            dcL.push (l.data(), n); dcL.scan (last);
            size_t m = dcL.ready();
            if (dual) { dcR.push (r.data(), n); dcR.scan (last); m = std::min (m, dcR.ready()); }
            dL.resize (m); dcL.pop (dL.data(), m);
            if (dual) { dR.resize (m); dcR.pop (dR.data(), m); } else dR = dL;
        }
        else { dL.assign (l.begin(), l.begin() + n); dR.assign (r.begin(), r.begin() + n); }
        // 2. fold to mono
        if (o.mono && dual) for (size_t i = 0; i < dL.size(); ++i) { const float m = 0.5f * (dL[i] + dR[i]); dL[i] = dR[i] = m; }
        lc.process (dL.data(), dR.data(), dL.size());
        // 3. denoise
        if (dn.active())
        {
            eL.clear(); eR.clear();
            dn.process (dL.data(), dR.data(), (int) dL.size(), eL, eR);
            if (last) dn.finish (eL, eR);
        }
        else { eL = dL; eR = dR; }
        if (spkActive && ! eL.empty()) spk.process (a.speakers, eL.data(), eR.data(), (int) eL.size(), emitPos);
        emitPos += (long long) eL.size();
        // 4. Auto Voice, speech mode
        // (Auto Voice applies its tone EQ per call with the settings it holds at the end of the call, so it has to be fed in short pieces,
        //  like a plug-in host would: long pieces make the EQ jump at every piece boundary, which is an audible click.)
        for (size_t off2 = 0; off2 < eL.size(); off2 += 32)
        {
            const int k = (int) std::min<size_t> (32, eL.size() - off2);
            av.process (eL.data() + off2, eR.data() + off2, k, vs);
        }
        wm.process (eL.data(), eR.data(), eL.size());
        if (! eL.empty()) { lm.push (eL.data(), eR.data(), (int) eL.size()); pre.append (eL.data(), eR.data(), (int) eL.size()); }
    };

    for (long long pos = 0; pos < len; pos += chunkFrames)
    {
        if (! P (0.65f * (float) pos / (float) len)) return false;
        const int n = (int) std::min<long long> (chunkFrames, len - pos);
        if (! src (l.data(), r.data(), pos, n)) return false;
        runChain (n, false);
    }
    runChain (0, true);
    if (! pre.finishWrite()) return false;
    if (pre.length() != len) return false;
    rep.declipped = dcL.repaired + (dual ? dcR.repaired : 0);
    rep.denoised = dn.active();
    rep.denoiseEased = eased && o.denoise > 0.01f;
    const double levelled = lm.integrated();
    if (levelled < -69.0) return false;

    // ---- loudness: find the gain that lands on the target *after* the limiter
    double gainDb = std::clamp ((double) o.targetLufs - levelled, -30.0, 40.0);
    float ceilUsed = o.ceilingDb;
    LimitResult lr;
    for (int outer = 0; outer < 3; ++outer)
    {
        for (int it = 0; it < 4; ++it)
        {
            if (! limitPass (pre, len, sr, std::pow (10.0, gainDb / 20.0), ceilUsed, nullptr, false, nullptr, lr, stage (0.65f, 0.9f))) return false;
            const double err = (double) o.targetLufs - lr.st.lufs;
            if (std::abs (err) < 0.15) break;
            gainDb += err * (lr.grDb < -1.0 ? 0.8 : 1.0);
            if (gainDb > 45.0) break;
        }
        // true peak check (the expensive measurement is only done once the gain has settled)
        if (! limitPass (pre, len, sr, std::pow (10.0, gainDb / 20.0), ceilUsed, nullptr, true, nullptr, lr, stage (0.65f, 0.9f))) return false;
        const double over = lr.st.truePeakDb - (double) o.ceilingDb;
        if (over <= 0.05) break;
        ceilUsed -= (float) (over + 0.02);
    }
    if (! limitPass (pre, len, sr, std::pow (10.0, gainDb / 20.0), ceilUsed, &out, true, waveformAfter, lr, stage (0.9f, 0.97f))) return false;
    rep.gainDb = gainDb;
    rep.limiterPeakGrDb = lr.grDb;
    rep.after = lr.st;

    // flat tops left in the result
    {
        ClipInfo ci[2];
        scanClipping ([&] (float* x, float* y, long long pos, int n) { return out.read (x, y, pos, n); }, len, stage (0.97f, 1.f), ci);
        rep.after.clipped = std::max (ci[0].clipped, ci[1].clipped);
    }
    rep.reachedTarget = std::abs (rep.after.lufs - (double) o.targetLufs) < 0.6;
    return P (1.f);
}

// ---------------------------------------------------------------------------------------------------------------
struct LiveChain::Impl
{
    std::shared_ptr<const Analysis> an;
    Options o;
    double sr;
    bool dual, doDeclip, stereoDn;
    ClipInfo off;
    DeclipStream dcL, dcR;
    bool eased = false;
    StftDenoiser dn;
    AutoVoice av;
    SpeakerEq spk;
    bool spkActive = false;
    LowCut lc;
    Warmth wm;
    long long emitPos = 0;
    VoxSettings vs;
    LoudnessMeter lm;
    LookaheadLimiter lim;
    int limSkip = 0;
    double gainDb, gainLin;
    long long heard = 0;
    std::vector<float> l, r, dL, dR, eL, eR;

    Impl (std::shared_ptr<const Analysis> a, const Options& opt, double rate, bool dualSource, double g0, long long startPos)
        : an (std::move (a)), o (opt), sr (rate), dual (dualSource),
          doDeclip (opt.declip && an != nullptr && (an->clip[0].on || (dualSource && an->clip[1].on))),
          stereoDn (dualSource && ! opt.mono),
          dcL (doDeclip ? an->clip[0] : off), dcR (doDeclip && dualSource ? an->clip[1] : off),
          dn (an->noise, opt.denoise, dualSource && ! opt.mono, eased, opt.echo),
          gainDb (g0), gainLin (std::pow (10.0, g0 / 20.0))
    {
        emitPos = startPos;
        av.prepare (sr);
        if (o.speakers && an->speakers.valid && an->speakers.count >= 2)
        {
            const auto plan = makeSpeakerPlan (an->speakers, o.tone, o.spkBalance);
            spkActive = plan.active;
            if (spkActive) { spk.prepare (sr, stereoDn); spk.setPlan (plan); }
        }
        lc.prepare (sr, o.lowCutHz);
        wm.prepare (sr, o.warmth);
        vs.on = true; vs.mode = VoxSpeech;
        vs.level = o.level; vs.tone = spkActive ? 0.f : o.tone; vs.deess = o.deess; vs.comp = o.comp;
        vs.clean = 0.25f * o.denoise; vs.targetDb = -20.f;
        lm.prepare (sr);
        lim.prepare (sr);
        limSkip = lim.getLatency();
    }
};

LiveChain::LiveChain (std::shared_ptr<const Analysis> a, const Options& o, double sampleRate, bool dualSource, double initialGainDb, long long startPos)
    : p (std::make_unique<Impl> (std::move (a), o, sampleRate, dualSource, initialGainDb, startPos)) {}
LiveChain::~LiveChain() = default;
double LiveChain::gainDb() const { return p->gainDb; }
const Options& LiveChain::options() const { return p->o; }

void LiveChain::push (const float* inL, const float* inR, int n, std::vector<float>& outL, std::vector<float>& outR)
{
    auto& s = *p;
    // 1. declip
    if (s.doDeclip)
    {
        s.dcL.push (inL, n); s.dcL.scan (false);
        size_t m = s.dcL.ready();
        if (s.dual) { s.dcR.push (inR, n); s.dcR.scan (false); m = std::min (m, s.dcR.ready()); }
        s.dL.resize (m); s.dcL.pop (s.dL.data(), m);
        if (s.dual) { s.dR.resize (m); s.dcR.pop (s.dR.data(), m); } else s.dR = s.dL;
    }
    else { s.dL.assign (inL, inL + n); s.dR.assign (s.dual ? inR : inL, (s.dual ? inR : inL) + n); }
    // 2. mono fold
    if (s.o.mono && s.dual) for (size_t i = 0; i < s.dL.size(); ++i) { const float m = 0.5f * (s.dL[i] + s.dR[i]); s.dL[i] = s.dR[i] = m; }
    s.lc.process (s.dL.data(), s.dR.data(), s.dL.size());
    // 3. denoise
    if (s.dn.active())
    {
        s.eL.clear(); s.eR.clear();
        s.dn.process (s.dL.data(), s.dR.data(), (int) s.dL.size(), s.eL, s.eR);
    }
    else { s.eL = s.dL; s.eR = s.dR; }
    if (s.eL.empty()) return;
    if (s.spkActive) s.spk.process (s.an->speakers, s.eL.data(), s.eR.data(), (int) s.eL.size(), s.emitPos);
    s.emitPos += (long long) s.eL.size();
    // 4. Auto Voice (short pieces, see processStream)
    for (size_t off2 = 0; off2 < s.eL.size(); off2 += 32)
        s.av.process (s.eL.data() + off2, s.eR.data() + off2, (int) std::min<size_t> (32, s.eL.size() - off2), s.vs);
    s.wm.process (s.eL.data(), s.eR.data(), s.eL.size());
    const int m = (int) s.eL.size();
    s.lm.push (s.eL.data(), s.eR.data(), m);
    // 5. gain: follows what the loudness meter has heard (after the first few seconds), a couple of dB per second at most
    s.heard += m;
    const double g0 = s.gainLin;
    if ((double) s.heard > 1.5 * s.sr)
    {
        const double heardLufs = s.lm.integrated();
        if (heardLufs > -69.0)
        {
            const double want = std::clamp ((double) s.o.targetLufs - heardLufs, -30.0, 40.0);
            const double step = (std::abs (want - s.gainDb) > 4.0 ? 8.0 : 2.5) * (double) m / s.sr;      // quick when it is far off (a changed target), gentle once close
            s.gainDb += std::clamp (want - s.gainDb, -step, step);
        }
    }
    s.gainLin = std::pow (10.0, s.gainDb / 20.0);
    for (int i = 0; i < m; ++i)
    {
        const double g = g0 + (s.gainLin - g0) * (double) (i + 1) / (double) m;
        s.eL[(size_t) i] = (float) (s.eL[(size_t) i] * g); s.eR[(size_t) i] = (float) (s.eR[(size_t) i] * g);
    }
    // 6. limiter (it delays by its look-ahead; the first frames out of it are its empty delay line)
    s.lim.process (s.eL.data(), s.eR.data(), m, s.o.ceilingDb);
    int from = 0;
    if (s.limSkip > 0) { from = std::min (m, s.limSkip); s.limSkip -= from; }
    outL.insert (outL.end(), s.eL.begin() + from, s.eL.end());
    outR.insert (outR.end(), s.eR.begin() + from, s.eR.end());
}

// ---------------------------------------------------------------------------------------------------------------
// in-memory helpers
Stats measure (const float* l, const float* r, size_t n, double sr)
{
    Stats s;
    if (n < 2048) return s;
    StatsAcc acc; acc.prepare (sr, true);
    constexpr size_t chunk = 1 << 14;
    for (size_t a = 0; a < n; a += chunk) acc.push (l + a, r + a, (int) std::min (chunk, n - a));
    s = acc.finish();
    s.clipped = std::max (countClipped (l, n), countClipped (r, n));
    return s;
}

long long countClipped (const float* x, size_t n) { return clipOfArray (x, n).clipped; }

long long declipChannel (float* x, size_t n)
{
    const auto ci = clipOfArray (x, n);
    if (! ci.on) return 0;
    DeclipStream ds (ci);
    std::vector<float> out (n);
    size_t got = 0;
    constexpr size_t step = 4096;
    for (size_t pos = 0; pos < n; pos += step)
    {
        const size_t m = std::min (step, n - pos);
        ds.push (x + pos, (int) m);
        ds.scan (pos + m >= n);
        const size_t k = ds.ready();
        ds.pop (out.data() + got, k); got += k;
    }
    std::copy (out.begin(), out.begin() + (long long) got, x);
    return ds.repaired;
}

bool denoise (float* l, float* r, size_t n, double sr, float amount, bool& eased, const Progress& prog)
{
    eased = false;
    if (amount < 0.01f || n < 8192) return true;
    const bool stereo = r != nullptr && r != l;
    const float* rr = stereo ? r : l;
    const auto a = analyse ([&] (float* x, float* y, long long pos, int m)
                            { std::copy (l + pos, l + pos + m, x); std::copy (rr + pos, rr + pos + m, y); return true; }, (long long) n, sr, nullptr);
    if (! a.valid || ! a.noise.valid) return true;
    StftDenoiser dn (a.noise, amount, stereo, eased);
    if (! dn.active()) return true;
    std::vector<float> oL, oR;
    oL.reserve (n); oR.reserve (n);
    constexpr size_t step = 1 << 16;
    for (size_t pos = 0; pos < n; pos += step)
    {
        if (prog && ! prog ((float) pos / (float) n)) return false;
        const size_t m = std::min (step, n - pos);
        dn.process (l + pos, rr + pos, (int) m, oL, oR);
    }
    dn.finish (oL, oR);
    std::copy (oL.begin(), oL.begin() + (long long) n, l);
    if (stereo) std::copy (oR.begin(), oR.begin() + (long long) n, r);
    return true;
}

bool process (juce::AudioBuffer<float>& buf, double sr, const Options& o, Report& rep, const Progress& prog, int chunkFrames)
{
    rep = {};
    const int nch = buf.getNumChannels();
    const long long n = buf.getNumSamples();
    if (n < 4096 || nch < 1) return false;
    const float* cl = buf.getReadPointer (0);
    const float* cr = buf.getReadPointer (nch > 1 ? 1 : 0);
    const ReadFn read = [&] (float* l, float* r, long long pos, int m)
    {
        std::copy (cl + pos, cl + pos + m, l); std::copy (cr + pos, cr + pos + m, r);
        return true;
    };
    auto sub = [&] (float lo, float hi) { return [&prog, lo, hi] (float x) { return ! prog || prog (lo + (hi - lo) * x); }; };
    const auto a = analyse (read, n, sr, sub (0.f, 0.15f));
    if (! a.valid) return false;
    MemStore pre, out;
    if (! processStream (read, n, sr, nch > 1, a, o, pre, out, rep, nullptr, sub (0.15f, 1.f), chunkFrames)) return false;
    for (int c = 0; c < nch; ++c)
        std::copy ((c == 0 ? out.L : out.R).begin(), (c == 0 ? out.L : out.R).end(), buf.getWritePointer (c));
    return true;
}
} // namespace podcast
} // namespace aio

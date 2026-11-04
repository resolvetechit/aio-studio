#include <cstdlib>
#include <cstdio>
#include "MasterEngine.h"

namespace aio
{
// =================================================================================================================
// Analyser
// =================================================================================================================
Analyser::Analyser()
{
    ring.assign ((size_t) kN, 0.f);
    fftData.assign ((size_t) kN * 2, 0.f);
    window.assign ((size_t) kN, 0.f);
    acc.assign ((size_t) kN / 2 + 1, 0.0);
    for (int i = 0; i < kN; ++i) window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) kN));
}

void Analyser::prepare (double sampleRate)
{
    sr = sampleRate;
    lm.prepare (sr);
    lpM.c = lpS.c = Coeffs::make (FilterType::LowPass, sr, 150.0, 0.0, 0.707);
    reset();
}

void Analyser::reset()
{
    lm.reset(); tpL.reset(); tpR.reset(); lpM.reset(); lpS.reset();
    eM = eS = eLowM = eLowS = sLL = sRR = sLR = 0.0;
    std::fill (ring.begin(), ring.end(), 0.f);
    std::fill (acc.begin(), acc.end(), 0.0);
    ringPos = 0; sinceHop = 0; filled = 0; frames = 0; total = 0;
}

void Analyser::push (const float* l, const float* r, int n)
{
    lm.push (l, r, n);
    for (int i = 0; i < n; ++i)
    {
        const float a = l[i], b = r[i];
        tpL.tick (a); tpR.tick (b);
        const float m = 0.5f * (a + b), s = 0.5f * (a - b);
        eM += (double) m * m; eS += (double) s * s;
        sLL += (double) a * a; sRR += (double) b * b; sLR += (double) a * b;
        const float lm_ = lpM.tick (m), ls_ = lpS.tick (s);
        eLowM += (double) lm_ * lm_; eLowS += (double) ls_ * ls_;

        ring[(size_t) ringPos] = m;
        ringPos = (ringPos + 1) % kN;
        ++filled;
        if (++sinceHop >= kHop)
        {
            sinceHop = 0;
            if (filled >= kN) runFrame();
        }
    }
    total += n;
}

void Analyser::runFrame()
{
    double e = 0;
    for (int i = 0; i < kN; ++i)
    {
        const float v = ring[(size_t) ((ringPos + i) % kN)];
        e += (double) v * v;
        fftData[(size_t) i] = v * window[(size_t) i];
    }
    if (10.0 * std::log10 (e / kN + 1.0e-20) < -60.0) return;   // skip near-silence
    std::fill (fftData.begin() + kN, fftData.end(), 0.f);
    fft.performFrequencyOnlyForwardTransform (fftData.data());
    for (int k = 0; k <= kN / 2; ++k)
    {
        const double mag = fftData[(size_t) k];
        acc[(size_t) k] += mag * mag / ((double) kN * (double) kN);
    }
    ++frames;
}

MasterAnalysis Analyser::result() const
{
    MasterAnalysis a;
    a.sampleRate = sr;
    a.seconds = seconds();
    a.lufs = lm.integrated();
    a.lufsMomentaryMax = lm.maxMomentaryLufs();
    a.loudHigh = lm.percentile (0.95);
    a.loudLow = lm.percentile (0.10);
    a.samplePeakDb = 20.0 * std::log10 (std::max ((double) std::max (tpL.samplePk(), tpR.samplePk()), 1.0e-9));
    a.truePeakDb   = 20.0 * std::log10 (std::max ((double) std::max (tpL.truePeak(), tpR.truePeak()), 1.0e-9));
    a.plr = a.lufs > -150.0 ? a.truePeakDb - a.lufs : 0.0;
    a.correlation = sLR / std::sqrt (sLL * sRR + 1.0e-20);
    a.sideMidDb = 10.0 * std::log10 ((eS + 1.0e-20) / (eM + 1.0e-20));
    a.lowSideMidDb = 10.0 * std::log10 ((eLowS + 1.0e-20) / (eLowM + 1.0e-20));

    const double df = sr / (double) kN;
    for (int j = 0; j < kMB; ++j)
    {
        const double fc = bandCentreHz (j), lo = fc * std::pow (2.0, -1.0 / 6.0), hi = fc * std::pow (2.0, 1.0 / 6.0);
        a.bandValid[(size_t) j] = frames > 0 && hi <= 0.5 * sr * 0.98;
        if (! a.bandValid[(size_t) j]) { a.bandDb[(size_t) j] = -200.f; continue; }
        double p = 0;
        const int k0 = std::max (0, (int) std::floor (lo / df)), k1 = std::min (kN / 2, (int) std::ceil (hi / df) + 1);
        for (int k = k0; k <= k1; ++k)
        {
            const double bl = ((double) k - 0.5) * df, bh = ((double) k + 0.5) * df;
            const double ov = std::min (hi, bh) - std::max (lo, bl);
            if (ov > 0) p += acc[(size_t) k] * ov / df;
        }
        a.bandDb[(size_t) j] = (float) (10.0 * std::log10 (p / (double) std::max<long long> (1, frames) + 1.0e-20));
    }
    a.valid = frames > 0 && a.lufs > -150.0;
    return a;
}

MasterAnalysis analyseBuffer (const float* l, const float* r, size_t n, double sr, const Progress& progress)
{
    Analyser an;
    an.prepare (sr);
    const size_t chunk = 8192;
    for (size_t i = 0; i < n; i += chunk)
    {
        an.push (l + i, r + i, (int) std::min (chunk, n - i));
        if (progress && ! progress ((float) i / (float) std::max<size_t> (1, n))) break;
    }
    return an.result();
}

// =================================================================================================================
// Target curve + derivation
// =================================================================================================================
float targetCurveDb (float hz, int style)
{
    static const float f[16] = { 25, 40, 63, 100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300, 10000, 16000, 20000 };
    static const float d[16] = { -9, -2, 2.5f, 4.5f, 4.0f, 3.0f, 1.5f, 0.5f, 0, -1.0f, -2.2f, -3.6f, -5.5f, -8.5f, -13, -17 };
    const float hzc = std::clamp (hz, f[0], f[15]);
    float v = d[15];
    for (int i = 0; i < 15; ++i)
        if (hzc <= f[i + 1])
        {
            const float t = std::log2 (hzc / f[i]) / std::log2 (f[i + 1] / f[i]);
            v = d[i] + t * (d[i + 1] - d[i]);
            break;
        }
    const float oct = std::log2 (std::max (hz, 1.f) / 1000.f);
    auto bump = [&] (float centre, float width, float amp) { const float x = std::log2 (hz / centre) / width; return amp * std::exp (-0.5f * x * x); };
    switch (style)
    {
        case 1: v += -0.6f * oct; break;                                    // Warm
        case 2: v += 0.6f * oct; break;                                     // Bright
        case 3: v += bump (90.f, 0.5f, 1.5f) + bump (3000.f, 0.7f, 1.0f); break;   // Punchy
        case 4: v += 0.25f * oct + bump (250.f, 0.6f, -1.0f); break;        // Loud
        default: break;
    }
    return v;
}

static bool solveLinear (std::array<std::array<double, 5>, 5> m, std::array<double, 5> v, std::array<double, 5>& x)
{
    const int n = 5;
    for (int c = 0; c < n; ++c)
    {
        int piv = c;
        for (int r = c + 1; r < n; ++r) if (std::abs (m[(size_t) r][(size_t) c]) > std::abs (m[(size_t) piv][(size_t) c])) piv = r;
        if (std::abs (m[(size_t) piv][(size_t) c]) < 1.0e-12) return false;
        std::swap (m[(size_t) c], m[(size_t) piv]); std::swap (v[(size_t) c], v[(size_t) piv]);
        for (int r = c + 1; r < n; ++r)
        {
            const double f = m[(size_t) r][(size_t) c] / m[(size_t) c][(size_t) c];
            for (int k = c; k < n; ++k) m[(size_t) r][(size_t) k] -= f * m[(size_t) c][(size_t) k];
            v[(size_t) r] -= f * v[(size_t) c];
        }
    }
    for (int r = n - 1; r >= 0; --r)
    {
        double s = v[(size_t) r];
        for (int k = r + 1; k < n; ++k) s -= m[(size_t) r][(size_t) k] * x[(size_t) k];
        x[(size_t) r] = s / m[(size_t) r][(size_t) r];
    }
    return true;
}

MasterSettings deriveMaster (const MasterAnalysis& a, const MasterOptions& o)
{
    MasterSettings s;
    s.ceilingDb = o.ceilingDb;
    const double sr = a.sampleRate > 0 ? a.sampleRate : 44100.0;

    for (int j = 0; j < kMB; ++j) s.targetDb[(size_t) j] = targetCurveDb (bandCentreHz (j), o.style);

    // ---- tonal error: measured minus target, with the overall level offset removed
    std::array<float, kMB> err {};
    {
        double mean = 0; int cnt = 0;
        for (int j = 0; j < kMB; ++j)
        {
            if (! a.valid || ! a.bandValid[(size_t) j]) continue;
            const float fc = bandCentreHz (j);
            err[(size_t) j] = a.bandDb[(size_t) j] - s.targetDb[(size_t) j];
            if (fc >= 100.f && fc <= 8000.f) { mean += err[(size_t) j]; ++cnt; }
        }
        mean /= std::max (1, cnt);
        for (int j = 0; j < kMB; ++j)
            err[(size_t) j] = (a.valid && a.bandValid[(size_t) j]) ? err[(size_t) j] - (float) mean : 0.f;
    }
    s.errorDb = err;

    // ---- fit five EQ bands (low shelf, three bells, high shelf) to the smoothed error by regularised least squares
    struct Slot { FilterType t; float f, q, lim; };
    static const Slot slot[5] = { { FilterType::LowShelf, 110.f, 0.71f, 5.f }, { FilterType::Bell, 320.f, 0.8f, 4.5f },
                                  { FilterType::Bell, 1600.f, 0.8f, 4.5f },    { FilterType::Bell, 5000.f, 0.8f, 4.5f },
                                  { FilterType::HighShelf, 10500.f, 0.71f, 5.f } };
    std::array<double, 5> g {};
    std::array<std::array<float, 5>, kMB> B {};
    if (a.valid && o.amount > 0.001f)
    {
        for (int j = 0; j < kMB; ++j)
            for (int k = 0; k < 5; ++k)
                B[(size_t) j][(size_t) k] = (float) gainToDb (Coeffs::make (slot[k].t, sr, slot[k].f, 1.0, slot[k].q).magnitude (bandCentreHz (j), sr));

        std::array<std::array<double, 5>, 5> M {};
        std::array<double, 5> V {};
        for (int j = 0; j < kMB; ++j)
        {
            if (! a.bandValid[(size_t) j]) continue;
            const float fc = bandCentreHz (j);
            const double w = (fc >= 50.f && fc <= 12000.f) ? 1.0 : 0.25;
            double e2 = 0.5 * err[(size_t) j], wn = 0.5;
            if (j > 0 && a.bandValid[(size_t) j - 1])           { e2 += 0.25 * err[(size_t) j - 1]; wn += 0.25; }
            if (j + 1 < kMB && a.bandValid[(size_t) j + 1])     { e2 += 0.25 * err[(size_t) j + 1]; wn += 0.25; }
            e2 /= wn;
            const double D = std::clamp (-e2 * (double) o.amount, -8.0, 8.0);
            for (int k = 0; k < 5; ++k)
            {
                V[(size_t) k] += w * B[(size_t) j][(size_t) k] * D;
                for (int m = 0; m < 5; ++m) M[(size_t) k][(size_t) m] += w * B[(size_t) j][(size_t) k] * B[(size_t) j][(size_t) m];
            }
        }
        for (int k = 0; k < 5; ++k) M[(size_t) k][(size_t) k] += 0.4;
        if (! solveLinear (M, V, g)) g.fill (0.0);
        for (int k = 0; k < 5; ++k) g[(size_t) k] = std::clamp (g[(size_t) k], (double) -slot[k].lim, (double) slot[k].lim);
    }

    s.eq[0] = { true, FilterType::HighPass, 24.f, 0.f, 0.707f };
    for (int k = 0; k < 5; ++k) s.eq[(size_t) k + 1] = { true, slot[k].t, slot[k].f, (float) g[(size_t) k], slot[k].q };
    s.eqOn = true;
    for (int j = 0; j < kMB; ++j)
    {
        double c = 0;
        for (int k = 0; k < 5; ++k) c += g[(size_t) k] * B[(size_t) j][(size_t) k];
        s.correctionDb[(size_t) j] = (float) c;
    }

    // ---- dynamics: how much of the loudness gap has to come from compression
    const double targetPlr = (double) o.ceilingDb - (double) o.targetLufs;
    const double overshoot = a.valid ? a.plr - targetPlr : 0.0;
    s.targetMeanReductionDb = (float) ((double) o.glue * (1.0 + 3.5 * std::clamp (overshoot / 10.0, 0.0, 1.0)));
    s.compOn = o.glue > 0.02f;
    s.comp.ratio = 1.6f + 1.6f * o.glue;
    s.comp.attackMs = 30.f; s.comp.releaseMs = 150.f; s.comp.kneeDb = 8.f; s.comp.makeupDb = 0.f; s.comp.mix = 1.f;
    s.comp.threshDb = (float) std::clamp ((a.valid ? a.lufs : -18.0) + 8.0 - 4.0 * (double) o.glue, -60.0, 0.0);

    // ---- saturation
    s.satOn = o.warmth > 0.02f;
    s.sat = { SatType::Tape, 1.5f + 6.f * o.warmth, 20000.f, 0.2f + 0.4f * o.warmth, 0.f };

    // ---- bass in mono when there is meaningful side energy in the low end
    s.monoBassHz = (o.monoBass && a.valid && a.lowSideMidDb > -18.0) ? 110.f : 0.f;

    // ---- first guess for the gain
    const double est = (a.valid ? a.lufs : (double) o.targetLufs) - 0.5 * (double) s.targetMeanReductionDb + (s.satOn ? 0.2 : 0.0);
    s.outGainDb = (float) std::clamp ((double) o.targetLufs - est, -24.0, 24.0);
    return s;
}

ChainParams chainFromSettings (const MasterSettings& s)
{
    ChainParams p;
    p.eqOn = s.eqOn; p.eq = s.eq;
    p.compOn = s.compOn; p.comp = s.comp;
    p.satOn = s.satOn; p.sat = s.sat;
    p.monoBassHz = s.monoBassHz;
    p.outGain = dbToGain (s.outGainDb);
    p.limOn = true; p.limCeil = s.ceilingDb;
    return p;
}

// =================================================================================================================
// Offline rendering
// =================================================================================================================
RenderStats renderPass (const float* l, const float* r, size_t n, double sr, const ChainParams& p, const Sink& sink, const Progress& progress)
{
    RenderStats st;
    constexpr int kBlock = 4096;
    Chain chain;
    chain.prepare (sr, kBlock, false);
    LoudnessMeter lm; lm.prepare (sr);
    TruePeak tpl, tpr;

    const size_t lat = p.limOn ? (size_t) chain.limiter.getLatency() : 0;
    const size_t total = n + lat;
    std::vector<float> bl ((size_t) kBlock), br ((size_t) kBlock);
    size_t done = 0;
    while (done < total)
    {
        const int m = (int) std::min<size_t> ((size_t) kBlock, total - done);
        for (int i = 0; i < m; ++i)
        {
            const size_t idx = done + (size_t) i;
            bl[(size_t) i] = idx < n ? l[idx] : 0.f;
            br[(size_t) i] = idx < n ? r[idx] : 0.f;
        }
        chain.processOffline (bl.data(), br.data(), m, p);

        const int skip = done < lat ? (int) std::min<size_t> ((size_t) m, lat - done) : 0;
        const int cnt = m - skip;
        if (cnt > 0)
        {
            const float* ol = bl.data() + skip; const float* orr = br.data() + skip;
            lm.push (ol, orr, cnt);
            for (int i = 0; i < cnt; ++i) { tpl.tick (ol[i]); tpr.tick (orr[i]); }
            if (sink) sink (ol, orr, cnt);
        }
        done += (size_t) m;
        if (progress && ! progress ((float) done / (float) std::max<size_t> (1, total))) { st.cancelled = true; break; }
    }
    st.lufs = lm.integrated();
    st.samplePeakDb = 20.0 * std::log10 (std::max ((double) std::max (tpl.samplePk(), tpr.samplePk()), 1.0e-9));
    st.truePeakDb   = 20.0 * std::log10 (std::max ((double) std::max (tpl.truePeak(), tpr.truePeak()), 1.0e-9));
    return st;
}

// mean level reduction (dB, positive) of the compressor on short excerpts of the EQ'd signal
static double meanReductionDb (const float* l, const float* r, size_t n, double sr, const MasterSettings& s, float thresholdDb)
{
    ParametricEQ eq; eq.prepare (sr); eq.setBands (s.eq);
    Compressor comp; comp.prepare (sr);
    CompSettings cs = s.comp; cs.threshDb = thresholdDb;

    const size_t seg = std::min (n, (size_t) std::lround (2.0 * sr));
    const int nseg = (int) std::clamp<size_t> (n / std::max<size_t> (seg, 1), 1, 16);
    constexpr int kB = 512;
    std::vector<float> el ((size_t) kB), er ((size_t) kB), cl ((size_t) kB), cr ((size_t) kB);
    double sum = 0; int cnt = 0;
    for (int sgi = 0; sgi < nseg; ++sgi)
    {
        const size_t start = nseg > 1 ? (size_t) sgi * (n - seg) / (size_t) (nseg - 1) : 0;
        eq.reset(); comp.reset();
        for (size_t pos = 0; pos + (size_t) kB <= seg; pos += (size_t) kB)
        {
            for (int i = 0; i < kB; ++i) { el[(size_t) i] = l[start + pos + (size_t) i]; er[(size_t) i] = r[start + pos + (size_t) i]; }
            if (s.eqOn) eq.process (el.data(), er.data(), kB);
            cl = el; cr = er;
            comp.process (cl.data(), cr.data(), kB, cs);
            double ein = 0, eout = 0;
            for (int i = 0; i < kB; ++i)
            {
                ein  += (double) el[(size_t) i] * el[(size_t) i] + (double) er[(size_t) i] * er[(size_t) i];
                eout += (double) cl[(size_t) i] * cl[(size_t) i] + (double) cr[(size_t) i] * cr[(size_t) i];
            }
            if (ein / (2.0 * kB) > 1.0e-5)
            {
                sum += 10.0 * std::log10 ((eout + 1.0e-20) / (ein + 1.0e-20));
                ++cnt;
            }
        }
    }
    return cnt > 0 ? -sum / cnt : 0.0;
}

MasterSettings refineMaster (const float* l, const float* r, size_t n, double sr, const MasterAnalysis& a,
                             MasterSettings s, const MasterOptions& o, RenderStats* finalStats, const Progress& progress)
{
    auto sub = [&] (float lo, float hi) -> Progress
    {
        if (! progress) return {};
        return [=] (float x) { return progress (lo + (hi - lo) * x); };
    };

    // ---- 1) compressor threshold: bisection so the average level reduction matches the target
    if (s.compOn && a.valid)
    {
        float lo = (float) std::max (-60.0, a.lufs - 12.0), hi = (float) std::min (0.0, a.lufs + 26.0);
        const double want = (double) s.targetMeanReductionDb;
        if (meanReductionDb (l, r, n, sr, s, lo) <= want) hi = lo;                       // cannot reach it: strongest setting
        else if (meanReductionDb (l, r, n, sr, s, hi) >= want) lo = hi;                  // already more than wanted: weakest
        else
            for (int it = 0; it < 8; ++it)
            {
                const float mid = 0.5f * (lo + hi);
                if (meanReductionDb (l, r, n, sr, s, mid) > want) lo = mid; else hi = mid;
            }
        s.comp.threshDb = 0.5f * (lo + hi);
        if (progress && ! progress (0.1f)) return s;
    }

    // ---- 2) gain: secant iterations on the full render so the integrated loudness lands on the target
    double g = (double) s.outGainDb, bestG = g, bestErr = 1.0e9;
    RenderStats best;
    double prevG = 0, prevL = 0; bool havePrev = false;
    for (int it = 0; it < 6; ++it)
    {
        s.outGainDb = (float) g;
        const auto st = renderPass (l, r, n, sr, chainFromSettings (s), {}, sub (0.1f + 0.15f * (float) it, 0.1f + 0.15f * (float) (it + 1)));
        if (st.cancelled) return s;
        const double err = (double) o.targetLufs - st.lufs;
        if (std::abs (err) < std::abs (bestErr)) { bestErr = err; bestG = g; best = st; }
        if (std::abs (err) < 0.15 || st.lufs < -150.0) break;
        double slope = 1.0;
        if (havePrev && std::abs (g - prevG) > 0.01) slope = std::clamp ((st.lufs - prevL) / (g - prevG), 0.3, 1.2);
        prevG = g; prevL = st.lufs; havePrev = true;
        g = std::clamp (g + err / slope, -24.0, 24.0);
        if (std::abs (g - prevG) < 0.01) break;
    }
    s.outGainDb = (float) bestG;
    if (finalStats) *finalStats = best;
    return s;
}
} // namespace aio

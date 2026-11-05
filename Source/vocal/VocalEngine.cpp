#include "VocalEngine.h"
#include "../dsp/Biquad.h"
#include <array>
#include <cstring>
#include <numeric>

namespace aio
{
namespace
{
struct Lp
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set (double fc, double sr, double Q)
    {
        const double w0 = 2.0 * kPi * fc / sr, cs = std::cos (w0), alpha = std::sin (w0) / (2.0 * Q), a0 = 1.0 + alpha;
        b0 = (1.0 - cs) * 0.5 / a0; b1 = (1.0 - cs) / a0; b2 = b0; a1 = -2.0 * cs / a0; a2 = (1.0 - alpha) / a0;
    }
    double run (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

float medianOf (const std::vector<float>& v, int a, int b)
{
    std::array<float, 64> t {};
    const int n = std::min (b - a, 64);
    for (int i = 0; i < n; ++i) t[(size_t) i] = v[(size_t) (a + i)];
    std::sort (t.begin(), t.begin() + n);
    return n > 0 ? t[(size_t) (n / 2)] : 0.f;
}

float centreOf (const std::vector<float>& m)
{
    const int L = (int) m.size();
    if (L == 0) return 60.f;
    const int a = L >= 5 ? L / 5 : 0, b = L >= 5 ? L - L / 5 : L;
    double s = 0;
    for (int i = a; i < b; ++i) s += m[(size_t) i];
    return (float) (s / (double) (b - a));
}

std::shared_ptr<const NoteCurve> makeCurve (int f0, std::vector<float> midi, float centre)
{
    auto c = std::make_shared<NoteCurve>();
    c->f0 = f0;
    const int L = (int) midi.size();
    c->slow.assign ((size_t) L, 0.f);
    c->fast.assign ((size_t) L, 0.f);
    std::vector<double> pre ((size_t) L + 1, 0.0);
    for (int i = 0; i < L; ++i) pre[(size_t) i + 1] = pre[(size_t) i] + (double) (midi[(size_t) i] - centre);
    const int hw = std::max (1, std::min (16, L / 2));
    for (int i = 0; i < L; ++i)
    {
        const int a = std::max (0, i - hw), b = std::min (L - 1, i + hw);
        const double mean = (pre[(size_t) b + 1] - pre[(size_t) a]) / (double) (b - a + 1);
        c->slow[(size_t) i] = (float) mean;
        c->fast[(size_t) i] = (float) ((double) (midi[(size_t) i] - centre) - mean);
    }
    c->midi = std::move (midi);
    return c;
}

inline float readCR (const float* x, long long n, double pos)
{
    const long long i = (long long) std::floor (pos);
    const float f = (float) (pos - (double) i);
    float y0, y1, y2, y3;
    if (i >= 1 && i + 2 < n) { y0 = x[i - 1]; y1 = x[i]; y2 = x[i + 1]; y3 = x[i + 2]; }
    else
    {
        auto g = [&] (long long k) { return (k >= 0 && k < n) ? x[k] : 0.f; };
        y0 = g (i - 1); y1 = g (i); y2 = g (i + 1); y3 = g (i + 2);
    }
    return y1 + 0.5f * f * (y2 - y0 + f * (2.f * y0 - 5.f * y1 + 4.f * y2 - y3 + f * (3.f * (y1 - y2) + y3 - y0)));
}
} // namespace

// =================================================================================================================
// Pitch tracking
// =================================================================================================================
PitchTrack analysePitch (const float* x, size_t n, double sr, const AnalysisOptions& ao,
                         const std::atomic<bool>* cancel, std::atomic<float>* progress)
{
    PitchTrack tr;
    tr.sr = sr;
    const int decim = std::max (1, (int) std::lround (sr / 12000.0));
    const double srD = sr / decim;
    const int hopD = 64;
    tr.hop = hopD * decim;
    const int nFrames = (int) (n / (size_t) tr.hop) + 1;
    tr.midi.assign ((size_t) nFrames, -1.f);
    tr.levelDb.assign ((size_t) nFrames, -120.f);
    if (n < 256) return tr;

    // low-pass (4th order) + decimate
    std::vector<float> xd (n / (size_t) decim + 2, 0.f);
    {
        const double fc = std::min (3000.0, 0.42 * sr);
        Lp a, b;
        a.set (fc, sr, 0.5412); b.set (fc, sr, 1.3066);
        for (size_t i = 0; i < n; ++i)
        {
            const double y = b.run (a.run ((double) x[i]));
            if (i % (size_t) decim == 0) xd[i / (size_t) decim] = (float) y;
        }
    }

    constexpr int W = 400;
    const int tauMin = std::max (2, (int) std::floor (srD / (double) ao.maxHz));
    const int tauMax = std::min (1000, (int) std::ceil (srD / (double) ao.minHz));
    struct Cand { float midi, cost; };
    std::vector<std::array<Cand, 4>> cand ((size_t) nFrames);
    std::vector<uint8_t> nc ((size_t) nFrames, 0);
    std::vector<float> seg ((size_t) (W + tauMax + 4)), cm ((size_t) tauMax + 3);

    for (int i = 0; i < nFrames; ++i)
    {
        if ((i & 127) == 0)
        {
            if (cancel && cancel->load()) return tr;
            if (progress) progress->store (0.9f * (float) i / (float) nFrames);
        }

        // level over ~3 hops
        {
            const long long c = (long long) i * tr.hop, a = std::max<long long> (0, c - tr.hop * 3 / 2), b = std::min<long long> ((long long) n, c + tr.hop * 3 / 2);
            double e = 0;
            for (long long k = a; k < b; ++k) e += (double) x[k] * x[k];
            tr.levelDb[(size_t) i] = (float) (10.0 * std::log10 (e / (double) std::max<long long> (1, b - a) + 1.0e-12));
        }
        if (tr.levelDb[(size_t) i] < ao.gateDb) continue;

        const long long cD = (long long) i * hopD, s0 = cD - (W + tauMax) / 2;
        for (int j = 0; j < W + tauMax + 2; ++j)
        {
            const long long k = s0 + j;
            seg[(size_t) j] = (k >= 0 && k < (long long) xd.size()) ? xd[(size_t) k] : 0.f;
        }

        double run = 0;
        cm[0] = 1.f;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            float sum = 0.f;
            const float* p = seg.data();
            const float* q = seg.data() + tau;
            for (int j = 0; j < W; ++j) { const float d = p[j] - q[j]; sum += d * d; }
            run += (double) sum;
            cm[(size_t) tau] = run > 1.0e-9 ? (float) ((double) sum * tau / run) : 1.f;
        }
        cm[(size_t) tauMax + 1] = 1.f;

        std::array<std::pair<float, int>, 24> mins {};
        int nm = 0;
        for (int tau = tauMin; tau < tauMax && nm < 24; ++tau)
            if (cm[(size_t) tau] < 0.35f && cm[(size_t) tau] <= cm[(size_t) tau - 1] && cm[(size_t) tau] < cm[(size_t) tau + 1])
                mins[(size_t) nm++] = { cm[(size_t) tau], tau };
        // a minimum at a whole multiple of the period of a shorter, almost-as-good minimum is a sub-harmonic: drop it
        {
            std::array<std::pair<float, int>, 24> kept {};
            int nk = 0;
            for (int k = 0; k < nm; ++k)
            {
                bool sub = false;
                for (int j = 0; j < nm && ! sub; ++j)
                {
                    if (mins[(size_t) j].second >= mins[(size_t) k].second) continue;
                    const double ratio = (double) mins[(size_t) k].second / (double) mins[(size_t) j].second;
                    const double m = std::round (ratio);
                    if (m >= 2.0 && std::abs (ratio - m) < 0.06 * m && mins[(size_t) j].first < mins[(size_t) k].first + 0.12f && mins[(size_t) j].first < 0.25f)
                        sub = true;
                }
                if (! sub) kept[(size_t) nk++] = mins[(size_t) k];
            }
            mins = kept; nm = nk;
        }
        std::sort (mins.begin(), mins.begin() + nm);

        int cnt = 0;
        std::array<int, 4> taken {};
        for (int k = 0; k < nm && cnt < 4; ++k)
        {
            const int tau = mins[(size_t) k].second;
            bool dup = false;
            for (int t = 0; t < cnt; ++t) if (std::abs (taken[(size_t) t] - tau) * 25 < tau) dup = true;
            if (dup) continue;
            const double s0v = cm[(size_t) tau - 1], s1v = cm[(size_t) tau], s2v = cm[(size_t) tau + 1];
            const double den = 2.0 * (2.0 * s1v - s2v - s0v);
            const double off = std::abs (den) > 1.0e-9 ? std::clamp ((s2v - s0v) / den, -1.0, 1.0) : 0.0;
            const double f = srD / ((double) tau + off);
            cand[(size_t) i][(size_t) cnt] = { (float) (69.0 + 12.0 * std::log2 (f / 440.0)), mins[(size_t) k].first };
            taken[(size_t) cnt++] = tau;
        }
        nc[(size_t) i] = (uint8_t) cnt;
    }

    // ---- Viterbi over (up to 4 voiced candidates + unvoiced) ----
    constexpr float INF = 1.0e9f;
    std::vector<std::array<float, 5>> acc ((size_t) nFrames);
    std::vector<std::array<int8_t, 5>> bp ((size_t) nFrames);
    auto obs = [&] (int i, int s) -> float
    {
        if (s == 4) return nc[(size_t) i] ? 0.40f : 0.f;
        if (s >= nc[(size_t) i]) return INF;
        return 1.5f * cand[(size_t) i][(size_t) s].cost;
    };
    for (int s = 0; s < 5; ++s) { acc[0][(size_t) s] = obs (0, s); bp[0][(size_t) s] = 0; }
    for (int i = 1; i < nFrames; ++i)
    {
        for (int s = 0; s < 5; ++s)
        {
            const float o = obs (i, s);
            float best = INF; int arg = 0;
            if (o < INF)
                for (int p = 0; p < 5; ++p)
                {
                    const float a = acc[(size_t) i - 1][(size_t) p];
                    if (a >= INF) continue;
                    float t;
                    if (s == 4 && p == 4) t = 0.f;
                    else if (s == 4 || p == 4) t = 0.30f;
                    else t = 0.06f * std::min (12.f, std::abs (cand[(size_t) i][(size_t) s].midi - cand[(size_t) i - 1][(size_t) p].midi));
                    if (a + t < best) { best = a + t; arg = p; }
                }
            acc[(size_t) i][(size_t) s] = best < INF ? best + o : INF;
            bp[(size_t) i][(size_t) s] = (int8_t) arg;
        }
    }
    {
        int s = 4; float best = INF;
        for (int k = 0; k < 5; ++k) if (acc[(size_t) nFrames - 1][(size_t) k] < best) { best = acc[(size_t) nFrames - 1][(size_t) k]; s = k; }
        for (int i = nFrames - 1; i >= 0; --i)
        {
            tr.midi[(size_t) i] = (s < 4 && s < nc[(size_t) i]) ? cand[(size_t) i][(size_t) s].midi : -1.f;
            s = bp[(size_t) i][(size_t) s];
        }
    }

    // 3-point median inside voiced runs
    {
        std::vector<float> src = tr.midi;
        for (int i = 1; i + 1 < nFrames; ++i)
            if (src[(size_t) i - 1] >= 0.f && src[(size_t) i] >= 0.f && src[(size_t) i + 1] >= 0.f)
            {
                const float a = src[(size_t) i - 1], b = src[(size_t) i], c = src[(size_t) i + 1];
                tr.midi[(size_t) i] = std::max (std::min (a, b), std::min (std::max (a, b), c));
            }
    }
    if (progress) progress->store (1.f);
    return tr;
}

// =================================================================================================================
// Note segmentation
// =================================================================================================================
namespace
{
void findDips (const PitchTrack& tr, int a, int s, int e, const SegmentOptions& so, std::vector<int>& out)
{
    if (e - s < 2 * so.minFrames + 2) return;
    const int N = tr.numFrames();
    auto lv = [&] (int k)
    {
        float t = 0.f;
        for (int d = -1; d <= 1; ++d) t += tr.levelDb[(size_t) std::clamp (a + k + d, 0, N - 1)];
        return t / 3.f;
    };
    int skipUntil = s + so.minFrames;
    for (int k = s + so.minFrames; k < e - so.minFrames; ++k)
    {
        if (k < skipUntil) continue;
        const float c = lv (k);
        bool isMin = true;
        for (int d = -4; d <= 4 && isMin; ++d)
        {
            const int kk = k + d;
            if (d == 0 || kk < s || kk >= e) continue;
            if (lv (kk) < c) isMin = false;
        }
        if (! isMin) continue;
        float mL = -200.f, mR = -200.f;
        for (int kk = std::max (s, k - 30); kk < k; ++kk) mL = std::max (mL, lv (kk));
        for (int kk = k + 1; kk < std::min (e, k + 31); ++kk) mR = std::max (mR, lv (kk));
        if (std::min (mL, mR) - c >= so.dipDb) { out.push_back (k); skipUntil = k + 8; }
    }
}

void splitRun (const PitchTrack& tr, const std::vector<float>& m, int a, int b, const SegmentOptions& so, std::vector<VocalNote>& out)
{
    const int L = b - a;
    std::vector<float> ms ((size_t) L);
    for (int k = 0; k < L; ++k)
    {
        float w[5];
        int c = 0;
        for (int d = -2; d <= 2; ++d) w[c++] = m[(size_t) (a + std::clamp (k + d, 0, L - 1))];
        std::sort (w, w + 5);
        ms[(size_t) k] = w[2];
    }

    // vibrato-free copy (about 120 ms moving average) decides *whether* the pitch has changed,
    // the unsmoothed copy decides *where*
    std::vector<float> sm ((size_t) L);
    {
        const int hw = 10;
        std::vector<double> pre ((size_t) L + 1, 0.0);
        for (int k = 0; k < L; ++k) pre[(size_t) k + 1] = pre[(size_t) k] + (double) ms[(size_t) k];
        for (int k = 0; k < L; ++k)
        {
            const int lo = std::max (0, k - hw), hi = std::min (L - 1, k + hw);
            sm[(size_t) k] = (float) ((pre[(size_t) hi + 1] - pre[(size_t) lo]) / (double) (hi - lo + 1));
        }
    }

    std::vector<int> cuts { 0 };
    int segStart = 0;
    for (int i = 1; i < L; ++i)
    {
        if (i - segStart < so.minFrames) continue;
        const int rs = std::max (segStart, i - 40);
        const float ref = medianOf (sm, rs, i);
        if (std::abs (sm[(size_t) i] - ref) <= so.splitSemis) continue;
        const int Le = std::min (L, i + 10);
        if (Le - i < 3) continue;
        const float look = medianOf (sm, i, Le);
        if (std::abs (look - ref) <= so.splitSemis * 0.9f) continue;
        const int lo = std::max (segStart + so.minFrames / 2, i - 12), hi = std::min (L - 1, i + 4);
        int best = i; float bs = -1.f;
        for (int j = std::max (lo, segStart + 1); j <= hi; ++j)
        {
            const float sl = std::abs (ms[(size_t) std::min (L - 1, j + 1)] - ms[(size_t) std::max (0, j - 2)]);
            if (sl > bs) { bs = sl; best = j; }
        }
        cuts.push_back (best);
        segStart = best;
    }
    cuts.push_back (L);

    std::vector<int> fin;
    for (size_t c = 0; c + 1 < cuts.size(); ++c)
    {
        fin.push_back (cuts[c]);
        findDips (tr, a, cuts[c], cuts[c + 1], so, fin);
    }
    fin.push_back (L);
    for (;;)
    {
        int shortest = -1, sl = so.shortNote;
        for (size_t c = 0; c + 1 < fin.size(); ++c)
            if (fin[c + 1] - fin[c] < sl && fin.size() > 2) { sl = fin[c + 1] - fin[c]; shortest = (int) c; }
        if (shortest < 0) break;
        const size_t c = (size_t) shortest;
        const int lenL = c > 0 ? fin[c] - fin[c - 1] : -1, lenR = c + 2 < fin.size() ? fin[c + 2] - fin[c + 1] : -1;
        fin.erase (fin.begin() + (lenL >= lenR ? (long) c : (long) c + 1));   // drop the boundary next to the longer neighbour
    }

    const int hop = tr.hop;
    for (size_t c = 0; c + 1 < fin.size(); ++c)
    {
        const int rs = fin[c], re = fin[c + 1];
        if (re - rs < so.minFrames) continue;
        const int s = a + rs, e = a + re;
        VocalNote n;
        n.t0 = std::max (0.0, ((double) s * hop - hop * 0.5) / tr.sr);
        n.t1 = ((double) e * hop - hop * 0.5) / tr.sr;
        std::vector<float> mm (m.begin() + s, m.begin() + e);
        n.centre = centreOf (mm);
        n.curve = makeCurve (s, std::move (mm), n.centre);
        double lvSum = 0; int cnt = 0;
        for (int f = s; f < e; ++f) if (tr.midi[(size_t) f] >= 0.f) { lvSum += tr.levelDb[(size_t) f]; ++cnt; }
        n.level = cnt ? (float) (lvSum / cnt) : -40.f;
        n.nt0 = n.t0; n.nt1 = n.t1;
        n.target = n.centre;
        out.push_back (std::move (n));
    }
}
} // namespace

std::vector<VocalNote> segmentNotes (const PitchTrack& tr, const SegmentOptions& so)
{
    std::vector<VocalNote> notes;
    const int N = tr.numFrames();
    if (N < 4) return notes;
    std::vector<float> m = tr.midi;
    std::vector<char> v ((size_t) N);
    for (int i = 0; i < N; ++i) v[(size_t) i] = m[(size_t) i] >= 0.f;

    for (int i = 1; i < N;)
    {
        if (v[(size_t) i]) { ++i; continue; }
        int j = i;
        while (j < N && ! v[(size_t) j]) ++j;
        if (v[(size_t) i - 1] && j < N && j - i <= so.maxGap && std::abs (m[(size_t) j] - m[(size_t) i - 1]) < 1.5f)
            for (int k = i; k < j; ++k)
            {
                m[(size_t) k] = m[(size_t) i - 1] + (m[(size_t) j] - m[(size_t) i - 1]) * (float) (k - i + 1) / (float) (j - i + 1);
                v[(size_t) k] = 1;
            }
        i = j;
    }

    for (int i = 0; i < N;)
    {
        if (! v[(size_t) i]) { ++i; continue; }
        const int a = i;
        while (i < N && v[(size_t) i]) ++i;
        if (i - a >= so.minFrames) splitRun (tr, m, a, i, so, notes);
    }
    return notes;
}

// =================================================================================================================
// Auto + editing
// =================================================================================================================
int nearestAllowedMidi (double midi, uint16_t mask)
{
    int best = (int) std::lround (midi);
    if (mask == 0 || mask == 0x0FFF) return best;
    double bd = 1.0e9;
    for (int k = (int) std::floor (midi) - 7; k <= (int) std::floor (midi) + 8; ++k)
    {
        if (! (mask & (1u << (((k % 12) + 12) % 12)))) continue;
        const double d = std::abs ((double) k - midi);
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

double newToOrig (const VocalNote& n, double t)
{
    const double l = n.nt1 - n.nt0;
    return l > 1.0e-9 ? n.t0 + (t - n.nt0) / l * (n.t1 - n.t0) : n.t0;
}
double origToNew (const VocalNote& n, double t)
{
    const double l = n.t1 - n.t0;
    return l > 1.0e-9 ? n.nt0 + (t - n.t0) / l * (n.nt1 - n.nt0) : n.nt0;
}

static double minGapBetween (const VocalNote& a, const VocalNote& b)
{
    const double g = b.t0 - a.t1;
    return g < 0.02 ? 0.0 : 0.25 * g;
}

void allowedRange (const std::vector<VocalNote>& notes, int i, double& lo, double& hi, double maxTime)
{
    hi = maxTime;
    if (i > 0) lo = notes[(size_t) i - 1].nt1 + minGapBetween (notes[(size_t) i - 1], notes[(size_t) i]);
    else       lo = notes[(size_t) i].t0 < 0.02 ? 0.0 : 0.25 * notes[(size_t) i].t0;
    if ((size_t) i + 1 < notes.size()) hi = notes[(size_t) i + 1].nt0 - minGapBetween (notes[(size_t) i], notes[(size_t) i + 1]);
}

void moveNote (std::vector<VocalNote>& notes, int i, double newStart, double maxTime)
{
    if (i < 0 || (size_t) i >= notes.size()) return;
    auto& n = notes[(size_t) i];
    double lo, hi;
    allowedRange (notes, i, lo, hi, maxTime);
    const double len = n.nt1 - n.nt0;
    if (hi - lo < len) return;
    n.nt0 = std::clamp (newStart, lo, hi - len);
    n.nt1 = n.nt0 + len;
}

void setNoteStart (std::vector<VocalNote>& notes, int i, double t, double maxTime)
{
    if (i < 0 || (size_t) i >= notes.size()) return;
    auto& n = notes[(size_t) i];
    double lo, hi;
    allowedRange (notes, i, lo, hi, maxTime);
    const double minLen = std::max (0.03, 0.25 * n.origLen()), maxLen = 4.0 * n.origLen();
    const double a = std::max (lo, n.nt1 - maxLen), b = n.nt1 - minLen;
    if (b < a) return;
    n.nt0 = std::clamp (t, a, b);
}

void setNoteEnd (std::vector<VocalNote>& notes, int i, double t, double maxTime)
{
    if (i < 0 || (size_t) i >= notes.size()) return;
    auto& n = notes[(size_t) i];
    double lo, hi;
    allowedRange (notes, i, lo, hi, maxTime);
    const double minLen = std::max (0.03, 0.25 * n.origLen()), maxLen = 4.0 * n.origLen();
    const double a = n.nt0 + minLen, b = std::min (hi, n.nt0 + maxLen);
    if (b < a) return;
    n.nt1 = std::clamp (t, a, b);
}

bool splitNote (std::vector<VocalNote>& notes, int i, double tNew, double sr, int hop)
{
    if (i < 0 || (size_t) i >= notes.size()) return false;
    const VocalNote n = notes[(size_t) i];
    if (! n.curve || tNew <= n.nt0 + 0.03 || tNew >= n.nt1 - 0.03) return false;
    const double to = newToOrig (n, tNew);
    const NoteCurve& cv = *n.curve;
    const int L = (int) cv.midi.size();
    const int b = (int) std::llround ((to * sr + hop * 0.5) / hop);
    const int rel = b - cv.f0;
    if (rel < 3 || rel > L - 3) return false;
    const double ts = ((double) b * hop - hop * 0.5) / sr;
    const double tsNew = origToNew (n, ts);

    VocalNote A = n, B = n;
    A.t1 = ts; A.nt1 = tsNew;
    B.t0 = ts; B.nt0 = tsNew;
    std::vector<float> mA (cv.midi.begin(), cv.midi.begin() + rel), mB (cv.midi.begin() + rel, cv.midi.end());
    A.centre = centreOf (mA); A.curve = makeCurve (cv.f0, std::move (mA), A.centre);
    B.centre = centreOf (mB); B.curve = makeCurve (cv.f0 + rel, std::move (mB), B.centre);
    notes[(size_t) i] = A;
    notes.insert (notes.begin() + i + 1, B);
    return true;
}

bool mergeWithNext (std::vector<VocalNote>& notes, int i)
{
    if (i < 0 || (size_t) i + 1 >= notes.size()) return false;
    const VocalNote A = notes[(size_t) i], B = notes[(size_t) i + 1];
    if (! A.curve || ! B.curve) return false;
    std::vector<float> mm = A.curve->midi;
    const int endA = A.curve->f0 + (int) A.curve->midi.size();
    const int gap = std::max (0, B.curve->f0 - endA);
    const float last = mm.back(), first = B.curve->midi.front();
    for (int g = 0; g < gap; ++g) mm.push_back (last + (first - last) * (float) (g + 1) / (float) (gap + 1));
    mm.insert (mm.end(), B.curve->midi.begin(), B.curve->midi.end());

    VocalNote M = A;
    M.t1 = B.t1; M.nt1 = B.nt1;
    M.centre = centreOf (mm);
    M.curve = makeCurve (A.curve->f0, std::move (mm), M.centre);
    const double la = A.origLen(), lb = B.origLen();
    M.level = (float) ((A.level * la + B.level * lb) / std::max (1.0e-6, la + lb));
    M.gainDb = 0.5f * (A.gainDb + B.gainDb);
    M.muted = A.muted && B.muted;
    notes[(size_t) i] = M;
    notes.erase (notes.begin() + i + 1);
    return true;
}

void resetNote (std::vector<VocalNote>& notes, int i, double maxTime)
{
    if (i < 0 || (size_t) i >= notes.size()) return;
    auto& n = notes[(size_t) i];
    n.target = n.centre; n.drift = 1.f; n.vib = 1.f; n.formant = 0.f; n.gainDb = 0.f; n.muted = false;
    n.nt1 = n.nt0 + n.origLen();
    moveNote (notes, i, n.t0, maxTime);
}

double editedLength (const std::vector<VocalNote>& notes, double clipSeconds)
{
    if (notes.empty()) return clipSeconds;
    return notes.back().nt1 + std::max (0.0, clipSeconds - notes.back().t1);
}

int noteAtNewTime (const std::vector<VocalNote>& notes, double t)
{
    for (size_t i = 0; i < notes.size(); ++i)
        if (t >= notes[i].nt0 && t <= notes[i].nt1) return (int) i;
    return -1;
}

void applyAuto (std::vector<VocalNote>& notes, const AutoSettings& s, const std::vector<char>* scope)
{
    const int N = (int) notes.size();
    auto in = [&] (int i) { return ! scope || scope->empty() || (i < (int) scope->size() && (*scope)[(size_t) i]); };
    constexpr double kMax = 1.0e9;

    // loudness target = median level of the notes in scope
    std::vector<float> lv;
    for (int i = 0; i < N; ++i) if (in (i)) lv.push_back (notes[(size_t) i].level);
    float medLevel = -40.f;
    if (! lv.empty()) { std::sort (lv.begin(), lv.end()); medLevel = lv[lv.size() / 2]; }

    for (int i = 0; i < N; ++i)
    {
        if (! in (i)) continue;
        auto& n = notes[(size_t) i];
        const double nearest = (double) nearestAllowedMidi ((double) n.centre, s.mask);
        n.target = n.centre + (float) (nearest - (double) n.centre) * std::clamp (s.correct, 0.f, 1.f);
        n.drift = 1.f - std::clamp (s.flatten, 0.f, 1.f);
        n.vib = std::clamp (s.vibrato, 0.f, 2.f);
        n.formant = 0.f;
        n.muted = false;
        n.gainDb = std::clamp ((medLevel - n.level) * std::clamp (s.level, 0.f, 1.f), -9.f, 9.f);
        n.nt1 = n.nt0 + n.origLen();                      // back to the natural length; position is handled below
    }

    // timing: left to right so every note respects the one before it
    const double grid = 60.0 / std::max (20.0, s.bpm) / (double) std::max (1, s.gridPerBeat);
    for (int i = 0; i < N; ++i)
    {
        if (! in (i)) continue;
        auto& n = notes[(size_t) i];
        double want = n.t0;
        if (s.timing > 0.001f)
        {
            const double ideal = std::round ((n.t0 - s.gridOffsetSec) / grid) * grid + s.gridOffsetSec;
            want = n.t0 + (ideal - n.t0) * (double) std::clamp (s.timing, 0.f, 1.f);
        }
        n.nt0 = n.t0; n.nt1 = n.t0 + n.origLen();
        moveNote (notes, i, want, kMax);
    }
}

// =================================================================================================================
// Rendering
// =================================================================================================================
std::vector<float> renderVocal (const std::vector<float>& src, double sr, int hop, const std::vector<VocalNote>& notes,
                                const RenderOptions& ro, const std::atomic<bool>* cancel)
{
    if (notes.empty() || src.empty()) return src;
    const long long nSrc = (long long) src.size();
    const float* x = src.data();

    struct Seg { int note; double a0, a1, b0, b1; };
    std::vector<Seg> segs;
    {
        double po = 0, pn = 0;
        for (size_t k = 0; k < notes.size(); ++k)
        {
            const auto& n = notes[k];
            const double a0 = std::max (po, n.t0 * sr), a1 = std::max (a0, n.t1 * sr);
            const double b0 = std::max (pn, n.nt0 * sr), b1 = std::max (b0, n.nt1 * sr);
            if (a0 - po > 0.5 || b0 - pn > 0.5) segs.push_back ({ -1, po, a0, pn, b0 });
            segs.push_back ({ (int) k, a0, a1, b0, b1 });
            po = a1; pn = b1;
        }
        segs.push_back ({ -1, po, (double) nSrc, pn, pn + std::max (0.0, (double) nSrc - po) });
    }
    const size_t total = (size_t) std::ceil (segs.back().b1) + 2;
    std::vector<float> acc (total, 0.f), wsum (total, 0.f);

    // ---- pitch offset per output position (note target - note centre), smoothed for the glide between notes
    constexpr int GH = 32;
    const size_t G = total / GH + 3;
    std::vector<float> off (G, 0.f);
    {
        auto noteOff = [&] (int k) { return std::clamp (notes[(size_t) k].target - notes[(size_t) k].centre, -24.f, 24.f); };
        for (size_t s = 0; s < segs.size(); ++s)
        {
            const auto& sg = segs[s];
            float vPrev = 0.f, vNext = 0.f;
            if (sg.note >= 0) vPrev = vNext = noteOff (sg.note);
            else
            {
                const bool hasP = s > 0, hasN = s + 1 < segs.size();
                vPrev = hasP ? noteOff (segs[s - 1].note) : (hasN ? noteOff (segs[s + 1].note) : 0.f);
                vNext = hasN ? noteOff (segs[s + 1].note) : vPrev;
            }
            const size_t g0 = (size_t) std::max (0.0, std::floor (sg.b0 / GH)), g1 = std::min (G - 1, (size_t) std::ceil (sg.b1 / GH));
            const double mid = 0.5 * (sg.b0 + sg.b1);
            for (size_t g = g0; g <= g1; ++g)
                off[g] = sg.note >= 0 ? vPrev : ((double) g * GH < mid ? vPrev : vNext);
        }
        if (ro.glideMs > 0.5f)
        {
            const double tc = (double) ro.glideMs * 0.001 * 0.5;
            const float c = (float) (1.0 - std::exp (-(double) GH / (sr * tc)));
            for (size_t g = 1; g < G; ++g) off[g] = off[g - 1] + c * (off[g] - off[g - 1]);
            for (size_t g = G - 1; g-- > 0;) off[g] = off[g + 1] + c * (off[g] - off[g + 1]);
        }
    }
    auto offAt = [&] (double T)
    {
        const double p = std::max (0.0, T) / GH;
        const size_t i = std::min (G - 2, (size_t) p);
        const float f = (float) (p - (double) i);
        return off[i] + (off[i + 1] - off[i]) * f;
    };

    // Hann lookup
    static const std::vector<float> hannTbl = [] { std::vector<float> t (1026); for (int i = 0; i < 1026; ++i) t[(size_t) i] = (float) (0.5 + 0.5 * std::cos (kPi * (double) i / 1024.0)); return t; }();
    auto addGrain = [&] (double T, double a, double Hw, double phi, float gain)
    {
        const long long o0 = std::max<long long> (0, (long long) std::ceil (T - Hw)), o1 = std::min<long long> ((long long) total - 1, (long long) std::floor (T + Hw));
        const double inv = 1024.0 / Hw;
        for (long long o = o0; o <= o1; ++o)
        {
            const double k = (double) o - T;
            const double wp = std::abs (k) * inv;
            const size_t wi = (size_t) wp;
            const float w = hannTbl[wi] + (hannTbl[wi + 1] - hannTbl[wi]) * (float) (wp - (double) wi);
            acc[(size_t) o] += gain * w * readCR (x, nSrc, a + k * phi);
            wsum[(size_t) o] += w;
        }
    };

    const double Hg = std::max (64.0, std::round (0.006 * sr));
    long long grains = 0;
    for (const auto& sg : segs)
    {
        if (cancel && cancel->load()) return {};
        const double lenB = sg.b1 - sg.b0, lenA = sg.a1 - sg.a0;
        if (lenB < 1.0 || lenA < 1.0) continue;

        if (sg.note < 0)
        {
            for (double T = sg.b0; T < sg.b1 + Hg * 0.5; T += Hg)
            {
                const double u = std::min (1.0, (T - sg.b0) / lenB);
                addGrain (T, sg.a0 + u * lenA, Hg, 1.0, 1.f);
                if ((++grains & 1023) == 0 && cancel && cancel->load()) return {};
            }
            continue;
        }

        const auto& n = notes[(size_t) sg.note];
        if (n.muted || ! n.curve || n.curve->midi.empty()) continue;
        if (lenB < 8.0 || lenA < 8.0) continue;
        const NoteCurve& cv = *n.curve;
        const int L = (int) cv.midi.size();
        const double phi = std::exp2 ((double) n.formant / 12.0 + (double) ro.globalFormant / 12.0);
        const float gain = std::pow (10.f, n.gainDb / 20.f);

        double T = sg.b0, prevA = 0, prevPin = 1.0;
        bool have = false;
        while (T < sg.b1)
        {
            const double u = (T - sg.b0) / lenB;
            const double aTgt = sg.a0 + u * lenA;
            const double fp = std::clamp (aTgt / hop - (double) cv.f0, 0.0, (double) (L - 1));
            const int i0 = (int) fp, i1 = std::min (i0 + 1, L - 1);
            const float fr = (float) (fp - i0);
            const float midiIn = cv.midi[(size_t) i0] + (cv.midi[(size_t) i1] - cv.midi[(size_t) i0]) * fr;
            const float slow = cv.slow[(size_t) i0] + (cv.slow[(size_t) i1] - cv.slow[(size_t) i0]) * fr;
            const float fast = cv.fast[(size_t) i0] + (cv.fast[(size_t) i1] - cv.fast[(size_t) i0]) * fr;
            const double shift = std::clamp ((double) offAt (T) + (double) ((n.drift - 1.f) * slow + (n.vib - 1.f) * fast), -18.0, 18.0);
            const double ratio = std::exp2 (shift / 12.0);
            const double f0 = 440.0 * std::exp2 (((double) midiIn - 69.0) / 12.0);
            const double Pin = std::clamp (sr / f0, sr / 1200.0, sr / 60.0);
            const double Tsyn = Pin / ratio;
            const double Hw = std::max (Pin, Tsyn);

            double a = have ? prevA + prevPin * std::round ((aTgt - prevA) / prevPin) : aTgt;
            while (a > sg.a1 + Pin) a -= Pin;
            while (a < sg.a0 - Pin) a += Pin;

            addGrain (T, a, Hw, phi, gain);
            prevA = a; prevPin = Pin; have = true;
            T += Tsyn;
            if ((++grains & 1023) == 0 && cancel && cancel->load()) return {};
        }
    }

    const size_t outLen = (size_t) std::llround (segs.back().b1);
    std::vector<float> out (std::min (total, outLen));
    for (size_t i = 0; i < out.size(); ++i) out[i] = acc[i] / std::max (wsum[i], 0.6f);
    return out;
}

// =================================================================================================================
// Resampling
// =================================================================================================================
std::vector<float> resampleSinc (const std::vector<float>& in, double srIn, double srOut)
{
    if (in.empty() || std::abs (srIn - srOut) < 1.0e-6) return in;
    const double ratio = srOut / srIn;
    const size_t outN = (size_t) std::floor ((double) in.size() * ratio);
    const double fc = std::min(1.0, ratio) * 0.97;
    constexpr int H = 14;
    const double half = (double) H / fc;
    const int iHalf = (int) std::ceil (half);
    constexpr int RES = 256;
    std::vector<float> tbl ((size_t) (half * RES) + 3);
    for (size_t i = 0; i < tbl.size(); ++i)
    {
        const double x = (double) i / RES;
        const double s = x < 1.0e-9 ? fc : std::sin (kPi * fc * x) / (kPi * x);
        const double w = x < half ? 0.5 + 0.5 * std::cos (kPi * x / half) : 0.0;
        tbl[i] = (float) (s * w);
    }
    std::vector<float> out (outN);
    const long long n = (long long) in.size();
    for (size_t o = 0; o < outN; ++o)
    {
        const double pos = (double) o / ratio;
        const long long c = (long long) std::floor (pos);
        double sum = 0;
        for (long long j = c - iHalf + 1; j <= c + iHalf; ++j)
        {
            if (j < 0 || j >= n) continue;
            const double d = std::abs (pos - (double) j) * RES;
            const size_t di = (size_t) d;
            if (di + 1 >= tbl.size()) continue;
            sum += (double) in[(size_t) j] * (double) (tbl[di] + (tbl[di + 1] - tbl[di]) * (float) (d - (double) di));
        }
        out[o] = (float) sum;
    }
    return out;
}

// =================================================================================================================
// Persistence
// =================================================================================================================
namespace
{
template <typename T> void put (std::vector<uint8_t>& b, T v) { const auto* p = reinterpret_cast<const uint8_t*> (&v); b.insert (b.end(), p, p + sizeof (T)); }
template <typename T> bool get (const uint8_t*& p, const uint8_t* end, T& v)
{
    if (p + sizeof (T) > end) return false;
    std::memcpy (&v, p, sizeof (T)); p += sizeof (T); return true;
}
}

std::vector<uint8_t> serialiseNotes (const std::vector<VocalNote>& notes, double sr, int hop)
{
    std::vector<uint8_t> b;
    put<uint32_t> (b, 0x314e4556u);                 // "VEN1"
    put<double> (b, sr); put<int32_t> (b, hop); put<int32_t> (b, (int32_t) notes.size());
    for (const auto& n : notes)
    {
        put<double> (b, n.t0); put<double> (b, n.t1); put<double> (b, n.nt0); put<double> (b, n.nt1);
        put<float> (b, n.centre); put<float> (b, n.level); put<float> (b, n.target); put<float> (b, n.drift);
        put<float> (b, n.vib); put<float> (b, n.formant); put<float> (b, n.gainDb); put<uint8_t> (b, n.muted ? 1 : 0);
        const int32_t L = n.curve ? (int32_t) n.curve->midi.size() : 0;
        put<int32_t> (b, n.curve ? n.curve->f0 : 0); put<int32_t> (b, L);
        for (int i = 0; i < L; ++i) put<float> (b, n.curve->midi[(size_t) i]);
    }
    return b;
}

bool deserialiseNotes (const uint8_t* data, size_t size, std::vector<VocalNote>& out, double& sr, int& hop)
{
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    uint32_t magic = 0; int32_t h = 0, cnt = 0; double s = 0;
    if (! get (p, end, magic) || magic != 0x314e4556u) return false;
    if (! get (p, end, s) || ! get (p, end, h) || ! get (p, end, cnt) || cnt < 0 || cnt > 100000 || h <= 0 || s <= 0) return false;
    std::vector<VocalNote> v;
    for (int k = 0; k < cnt; ++k)
    {
        VocalNote n;
        uint8_t mu = 0; int32_t f0 = 0, L = 0;
        if (! get (p, end, n.t0) || ! get (p, end, n.t1) || ! get (p, end, n.nt0) || ! get (p, end, n.nt1)
            || ! get (p, end, n.centre) || ! get (p, end, n.level) || ! get (p, end, n.target) || ! get (p, end, n.drift)
            || ! get (p, end, n.vib) || ! get (p, end, n.formant) || ! get (p, end, n.gainDb) || ! get (p, end, mu)
            || ! get (p, end, f0) || ! get (p, end, L) || L < 1 || L > 2000000 || p + (size_t) L * sizeof (float) > end)
            return false;
        std::vector<float> m ((size_t) L);
        std::memcpy (m.data(), p, (size_t) L * sizeof (float));
        p += (size_t) L * sizeof (float);
        n.muted = mu != 0;
        n.curve = makeCurve (f0, std::move (m), n.centre);
        v.push_back (std::move (n));
    }
    out = std::move (v); sr = s; hop = h;
    return true;
}
} // namespace aio

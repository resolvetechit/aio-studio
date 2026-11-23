#include "Speakers.h"
#include "Podcast.h"
#include "../dsp/Biquad.h"
#include "../dsp/AutoVoice.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace aio
{
namespace podcast
{
// =================================================================================================================
// map
// =================================================================================================================
int SpeakerMap::speakerAt (long long pos) const
{
    if (cutPos.empty()) return 0;
    const auto it = std::upper_bound (cutPos.begin(), cutPos.end(), pos);
    const size_t i = it == cutPos.begin() ? 0 : (size_t) (it - cutPos.begin()) - 1;
    return cutSpk[i];
}

int SpeakerMap::chunkAt (long long pos) const
{
    if (chunks.empty()) return -1;
    int best = 0;
    long long bestD = std::numeric_limits<long long>::max();
    for (size_t i = 0; i < chunks.size(); ++i)
    {
        const auto& c = chunks[i];
        const long long d = pos < c.a ? c.a - pos : (pos >= c.b ? pos - c.b + 1 : 0);
        if (d < bestD) { bestD = d; best = (int) i; }
        if (d == 0) break;
    }
    return best;
}

void SpeakerMap::rebuild()
{
    for (auto& s : speaker) s = {};
    cutPos.clear(); cutSpk.clear();
    if (chunks.empty() || label.size() != chunks.size()) { count = chunks.empty() ? 0 : 1; valid = false; return; }

    std::array<double, kMaxSpeakers> sec {}, pw {};
    std::array<std::array<double, kSpkBands>, kMaxSpeakers> band {};
    std::array<std::vector<float>, kMaxSpeakers> f0s;
    for (size_t i = 0; i < chunks.size(); ++i)
    {
        const int s = std::clamp (label[i], 0, kMaxSpeakers - 1);
        const auto& c = chunks[i];
        const double w = std::max (0.05f, c.seconds);
        sec[(size_t) s] += w;
        pw[(size_t) s] += w * std::pow (10.0, (double) c.levelDb / 10.0);
        for (int k = 0; k < kSpkBands; ++k) band[(size_t) s][(size_t) k] += w * (double) c.bandPow[(size_t) k];
        f0s[(size_t) s].push_back (c.f0);
    }
    for (int s = 0; s < kMaxSpeakers; ++s)
    {
        auto& sp = speaker[(size_t) s];
        if (sec[(size_t) s] <= 0.0) continue;
        sp.valid = true;
        sp.seconds = (float) sec[(size_t) s];
        sp.levelDb = (float) (10.0 * std::log10 (pw[(size_t) s] / sec[(size_t) s] + 1.0e-12));
        auto& f = f0s[(size_t) s];
        std::nth_element (f.begin(), f.begin() + (long) f.size() / 2, f.end());
        sp.f0 = f[f.size() / 2];
        for (int k = 0; k < kSpkBands; ++k) sp.bandDb[(size_t) k] = (float) (10.0 * std::log10 (band[(size_t) s][(size_t) k] / sec[(size_t) s] + 1.0e-14));
    }
    int n = 0;
    for (int s = 0; s < kMaxSpeakers; ++s) if (speaker[(size_t) s].valid) n = s + 1;
    count = std::max (1, n);

    cutPos.push_back (0); cutSpk.push_back (label[0]);
    for (size_t i = 1; i < chunks.size(); ++i)
        if (label[i] != label[i - 1])
        {
            const long long a = chunks[i - 1].b, b = chunks[i].a;
            cutPos.push_back (b >= a ? (a + b) / 2 : b);
            cutSpk.push_back (label[i]);
        }
    valid = true;
}

// =================================================================================================================
// clustering
// =================================================================================================================
namespace
{
constexpr int D = kCepDims + 1;
using Vec = std::array<double, D>;

inline double dist2 (const Vec& a, const Vec& b)
{
    double s = 0.0;
    for (int d = 0; d < D; ++d) { const double e = a[(size_t) d] - b[(size_t) d]; s += e * e; }
    return s;
}

// Viterbi labelling: every chunk pays its distance to its voice, a change of voice between neighbouring chunks pays `lambda`
void viterbi (const std::vector<Vec>& X, const std::vector<Vec>& C, double lambda, std::vector<int>& lab)
{
    const size_t n = X.size(), K = C.size();
    lab.assign (n, 0);
    if (n == 0 || K == 0) return;
    std::vector<double> dp (K), nd (K);
    std::vector<signed char> from (n * K, 0);
    for (size_t c = 0; c < K; ++c) dp[c] = dist2 (X[0], C[c]) / D;
    for (size_t i = 1; i < n; ++i)
    {
        size_t bi = 0;
        for (size_t c = 1; c < K; ++c) if (dp[c] < dp[bi]) bi = c;
        for (size_t c = 0; c < K; ++c)
        {
            const double stay = dp[c], sw = dp[bi] + lambda;
            const bool useStay = stay <= sw || bi == c;
            nd[c] = (useStay ? stay : sw) + dist2 (X[i], C[c]) / D;
            from[i * K + c] = (signed char) (useStay ? (int) c : (int) bi);
        }
        dp = nd;
    }
    size_t c = 0;
    for (size_t k = 1; k < K; ++k) if (dp[k] < dp[c]) c = k;
    for (size_t i = n; i-- > 0;) { lab[i] = (int) c; if (i > 0) c = (size_t) from[i * K + c]; }
}

void centroidsOf (const std::vector<Vec>& X, const std::vector<int>& lab, int K, std::vector<Vec>& C)
{
    std::vector<int> cnt ((size_t) K, 0);
    for (auto& c : C) c.fill (0.0);
    for (size_t i = 0; i < X.size(); ++i)
    {
        ++cnt[(size_t) lab[i]];
        for (int d = 0; d < D; ++d) C[(size_t) lab[i]][(size_t) d] += X[i][(size_t) d];
    }
    for (int k = 0; k < K; ++k)
        if (cnt[(size_t) k] > 0) for (int d = 0; d < D; ++d) C[(size_t) k][(size_t) d] /= (double) cnt[(size_t) k];
}

double kmeansPlusPlus (const std::vector<Vec>& X, int K, unsigned seed, std::vector<Vec>& C, std::vector<int>& lab)
{
    std::mt19937 rng (seed);
    const size_t n = X.size();
    C.assign ((size_t) K, Vec {});
    C[0] = X[std::uniform_int_distribution<size_t> (0, n - 1) (rng)];
    std::vector<double> dmin (n, 1.0e30);
    for (int k = 1; k < K; ++k)
    {
        double tot = 0.0;
        for (size_t i = 0; i < n; ++i) { dmin[i] = std::min (dmin[i], dist2 (X[i], C[(size_t) k - 1])); tot += dmin[i]; }
        double pick = std::uniform_real_distribution<double> (0.0, std::max (tot, 1.0e-12)) (rng);
        size_t chosen = n - 1;
        for (size_t i = 0; i < n; ++i) { pick -= dmin[i]; if (pick <= 0.0) { chosen = i; break; } }
        C[(size_t) k] = X[chosen];
    }
    lab.assign (n, 0);
    for (int it = 0; it < 30; ++it)
    {
        bool changed = false;
        for (size_t i = 0; i < n; ++i)
        {
            int best = 0; double bd = 1.0e30;
            for (int k = 0; k < K; ++k) { const double d = dist2 (X[i], C[(size_t) k]); if (d < bd) { bd = d; best = k; } }
            if (best != lab[i]) { lab[i] = best; changed = true; }
        }
        centroidsOf (X, lab, K, C);
        if (! changed) break;
    }
    double inertia = 0.0;
    for (size_t i = 0; i < n; ++i) inertia += dist2 (X[i], C[(size_t) lab[i]]);
    return inertia;
}

double silhouetteOf (const std::vector<Vec>& X, const std::vector<int>& lab, int K)
{
    const size_t n = X.size();
    const size_t stride = std::max<size_t> (1, n / 700);
    std::vector<size_t> pick;
    for (size_t i = 0; i < n; i += stride) pick.push_back (i);
    double total = 0.0; int cnt = 0;
    for (size_t pi = 0; pi < pick.size(); ++pi)
    {
        const size_t i = pick[pi];
        std::vector<double> sum ((size_t) K, 0.0); std::vector<int> num ((size_t) K, 0);
        for (size_t pj = 0; pj < pick.size(); ++pj)
        {
            if (pj == pi) continue;
            const size_t j = pick[pj];
            sum[(size_t) lab[j]] += std::sqrt (dist2 (X[i], X[j])); ++num[(size_t) lab[j]];
        }
        const int own = lab[i];
        if (num[(size_t) own] == 0) continue;
        const double a = sum[(size_t) own] / num[(size_t) own];
        double b = 1.0e30;
        for (int k = 0; k < K; ++k) if (k != own && num[(size_t) k] > 0) b = std::min (b, sum[(size_t) k] / num[(size_t) k]);
        if (b > 1.0e29) continue;
        total += (b - a) / std::max (a, b); ++cnt;
    }
    return cnt > 0 ? total / cnt : 0.0;
}

struct Grouping { std::vector<int> lab; double sil = -1.0; bool shareOk = false; double minSep = 0.0; };

Grouping groupInto (const std::vector<Vec>& X, int K, const std::vector<double>& secs)
{
    Grouping g;
    std::vector<Vec> C; std::vector<int> lab, bestLab; std::vector<Vec> bestC;
    double bestInertia = 1.0e300;
    for (unsigned r = 0; r < 6; ++r)
    {
        const double in = kmeansPlusPlus (X, K, 1234u + 77u * r, C, lab);
        if (in < bestInertia) { bestInertia = in; bestLab = lab; bestC = C; }
    }
    // temporal smoothing: re-estimate the voices while labelling with a penalty for changing voice between neighbouring chunks
    for (int it = 0; it < 8; ++it)
    {
        std::vector<int> nl;
        viterbi (X, bestC, 1.1, nl);
        std::vector<int> cnt ((size_t) K, 0);
        for (int l : nl) ++cnt[(size_t) l];
        bool empty = false; for (int c : cnt) if (c == 0) empty = true;
        if (empty) break;
        const bool same = nl == bestLab;
        bestLab = nl;
        centroidsOf (X, bestLab, K, bestC);
        if (same) break;
    }
    g.lab = bestLab;
    std::vector<double> share ((size_t) K, 0.0); std::vector<int> cnt ((size_t) K, 0);
    double tot = 0.0;
    for (size_t i = 0; i < X.size(); ++i) { share[(size_t) g.lab[i]] += secs[i]; ++cnt[(size_t) g.lab[i]]; tot += secs[i]; }
    g.shareOk = true;
    for (int k = 0; k < K; ++k) if (share[(size_t) k] < 0.05 * tot || cnt[(size_t) k] < 3) g.shareOk = false;
    g.sil = silhouetteOf (X, g.lab, K);
    // the silhouette doesn't care about scale: it would happily split one voice's tiny natural variation in two. Also demand an absolute gap
    // between the voices (in the floored, weighted feature units: a man and a woman are ~20 apart, two similar women ~6, one voice < 1).
    std::vector<Vec> cc ((size_t) K);
    centroidsOf (X, g.lab, K, cc);
    g.minSep = 1.0e30;
    for (int a = 0; a < K; ++a) for (int b = a + 1; b < K; ++b) g.minSep = std::min (g.minSep, std::sqrt (dist2 (cc[(size_t) a], cc[(size_t) b])));
    return g;
}
} // namespace

void clusterSpeakers (SpeakerMap& m, int requested)
{
    m.requested = requested;
    const size_t n = m.chunks.size();
    m.label.assign (n, 0);
    m.separation = 0.f;
    if (n == 0) { m.count = 0; m.valid = false; m.cutPos.clear(); m.cutSpk.clear(); return; }

    std::vector<int> result (n, 0);
    int chosenK = 1;
    if (n >= 8 && requested != 1)
    {
        // standardise (with floors so a clip of one voice isn't stretched into several)
        std::vector<Vec> X (n);
        std::vector<double> secs (n);
        Vec mu {}, sd {};
        for (size_t i = 0; i < n; ++i) { secs[i] = std::max (0.05f, m.chunks[i].seconds); for (int d = 0; d < D; ++d) mu[(size_t) d] += (double) m.chunks[i].feat[d]; }
        for (int d = 0; d < D; ++d) mu[(size_t) d] /= (double) n;
        for (size_t i = 0; i < n; ++i) for (int d = 0; d < D; ++d) { const double e = (double) m.chunks[i].feat[d] - mu[(size_t) d]; sd[(size_t) d] += e * e; }
        for (int d = 0; d < D; ++d)
        {
            sd[(size_t) d] = std::sqrt (sd[(size_t) d] / (double) n);
            sd[(size_t) d] = std::max (sd[(size_t) d], d == D - 1 ? 0.12 : 0.35);      // pitch: at least 0.12 octave; cepstrum: a floor of its own
        }
        const double wgt[D] = { 1, 1, 1, 1, 1, 1, 2.5 };
        for (size_t i = 0; i < n; ++i) for (int d = 0; d < D; ++d) X[i][(size_t) d] = ((double) m.chunks[i].feat[d] - mu[(size_t) d]) / sd[(size_t) d] * wgt[d];

        const int maxK = std::min<int> (kMaxSpeakers, (int) n / 4);
        if (requested >= 2)
        {
            const int K = std::min (requested, std::max (2, maxK));
            result = groupInto (X, K, secs).lab; chosenK = K;
            m.separation = (float) std::max (0.0, silhouetteOf (X, result, K));
        }
        else
        {
            double bestScore = 0.14;                  // below this the voices are not distinct enough to treat them differently
            for (int K = 2; K <= maxK; ++K)
            {
                const auto g = groupInto (X, K, secs);
                if (! g.shareOk || g.minSep < 2.0) continue;
                const double score = g.sil - 0.015 * (K - 2);
                if (score > bestScore) { bestScore = score; result = g.lab; chosenK = K; m.separation = (float) g.sil; }
            }
        }
    }
    // number the voices by how much they speak (voice 1 = the one who talks most)
    std::array<double, kMaxSpeakers> sec {};
    for (size_t i = 0; i < n; ++i) sec[(size_t) std::clamp (result[i], 0, kMaxSpeakers - 1)] += std::max (0.05f, m.chunks[i].seconds);
    std::vector<int> order ((size_t) kMaxSpeakers);
    std::iota (order.begin(), order.end(), 0);
    std::stable_sort (order.begin(), order.end(), [&] (int a, int b) { return sec[(size_t) a] > sec[(size_t) b]; });
    std::array<int, kMaxSpeakers> remap {};
    for (int r = 0; r < kMaxSpeakers; ++r) remap[(size_t) order[(size_t) r]] = r;
    for (size_t i = 0; i < n; ++i) m.label[i] = remap[(size_t) std::clamp (result[i], 0, kMaxSpeakers - 1)];
    (void) chosenK;
    m.rebuild();
}

// =================================================================================================================
// plan + runtime EQ
// =================================================================================================================
SpeakerPlan makeSpeakerPlan (const SpeakerMap& m, float tone, float balance)
{
    SpeakerPlan p;
    if (! m.valid || m.count < 2) return p;
    p.active = true; p.count = m.count;
    double refW = 0.0, refSum = 0.0;
    for (int s = 0; s < m.count; ++s) if (m.speaker[(size_t) s].valid) { refW += m.speaker[(size_t) s].seconds; refSum += (double) m.speaker[(size_t) s].seconds * (double) m.speaker[(size_t) s].levelDb; }
    const double ref = refW > 0.0 ? refSum / refW : -30.0;
    for (int s = 0; s < m.count; ++s)
    {
        const auto& sp = m.speaker[(size_t) s];
        p.eqDb[(size_t) s].fill (0.f); p.trimDb[(size_t) s] = 0.f;
        if (! sp.valid) continue;
        const float conf = std::min (1.f, sp.seconds / 8.f);
        if (tone > 0.01f)
        {
            std::array<float, kSpkBands> corr {};
            AutoVoice::computeCorrection (sp.bandDb, VoxSpeech, tone, corr);
            for (int k = 0; k < kSpkBands; ++k) p.eqDb[(size_t) s][(size_t) k] = corr[(size_t) k] * conf;
        }
        p.trimDb[(size_t) s] = (float) std::clamp (ref - (double) sp.levelDb, -6.0, 6.0) * std::clamp (balance, 0.f, 1.f) * conf;
    }
    return p;
}

struct SpeakerEq::Chain
{
    std::array<std::array<std::array<Biquad, kSpkBands>, 2>, kMaxSpeakers> f;     // [voice][channel][band]
    std::array<std::array<bool, kSpkBands>, kMaxSpeakers> on {};
};

void SpeakerEq::prepare (double sampleRate, bool st)
{
    sr = sampleRate; stereo = st;
    chains = std::make_shared<Chain>();
    alpha = 1.0 - std::exp (-1.0 / (sr * 0.012));
    for (auto& v : w) v = 0.f;
    idx = 0;
    setPlan (plan);
}

void SpeakerEq::setPlan (const SpeakerPlan& p)
{
    plan = p;
    if (! chains) chains = std::make_shared<Chain>();
    for (int s = 0; s < kMaxSpeakers; ++s)
    {
        trimLin[s] = (float) std::pow (10.0, (double) plan.trimDb[(size_t) s] / 20.0);
        for (int k = 0; k < kSpkBands; ++k)
        {
            const float g = plan.eqDb[(size_t) s][(size_t) k];
            const bool on = std::abs (g) > 0.05f;
            if (on != chains->on[(size_t) s][(size_t) k] || on)
            {
                const FilterType t = k == 0 ? FilterType::LowShelf : (k == kSpkBands - 1 ? FilterType::HighShelf : FilterType::Bell);
                const double f = k == 0 ? 150.0 : (k == kSpkBands - 1 ? 9000.0 : (double) AutoVoice::kBandHz[(size_t) k]);
                const auto c = Coeffs::make (t, sr, f, (double) g, k == 0 || k == kSpkBands - 1 ? 0.71 : 1.0);
                for (int ch = 0; ch < 2; ++ch) { chains->f[(size_t) s][(size_t) ch][(size_t) k].c = c; if (! on) chains->f[(size_t) s][(size_t) ch][(size_t) k].reset(); }
            }
            chains->on[(size_t) s][(size_t) k] = on;
        }
    }
}

void SpeakerEq::process (const SpeakerMap& m, float* l, float* r, int n, long long pos)
{
    if (! plan.active || ! chains || ! m.valid || m.cutPos.empty()) return;
    const int S = std::min (plan.count, kMaxSpeakers);
    // find the cut that is in force at `pos`
    {
        const auto it = std::upper_bound (m.cutPos.begin(), m.cutPos.end(), pos);
        idx = it == m.cutPos.begin() ? 0 : (size_t) (it - m.cutPos.begin()) - 1;
    }
    const int nch = stereo ? 2 : 1;
    for (int i = 0; i < n; ++i)
    {
        const long long p = pos + i;
        while (idx + 1 < m.cutPos.size() && m.cutPos[idx + 1] <= p) ++idx;
        const int tgt = std::clamp (m.cutSpk[idx], 0, S - 1);
        for (int s = 0; s < S; ++s) w[s] += (float) (alpha * ((s == tgt ? 1.0 : 0.0) - (double) w[s]));
        float outL = 0.f, outR = 0.f;
        for (int s = 0; s < S; ++s)
        {
            // every voice's cascade runs all the time (so its filters are "warm" when the voice becomes the active one: no start-up click)
            float yl = l[i], yr = stereo ? r[i] : 0.f;
            for (int k = 0; k < kSpkBands; ++k)
            {
                if (! chains->on[(size_t) s][(size_t) k]) continue;
                yl = chains->f[(size_t) s][0][(size_t) k].tick (yl);
                if (nch > 1) yr = chains->f[(size_t) s][1][(size_t) k].tick (yr);
            }
            const float g = w[s] * trimLin[s];
            outL += g * yl; outR += g * yr;
        }
        l[i] = outL;
        r[i] = stereo ? outR : outL;
    }
}

// =================================================================================================================
// detection
// =================================================================================================================
namespace
{
constexpr int kDecRate = 8000;
constexpr int kWinD = 320, kHopD = 160;       // 40 ms window, 20 ms hop at 8 kHz
constexpr int kLagMin = 19, kLagMax = 114;    // 420 Hz .. 70 Hz
constexpr int kMel = 20;
constexpr int kFullN = 1024;

struct Frame
{
    float db = -120.f, f0 = 0.f, conf = 0.f;
    float cep[kCepDims] {};
    std::array<float, kSpkBands> band {};
};

class FrameAnalyser
{
public:
    FrameAnalyser (double sampleRate, const NoiseProfile& np)
        : sr (sampleRate), fftD (9), fftF (10), noise (np)
    {
        hannD.resize ((size_t) kWinD); hannF.resize ((size_t) kFullN);
        for (int i = 0; i < kWinD; ++i) hannD[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * (i + 0.5) / kWinD));
        for (int i = 0; i < kFullN; ++i) hannF[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * (i + 0.5) / kFullN));
        wac.assign ((size_t) kLagMax + 2, 0.0);
        for (int t = 0; t <= kLagMax + 1; ++t) { double s = 0.0; for (int i = 0; i + t < kWinD; ++i) s += (double) hannD[(size_t) i] * hannD[(size_t) (i + t)]; wac[(size_t) t] = s; }
        // mel filterbank over the 512-point FFT at 8 kHz
        auto mel = [] (double f) { return 2595.0 * std::log10 (1.0 + f / 700.0); };
        auto imel = [] (double m) { return 700.0 * (std::pow (10.0, m / 2595.0) - 1.0); };
        const double m0 = mel (120.0), m1 = mel (3600.0);
        std::vector<double> edge ((size_t) kMel + 2);
        for (int j = 0; j < kMel + 2; ++j) edge[(size_t) j] = imel (m0 + (m1 - m0) * j / (kMel + 1));
        melW.assign ((size_t) kMel * 257, 0.f);
        for (int j = 0; j < kMel; ++j)
            for (int b = 0; b < 257; ++b)
            {
                const double f = b * (double) kDecRate / 512.0;
                double v = 0.0;
                if (f > edge[(size_t) j] && f <= edge[(size_t) j + 1]) v = (f - edge[(size_t) j]) / (edge[(size_t) j + 1] - edge[(size_t) j]);
                else if (f > edge[(size_t) j + 1] && f < edge[(size_t) j + 2]) v = (edge[(size_t) j + 2] - f) / (edge[(size_t) j + 2] - edge[(size_t) j + 1]);
                melW[(size_t) j * 257 + (size_t) b] = (float) v;
            }
        dct.assign ((size_t) kCepDims * kMel, 0.f);
        for (int m = 0; m < kCepDims; ++m)
            for (int j = 0; j < kMel; ++j) dct[(size_t) m * kMel + (size_t) j] = (float) (std::cos (3.14159265358979 * (m + 1) * (j + 0.5) / kMel) * std::sqrt (2.0 / kMel));
        // octave bands on the 1024-point FFT of the full-rate signal
        const double nyq = sr * 0.5;
        for (int k = 0; k < kSpkBands; ++k)
        {
            const double fc = (double) AutoVoice::kBandHz[(size_t) k];
            double lo = fc / 1.41421356, hi = std::min (fc * 1.41421356, nyq * 0.98);
            if (k == 0) lo = 60.0;
            bandLo[k] = std::max (1, (int) std::ceil (lo * kFullN / sr)); bandHi[k] = std::max (bandLo[k], std::min (kFullN / 2 - 1, (int) std::floor (hi * kFullN / sr)));
        }
        fx.assign ((size_t) 2 * kFullN, 0.f); fd.assign ((size_t) 2 * 512, 0.f);
        // noise power per full-rate bin (the profile was made with a 2048 sine window, this uses a 1024 Hann window: 0.375 of the power)
        noiseBand.fill (0.0);
        if (noise.valid && noise.psd.size() == (size_t) (kN2 / 2 + 1))
            for (int k = 0; k < kSpkBands; ++k)
                for (int b = bandLo[k]; b <= bandHi[k]; ++b) noiseBand[(size_t) k] += 0.375 * noise.psd[(size_t) std::min (2 * b, kN2 / 2)];
    }

    // x: kFullN full-rate samples, d: kWinD decimated samples
    void analyse (const float* x, const float* d, Frame& f)
    {
        double ms = 0.0;
        for (int i = 0; i < kFullN; ++i) ms += (double) x[i] * x[i];
        ms /= kFullN;
        f.db = (float) (10.0 * std::log10 (ms + 1.0e-12));
        f.f0 = 0.f; f.conf = 0.f;
        if (f.db < -75.f) return;

        // ---- pitch: normalised autocorrelation of the windowed 8 kHz frame
        float xw[kWinD];
        double r0 = 0.0;
        for (int i = 0; i < kWinD; ++i) { xw[i] = d[i] * hannD[(size_t) i]; r0 += (double) xw[i] * xw[i]; }
        if (r0 > 1.0e-12)
        {
            double r[kLagMax + 2];
            double best = 0.0;
            for (int t = kLagMin - 1; t <= kLagMax + 1; ++t)
            {
                double s = 0.0;
                for (int i = 0; i + t < kWinD; ++i) s += (double) xw[i] * xw[i + t];
                r[t] = std::min (1.0, (s / r0) / (wac[(size_t) t] / wac[0]));
                if (t >= kLagMin && t <= kLagMax) best = std::max (best, r[t]);
            }
            int pick = -1;
            for (int t = kLagMin; t <= kLagMax; ++t)
                if (r[t] >= 0.88 * best && r[t] >= r[t - 1] && r[t] >= r[t + 1]) { pick = t; break; }
            if (pick > 0)
            {
                const double a1 = r[pick - 1], a2 = r[pick], a3 = r[pick + 1];
                const double den = a1 - 2.0 * a2 + a3;
                const double off = std::abs (den) > 1.0e-9 ? std::clamp (0.5 * (a1 - a3) / den, -0.5, 0.5) : 0.0;
                f.f0 = (float) ((double) kDecRate / ((double) pick + off));
                f.conf = (float) a2;
            }
        }

        // ---- timbre: mel cepstrum of the same frame
        for (int i = 0; i < kWinD; ++i) fd[(size_t) i] = xw[i];
        std::fill (fd.begin() + kWinD, fd.end(), 0.f);
        fftD.performRealOnlyForwardTransform (fd.data(), true);
        double pw[257];
        for (int b = 0; b < 257; ++b) pw[b] = (double) fd[(size_t) 2 * b] * fd[(size_t) 2 * b] + (double) fd[(size_t) 2 * b + 1] * fd[(size_t) 2 * b + 1];
        float lm[kMel];
        for (int j = 0; j < kMel; ++j)
        {
            double e = 0.0;
            for (int b = 0; b < 257; ++b) e += (double) melW[(size_t) j * 257 + (size_t) b] * pw[b];
            lm[j] = (float) std::log (e + 1.0e-9);
        }
        for (int m = 0; m < kCepDims; ++m)
        {
            double s = 0.0;
            for (int j = 0; j < kMel; ++j) s += (double) dct[(size_t) m * kMel + (size_t) j] * lm[j];
            f.cep[m] = (float) s;
        }

        // ---- octave-band spectrum at the full rate
        for (int i = 0; i < kFullN; ++i) fx[(size_t) i] = x[i] * hannF[(size_t) i];
        std::fill (fx.begin() + kFullN, fx.end(), 0.f);
        fftF.performRealOnlyForwardTransform (fx.data(), true);
        for (int k = 0; k < kSpkBands; ++k)
        {
            double e = 0.0;
            for (int b = bandLo[k]; b <= bandHi[k]; ++b) e += (double) fx[(size_t) 2 * b] * fx[(size_t) 2 * b] + (double) fx[(size_t) 2 * b + 1] * fx[(size_t) 2 * b + 1];
            const double clean = std::max (e - 0.7 * noiseBand[(size_t) k], 0.15 * e);
            f.band[(size_t) k] = (float) clean;
        }
    }

private:
    static constexpr int kN2 = 2048;
    double sr;
    juce::dsp::FFT fftD, fftF;
    const NoiseProfile& noise;
    std::vector<float> hannD, hannF, melW, dct, fx, fd;
    std::vector<double> wac;
    int bandLo[kSpkBands] {}, bandHi[kSpkBands] {};
    std::array<double, kSpkBands> noiseBand {};
};
} // namespace

void detectSpeakers (const ReadFn& src, long long len, double sr, double noiseFloorDb, const NoiseProfile& np, const Progress& prog, SpeakerMap& out)
{
    out = {};
    out.length = len; out.sampleRate = sr;
    if (len < (long long) (3.0 * sr) || sr < 8000.0) return;

    FrameAnalyser fa (sr, np);
    const double step = sr / (double) kDecRate;
    const double hopFull = 0.02 * sr;
    Biquad lp[2];
    lp[0].c = Coeffs::make (FilterType::LowPass, sr, std::min (3400.0, sr * 0.45), 0.0, 0.5412);
    lp[1].c = Coeffs::make (FilterType::LowPass, sr, std::min (3400.0, sr * 0.45), 0.0, 1.3066);

    std::vector<Frame> frames;
    frames.reserve ((size_t) (len / hopFull) + 4);
    std::vector<float> full, dec;                 // working windows of the mid signal at full rate and at 8 kHz
    long long fullBase = 0, decBase = 0, decCount = 0;
    double nextPos = 0.0, prevY = 0.0;
    long long inIndex = 0;
    long long frameK = 0;

    constexpr int C = 1 << 16;
    std::vector<float> l ((size_t) C), r ((size_t) C);
    for (long long pos = 0; pos < len; pos += C)
    {
        if (prog && ! prog ((float) pos / (float) len)) return;
        const int n = (int) std::min<long long> (C, len - pos);
        if (! src (l.data(), r.data(), pos, n)) return;
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (l[(size_t) i] + r[(size_t) i]);
            full.push_back (m);
            const double y = lp[1].tick (lp[0].tick (m));
            while (nextPos <= (double) inIndex)
            {
                const double frac = nextPos - (double) (inIndex - 1);
                dec.push_back ((float) (prevY + std::clamp (frac, 0.0, 1.0) * (y - prevY)));
                ++decCount;
                nextPos += step;
            }
            prevY = y; ++inIndex;
        }
        // frames that are complete in both streams
        for (;;)
        {
            const long long fs = (long long) std::llround ((double) frameK * hopFull), ds = frameK * kHopD;
            if (fs + kFullN > fullBase + (long long) full.size() || ds + kWinD > decBase + (long long) dec.size()) break;
            Frame fr;
            fa.analyse (full.data() + (fs - fullBase), dec.data() + (ds - decBase), fr);
            frames.push_back (fr);
            ++frameK;
        }
        const long long keepF = (long long) std::llround ((double) frameK * hopFull), keepD = frameK * kHopD;
        if (keepF > fullBase) { full.erase (full.begin(), full.begin() + (keepF - fullBase)); fullBase = keepF; }
        if (keepD > decBase) { dec.erase (dec.begin(), dec.begin() + (keepD - decBase)); decBase = keepD; }
    }
    if (frames.size() < 100) return;

    // ---- which frames are speech: relative to the loud frames of this file
    std::vector<float> dbs;
    dbs.reserve (frames.size());
    for (const auto& f : frames) if (f.db > -90.f) dbs.push_back (f.db);
    if (dbs.size() < 100) return;
    std::nth_element (dbs.begin(), dbs.begin() + (long) (dbs.size() * 9 / 10), dbs.end());
    const double p90 = dbs[dbs.size() * 9 / 10];
    const double thr = std::max (p90 - 16.0, noiseFloorDb + 9.0);

    // ---- chunks: speech runs, split at pauses of 0.25 s, at most 6 s
    const int gap = 13, maxLen = 300, minLen = 30;
    std::vector<std::pair<size_t, size_t>> runs;
    {
        const size_t nF = frames.size();
        size_t i = 0;
        while (i < nF)
        {
            if (frames[i].db <= thr) { ++i; continue; }
            size_t j = i, last = i;
            while (j < nF && (frames[j].db > thr || j - last < (size_t) gap))
            {
                if (frames[j].db > thr) last = j;
                ++j;
                if (j - i >= (size_t) maxLen) break;
            }
            runs.push_back ({ i, last + 1 });
            i = std::max (last + 1, i + 1);
        }
    }
    for (auto [a, b] : runs)
    {
        if (b - a < (size_t) minLen) continue;
        std::vector<size_t> voiced;
        for (size_t k = a; k < b; ++k) if (frames[k].conf > 0.65f && frames[k].db > thr - 4.0 && frames[k].f0 > 0.f) voiced.push_back (k);
        if (voiced.size() < 12) continue;
        std::vector<float> f0s;
        for (size_t k : voiced) f0s.push_back (frames[k].f0);
        std::sort (f0s.begin(), f0s.end());
        const float med = f0s[f0s.size() / 2];
        std::vector<size_t> vk;                                  // octave errors out
        for (size_t k : voiced) if (frames[k].f0 > 0.7f * med && frames[k].f0 < 1.4f * med) vk.push_back (k);
        if (vk.size() < 10) continue;
        std::vector<float> f1;
        for (size_t k : vk) f1.push_back (frames[k].f0);
        std::nth_element (f1.begin(), f1.begin() + (long) f1.size() / 2, f1.end());

        SpeakerChunk c;
        c.a = (long long) std::llround ((double) a * hopFull);
        c.b = std::min (len, (long long) std::llround ((double) b * hopFull) + (long long) kFullN);
        c.f0 = f1[f1.size() / 2];
        c.seconds = (float) ((double) vk.size() * 0.02);
        double pw = 0.0;
        for (size_t k = a; k < b; ++k) pw += std::pow (10.0, (double) frames[k].db / 10.0);
        c.levelDb = (float) (10.0 * std::log10 (pw / (double) (b - a) + 1.0e-12));
        for (size_t k : vk)
        {
            for (int d = 0; d < kCepDims; ++d) c.feat[d] += frames[k].cep[d] / (float) vk.size();
            for (int d = 0; d < kSpkBands; ++d) c.bandPow[(size_t) d] += frames[k].band[(size_t) d] / (float) vk.size();
        }
        c.feat[kCepDims] = std::log2 (c.f0);
        out.chunks.push_back (c);
    }
    // chunks must not overlap (the 1024-sample tail can run into the next one)
    for (size_t i = 0; i + 1 < out.chunks.size(); ++i) out.chunks[i].b = std::min (out.chunks[i].b, out.chunks[i + 1].a);
    clusterSpeakers (out, 0);
}
} // namespace podcast
} // namespace aio

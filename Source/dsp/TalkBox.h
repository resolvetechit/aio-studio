#pragma once
#include <vector>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include "PitchDetector.h"

namespace aio
{
/*
   Talk box.

   A real talk box pipes a synth through a tube into the player's mouth, where the mouth shapes the sound, and a
   microphone picks the result up.  The mouth acts as a filter whose resonances (the formants) move with every vowel.
   This models exactly that: the incoming voice is the "mouth" and a built-in synth lead is the sound source.

     voice  -> linear-prediction analysis (about every 2.5 ms)  -> the formant filter (all-pole lattice, interpolated)
     synth lead (saw / pulse / super-saw, glide, vibrato)        -> excitation of that filter
     noise burst, only while the voice is a consonant (s, sh, f, t, k)  -> excitation as well, so words stay readable
     the result is scaled to follow the voice's loudness, then drive + tone

   Where the notes come from: the pitch of the voice (snapped to the scale), MIDI keys, the Compose melody,
   or one fixed note (keys on screen).
*/

// ----------------------------------------------------------------------------------------------------------
// small LPC toolbox (also used directly by the tests)
// ----------------------------------------------------------------------------------------------------------
// Levinson-Durbin.  R[0..p] -> k[1..p] (reflection coefficients, k[0] unused) and a[0..p] (a[0] = 1, A(z) = sum a_i z^-i).
// Returns the final prediction error.
inline double levinsonDurbin (const double* R, int p, double* k, double* a, double* tmp)
{
    double e = R[0];
    a[0] = 1.0;
    for (int i = 1; i <= p; ++i) { a[i] = 0.0; k[i] = 0.0; }
    if (e <= 0.0) return 0.0;
    for (int m = 1; m <= p; ++m)
    {
        double acc = R[m];
        for (int j = 1; j < m; ++j) acc += a[j] * R[m - j];
        double km = -acc / e;
        km = std::clamp (km, -0.9995, 0.9995);
        k[m] = km;
        for (int j = 1; j < m; ++j) tmp[j] = a[j] + km * a[m - j];
        for (int j = 1; j < m; ++j) a[j] = tmp[j];
        a[m] = km;
        e *= (1.0 - km * km);
        if (e <= 1.0e-30) { for (int q = m + 1; q <= p; ++q) k[q] = 0.0; return e; }
    }
    return e;
}

// all-pole lattice filter 1/A(z), k's given per call (so they can be interpolated sample by sample)
struct AllPoleLattice
{
    std::vector<double> g;                 // backward errors b_m(n-1)
    int order = 0;
    void setOrder (int p) { order = p; g.assign ((size_t) p + 1, 0.0); }
    void reset() { std::fill (g.begin(), g.end(), 0.0); }
    inline double tick (double x, const double* k)
    {
        double f = x;
        for (int m = order; m >= 1; --m)
        {
            f -= k[m] * g[(size_t) (m - 1)];
            g[(size_t) m] = g[(size_t) (m - 1)] + k[m] * f;
        }
        g[0] = f;
        return f;
    }
};

struct TalkSettings
{
    bool  on = false;
    int   source = 1;               // 0 voice pitch, 1 MIDI / keys, 2 Compose, 3 fixed note
    int   wave = 0;                 // 0 saw lead, 1 pulse, 2 super-saw, 3 square
    int   octave = 0;               // -2 .. +2
    float glideMs = 60.f;
    float vibRate = 5.5f, vibDepth = 12.f;      // cents
    float sub = 30.f;               // %
    float carrierTone = 9000.f;     // Hz, low-pass on the raw synth
    float sharp = 60.f;             // % formant sharpness (narrower resonances = more "vowel")
    float shiftSt = 0.f;            // mouth size: formants up / down
    float presence = 60.f;          // % brightness
    float consonants = 60.f;        // %
    float dynamics = 50.f;          // % follow the voice's loudness (0 = even loudness)
    float gateDb = -52.f;
    float drive = 25.f;             // %
    float toneHz = 7000.f;
    float mix = 100.f;              // % wet
    float levelDb = 0.f;
    float snap = 100.f;             // % (voice pitch source)
    uint16_t mask = 0x0FFF;
    int   fixedNote = 60;
};

class TalkBox
{
public:
    static constexpr int kEnvPoints = 40;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        order = std::clamp ((int) std::lround (sr / 1000.0) + 4, 16, 56);
        win = (int) std::lround (sr * 0.020);
        hop = std::max (16, (int) std::lround (sr * 0.0025));
        ringSize = 1;
        while (ringSize < win * 3) ringSize <<= 1;
        ring.assign ((size_t) ringSize, 0.0);
        frame.assign ((size_t) win, 0.0);
        hann.assign ((size_t) win, 0.0);
        for (int i = 0; i < win; ++i) hann[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * kPi * (i + 0.5) / win);
        R.assign ((size_t) order + 1, 0.0);
        lag.assign ((size_t) order + 1, 1.0);
        kCur.assign ((size_t) order + 1, 0.0); dk.assign ((size_t) order + 1, 0.0); kTgt.assign ((size_t) order + 1, 0.0);
        aCoef.assign ((size_t) order + 1, 0.0); tmp.assign ((size_t) order + 1, 0.0);
        lat.setOrder (order);
        det.prepare (sr);
        lastSharp = -1.f;
        hpA = 1.0 - std::exp (-2.0 * kPi * 3500.0 / sr);
        reset();
    }

    void reset()
    {
        std::fill (ring.begin(), ring.end(), 0.0);
        std::fill (kCur.begin(), kCur.end(), 0.0); std::fill (dk.begin(), dk.end(), 0.0); std::fill (kTgt.begin(), kTgt.end(), 0.0);
        lat.reset(); det.reset();
        wpos = 0; hopCount = 0; prevX = 0.0; deemph = 0.0;
        phase = phase2 = phase3 = subPhase = 0.0;
        curMidi = -1.0; haveMidi = false; noteGate = 0.f; v1 = v2 = -1.0;
        pv = py = 1.0e-9; pRef = 1.0e-7; gAgc = 0.0;
        hp1 = hp2 = 0.0; eHf = eTot = 0.0; uns = 0.f; gate = 0.f; gateOpen = false;
        vibPhase = 0.0; vibFade = 0.f; carLp = 0.0;
        driveLp1 = driveLp2 = driveHp = 0.0;
        held.clear(); lastVoiceMidi = -1.0; voiceHold = 0; bend = 0.f; mod = 0.f;
        rng = 0x1234567u; envCount = 0;
        for (auto& e : envDb) e.store (-90.f);
    }

    // ---- notes
    void noteOn (int note, int vel)
    {
        (void) vel;
        held.erase (std::remove (held.begin(), held.end(), note), held.end());
        held.push_back (note);
        if (held.size() > 32) held.erase (held.begin());
    }
    void noteOff (int note) { held.erase (std::remove (held.begin(), held.end(), note), held.end()); }
    void allNotesOff() { held.clear(); }
    void setBend (float semitones) { bend = semitones; }
    void setMod (float m01) { mod = m01; }
    bool anyHeld() const { return ! held.empty(); }

    // replaces l / r with the talk box output (dry/wet by s.mix).  The modulator is the mono sum of the input.
    void process (float* l, float* r, int n, const TalkSettings& s)
    {
        if (! s.on) return;
        updateStatic (s);

        const double wet = std::clamp ((double) s.mix * 0.01, 0.0, 1.0);
        const double lvl = std::pow (10.0, (double) s.levelDb / 20.0);
        const double muA = 0.95;
        const double muS = muA * (1.0 - std::clamp ((double) s.presence * 0.01, 0.0, 1.0) * 0.92);
        const double consAmt = std::clamp ((double) s.consonants * 0.01, 0.0, 1.0);
        const double dyn = std::clamp ((double) s.dynamics * 0.01, 0.0, 1.0);
        const double gateLin = std::pow (10.0, (double) s.gateDb / 10.0);              // power
        const double aEnv = 1.0 - std::exp (-1.0 / (sr * 0.006));
        const double aRef = std::exp (-1.0 / (sr * 3.0));                               // reference level falls ~ 1.4 dB per second
        const double aUns = 1.0 - std::exp (-1.0 / (sr * 0.004));
        const double aGateUp = 1.0 - std::exp (-1.0 / (sr * 0.004)), aGateDn = 1.0 - std::exp (-1.0 / (sr * 0.035));
        const double aNote = 1.0 - std::exp (-1.0 / (sr * 0.004)), aNoteRel = 1.0 - std::exp (-1.0 / (sr * 0.070));
        const double aGlide = s.glideMs < 1.f ? 1.0 : 1.0 - std::exp (-1.0 / (sr * (double) s.glideMs * 0.001));
        const double carA = 1.0 - std::exp (-2.0 * kPi * std::clamp ((double) s.carrierTone, 500.0, 18000.0) / sr);
        const double tA = 1.0 - std::exp (-2.0 * kPi * std::clamp ((double) s.toneHz, 800.0, 18000.0) / sr);
        const double hA = 1.0 - std::exp (-2.0 * kPi * 110.0 / sr);
        const double driveG = 1.0 + 7.0 * std::clamp ((double) s.drive * 0.01, 0.0, 1.0);
        const double driveMk = 1.0 / std::sqrt (driveG);
        const double vibCents = (double) s.vibDepth * (1.0 + 1.5 * (double) mod);
        const int voiceSrc = s.source == 0;
        const double fixedMidi = (double) s.fixedNote;
        const int oct = s.octave * 12;

        double inPk = 0.0, outPk = 0.0;
        bool voicedNow = false;

        for (int i = 0; i < n; ++i)
        {
            const double x = 0.5 * ((double) l[i] + (double) r[i]);
            const double dry = x;

            // ---- level tracking of the modulator
            pv += aEnv * (x * x - pv);
            pRef = std::max (pv, pRef * aRef);
            hp1 += hpA * (x - hp1); const double h1 = x - hp1;
            hp2 += hpA * (h1 - hp2); const double h2 = h1 - hp2;
            eHf += aUns * (h2 * h2 - eHf);
            eTot += aUns * (x * x - eTot);
            const double ratio = eHf / (eTot + 1.0e-12);
            double uT = std::clamp ((ratio - 0.12) / 0.2, 0.0, 1.0);
            if (pv < gateLin) uT = 0.0;
            uns += (float) (aUns * (uT - uns));

            // gate (with a little hysteresis)
            if (! gateOpen && pv > gateLin * 1.6) gateOpen = true;
            else if (gateOpen && pv < gateLin * 0.8) gateOpen = false;
            gate += (float) ((gateOpen ? aGateUp : aGateDn) * ((gateOpen ? 1.0 : 0.0) - gate));

            // ---- history for the analysis (pre-emphasised)
            const double pe = x - muA * prevX; prevX = x;
            ring[(size_t) (wpos & (ringSize - 1))] = pe;
            ++wpos;
            if (++hopCount >= hop) { hopCount = 0; analyse (s); }

            // ---- pitch of the carrier
            if (voiceSrc)
            {
                if (det.push (x, s.gateDb, 65.f, 1000.f))
                {
                    if (det.voiced())
                    {
                        const double m = det.midi();
                        double med = m;
                        if (v1 > 0.0 && v2 > 0.0) med = std::max (std::min (m, v1), std::min (std::max (m, v1), v2));
                        v2 = v1; v1 = m;
                        lastVoiceMidi = med;
                        voiceHold = (int) (sr * 0.25);
                    }
                }
                if (voiceHold > 0) { --voiceHold; voicedNow = true; }
            }
            double target = curMidi;
            bool noteWanted = false;
            switch (s.source)
            {
                case 0:
                    if (lastVoiceMidi > 0.0)
                    {
                        const double snapped = nearestAllowed (lastVoiceMidi, s.mask);
                        target = lastVoiceMidi + (snapped - lastVoiceMidi) * std::clamp ((double) s.snap * 0.01, 0.0, 1.0);
                    }
                    noteWanted = gateOpen && lastVoiceMidi > 0.0;
                    break;
                case 3:
                    target = fixedMidi;
                    noteWanted = true;
                    break;
                default:
                    if (! held.empty()) { target = (double) held.back(); noteWanted = true; }
                    break;
            }
            if (noteWanted && target > 0.0)
            {
                target += (double) oct;
                if (! haveMidi) { curMidi = target; haveMidi = true; vibFade = 0.f; }
                else curMidi += aGlide * (target - curMidi);
            }
            const float nt = noteWanted ? 1.f : 0.f;
            noteGate += (float) ((nt > noteGate ? aNote : aNoteRel) * (nt - noteGate));
            if (! noteWanted && noteGate < 1.0e-4f) haveMidi = false;
            if (noteWanted) vibFade = std::min (1.f, vibFade + (float) (1.0 / (sr * 0.35)));
            else vibFade = 0.f;

            // ---- the carrier (a band-limited synth voice)
            double car = 0.0;
            if (haveMidi)
            {
                vibPhase += (double) s.vibRate / sr; if (vibPhase >= 1.0) vibPhase -= 1.0;
                const double vib = std::sin (2.0 * kPi * vibPhase) * vibCents * 0.01 * (double) vibFade;
                const double m = curMidi + vib + (double) bend;
                const double f = 440.0 * std::exp2 ((m - 69.0) / 12.0);
                const double dph = std::clamp (f / sr, 1.0e-5, 0.45);
                car = oscillator (s.wave, dph, (double) s.sub * 0.01);
            }
            carLp += carA * (car - carLp);

            // ---- excitation: carrier, or noise on consonants
            const double nz = noise();
            const double u = (double) uns * consAmt;
            const double exc = carLp * (1.0 - 0.9 * u) + nz * 0.9 * u;

            // ---- formant filter + loudness matching
            for (int m = 1; m <= order; ++m) kCur[(size_t) m] += dk[(size_t) m];
            double ys = lat.tick (exc, kCur.data());
            if (! std::isfinite (ys)) { lat.reset(); ys = 0.0; }
            deemph = ys + muS * deemph;
            double yd = deemph;
            if (! std::isfinite (yd) || std::abs (yd) > 1.0e6) { deemph = 0.0; lat.reset(); yd = 0.0; }
            py += aEnv * (yd * yd - py);
            // loudness: follow the voice by `dyn`, otherwise the long-term reference
            const double pTarget = std::pow (std::max (pv, 1.0e-12) / std::max (pRef, 1.0e-12), dyn) * pRef;
            const double gWant = std::min (std::sqrt (pTarget / (py + 1.0e-12)), 300.0);
            gAgc += aEnv * (gWant - gAgc);
            double y = yd * gAgc * (double) gate * (double) noteGate;

            // ---- drive + tone
            y *= driveG;
            y = std::tanh (y) * driveMk;
            driveHp += hA * (y - driveHp); y -= driveHp;
            driveLp1 += tA * (y - driveLp1); driveLp2 += tA * (driveLp1 - driveLp2);
            y = driveLp2 * lvl;

            const double o = dry * (1.0 - wet) + y * wet;
            l[i] = r[i] = (float) o;
            inPk = std::max (inPk, std::abs (x)); outPk = std::max (outPk, std::abs (o));
        }

        inLevel.store ((float) inPk); outLevel.store ((float) outPk);
        gateMeter.store (gate); unvoicedMeter.store (uns);
        carrierMidi.store (haveMidi ? (float) curMidi : -1.f);
        voicedMeter.store (voicedNow ? 1.f : 0.f);
        active.store (noteGate > 0.02f);
    }

    // ---- meters / display (read by the editor)
    std::atomic<float> inLevel { 0.f }, outLevel { 0.f }, gateMeter { 0.f }, unvoicedMeter { 0.f }, carrierMidi { -1.f }, voicedMeter { 0.f };
    std::atomic<bool>  active { false };
    std::array<std::atomic<float>, kEnvPoints> envDb;          // the mouth's spectral shape, dB, 100 Hz .. 8 kHz
    TalkBox() { for (auto& e : envDb) e.store (-90.f); }

    static double nearestAllowed (double midi, uint16_t mask)
    {
        if (mask == 0 || mask == 0x0FFF) return midi;
        double best = midi, bd = 1.0e9;
        const int base = (int) std::floor (midi);
        for (int c = base - 6; c <= base + 7; ++c)
            if (mask & (1u << (((c % 12) + 12) % 12)))
            {
                const double d = std::abs ((double) c - midi);
                if (d < bd) { bd = d; best = (double) c; }
            }
        return best;
    }

private:
    void updateStatic (const TalkSettings& s)
    {
        if (std::abs (s.sharp - lastSharp) > 0.01f)
        {
            lastSharp = s.sharp;
            // Gaussian lag window: a wider one smears the resonances (soft, "ooh"), a narrow one keeps them sharp (vowel-like, ringing)
            const double bw = 30.0 + 260.0 * std::pow (1.0 - std::clamp ((double) s.sharp * 0.01, 0.0, 1.0), 1.5);
            for (int i = 0; i <= order; ++i)
            {
                const double t = 2.0 * kPi * bw * (double) i / sr;
                lag[(size_t) i] = std::exp (-0.5 * t * t);
            }
        }
    }

    void analyse (const TalkSettings& s)
    {
        // read one window, resampled by the formant shift
        const double shift = std::exp2 ((double) s.shiftSt / 12.0);      // formants up by this ratio
        const double step = shift;                                       // reading the voice faster moves its resonances up
        const double newest = (double) (wpos - 1);
        double e0 = 0.0;
        for (int i = 0; i < win; ++i)
        {
            const double pos = newest - (double) (win - 1 - i) * step;
            const double fl = std::floor (pos);
            const double fr = pos - fl;
            const long i0 = (long) fl;
            const double a = ring[(size_t) ((i0) & (ringSize - 1))], b = ring[(size_t) ((i0 + 1) & (ringSize - 1))];
            const double v = (a + (b - a) * fr) * hann[(size_t) i];
            frame[(size_t) i] = v;
            e0 += v * v;
        }
        const double frameFloor = 1.0e-9 * (double) win;
        const bool quiet = e0 < frameFloor;
        const double decayK = 0.9;
        if (quiet)
        {
            for (int m = 1; m <= order; ++m) kTgt[(size_t) m] = kCur[(size_t) m] * decayK;
        }
        else
        {
            for (int m = 0; m <= order; ++m)
            {
                double acc = 0.0;
                for (int i = m; i < win; ++i) acc += frame[(size_t) i] * frame[(size_t) (i - m)];
                R[(size_t) m] = acc * lag[(size_t) m];
            }
            R[0] = R[0] * (1.0 + 1.0e-4) + 1.0e-12;
            levinsonDurbin (R.data(), order, kTgt.data(), aCoef.data(), tmp.data());
        }
        const double inv = 1.0 / (double) hop;
        for (int m = 1; m <= order; ++m) dk[(size_t) m] = (kTgt[(size_t) m] - kCur[(size_t) m]) * inv;

        if (! quiet && (++envCount % 8) == 0) publishEnvelope();
    }

    void publishEnvelope()
    {
        // |1 / A(e^jw)|^2 on a log frequency grid
        double mx = -1.0e9;
        std::array<double, kEnvPoints> d {};
        for (int p = 0; p < kEnvPoints; ++p)
        {
            const double fHz = 100.0 * std::pow (80.0, (double) p / (kEnvPoints - 1));
            const double w = 2.0 * kPi * std::min (fHz, 0.45 * sr) / sr;
            double re = 0.0, im = 0.0;
            for (int m = 0; m <= order; ++m) { re += aCoef[(size_t) m] * std::cos (w * m); im -= aCoef[(size_t) m] * std::sin (w * m); }
            d[(size_t) p] = -10.0 * std::log10 (re * re + im * im + 1.0e-12);
            mx = std::max (mx, d[(size_t) p]);
        }
        for (int p = 0; p < kEnvPoints; ++p) envDb[(size_t) p].store ((float) (d[(size_t) p] - mx));
    }

    double noise() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (double) (rng & 0xFFFFFF) / 8388608.0 - 1.0; }

    static double polyBlep (double t, double dt)
    {
        if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
        return 0.0;
    }
    static double sawBl (double ph, double dt) { return 2.0 * ph - 1.0 - polyBlep (ph, dt); }
    static double pulseBl (double ph, double dt, double w)
    {
        double v = ph < w ? 1.0 : -1.0;
        v += polyBlep (ph, dt);
        double t2 = ph - w; if (t2 < 0.0) t2 += 1.0;
        v -= polyBlep (t2, dt);
        return v;
    }
    static void adv (double& ph, double dph) { ph += dph; if (ph >= 1.0) ph -= 1.0; }

    double oscillator (int wave, double dph, double sub)
    {
        double o = 0.0;
        switch (wave)
        {
            case 1:  o = pulseBl (phase, dph, 0.28) * 0.7; break;
            case 2:
            {
                const double d1 = dph * 1.0045, d2 = dph * 0.9955;
                o = (sawBl (phase, dph) + sawBl (phase2, std::min (d1, 0.45)) + sawBl (phase3, std::min (d2, 0.45))) * 0.36;
                adv (phase2, d1); adv (phase3, d2);
                break;
            }
            case 3:  o = pulseBl (phase, dph, 0.5) * 0.7; break;
            default: o = sawBl (phase, dph) * 0.8; break;
        }
        adv (phase, dph);
        if (sub > 0.0)
        {
            const double ds = dph * 0.5;
            o += sub * 0.55 * pulseBl (subPhase, ds, 0.5);
            adv (subPhase, ds);
        }
        return o;
    }

    double sr = 48000.0;
    int order = 24, win = 960, hop = 120, ringSize = 4096;
    std::vector<double> ring, frame, hann, R, lag, kCur, dk, kTgt, aCoef, tmp;
    AllPoleLattice lat;
    PitchDetector det;
    long wpos = 0;
    int hopCount = 0, envCount = 0;
    double prevX = 0.0, deemph = 0.0;
    double phase = 0.0, phase2 = 0.0, phase3 = 0.0, subPhase = 0.0, vibPhase = 0.0, carLp = 0.0;
    double curMidi = -1.0, lastVoiceMidi = -1.0;
    bool haveMidi = false, gateOpen = false;
    double v1 = -1.0, v2 = -1.0;
    int voiceHold = 0;
    float noteGate = 0.f, uns = 0.f, gate = 0.f, vibFade = 0.f, lastSharp = -1.f, bend = 0.f, mod = 0.f;
    double pv = 1.0e-9, py = 1.0e-9, pRef = 1.0e-7, gAgc = 0.0;
    double hp1 = 0, hp2 = 0, eHf = 0, eTot = 0, hpA = 0.3;
    double driveLp1 = 0, driveLp2 = 0, driveHp = 0;
    std::vector<int> held;
    uint32_t rng = 0x1234567u;
};
} // namespace aio

// DELAY: rest, where the echoes land (every note at two tempos, free time,
// every mode), the loop's per-repeat response and the drive curves against
// the reference's measurements, modulation and noise per mode, 100 %
// feedback, ducking (sidechain and own input), diffusion, aliasing, block
// sizes, state copies and clicks (mode, style, time, tempo changes).
#include "SynthVoice.h"
#include "TestUtil.h"
#include "fx/Delay.h"
#include "fx/Sync.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

using namespace voc::testutil;
using voc::Delay;
using voc::P;

namespace
{
    // VOC_DELAY_REPORT=1 prints the measurements (for the notes).
    template <typename... Args>
    void report (const char* format, Args... args)
    {
        if (std::getenv ("VOC_DELAY_REPORT") != nullptr) std::printf (format, args...);
    }

    const char* modeName (int m)
    {
        static const char* names[] = { "Tape", "BBD", "Digital", "Clean", "Lo-Fi" };
        return names[m];
    }

    voc::Settings delaySettings (int mode, double mix = 100.0, double feedback = 0.0, int style = Delay::mono, double age = 0.0)
    {
        voc::Settings s;
        s[P::delayMix] = (float) mix;
        s[P::delayMode] = (float) mode;
        s[P::delayStyle] = (float) style;
        s[P::delayFeedback] = (float) feedback;
        s[P::delayAge] = (float) age;
        return s;
    }

    voc::Settings freeTime (voc::Settings s, double ms)
    {
        s[P::delaySync] = 0.f;
        s[P::delayMs] = (float) ms;
        return s;
    }

    voc::Transport steady (double bpm)
    {
        voc::Transport t;
        t.bpm = bpm;
        t.tempoKnown = true;
        t.playing = true;
        return t;
    }

    using Each = std::function<void (size_t, voc::Settings&, voc::Transport&)>;

    // x through a fresh DELAY in blocks; `each` may change the settings and
    // the transport at a block's first sample.
    Stereo run (voc::Delay& m, const Stereo& x, double fs, voc::Settings s, int block = 64, voc::Transport t = steady (120.0),
                const Signal* key = nullptr, const Each& each = {})
    {
        m.set (s);
        m.prepare (fs, block);
        Stereo y = x;
        for (size_t at = 0; at < y.size(); at += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, y.size() - at);
            if (each) each (at, s, t);
            m.set (s);
            voc::Block b;
            b.transport = t;
            b.key = key != nullptr ? key->data() + at : nullptr;
            m.process (y.left.data() + at, y.right.data() + at, n, b);
            t.ppq += (double) n / fs * t.bpm / 60.0;
        }
        return y;
    }

    Stereo run (const Stereo& x, double fs, const voc::Settings& s, int block = 64, voc::Transport t = steady (120.0),
                const Signal* key = nullptr, const Each& each = {})
    {
        voc::Delay m;
        return run (m, x, fs, s, block, t, key, each);
    }

    Signal wetOf (const Signal& y, const Signal& x)
    {
        Signal w (y.size());
        for (size_t i = 0; i < w.size(); ++i) w[i] = (float) ((double) y[i] - (double) x[i]);
        return w;
    }

    Stereo mono (const Signal& x) { return { x, x }; }
    Stereo silence (size_t n) { return { Signal (n, 0.f), Signal (n, 0.f) }; }

    // A 1 kHz tone burst under a Gaussian (sigma 2 ms) centred at `at`.
    Signal burst (size_t n, size_t at, double fs, double amp = 0.03, double hz = 1000.0)
    {
        Signal x (n, 0.f);
        const double sigma = 0.002 * fs;
        for (size_t i = 0; i < n; ++i)
        {
            const double t = ((double) i - (double) at) / sigma;
            if (std::abs (t) < 6.0) x[i] = (float) (amp * std::exp (-0.5 * t * t) * std::sin (2.0 * pi * hz * ((double) i - (double) at) / fs));
        }
        return x;
    }

    // The energy centroid of x in [from, to), in samples.
    double centroid (const Signal& x, size_t from, size_t to)
    {
        double e = 0.0, m = 0.0;
        for (size_t i = from; i < std::min (to, x.size()); ++i)
        {
            const double p = (double) x[i] * x[i];
            e += p;
            m += p * (double) i;
        }
        return e > 0.0 ? m / e : 0.0;
    }

    // |X| in dB at hz of x[from, from + n) (rectangular window; the echo is
    // a transient inside it).
    double levelAt (const Signal& x, size_t from, size_t n, double hz, double fs)
    {
        cplx z {};
        for (size_t i = 0; i < n && from + i < x.size(); ++i) z += (double) x[from + i] * std::polar (1.0, -2.0 * pi * hz * (double) i / fs);
        return db (std::abs (z));
    }

    double analog1Pole (bool highpass, double fc, double f)
    {
        const double w = f / fc;
        return highpass ? w / std::sqrt (1.0 + w * w) : 1.0 / std::sqrt (1.0 + w * w);
    }

    // Butterworth through the bilinear transform, the cutoff prewarped:
    // steeper than the analog one near Nyquist.
    double warpedButterworthDb (int order, double fc, double f, double fs)
    {
        const double w = std::tan (pi * f / fs) / std::tan (pi * fc / fs);
        return -10.0 * std::log10 (1.0 + std::pow (w, 2.0 * order));
    }

    // What the 3-tap pre-emphasis and the ADAA's two-tap average leave at f
    // (Digital's write, in front of its per-pass low-pass).
    double adaaDb (double f, double fs)
    {
        const double h = pi * f / fs, s = std::sin (h);
        return db ((1.0 + 0.5 * s * s) * std::cos (h));
    }

    // The power of x[from, from + 2^15) (Hann) within +-width Hz of hz.
    double bandPower (const Signal& x, size_t from, double hz, double width, double fs)
    {
        const size_t n = 1 << 15;
        std::vector<cplx> z (n);
        for (size_t i = 0; i < n; ++i)
            z[i] = (from + i < x.size() ? (double) x[from + i] : 0.0) * (0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n));
        fft (z);
        double p = 0.0;
        for (size_t k = 1; k < n / 2; ++k)
            if (std::abs ((double) k * fs / (double) n - hz) <= width) p += std::norm (z[k]);
        return p;
    }

    // A harmonic's level re the fundamental (dBc), as band energies, so a
    // modulated tone's spread doesn't hide it.
    double harmonicDbc (const Signal& x, size_t from, double f0, int h, double fs)
    {
        return 10.0 * std::log10 (bandPower (x, from, h * f0, 60.0, fs) / bandPower (x, from, f0, 60.0, fs));
    }

    // |H| (dB) from x to y, y read `lag` samples later: averaged cross- and
    // auto-spectra (Hann, 4096 points, half overlap), so what a nonlinearity
    // adds (uncorrelated with x) averages out. Bin k is k fs / 4096.
    std::vector<double> transferDb (const Signal& x, const Signal& y, size_t lag, size_t from, size_t to)
    {
        const size_t n = 4096;
        std::vector<cplx> sxy (n / 2, cplx {}), sxx (n / 2, cplx {});
        for (size_t at = from; at + n + lag <= std::min (to, y.size()); at += n / 2)
        {
            std::vector<cplx> a (n), b (n);
            for (size_t i = 0; i < n; ++i)
            {
                const double w = 0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n);
                a[i] = w * (double) x[at + i];
                b[i] = w * (double) y[at + lag + i];
            }
            fft (a);
            fft (b);
            for (size_t k = 0; k < n / 2; ++k)
            {
                sxy[k] += b[k] * std::conj (a[k]);
                sxx[k] += a[k] * std::conj (a[k]);
            }
        }
        std::vector<double> h (n / 2);
        for (size_t k = 0; k < n / 2; ++k) h[k] = db (std::abs (sxy[k]) / std::max (std::abs (sxx[k]), 1e-30));
        return h;
    }

    // The pitch of a tone near hz over time, in cents, one value per
    // millisecond: mixed down to 0 Hz, low-passed (4th-order Butterworth at
    // 150 Hz: the 2 hz image -100 dB, a 45 Hz warble untouched), its phase
    // differentiated.
    std::vector<double> pitchCents (const Signal& y, double hz, double fs, size_t from)
    {
        auto a1 = Biquad::lowpass (150.0, 0.5412, fs), a2 = Biquad::lowpass (150.0, 1.3066, fs);
        auto b1 = Biquad::lowpass (150.0, 0.5412, fs), b2 = Biquad::lowpass (150.0, 1.3066, fs);
        const auto hop = (size_t) std::lround (fs / 1000.0);
        std::vector<double> cents;
        double prev = 0.0;
        bool have = false;
        for (size_t i = 0; i < y.size(); ++i)
        {
            const double ph = 2.0 * pi * hz * (double) i / fs;
            const double re = a2.process (a1.process ((double) y[i] * std::cos (ph)));
            const double im = b2.process (b1.process (-(double) y[i] * std::sin (ph)));
            if (i >= from && (i - from) % hop == 0)
            {
                const double p = std::atan2 (im, re);
                if (have)
                {
                    const double dHz = std::remainder (p - prev, 2.0 * pi) / (2.0 * pi * (double) hop / fs);
                    cents.push_back (1200.0 * std::log2 ((hz + dHz) / hz));
                }
                prev = p;
                have = true;
            }
        }
        return cents;
    }

    double rmsOf (const std::vector<double>& v)
    {
        double s = 0.0, m = 0.0;
        for (double x : v) m += x / (double) v.size();
        for (double x : v) s += (x - m) * (x - m);
        return std::sqrt (s / (double) v.size());
    }
} // namespace

// ---- Rest ----------------------------------------------------------------------

TEST_CASE ("Delay: at rest the voice passes bit for bit", "[delay][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto x = Stereo { noise ((size_t) (0.5 * fs), 4), noise ((size_t) (0.5 * fs), 5) };
    auto off = delaySettings (Delay::tape, 70.0, 60.0, Delay::pingPong, 25.0);
    off[P::delayOn] = 0.f;
    for (const auto& s : { voc::Settings {}, delaySettings (Delay::bbd, 0.0, 90.0), off })
    {
        voc::Delay m;
        const auto y = run (m, x, fs, s);
        INFO ("fs " << fs);
        CHECK (m.latency() == 0);
        CHECK_FALSE (m.engaged());
        CHECK (m.tailSeconds() == 0.0);
        CHECK (bitIdentical (y.left, x.left));
        CHECK (bitIdentical (y.right, x.right));
    }
}

TEST_CASE ("Delay: Mix back to 0 fades the echoes, then rest is exact again", "[delay][rest]")
{
    const double fs = 48000.0;
    const auto x = Stereo { noise ((size_t) (3.0 * fs), 6, 0.3), noise ((size_t) (3.0 * fs), 7, 0.3) };
    voc::Delay m;
    const auto down = (size_t) (1.0 * fs);
    const auto y = run (m, x, fs, delaySettings (Delay::tape, 70.0, 80.0, Delay::pingPong, 25.0), 64, steady (145.0), nullptr,
                        [&] (size_t at, voc::Settings& s, voc::Transport&) { s[P::delayMix] = at < down ? 70.f : 0.f; });
    // Rest within half a second of the knob (the mix glide lands); from there on the input exactly.
    const auto restFrom = down + (size_t) (0.5 * fs);
    CHECK_FALSE (m.engaged());
    CHECK (bitIdentical (Signal (y.left.begin() + (std::ptrdiff_t) restFrom, y.left.end()), Signal (x.left.begin() + (std::ptrdiff_t) restFrom, x.left.end())));
    CHECK (bitIdentical (Signal (y.right.begin() + (std::ptrdiff_t) restFrom, y.right.end()), Signal (x.right.begin() + (std::ptrdiff_t) restFrom, x.right.end())));
    // ...and it did echo before.
    CHECK (maxAbsDiff (y.left, x.left) > 0.05);
    const double click = std::max (worstClickP (wetOf (y.left, x.left), fs), worstClickP (wetOf (y.right, x.right), fs));
    INFO ("worst click P in the echoes " << click);
    CHECK (click < 4.0);
}

// ---- Time ----------------------------------------------------------------------

TEST_CASE ("Delay: the echo lands on the note at 120 and 145 BPM, every division", "[delay][time]")
{
    const double fs = 48000.0;
    std::array<double, Delay::kModes> worst {};
    for (double bpm : { 120.0, 145.0 })
        for (int div = 0; div < (int) voc::sync::kNotes.size(); ++div)
        {
            const double expected = voc::sync::seconds (div, bpm) * fs;
            const auto n = (size_t) (expected + 0.05 * fs);
            // Clean: an impulse comes back as itself, on the sample.
            {
                auto s = delaySettings (Delay::clean);
                s[P::delayDiv] = (float) div;
                const auto x = mono (impulse (n, 1000, 0.5));
                const auto w = wetOf (run (x, fs, s, 64, steady (bpm)).left, x.left);
                size_t peak = 0;
                for (size_t i = 0; i < w.size(); ++i) if (std::abs (w[i]) > std::abs (w[peak])) peak = i;
                INFO (bpm << " BPM, " << voc::choices::delayDiv[(size_t) div] << ": expected " << expected << ", the echo at " << (double) peak - 1000.0);
                CHECK (std::abs ((double) peak - 1000.0 - expected) <= 0.5);
            }
            // Every mode (Age 0): a 1 kHz burst's echo, its energy centroid.
            for (int mode = 0; mode < Delay::kModes; ++mode)
            {
                auto s = delaySettings (mode);
                s[P::delayDiv] = (float) div;
                const auto x = mono (burst (n + 1000, 2000, fs));
                const auto w = wetOf (run (x, fs, s, 64, steady (bpm)).left, x.left);
                const double at = centroid (w, 2000 + (size_t) expected - 600, 2000 + (size_t) expected + 600) - centroid (x.left, 0, 3000);
                INFO (modeName (mode) << ", " << bpm << " BPM, " << voc::choices::delayDiv[(size_t) div] << ": expected " << expected << ", the echo at " << at);
                CHECK (std::abs (at - expected) <= 1.0);
                worst[(size_t) mode] = std::max (worst[(size_t) mode], std::abs (at - expected));
            }
        }
    for (int mode = 0; mode < Delay::kModes; ++mode)
        report ("time  %-7s every note at 120 and 145 BPM: at most %.2f samples off\n", modeName (mode), worst[(size_t) mode]);
}

TEST_CASE ("Delay: free time is exact; Dual's right channel answers on the partner note", "[delay][time]")
{
    const double fs = 48000.0;
    for (double ms : { 1.0, 37.5, 375.0, 1234.0, 4000.0 })
    {
        const double expected = ms * 0.001 * fs;
        const auto x = mono (impulse ((size_t) (expected + 0.05 * fs), 1000, 0.5));
        const auto w = wetOf (run (x, fs, freeTime (delaySettings (Delay::clean), ms)).left, x.left);
        size_t peak = 0;
        for (size_t i = 0; i < w.size(); ++i) if (std::abs (w[i]) > std::abs (w[peak])) peak = i;
        INFO (ms << " ms: the echo at " << (double) peak - 1000.0 << " samples, expected " << expected);
        CHECK (std::abs ((double) peak - 1000.0 - expected) <= 0.5);
    }
    // Dual at 145 BPM: 1/8D left, 1/4 right; 1/8 left, 1/8D right.
    for (const auto [div, partner] : { std::pair { 10, 11 }, std::pair { 8, 10 }, std::pair { 6, 8 } })
    {
        auto s = delaySettings (Delay::clean, 100.0, 0.0, Delay::dual);
        s[P::delayDiv] = (float) div;
        const double l = voc::sync::seconds (div, 145.0) * fs, r = voc::sync::seconds (partner, 145.0) * fs;
        const auto x = mono (impulse ((size_t) (std::max (l, r) + 0.05 * fs), 1000, 0.5));
        const auto y = run (x, fs, s, 64, steady (145.0));
        const auto wl = wetOf (y.left, x.left), wr = wetOf (y.right, x.right);
        size_t pl = 0, pr = 0;
        for (size_t i = 0; i < wl.size(); ++i)
        {
            if (std::abs (wl[i]) > std::abs (wl[pl])) pl = i;
            if (std::abs (wr[i]) > std::abs (wr[pr])) pr = i;
        }
        INFO (voc::choices::delayDiv[(size_t) div] << ": L at " << (double) pl - 1000.0 << " (" << l << "), R at " << (double) pr - 1000.0 << " (" << r << ")");
        CHECK (std::abs ((double) pl - 1000.0 - l) <= 0.5);
        CHECK (std::abs ((double) pr - 1000.0 - r) <= 0.5);
    }
}

TEST_CASE ("Delay: Ping-Pong sums to mono at -6 dB and alternates L, R, L at the note", "[delay][time]")
{
    const double fs = 48000.0;
    const double T = 0.25 * fs;
    const auto n = (size_t) (4.5 * T);
    Stereo x { impulse (n, 1000, 0.5), Signal (n, 0.f) };   // the impulse on the left only
    const auto y = run (x, fs, freeTime (delaySettings (Delay::clean, 100.0, 50.0, Delay::pingPong), 250.0));
    const auto wl = wetOf (y.left, x.left), wr = wetOf (y.right, x.right);
    for (int k = 1; k <= 4; ++k)
    {
        const auto at = (size_t) (1000.0 + k * T);
        const auto& hot = k % 2 == 1 ? wl : wr;
        const auto& cold = k % 2 == 1 ? wr : wl;
        const double expected = 0.25 * std::pow (0.5, k - 1);   // (L + R) / 2, then x 0.5 a repeat
        INFO ("repeat " << k << ": " << hot[at] << " (expected " << expected << "), the other side " << cold[at]);
        CHECK (std::abs (hot[at] - expected) < 1e-3 * expected);
        CHECK (std::abs (cold[at]) < 1e-9);
    }
}

TEST_CASE ("Delay: a tempo change glides to the new time without a click; jitter doesn't move it", "[delay][time][click]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 6.0;
    o.plosives = false;
    const auto v = synth::make (o).mono;
    const auto s = delaySettings (Delay::tape, 70.0, 60.0, Delay::pingPong, 25.0);
    const auto change = (size_t) (2.5 * fs);
    voc::Delay a, b;
    const auto ref = run (a, mono (v), fs, s, 64, steady (145.0));
    const auto y = run (b, mono (v), fs, s, 64, steady (145.0), nullptr, [&] (size_t at, voc::Settings&, voc::Transport& t) {
        t.bpm = at < change ? 145.0 + 0.004 * std::sin ((double) at) : 138.0;   // jitter, then a real change
    });
    const double pRef = std::max (worstClickP (ref.left, fs), worstClickP (ref.right, fs));
    const double pY = std::max (worstClickP (y.left, fs), worstClickP (y.right, fs));
    const double pWet = std::max (worstClickP (wetOf (y.left, v), fs), worstClickP (wetOf (y.right, v), fs));
    const double pWetRef = std::max (worstClickP (wetOf (ref.left, v), fs), worstClickP (wetOf (ref.right, v), fs));
    INFO ("worst click P " << pY << " (no change: " << pRef << "), the echoes alone " << pWet << " (no change: " << pWetRef << "); the time now "
          << b.readDelay (0) << ", the note at 138 BPM " << voc::sync::seconds (10, 138.0) * fs << " less the loop's " << b.alignment (Delay::tape));
    CHECK (pY < std::max (4.0, 1.5 * pRef));
    CHECK (pWet < std::max (4.0, 1.5 * pWetRef));
    report ("click tempo change: P %.2f (no change %.2f), echoes alone %.2f (no change %.2f)\n", pY, pRef, pWet, pWetRef);
    CHECK (b.heldBpm() == 138.0);
    CHECK (std::abs (b.readDelay (0) + b.diffuserDelay (0) + b.alignment (Delay::tape) - voc::sync::seconds (10, 138.0) * fs) <= 0.5);
    // Before the change the jitter was held: the same output as the steady run.
    CHECK (bitIdentical (Signal (y.left.begin(), y.left.begin() + (std::ptrdiff_t) change), Signal (ref.left.begin(), ref.left.begin() + (std::ptrdiff_t) change)));
}

// ---- The loop ------------------------------------------------------------------

TEST_CASE ("Delay: the loop's response per repeat and the first echo match the reference's", "[delay][loop]")
{
    // A quiet tone burst per frequency (sigma 3 ms), Age 0, feedback 100 %:
    // each echo's level at the burst's frequency, so a kink's harmonics (at
    // 2f and up) don't count. Per repeat: (echo 3 - echo 1) / 2.
    const double fs = 48000.0;
    const double T = 0.25 * fs;
    const size_t win = 8192;
    struct Target { int mode; double hz, perRepeat, first; };
    const auto tapeLoop = [] (double f) { return db (analog2Pole (false, 3400.0, 0.52, f) * analog1Pole (true, 120.0, f)); };
    const auto tapeIn = [] (double f) { return db (analog2Pole (false, 3100.0, 0.66, f) * analog1Pole (true, 50.0, f)); };
    const auto lofiIn = [] (double f) { return -2.0 + warpedButterworthDb (4, 2300.0, f, 48000.0) + db (analog2Pole (true, 200.0, std::sqrt (0.5), f)); };
    std::vector<Target> targets;
    for (double f : { 100.0, 300.0, 1000.0, 2000.0, 3400.0, 5000.0 })
    {
        targets.push_back ({ Delay::tape, f, tapeLoop (f), tapeIn (f) });
        targets.push_back ({ Delay::lofi, f, tapeLoop (f), lofiIn (f) });
    }
    for (double f : { 50.0, 300.0, 1000.0, 3000.0, 8000.0, 12000.0, 18000.0 }) targets.push_back ({ Delay::clean, f, 0.0, 0.0 });
    for (double f : { 300.0, 1000.0, 2000.0, 2180.0, 3000.0, 4360.0 })
        targets.push_back ({ Delay::bbd, f, warpedButterworthDb (7, 2180.0, f, fs), warpedButterworthDb (7, 2180.0, f, fs) });
    for (double f : { 300.0, 1000.0, 5000.0, 8000.0, 8400.0, 10000.0, 16800.0 })
        targets.push_back ({ Delay::digital, f, warpedButterworthDb (10, 8400.0, f, fs) + adaaDb (f, fs), warpedButterworthDb (10, 8400.0, f, fs) + adaaDb (f, fs) });

    for (const auto& t : targets)
    {
        const auto x = mono (burst ((size_t) (3.6 * T), 4096, fs, 0.01, t.hz));
        const auto w = wetOf (run (x, fs, freeTime (delaySettings (t.mode, 100.0, 100.0), 250.0)).left, x.left);
        const auto echo = [&] (int k) { return levelAt (w, (size_t) (k * T), win, t.hz, fs); };
        const double in = levelAt (x.left, 0, win, t.hz, fs);
        const double e1 = echo (1), e2 = echo (2), e3 = echo (3);
        const double perRepeat = 0.5 * (e3 - e1), first = e1 - in;
        const bool deep = t.perRepeat < -25.0;   // two repeats down sits in the floor: the first echo (one pass) says it
        const double tol = t.mode == Delay::clean ? 0.01 : 1.0;
        INFO (modeName (t.mode) << " at " << t.hz << " Hz: per repeat " << perRepeat << " dB (target " << t.perRepeat << "), first echo "
                                << first << " dB (target " << t.first << ")");
        if (! deep)
        {
            CHECK (std::abs (perRepeat - t.perRepeat) <= tol);
            CHECK (std::abs ((e2 - e1) - (e3 - e2)) <= 0.05);   // the same every repeat
        }
        CHECK (std::abs (first - t.first) <= (t.first < -40.0 ? 3.0 : tol));
        report ("loop  %-7s %6.0f Hz: per repeat %+7.2f dB (target %+7.2f), first echo %+7.2f dB (target %+7.2f)\n", modeName (t.mode), t.hz,
                deep ? 0.0 : perRepeat, deep ? 0.0 : t.perRepeat, first, t.first);
    }
    // Digital's slope: >= 60 dB from its corner to the octave above.
    const auto x = mono (noise ((size_t) (2.0 * fs), 21, 0.05));
    const auto w = wetOf (run (x, fs, freeTime (delaySettings (Delay::digital, 100.0, 0.0), 10.0)).left, x.left);
    const auto h = transferDb (x.left, w, 480, 0, w.size());
    const auto bin = [&] (double f) { return h[(size_t) std::lround (f * 4096.0 / fs)]; };
    INFO ("Digital: " << bin (8400.0) - bin (1000.0) << " dB at 8.4 kHz, " << bin (16800.0) - bin (1000.0) << " dB at 16.8 kHz");
    CHECK (bin (8400.0) - bin (16800.0) >= 57.0);
    report ("loop  Digital slope: %+.1f dB at 8.4 kHz, %+.1f dB at 16.8 kHz\n", bin (8400.0) - bin (1000.0), bin (16800.0) - bin (1000.0));
}

TEST_CASE ("Delay: Lo-Fi's Age narrows its band and drops its level", "[delay][loop]")
{
    // A quiet tone per 1/24 octave; the first echo's RMS over the tone's:
    // the warble moves the tone's phase but not its power, and the kink's
    // harmonics (28 dB down or more) add under 0.01 dB.
    const double fs = 48000.0;
    for (double age : { 0.0, 50.0, 100.0 })
    {
        std::vector<std::pair<double, double>> curve;   // Hz, dB
        for (double f = 300.0; f < 4000.0; f *= std::exp2 (1.0 / 24.0))
        {
            const auto x = mono (sine (f, fs, (size_t) (0.35 * fs), 0.03));
            const auto w = wetOf (run (x, fs, freeTime (delaySettings (Delay::lofi, 100.0, 0.0, Delay::mono, age), 100.0)).left, x.left);
            curve.emplace_back (f, db (rms (w, (size_t) (0.18 * fs)) / rms (x.left, (size_t) (0.18 * fs))));
        }
        size_t peak = 0;
        for (size_t k = 1; k < curve.size(); ++k) if (curve[k].second > curve[peak].second) peak = k;
        size_t k = peak;
        while (k + 1 < curve.size() && curve[k + 1].second > curve[peak].second - 3.0) ++k;
        // The -3 dB crossing, interpolated in log frequency.
        const double frac = (curve[k].second - (curve[peak].second - 3.0)) / (curve[k].second - curve[k + 1].second);
        const double edge = curve[k].first * std::exp2 (frac / 24.0);
        const double a = age / 100.0;
        const double edgeTarget = 2300.0 * std::pow (1200.0 / 2300.0, a), levelTarget = -2.0 - 9.0 * a;
        INFO ("Age " << age << ": peak " << curve[peak].second << " dB at " << curve[peak].first << " Hz (target level " << levelTarget
                     << "), upper -3 dB at " << edge << " Hz (target " << edgeTarget << ")");
        CHECK (std::abs (edge - edgeTarget) <= 0.04 * edgeTarget);
        CHECK (std::abs (curve[peak].second - levelTarget) <= 1.0);
        report ("lofi  Age %3.0f %%: level %+6.2f dB (target %+6.2f), -3 dB at %5.0f Hz (target %5.0f)\n", age, curve[peak].second, levelTarget, edge, edgeTarget);
    }
}

TEST_CASE ("Delay: each mode's drive curve and 2nd harmonic match the reference's", "[delay][drive]")
{
    const double fs = 48000.0;
    const double T = 0.1 * fs;
    struct Point { int mode; double hz, dbfs, gain; };
    const std::vector<Point> points {
        { Delay::tape, 1000.0, -12.0, -0.36 }, { Delay::tape, 1000.0, -6.0, -1.3 }, { Delay::tape, 1000.0, 0.0, -3.7 },
        { Delay::digital, 1000.0, -12.0, -0.3 }, { Delay::digital, 1000.0, -6.0, -1.1 }, { Delay::digital, 1000.0, 0.0, -3.3 },
        { Delay::clean, 1000.0, -12.0, -0.27 }, { Delay::clean, 1000.0, -6.0, -0.98 }, { Delay::clean, 1000.0, 0.0, -3.0 },
        { Delay::bbd, 1000.0, -18.0, -0.6 }, { Delay::bbd, 1000.0, -6.0, -3.0 }, { Delay::bbd, 1000.0, 0.0, -6.4 },
    };
    const auto echoLevel = [&] (int mode, double hz, double dbfs, double age, double* second) {
        const auto x = mono (sine (hz, fs, (size_t) (T + 1.2 * fs), fromDb (dbfs)));
        const auto w = wetOf (run (x, fs, freeTime (delaySettings (mode, 100.0, 0.0, Delay::mono, age), 100.0)).left, x.left);
        const auto from = (size_t) (T + 0.4 * fs);
        const double f1 = toneLevel (w, hz, fs, from);
        if (second != nullptr) *second = harmonicDbc (w, from, hz, 2, fs);
        return db (f1) - dbfs;
    };
    for (const auto& p : points)
    {
        const double small = echoLevel (p.mode, p.hz, -40.0, 0.0, nullptr);
        const double gain = echoLevel (p.mode, p.hz, p.dbfs, 0.0, nullptr) - small;
        INFO (modeName (p.mode) << " at " << p.dbfs << " dBFS: " << gain << " dB (target " << p.gain << ")");
        CHECK (std::abs (gain - p.gain) <= 1.0);
        report ("drive %-7s %+4.0f dBFS: %+6.2f dB (target %+6.2f)\n", modeName (p.mode), p.dbfs, gain, p.gain);
    }
    struct Harmonic { int mode; double hz, dbfs, age, target, tol; };
    for (const auto& h : { Harmonic { Delay::tape, 1000.0, -18.0, 0.0, -44.0, 1.5 }, Harmonic { Delay::bbd, 500.0, -18.0, 0.0, -47.0, 1.5 },
                           Harmonic { Delay::bbd, 500.0, -6.0, 0.0, -47.0, 2.0 }, Harmonic { Delay::lofi, 500.0, -18.0, 0.0, -37.0, 1.5 },
                           Harmonic { Delay::lofi, 500.0, -18.0, 100.0, -28.0, 1.5 }, Harmonic { Delay::digital, 1000.0, -18.0, 0.0, -300.0, 0.0 },
                           Harmonic { Delay::clean, 1000.0, -6.0, 0.0, -300.0, 0.0 } })
    {
        double second = 0.0;
        echoLevel (h.mode, h.hz, h.dbfs, h.age, &second);
        INFO (modeName (h.mode) << " at " << h.dbfs << " dBFS, Age " << h.age << ": 2nd " << second << " dBc (target " << h.target << ")");
        if (h.target > -100.0) CHECK (std::abs (second - h.target) <= h.tol);
        else CHECK (second < -90.0);   // symmetric: no even harmonics
        report ("2nd   %-7s %+4.0f dBFS Age %3.0f: %+6.1f dBc (target %+6.1f)\n", modeName (h.mode), h.dbfs, h.age, second, h.target);
    }
}

TEST_CASE ("Delay: 100 % feedback rings bounded in every mode and style, for 30 s", "[delay][feedback]")
{
    const double fs = 48000.0;
    const auto n = (size_t) (31.0 * fs);
    // Half a second of loud noise and a full-scale tone, then nothing.
    Signal burstL = noise (n, 11, 0.9), burstR = noise (n, 12, 0.9);
    for (size_t i = 0; i < n; ++i)
    {
        if (i < (size_t) (0.5 * fs)) { burstL[i] += (float) (0.5 * std::sin (2.0 * pi * 440.0 * (double) i / fs)); burstR[i] = burstL[i]; }
        else burstL[i] = burstR[i] = 0.f;
    }
    const Stereo x { burstL, burstR };
    for (int mode = 0; mode < Delay::kModes; ++mode)
        for (int style : { (int) Delay::mono, (int) Delay::pingPong })
            for (double diffuse : { 0.0, 100.0 })
            {
                auto s = delaySettings (mode, 100.0, 100.0, style, 25.0);
                s[P::delayDiffuse] = (float) diffuse;
                const auto y = run (x, fs, s, 512, steady (145.0));
                const auto last = (size_t) (30.0 * fs);
                const double peak = std::max (peakAbs (y.left, last), peakAbs (y.right, last));
                const double level = db (std::max (rms (y.left, last), rms (y.right, last)));
                INFO (modeName (mode) << (style == Delay::mono ? ", Mono" : ", Ping-Pong") << ", Diffuse " << diffuse << ": the last second peaks at "
                                      << db (peak) << " dBFS, RMS " << level << " dBFS");
                CHECK ((allFinite (y.left) && allFinite (y.right)));
                CHECK (peak < 1.0);
                report ("fb100 %-7s %-9s diffuse %3.0f: peak %+7.1f dBFS, rms %+7.1f dBFS after 30 s\n", modeName (mode),
                        style == Delay::mono ? "Mono" : "Ping-Pong", diffuse, db (peak), level);
            }
}

// ---- Modulation and noise ------------------------------------------------------

TEST_CASE ("Delay: each mode's modulation, in cents RMS", "[delay][modulation]")
{
    const double fs = 48000.0;
    struct Case { int mode; double age, cents, tol; };
    const double tapeAt = [] (double a) { return std::sqrt (std::pow (22.6 * std::pow (a, 0.75), 2.0) + std::pow (14.0 * a, 2.0) + std::pow (4.5 * a, 2.0)); } (0.25);
    const double tapeFull = std::sqrt (22.6 * 22.6 + 14.0 * 14.0 + 4.5 * 4.5);
    const std::vector<Case> cases {
        { Delay::tape, 0.0, 0.0, 0.05 }, { Delay::tape, 25.0, tapeAt, 0.35 }, { Delay::tape, 100.0, tapeFull, 0.35 },
        { Delay::bbd, 0.0, 0.0, 0.05 }, { Delay::bbd, 25.0, 3.0, 0.2 }, { Delay::bbd, 100.0, 12.0, 0.2 },
        { Delay::digital, 0.0, 0.0, 0.05 }, { Delay::digital, 25.0, 26.6 / 16.0, 0.25 }, { Delay::digital, 100.0, 26.6, 0.2 },
        { Delay::clean, 100.0, 0.0, 0.05 },
        { Delay::lofi, 0.0, 0.0, 0.05 }, { Delay::lofi, 25.0, 4.0, 0.35 }, { Delay::lofi, 50.0, 15.0, 0.3 }, { Delay::lofi, 75.0, 31.0, 0.3 },
        { Delay::lofi, 100.0, 51.0, 0.3 },
    };
    const double seconds = 24.0;
    const auto x = mono (sine (1000.0, fs, (size_t) (seconds * fs), 0.1));
    for (const auto& c : cases)
    {
        const auto w = wetOf (run (x, fs, freeTime (delaySettings (c.mode, 100.0, 0.0, Delay::mono, c.age), 300.0)).left, x.left);
        const auto track = pitchCents (w, 1000.0, fs, (size_t) (2.0 * fs));
        const double measured = rmsOf (track);
        INFO (modeName (c.mode) << ", Age " << c.age << ": " << measured << " cents RMS (target " << c.cents << ")");
        if (c.cents == 0.0) CHECK (measured < c.tol);
        else CHECK (std::abs (measured - c.cents) <= c.tol * c.cents);
        report ("mod   %-7s Age %3.0f %%: %6.2f cents RMS (target %6.2f)\n", modeName (c.mode), c.age, measured, c.cents);
    }
}

TEST_CASE ("Delay: BBD hisses with Age, darkly; no other mode makes noise", "[delay][noise]")
{
    const double fs = 48000.0;
    const auto x = silence ((size_t) (3.0 * fs));
    for (double age : { 50.0, 100.0 })
    {
        const auto y = run (x, fs, delaySettings (Delay::bbd, 100.0, 0.0, Delay::mono, age));
        const double level = db (rms (y.left, (size_t) (0.5 * fs)));
        const double target = -60.0 + 40.0 * std::log10 (age / 100.0);
        // Dark: the band above 4 kHz is 30 dB under the whole.
        Biquad h1 = Biquad::highpass (4000.0, 0.5412, fs), h2 = Biquad::highpass (4000.0, 1.3066, fs);
        Signal top (y.left.size());
        for (size_t i = 0; i < top.size(); ++i) top[i] = (float) h2.process (h1.process (y.left[i]));
        const double above = db (rms (top, (size_t) (0.5 * fs)));
        INFO ("Age " << age << ": hiss " << level << " dBFS RMS (target " << target << "), above 4 kHz " << above << " dBFS");
        CHECK (std::abs (level - target) <= 1.5);
        CHECK (above < level - 25.0);
        CHECK (correlation (y.left, y.right, (size_t) (0.5 * fs)) < 0.1);   // each channel its own
        report ("hiss  BBD Age %3.0f %%: %+6.1f dBFS RMS (target %+6.1f), above 4 kHz %+6.1f dBFS\n", age, level, target, above);
    }
    for (int mode : { (int) Delay::tape, (int) Delay::digital, (int) Delay::clean, (int) Delay::lofi })
    {
        const auto y = run (x, fs, delaySettings (mode, 100.0, 60.0, Delay::pingPong, 100.0));
        INFO (modeName (mode) << ": peak " << peakAbs (y.left) << " / " << peakAbs (y.right));
        CHECK (peakAbs (y.left) < 1e-9);
        CHECK (peakAbs (y.right) < 1e-9);
    }
}

// ---- Duck --------------------------------------------------------------------------

TEST_CASE ("Delay: Duck pulls the echoes down >= 12 dB while the key plays and gives them back within the release", "[delay][duck]")
{
    const double fs = 48000.0;
    const auto n = (size_t) (3.0 * fs);
    // The sidechain: a steady voice-level tone into a steady echo (feedback 0).
    {
        const auto x = mono (sine (500.0, fs, n, 0.1));
        Signal key (n, 0.f);
        const auto on = (size_t) (1.0 * fs), off = (size_t) (1.8 * fs);
        for (size_t i = on; i < off; ++i) key[i] = (float) (0.1 * std::sin (2.0 * pi * 220.0 * (double) i / fs));   // -20 dBFS
        auto s = delaySettings (Delay::clean, 100.0, 0.0, Delay::mono);
        const auto plain = wetOf (run (x, fs, s, 64, steady (120.0), &key).left, x.left);
        s[P::delayDuck] = 100.f;
        voc::Delay m;
        const auto ducked = wetOf (run (m, x, fs, s, 64, steady (120.0), &key).left, x.left);
        const double during = db (rms (ducked, on + (size_t) (0.1 * fs), off) / rms (plain, on + (size_t) (0.1 * fs), off));
        const double after = db (rms (ducked, off + (size_t) (0.5 * fs), off + (size_t) (0.7 * fs)) / rms (plain, off + (size_t) (0.5 * fs), off + (size_t) (0.7 * fs)));
        const double before = db (rms (ducked, (size_t) (0.6 * fs), on) / rms (plain, (size_t) (0.6 * fs), on));
        INFO ("sidechain: before the key " << before << " dB, during " << during << " dB, 0.5 s after " << after << " dB");
        CHECK (during <= -12.0);
        CHECK (after >= -1.0);
        CHECK (std::abs (before) < 0.1);   // the voice itself doesn't duck when the key is the sidechain
        report ("duck  sidechain at 100 %%: during %+6.1f dB, 0.5 s after %+5.2f dB, before %+5.2f dB\n", during, after, before);
    }
    // Its own input: a phrase, then a gap; the echo of the phrase is ducked
    // while the phrase lasts and swells in the gap.
    {
        Signal v (n, 0.f);
        const auto end = (size_t) (1.5 * fs);
        for (size_t i = 0; i < end; ++i) v[i] = (float) (0.1 * std::sin (2.0 * pi * 300.0 * (double) i / fs));
        const auto x = mono (v);
        auto s = freeTime (delaySettings (Delay::clean, 100.0, 0.0, Delay::mono), 1000.0);
        const auto plain = wetOf (run (x, fs, s).left, x.left);
        s[P::delayDuck] = 100.f;
        const auto ducked = wetOf (run (x, fs, s).left, x.left);
        const double during = db (rms (ducked, (size_t) (1.1 * fs), end) / rms (plain, (size_t) (1.1 * fs), end));
        const double gap = db (rms (ducked, end + (size_t) (0.5 * fs), (size_t) (2.45 * fs)) / rms (plain, end + (size_t) (0.5 * fs), (size_t) (2.45 * fs)));
        INFO ("own input: while singing " << during << " dB, in the gap " << gap << " dB");
        CHECK (during <= -12.0);
        CHECK (gap >= -1.0);
        report ("duck  own input at 100 %%: while singing %+6.1f dB, in the gap %+5.2f dB\n", during, gap);
    }
}

TEST_CASE ("Delay: the duck's thresholds and the feedback's share", "[delay][duck]")
{
    const double fs = 48000.0;
    const auto n = (size_t) (2.0 * fs);
    // A steady echo and a steady key at -20 dBFS: the output's reduction is
    // the key's excess over 13 - 60 d dBFS (past the knee).
    for (double d : { 0.25, 0.5, 0.75, 1.0 })
    {
        const auto x = mono (sine (500.0, fs, n, 0.1));
        const Signal key = sine (220.0, fs, n, 0.1);
        auto s = delaySettings (Delay::clean, 100.0, 0.0, Delay::mono);
        const auto plain = wetOf (run (x, fs, s, 64, steady (120.0), &key).left, x.left);
        s[P::delayDuck] = (float) (100.0 * d);
        const auto ducked = wetOf (run (x, fs, s, 64, steady (120.0), &key).left, x.left);
        const double gr = -db (rms (ducked, (size_t) (1.0 * fs)) / rms (plain, (size_t) (1.0 * fs)));
        // The follower reads a steady sine near its peak (-20 dBFS).
        const double over = -20.0 - (13.0 - 60.0 * d);
        const double expected = over <= -5.0 ? 0.0 : over >= 5.0 ? over : (over + 5.0) * (over + 5.0) / 20.0;
        INFO ("duck " << d * 100.0 << " %: " << gr << " dB of reduction (expected about " << expected << ")");
        CHECK (std::abs (gr - expected) <= 2.0);
        report ("duck  %3.0f %%: key -20 dBFS, the echo down %5.1f dB (expected %5.1f)\n", d * 100.0, gr, expected);
    }
}

TEST_CASE ("Delay: a key between the two thresholds ducks the repeats, not the first echo", "[delay][duck]")
{
    // Duck 50 %: output threshold -17 dBFS, feedback -28. A steady key near
    // -23 dBFS (as the follower reads a -22 dBFS sine) sits 5 dB over the
    // feedback's and 6 under the output's (outside its knee).
    const double fs = 48000.0;
    const double T = 0.25 * fs;
    const auto n = (size_t) (4.0 * T);
    const auto x = mono (burst (n, (size_t) (0.5 * T), fs, 0.05));
    const Signal key = sine (220.0, fs, n, fromDb (-22.0));
    auto s = freeTime (delaySettings (Delay::clean, 100.0, 50.0, Delay::mono), 250.0);
    const auto plain = wetOf (run (x, fs, s, 64, steady (120.0), &key).left, x.left);
    s[P::delayDuck] = 50.f;
    const auto ducked = wetOf (run (x, fs, s, 64, steady (120.0), &key).left, x.left);
    const auto echo = [&] (const Signal& w, int k) { return db (rms (w, (size_t) ((k + 0.25) * T), (size_t) ((k + 0.75) * T))); };
    const double first = echo (ducked, 1) - echo (plain, 1);
    const double perRepeat = (echo (ducked, 3) - echo (ducked, 1)) / 2.0, plainRepeat = (echo (plain, 3) - echo (plain, 1)) / 2.0;
    INFO ("first echo " << first << " dB re unducked; per repeat " << perRepeat << " dB (unducked " << plainRepeat << ")");
    CHECK (std::abs (first) < 0.5);
    CHECK (perRepeat < plainRepeat - 3.0);
    report ("duck  50 %%, key -22 dBFS: first echo %+5.2f dB, per repeat %+6.2f dB (unducked %+6.2f)\n", first, perRepeat, plainRepeat);
}

// ---- Diffuse ---------------------------------------------------------------------

TEST_CASE ("Delay: Diffuse smears the echo around its time, wider with the knob, L and R apart", "[delay][diffuse]")
{
    const double fs = 48000.0;
    const double T = 1.0 * fs;
    const auto x = mono (impulse ((size_t) (1.6 * T), 1000, 0.2));
    double lastSpread = 0.0;
    for (double amount : { 0.0, 30.0, 70.0, 100.0 })
    {
        auto s = freeTime (delaySettings (Delay::clean, 100.0, 0.0, Delay::mono), 1000.0);
        s[P::delayDiffuse] = (float) amount;
        const auto y = run (x, fs, s);
        const auto wl = wetOf (y.left, x.left), wr = wetOf (y.right, x.right);
        const auto from = (size_t) (1000.0 + 0.5 * T), to = (size_t) (1000.0 + 1.5 * T);
        // Where the energy sits and how wide (its standard deviation).
        const double c = centroid (wl, from, to);
        double e = 0.0, m2 = 0.0;
        for (size_t i = from; i < to; ++i)
        {
            e += (double) wl[i] * wl[i];
            m2 += (double) wl[i] * wl[i] * ((double) i - c) * ((double) i - c);
        }
        const double spreadMs = 1000.0 * std::sqrt (m2 / e) / fs, offsetMs = 1000.0 * (c - 1000.0 - T) / fs;
        const double r = correlation (wl, wr, from, to);
        INFO ("Diffuse " << amount << ": centre " << offsetMs << " ms from the echo time, spread " << spreadMs << " ms, L/R correlation " << r);
        // The design: 5 ms x 20^a of smear (at the full 100 ms length).
        const double designMs = 5.0 * std::pow (20.0, amount / 100.0);
        if (amount == 0.0)
        {
            CHECK (spreadMs < 0.05);
            CHECK (r > 0.999);
            CHECK (bitIdentical (wl, wr));   // the allpasses are pure delays, made up for
        }
        else
        {
            CHECK (std::abs (offsetMs) < 0.05 * spreadMs + 0.5);
            CHECK (spreadMs > lastSpread);
            CHECK (std::abs (spreadMs - designMs) < 0.1 * designMs);
            CHECK (r < (amount >= 70.0 ? 0.3 : 0.95));   // a light smear keeps most of the echo where it was on both sides
        }
        lastSpread = spreadMs;
        report ("diff  %3.0f %%: centre %+6.2f ms, spread %6.2f ms, L/R correlation %+5.2f\n", amount, offsetMs, spreadMs, r);
    }
}

// ---- Aliasing ----------------------------------------------------------------------

TEST_CASE ("Delay: the shapers' aliasing stays far down", "[delay][alias]")
{
    const double fs = 48000.0;
    struct Case { int mode; double hz, dbfs, limit; };
    for (const auto& c : { Case { Delay::digital, 7000.0, -6.0, -80.0 }, Case { Delay::digital, 3100.0, 0.0, -80.0 },
                           Case { Delay::tape, 2500.0, 0.0, -80.0 }, Case { Delay::bbd, 1500.0, 0.0, -70.0 } })
    {
        const auto x = mono (sine (c.hz, fs, (size_t) (1.2 * fs), fromDb (c.dbfs)));
        const auto w = wetOf (run (x, fs, freeTime (delaySettings (c.mode, 100.0, 0.0), 100.0)).left, x.left);
        const Signal steadyPart (w.begin() + (std::ptrdiff_t) (0.3 * fs), w.end());
        const double alias = worstAliasDb (steadyPart, fs, c.hz, 20000.0);
        INFO (modeName (c.mode) << ", " << c.hz << " Hz at " << c.dbfs << " dBFS: worst alias " << alias << " dB");
        CHECK (alias < c.limit);
        report ("alias %-7s %5.0f Hz %+3.0f dBFS: %+6.1f dB\n", modeName (c.mode), c.hz, c.dbfs, alias);
    }
}

// ---- Engine ------------------------------------------------------------------------

TEST_CASE ("Delay: the output doesn't depend on the block size, synced, ducked, modulated", "[delay][blocks]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 2.0;
    const auto v = synth::make (o).mono;
    const Stereo x { v, scaled (v, 0.7) };
    const Signal key = noise (v.size(), 9, 0.2);
    for (int mode = 0; mode < Delay::kModes; ++mode)
        for (bool sidechain : { false, true })
        {
            auto s = delaySettings (mode, 70.0, 60.0, Delay::pingPong, 60.0);
            s[P::delayDuck] = 50.f;
            s[P::delayDiffuse] = 40.f;
            s[P::delayDiv] = 6.f;
            s[P::quality] = mode == Delay::tape ? 1.f : 0.f;
            const Signal* k = sidechain ? &key : nullptr;
            const auto ref = run (x, fs, s, 64, steady (145.0), k);
            for (int block : { 1, 37, 4096 })
            {
                const auto y = run (x, fs, s, block, steady (145.0), k);
                INFO (modeName (mode) << (sidechain ? ", sidechain" : ", own key") << ", block " << block);
                CHECK (bitIdentical (y.left, ref.left));
                CHECK (bitIdentical (y.right, ref.right));
            }
        }
}

TEST_CASE ("Delay: a copied instance carries on exactly where the original is", "[delay][copy]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 3.0;
    const auto v = synth::make (o).mono;
    for (int mode = 0; mode < Delay::kModes; ++mode)
    {
        auto s = delaySettings (mode, 70.0, 70.0, Delay::pingPong, 50.0);
        s[P::delayDuck] = 40.f;
        s[P::delayDiffuse] = 50.f;
        s[P::delayLo] = 150.f;
        s[P::delayHi] = 6000.f;
        voc::Delay a, b;
        for (auto* m : { &a, &b })
        {
            m->set (s);
            m->prepare (fs, 64);
        }
        Signal la = v, ra = scaled (v, 0.8);
        const size_t half = v.size() / 2 + 13;
        voc::Block blk;
        blk.transport = steady (145.0);
        for (size_t at = 0; at < half; at += 64)
            a.process (la.data() + at, ra.data() + at, (int) std::min<size_t> (64, half - at), blk);
        // Mid-way through a mode change, too.
        s[P::delayMode] = (float) ((mode + 2) % Delay::kModes);
        a.set (s);
        b.set (s);
        a.process (la.data() + half, ra.data() + half, 7, blk);
        const auto rings = a.rings();
        for (size_t i = 0; i < rings.size(); ++i) b.rings()[i]->copyAll (*rings[i]);
        b.copySmallState (a);
        Signal lb = la, rb = ra;
        for (size_t at = half + 7; at < v.size(); at += 64)
        {
            const int n = (int) std::min<size_t> (64, v.size() - at);
            a.process (la.data() + at, ra.data() + at, n, blk);
            b.process (lb.data() + at, rb.data() + at, n, blk);
        }
        INFO (modeName (mode));
        CHECK (a.engaged());
        CHECK (bitIdentical (Signal (la.begin() + (std::ptrdiff_t) half, la.end()), Signal (lb.begin() + (std::ptrdiff_t) half, lb.end())));
        CHECK (bitIdentical (Signal (ra.begin() + (std::ptrdiff_t) half, ra.end()), Signal (rb.begin() + (std::ptrdiff_t) half, rb.end())));
    }
}

TEST_CASE ("Delay: no latency, an honest tail, works at every rate", "[delay]")
{
    const double note = voc::sync::seconds (10, 145.0);   // 1/8D at 145 BPM
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int mode = 0; mode < Delay::kModes; ++mode)
        {
            // Busy and modulated: finite, no latency.
            {
                voc::Delay m;
                auto s = delaySettings (mode, 70.0, 60.0, Delay::pingPong, 60.0);
                s[P::delayDiffuse] = 50.f;
                s[P::delayDuck] = 50.f;
                const auto x = mono (noise ((size_t) (1.5 * fs), 41, 0.5));
                const auto y = run (m, x, fs, s, 512, steady (145.0));
                CHECK (m.latency() == 0);
                CHECK (m.wantsKey());
                CHECK ((allFinite (y.left) && allFinite (y.right)));
                CHECK (std::max (peakAbs (y.left), peakAbs (y.right)) < 2.0);
            }
            // The echo on the note, at every rate (Age 0, Mono).
            const double expected = note * fs;
            const auto at0 = (size_t) (0.05 * fs);
            const auto x = mono (burst ((size_t) (0.5 * fs), at0, fs));
            const auto w = wetOf (run (x, fs, delaySettings (mode), 512, steady (145.0)).left, x.left);
            const auto span = (size_t) (0.015 * fs);
            const double at = centroid (w, at0 + (size_t) expected - span, at0 + (size_t) expected + span) - centroid (x.left, 0, 2 * at0);
            INFO (modeName (mode) << " at " << fs << " Hz: the echo at " << at << " samples (the note: " << expected << ")");
            CHECK (std::abs (at - expected) <= 1.0);
            if (mode == Delay::tape) report ("rate  %6.0f Hz: Tape's echo %+5.2f samples off the note\n", fs, at - expected);
        }
    // An honest tail: until the repeats are 60 dB down, plus one period
    // (Ping-Pong's other side), at most 30 s.
    for (int mode = 0; mode < Delay::kModes; ++mode)
        for (double fb : { 0.0, 60.0, 100.0 })
        {
            voc::Delay m;
            m.set (delaySettings (mode, 70.0, fb, Delay::pingPong, 25.0));
            m.prepare (48000.0, 512);
            const double loop = (fb / 100.0) * (mode == Delay::tape || mode == Delay::lofi ? analog2Pole (false, 3400.0, 0.52, 1000.0) * analog1Pole (true, 120.0, 1000.0) : 1.0);
            const double expected = loop >= 0.999 ? 30.0 : std::min (30.0, ((loop > 0.0 ? 60.0 / -db (loop) : 0.0) + 2.0) * voc::sync::seconds (10, 120.0));   // no transport yet: 120 BPM
            INFO (modeName (mode) << ", feedback " << fb << ": tail " << m.tailSeconds() << " s (expected " << expected << ")");
            CHECK (std::abs (m.tailSeconds() - expected) < 0.01);
            m.set (delaySettings (mode, 0.0, fb));
            CHECK (m.tailSeconds() == 0.0);
        }
}

// ---- Clicks ------------------------------------------------------------------------

TEST_CASE ("Delay: mode, style, note, quality and knob changes while the echoes ring never click", "[delay][click]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 9.0;
    o.plosives = false;
    o.sibilanceDb = -12.0;
    const auto v = synth::make (o).mono;
    const auto x = mono (v);
    const auto base = delaySettings (Delay::tape, 70.0, 70.0, Delay::pingPong, 25.0);
    const auto ref = run (x, fs, base, 64, steady (145.0));
    const double pRef = std::max (worstClickP (ref.left, fs), worstClickP (ref.right, fs));
    const auto y = run (x, fs, base, 64, steady (145.0), nullptr, [&] (size_t at, voc::Settings& s, voc::Transport&) {
        const double t = (double) at / fs;
        const int step = (int) (t / 0.7);   // a change every 0.7 s
        s[P::delayMode] = (float) (step % Delay::kModes);
        s[P::delayStyle] = (float) ((step / 2) % Delay::kStyles);
        if (t > 0.35 && t < 0.65)   // first, a new mode and style every block (the latest waits for the fade)
        {
            s[P::delayMode] = (float) ((at / 64) % Delay::kModes);
            s[P::delayStyle] = (float) ((at / 192) % Delay::kStyles);
        }
        s[P::delayDiv] = (float) (t > 6.0 ? 8 : 10);
        s[P::quality] = (t > 4.0 && t < 5.0) ? 1.f : 0.f;
        s[P::delayDiffuse] = (float) (t > 5.2 && t < 6.6 ? 60.0 : 0.0);
        s[P::delayAge] = (float) (25.0 + 60.0 * std::max (0.0, std::sin (t)));
        s[P::delayLo] = (float) (t > 7.0 ? 400.0 : 20.0);
        s[P::delayHi] = (float) (t > 7.5 ? 3000.0 : 20000.0);
        s[P::delayDuck] = (float) (t > 3.0 ? 60.0 : 0.0);
        s[P::delayOn] = (t > 8.0 && t < 8.3) ? 0.f : 1.f;
    });
    const double pY = std::max (worstClickP (y.left, fs), worstClickP (y.right, fs));
    // The echoes alone, where the dry voice can't hide a click.
    const double pWet = std::max (worstClickP (wetOf (y.left, x.left), fs), worstClickP (wetOf (y.right, x.right), fs));
    const double pVoice = worstClickP (v, fs);

    // Control: the reference's echoes cut off at once (where they are
    // loudest) must read as a click.
    const auto wet = wetOf (ref.left, x.left);
    size_t cut = (size_t) (1.0 * fs);
    for (size_t i = cut; i < (size_t) (8.5 * fs); ++i) if (std::abs (wet[i]) > std::abs (wet[cut])) cut = i;
    Signal torn = ref.left;
    for (size_t i = cut; i < torn.size(); ++i) torn[i] = x.left[i];
    const double pControl = worstClickP (torn, fs);
    INFO ("worst click P " << pY << " (no changes: " << pRef << "), the echoes alone " << pWet << " (the voice: " << pVoice
                           << "); control, the echoes cut at " << (double) cut / fs << " s: " << pControl);
    CHECK (pY < std::max (4.0, 1.5 * pRef));
    CHECK (pWet < std::max (4.0, 1.5 * pVoice));
    CHECK (pControl > 4.0);
    CHECK ((allFinite (y.left) && allFinite (y.right)));
    report ("click changes: P %.2f (reference %.2f), echoes alone %.2f (voice %.2f), control %.2f\n", pY, pRef, pWet, pVoice, pControl);
}

// ---- Cost (hidden: ./vocanal_tests "[.delaybench]") --------------------------------

TEST_CASE ("Delay: what each mode costs at 48 kHz", "[.delaybench]")
{
    const double fs = 48000.0;
    const int block = 64;
    const auto x = Stereo { noise ((size_t) (10.0 * fs), 31, 0.3), noise ((size_t) (10.0 * fs), 32, 0.3) };
    for (int mode = 0; mode < Delay::kModes; ++mode)
        for (int busy = 0; busy < 2; ++busy)
        {
            // The bench's settings (mix 70, feedback 60, duck 70); busy adds
            // Diffuse, the high quality, full Age and both cuts.
            auto s = delaySettings (mode, 70.0, 60.0, Delay::pingPong, busy ? 100.0 : 25.0);
            s[P::delayDuck] = 70.f;
            if (busy)
            {
                s[P::delayDiffuse] = 100.f;
                s[P::quality] = 1.f;
                s[P::delayLo] = 150.f;
                s[P::delayHi] = 8000.f;
            }
            voc::Delay m;
            m.set (s);
            m.prepare (fs, block);
            Stereo y = x;
            voc::Block b;
            b.transport = steady (145.0);
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t at = 0; at + (size_t) block <= y.size(); at += (size_t) block)
                m.process (y.left.data() + at, y.right.data() + at, block, b);
            const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            std::printf ("cost  %-7s %-5s %6.1f ns a sample, %5.2f %% of a core\n", modeName (mode), busy ? "busy" : "bench",
                         1e9 * seconds / (double) y.size(), 100.0 * seconds / 10.0);
            CHECK (allFinite (y.left));
        }
}

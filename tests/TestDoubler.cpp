// DOUBLE: rest, the mono sum, where the doubles sit, how their pitch
// wanders (and that it never repeats), width, loudness and balance, the
// interpolation, determinism, block sizes, state copies and clicks.
#include "SynthVoice.h"
#include "TestUtil.h"
#include "fx/Doubler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <limits>
#include <string>

using namespace voc::testutil;
using voc::P;

namespace
{
    voc::Settings doubleSettings (double amount, double width = 100.0, bool hq = false)
    {
        voc::Settings s;
        s[P::doubler] = (float) amount;
        s[P::doubleWidth] = (float) width;
        s[P::quality] = hq ? 1.f : 0.f;
        return s;
    }

    // x through a fresh DOUBLE in blocks; `each` may change the settings at a
    // block's first sample and look at the module after it.
    template <typename Each>
    Stereo run (const Stereo& x, double fs, voc::Settings s, int block, Each&& each, voc::Doubler& m)
    {
        m.set (s);
        m.prepare (fs, block);
        Stereo y = x;
        for (size_t at = 0; at < y.size(); at += (size_t) block)
        {
            each (at, s);
            m.set (s);
            m.process (y.left.data() + at, y.right.data() + at, (int) std::min<size_t> ((size_t) block, y.size() - at), {});
        }
        return y;
    }

    Stereo run (const Stereo& x, double fs, const voc::Settings& s, int block = 64, int solo = 0)
    {
        voc::Doubler m;
        m.soloForTests (solo);
        return run (x, fs, s, block, [] (size_t, voc::Settings&) {}, m);
    }

    Signal sideOf (const Stereo& y)
    {
        Signal s (y.size());
        for (size_t i = 0; i < s.size(); ++i) s[i] = (float) (0.5 * ((double) y.left[i] - (double) y.right[i]));
        return s;
    }

    Signal midOf (const Stereo& y)
    {
        Signal s (y.size());
        for (size_t i = 0; i < s.size(); ++i) s[i] = (float) (0.5 * ((double) y.left[i] + (double) y.right[i]));
        return s;
    }

    synth::Voice voice (double fs, double seconds, uint64_t seed = 1, bool female = false)
    {
        synth::Options o;
        o.fs = fs;
        o.seconds = seconds;
        o.seed = seed;
        o.sibilanceDb = -12.0;
        if (female)
        {
            o.female = true;
            o.f0 = 220.0;
        }
        return synth::make (o);
    }

    // K-weighted power per 400 ms window, hops of 100 ms (BS.1770 momentary).
    std::vector<double> momentaryPower (const Signal& x, double fs)
    {
        KWeighting k (fs);
        const auto hop = (size_t) (fs / 10.0);
        std::vector<double> hops, out;
        double acc = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double y = k.process (x[i]);
            acc += y * y;
            if ((i + 1) % hop == 0)
            {
                hops.push_back (acc);
                acc = 0.0;
            }
        }
        for (size_t i = 3; i < hops.size(); ++i) out.push_back ((hops[i] + hops[i - 1] + hops[i - 2] + hops[i - 3]) / (4.0 * (double) hop));
        return out;
    }

    // The pitch of a tone near hz over time, in cents re hz, one value per
    // 10 ms: the tone mixed down to 0 Hz, low-passed (four one-poles at 60 Hz,
    // -93 dB at the 2 hz image), its phase unwrapped and differentiated hop
    // by hop. `phase` gets the unwrapped phase per hop (radians).
    std::vector<double> pitchTrackCents (const Signal& y, double hz, double fs, size_t from, std::vector<double>* phase = nullptr)
    {
        const double w = 2.0 * pi * hz / fs, a = 1.0 - std::exp (-2.0 * pi * 60.0 / fs);
        const auto hop = (size_t) std::lround (fs / 100.0);
        cplx s1 {}, s2 {}, s3 {}, s4 {};
        std::vector<double> ph;
        double prev = 0.0, unwrapped = 0.0;
        for (size_t i = 0; i < y.size(); ++i)
        {
            const cplx z = (double) y[i] * std::polar (1.0, -w * (double) i);
            s1 += a * (z - s1);
            s2 += a * (s1 - s2);
            s3 += a * (s2 - s3);
            s4 += a * (s3 - s4);
            if (i >= from && (i - from) % hop == 0)
            {
                const double p = std::arg (s4);
                if (! ph.empty()) unwrapped += std::remainder (p - prev, 2.0 * pi);
                prev = p;
                ph.push_back (unwrapped);
            }
        }
        std::vector<double> cents;
        for (size_t k = 1; k < ph.size(); ++k)
            cents.push_back (1200.0 * std::log2 (1.0 + (ph[k] - ph[k - 1]) / (2.0 * pi * hz * (double) hop / fs)));
        if (phase != nullptr) *phase = ph;
        return cents;
    }

    // How periodic a control track (100 values per second) is:
    //   share: the most of its power (Welch, 10.24 s Hann frames) that any
    //          three neighbouring 0.1 Hz bins hold, 0.1 to 10 Hz (an LFO: ~1);
    //   bins:  how many 0.1 Hz bins its power is spread over, (sum P)^2 /
    //          sum P^2 (an LFO: ~2, the window's main lobe);
    //   echo:  its autocorrelation's highest point after the first zero
    //          crossing, lags up to 10 s (an LFO: ~1 at its period).
    struct Periodicity
    {
        double share = 0.0, shareHz = 0.0, bins = 0.0, echo = 0.0, echoSeconds = 0.0;
    };

    Periodicity periodicity (const std::vector<double>& t)
    {
        Periodicity r;
        const size_t n = 1024;
        std::vector<double> power (n / 2, 0.0);
        for (size_t at = 0; at + n <= t.size(); at += n / 2)
        {
            double mean = 0.0;
            for (size_t i = 0; i < n; ++i) mean += t[at + i] / (double) n;
            std::vector<cplx> z (n);
            for (size_t i = 0; i < n; ++i) z[i] = (t[at + i] - mean) * (0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n));
            fft (z);
            for (size_t k = 0; k < n / 2; ++k) power[k] += std::norm (z[k]);
        }
        const auto lo = (size_t) std::ceil (0.1 * n / 100.0), hi = (size_t) (10.0 * n / 100.0);
        double total = 0.0, squares = 0.0;
        for (size_t k = lo; k <= hi; ++k)
        {
            total += power[k];
            squares += power[k] * power[k];
        }
        r.bins = total * total / squares;
        for (size_t k = lo + 1; k < hi; ++k)
            if (const double s = (power[k - 1] + power[k] + power[k + 1]) / total; s > r.share)
            {
                r.share = s;
                r.shareHz = (double) k * 100.0 / (double) n;
            }

        double mean = 0.0;
        for (double v : t) mean += v / (double) t.size();
        const auto ac = [&] (size_t lag) {
            double s = 0.0, s0 = 0.0;
            for (size_t i = 0; i < t.size(); ++i)
            {
                s0 += (t[i] - mean) * (t[i] - mean);
                if (i + lag < t.size()) s += (t[i] - mean) * (t[i + lag] - mean);
            }
            return s / s0;
        };
        size_t z = 1;
        while (z < 1000 && ac (z) > 0.0) ++z;
        r.echo = -1.0;
        for (size_t lag = z; lag <= 1000; ++lag)
            if (const double v = ac (lag); v > r.echo)
            {
                r.echo = v;
                r.echoSeconds = (double) lag / 100.0;
            }
        return r;
    }

    double ulpOf (float v)
    {
        const float a = std::abs (v);
        return (double) std::nextafter (a, std::numeric_limits<float>::infinity()) - (double) a;
    }

    // Per-channel K-weighted loudness: the loudest momentary and the whole.
    double integratedLufs (const Signal& x, double fs)
    {
        KWeighting k (fs);
        double e = 0.0;
        for (float v : x)
        {
            const double y = k.process (v);
            e += y * y;
        }
        return -0.691 + 10.0 * std::log10 (e / (double) x.size() + 1e-30);
    }
} // namespace

TEST_CASE ("Double: at rest the voice passes bit for bit", "[double][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto x = Stereo { noise ((size_t) (0.5 * fs), 4), noise ((size_t) (0.5 * fs), 5) };
    auto off = doubleSettings (80.0);
    off[P::doubleOn] = 0.f;
    for (const auto& s : { voc::Settings {}, doubleSettings (0.0), off, doubleSettings (80.0, 0.0), doubleSettings (0.0, 100.0, true) })
    {
        voc::Doubler m;
        const auto y = run (x, fs, s, 64, [] (size_t, voc::Settings&) {}, m);
        INFO ("fs " << fs << ", latency " << m.latency());
        CHECK (m.latency() == 0);
        CHECK_FALSE (m.engaged());
        CHECK (bitIdentical (y.left, x.left));
        CHECK (bitIdentical (y.right, x.right));
    }
}

TEST_CASE ("Double: no latency at any rate or quality; the doubles arrive 11 to 32 ms late", "[double][latency]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (bool hq : { false, true })
        {
            // An impulse after 0.3 s of silence: the side shows the two
            // doubles, nothing before them; the mid is the impulse itself.
            const auto at = (size_t) (0.3 * fs);
            const auto x = impulse ((size_t) (0.4 * fs), at, 0.5);
            const auto y = run ({ x, x }, fs, doubleSettings (100.0, 100.0, hq));
            const auto side = sideOf (y);
            const auto ms = [fs] (double t) { return (size_t) (t * 1e-3 * fs); };
            double before = 0.0;
            for (size_t i = 0; i < at + ms (10.0); ++i) before = std::max (before, (double) std::abs (side[i]));
            const auto peakIn = [&] (double a, double b) {
                size_t best = at + ms (a);
                for (size_t i = at + ms (a); i < at + ms (b); ++i)
                    if (std::abs (side[i]) > std::abs (side[best])) best = i;
                return std::pair { 1e3 * (double) (best - at) / fs, (double) std::abs (side[best]) };
            };
            const auto [ta, pa] = peakIn (10.0, 21.5);
            const auto [tb, pb] = peakIn (21.5, 33.0);
            INFO ("fs " << fs << (hq ? " high" : " normal") << ": doubles at " << ta << " ms (" << pa << ") and " << tb << " ms (" << pb
                        << "), the side before 10 ms peaks at " << before);
            voc::Doubler m;
            m.set (doubleSettings (100.0, 100.0, hq));
            m.prepare (fs, 64);
            CHECK (m.latency() == 0);
            CHECK (before == 0.0);
            CHECK ((ta > 11.0 && ta < 21.0));
            CHECK ((tb > 22.0 && tb < 32.0));
            CHECK ((pa > 0.05 && pb > 0.05));
            CHECK (bitIdentical (midOf (y), midOf ({ x, x })));
        }
}

TEST_CASE ("Double: the mono sum is the dry input, at any knob and width, mono or stereo", "[double][mono]")
{
    // L + R against the input's L + R, sample by sample, while every knob
    // moves: the only difference allowed is R's own rounding (half an ulp).
    const double fs = 48000.0;
    const auto v = voice (fs, 4.0).mono;
    const auto v2 = voice (fs, 4.0, 7).mono;
    for (bool stereo : { false, true })
        for (bool hq : { false, true })
        {
            const Stereo x { v, stereo ? sum (scaled (v2, 0.6), scaled (v, 0.3)) : v };
            voc::Doubler m;
            const auto y = run (x, fs, doubleSettings (0.0, 100.0, hq), 64, [&] (size_t at, voc::Settings& s) {
                const double t = (double) at / fs;
                s[P::doubler] = (float) std::clamp (60.0 * t, 0.0, 100.0);
                s[P::doubleWidth] = (float) (50.0 + 50.0 * std::sin (2.0 * pi * 0.9 * t));
                s[P::doubleOn] = (t > 2.6 && t < 2.9) ? 0.f : 1.f;
            }, m);
            double worstUlps = 0.0, err2 = 0.0, sig2 = 0.0;
            for (size_t i = 0; i < x.size(); ++i)
            {
                const double e = std::abs (((double) y.left[i] + y.right[i]) - ((double) x.left[i] + x.right[i]));
                worstUlps = std::max (worstUlps, e / ulpOf (y.right[i]));
                err2 += e * e;
                sig2 += ((double) x.left[i] + x.right[i]) * ((double) x.left[i] + x.right[i]);
            }
            const auto added = sum (sideOf (y), scaled (sideOf (x), -1.0));
            INFO ((stereo ? "stereo" : "mono") << (hq ? ", high" : ", normal") << ": worst " << worstUlps << " ulp of R, residual "
                  << 10.0 * std::log10 (err2 / sig2 + 1e-300) << " dB; the doubles' side " << db (rms (added) / rms (midOf (x))) << " dB re the mid");
            CHECK (worstUlps <= 0.5);
            CHECK (rms (added) > 0.1 * rms (midOf (x)));   // the doubles are really there
        }
}

TEST_CASE ("Double: a stereo input keeps its own side; the doubles come from its mid", "[double]")
{
    // The side added to a stereo input is the side a mono input of the same
    // mid gets, to the float rounding of the two outputs.
    const double fs = 48000.0;
    const auto a = voice (fs, 2.0).mono, b = voice (fs, 2.0, 5).mono;
    const Stereo x { a, scaled (b, 0.5) };
    const auto mid = midOf (x);
    const auto y = run (x, fs, doubleSettings (80.0, 70.0));
    const auto ym = run ({ mid, mid }, fs, doubleSettings (80.0, 70.0));
    const auto added = sum (sideOf (y), scaled (sideOf (x), -1.0)), addedMono = sideOf (ym);
    const double diff = maxAbsDiff (added, addedMono);
    INFO ("added side " << db (rms (added)) << " dB RMS; stereo vs mono input differ by at most " << diff << " (" << db (diff / peakAbs (added)) << " dB re its peak)");
    CHECK (diff < 1e-6);
    CHECK (rms (added) > 0.01);
}

TEST_CASE ("Double: each double's pitch wanders 5 to 15 cents, at no single rate, never repeating", "[double][detune]")
{
    // 90 s of a 440 Hz sine through one double at a time (the side, soloed),
    // its pitch tracked every 10 ms. Control: a tone vibrated by a sine LFO
    // (+-10 cents at 0.8 Hz) through the same analysis.
    const double fs = 48000.0, hz = 440.0;
    const auto x = sine (hz, fs, (size_t) (90.0 * fs), 0.5);
    const auto from = (size_t) (3.0 * fs);
    std::vector<std::vector<double>> tracks;
    for (int solo : { 1, 2 })
    {
        voc::Doubler m;
        m.soloForTests (solo);
        std::vector<double> delays;   // the module's own read delay, every 10 ms (ms)
        const auto y = run ({ x, x }, fs, doubleSettings (100.0), 480, [&] (size_t at, voc::Settings&) {
            if (at >= from) delays.push_back (1e3 * m.delaySamples (solo - 1) / fs);
        }, m);
        const auto cents = pitchTrackCents (sideOf (y), hz, fs, from);
        double peak = 0.0, sq = 0.0, lo = 1e9, hi = -1e9;
        for (double c : cents)
        {
            peak = std::max (peak, std::abs (c));
            sq += c * c;
        }
        for (double d : delays)
        {
            lo = std::min (lo, d);
            hi = std::max (hi, d);
        }
        const auto p = periodicity (cents), pd = periodicity (delays);
        // The delay modulation's spectrum, octave bands from 0.05 Hz.
        std::string bands;
        {
            const size_t n = 4096;   // 41 s frames: 0.024 Hz bins
            std::vector<double> power (n / 2, 0.0);
            for (size_t at = 0; at + n <= delays.size(); at += n / 2)
            {
                double mean = 0.0;
                for (size_t i = 0; i < n; ++i) mean += delays[at + i] / (double) n;
                std::vector<cplx> z (n);
                for (size_t i = 0; i < n; ++i) z[i] = (delays[at + i] - mean) * (0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n));
                fft (z);
                for (size_t k = 0; k < n / 2; ++k) power[k] += std::norm (z[k]);
            }
            double total = 0.0;
            for (size_t k = 2; k < n / 2; ++k) total += power[k];
            for (double f = 0.05; f < 6.4; f *= 2.0)
            {
                double e = 0.0;
                for (auto k = (size_t) std::ceil (f * n / 100.0); k < (size_t) std::ceil (2.0 * f * n / 100.0); ++k) e += power[k];
                char buf[64];
                std::snprintf (buf, sizeof buf, " %.2g-%.2g Hz %.0f %%;", f, 2.0 * f, 100.0 * e / total);
                bands += buf;
            }
        }
        INFO ("double " << solo << ": pitch peak " << peak << " cents, RMS " << std::sqrt (sq / (double) cents.size())
                        << "; spread over " << p.bins << " bins of 0.1 Hz, strongest 0.3 Hz band " << 100.0 * p.share << " % at "
                        << p.shareHz << " Hz, echo " << p.echo << " at " << p.echoSeconds << " s; delay " << lo << " .. " << hi
                        << " ms, spread over " << pd.bins << " bins, octaves:" << bands);
        CHECK ((peak >= 5.0 && peak <= 15.0));
        CHECK (peak < 14.5);   // the soft limit is 14 cents
        CHECK (p.bins > 5.0);
        CHECK (p.echo < 0.3);
        CHECK ((lo > 11.0 && hi < 32.0));
        tracks.push_back (cents);
    }
    const double together = correlation (Signal (tracks[0].begin(), tracks[0].end()), Signal (tracks[1].begin(), tracks[1].end()));

    Signal lfo (x.size());
    for (size_t i = 0; i < lfo.size(); ++i)
    {
        const double t = (double) i / fs, d = 1.15e-3 * std::sin (2.0 * pi * 0.8 * t);
        lfo[i] = (float) (0.5 * std::sin (2.0 * pi * hz * (t - d)));
    }
    const auto control = pitchTrackCents (lfo, hz, fs, from);
    double controlPeak = 0.0;
    for (double c : control) controlPeak = std::max (controlPeak, std::abs (c));
    const auto pc = periodicity (control);
    INFO ("the two doubles' pitch tracks correlate " << together << "; LFO control: peak " << controlPeak << " cents, "
          << pc.bins << " bins, band " << 100.0 * pc.share << " % at " << pc.shareHz << " Hz, echo " << pc.echo << " at " << pc.echoSeconds << " s");
    CHECK (std::abs (together) < 0.3);
    CHECK (std::abs (controlPeak - 10.0) < 0.5);   // the tracker reads cents right
    CHECK (pc.bins < 3.0);
    CHECK (pc.share > 0.9);
    CHECK (pc.echo > 0.9);
}

TEST_CASE ("Double: the knob widens the voice: L/R correlation falls", "[double]")
{
    const double fs = 48000.0;
    const auto v = voice (fs, 6.0).mono;
    const auto from = (size_t) (0.5 * fs);
    const auto corr = [&] (double amount, double width) {
        const auto y = run ({ v, v }, fs, doubleSettings (amount, width));
        return correlation (y.left, y.right, from);
    };
    const std::array<double, 5> amounts { 0.0, 25.0, 50.0, 75.0, 100.0 }, widths { 0.0, 25.0, 50.0, 75.0, 100.0 };
    std::array<double, 5> byAmount {}, byWidth {};
    std::string line;
    for (size_t k = 0; k < 5; ++k)
    {
        byAmount[k] = corr (amounts[k], 100.0);
        byWidth[k] = corr (100.0, widths[k]);
        line += " " + std::to_string ((int) amounts[k]) + " %: " + std::to_string (byAmount[k]) + " / " + std::to_string (byWidth[k]) + ";";
    }
    INFO ("L/R correlation by Double (Width 100 %) / by Width (Double 100 %):" << line);
    for (size_t k = 1; k < 5; ++k)
    {
        CHECK (byAmount[k] < byAmount[k - 1]);
        CHECK (byWidth[k] < byWidth[k - 1]);
    }
    CHECK (byAmount[0] == 1.0);
    CHECK (byAmount[2] < 0.9);
    CHECK (byAmount[4] < 0.7);
}

TEST_CASE ("Double: each channel gets at most 1.5 dB louder, the mono sum not at all, the lead stays centred", "[double][loudness]")
{
    // Six synthetic voices (three low, three high) at the full knob: the
    // loudest moment (K-weighted 400 ms) and the whole of each channel
    // against the dry; the worst L/R balance of a loud moment (within 6 dB
    // of the loudest) and of any moment within 20 dB (the worst of those are
    // phrase ends: the doubles outlast the fading dry by 16-27 ms, and in an
    // otherwise quiet window that tail decides the balance).
    const double fs = 48000.0;
    for (int which = 0; which < 6; ++which)
    {
        const auto v = voice (fs, 8.0, (uint64_t) (which % 3 + 1), which >= 3).mono;
        const auto y = run ({ v, v }, fs, doubleSettings (100.0));
        const double dry = maxMomentaryLufs (v, {}, fs), dryAll = integratedLufs (v, fs);
        const double l = maxMomentaryLufs (y.left, {}, fs) - dry, r = maxMomentaryLufs (y.right, {}, fs) - dry;
        const double li = integratedLufs (y.left, fs) - dryAll, ri = integratedLufs (y.right, fs) - dryAll;
        const double mono = maxMomentaryLufs (midOf (y), {}, fs) - dry;
        const double side = integratedLufs (sideOf (y), fs) - dryAll;
        const auto pl = momentaryPower (y.left, fs), pr = momentaryPower (y.right, fs), pd = momentaryPower (v, fs);
        const double loudest = *std::max_element (pd.begin(), pd.end());
        double tiltLoud = 0.0, tiltAny = 0.0;
        for (size_t w = 0; w < pd.size(); ++w)
        {
            const double t = std::abs (10.0 * std::log10 (pl[w] / pr[w]));
            if (pd[w] > loudest * 0.25) tiltLoud = std::max (tiltLoud, t);
            if (pd[w] > loudest * 0.01) tiltAny = std::max (tiltAny, t);
        }
        INFO ((which >= 3 ? "high" : "low") << " voice " << which % 3 + 1 << ": loudest moment L " << l << " dB, R " << r << " dB; whole L " << li
                       << " dB, R " << ri << " dB; mono sum " << mono << " dB; the doubles' side " << side << " dB re the dry; worst L/R balance "
                       << tiltLoud << " dB loud, " << tiltAny << " dB within 20 dB");
        CHECK ((l <= 1.5 && r <= 1.5 && li <= 1.5 && ri <= 1.5));
        CHECK (std::abs (mono) < 1e-4);
        CHECK (side > -9.0);   // clearly doubled
        CHECK (tiltLoud < 3.5);
        CHECK (tiltAny < 4.0);
    }
}

TEST_CASE ("Double: the high quality reads flatter at the top, at the same delay", "[double]")
{
    // A 12 kHz tone through one double: as the drifting delay crosses
    // samples, the interpolator's top-end loss comes and goes. Its level per
    // millisecond (Teager-Kaiser: x[n]^2 - x[n-1] x[n+1] = A^2 sin^2 w for a
    // sine), max over min.
    const double fs = 48000.0, hz = 12000.0;
    const auto x = sine (hz, fs, (size_t) (12.0 * fs), 0.5);
    double wobble[2] {};
    for (int hq = 0; hq < 2; ++hq)
    {
        const auto side = sideOf (run ({ x, x }, fs, doubleSettings (100.0, 100.0, hq == 1), 64, 1));
        const auto win = (size_t) (0.001 * fs);
        double lo = 1e9, hi = 0.0;
        for (auto at = (size_t) (2.0 * fs); at + win + 1 < side.size(); at += win)
        {
            double e = 0.0;
            for (size_t i = at; i < at + win; ++i) e += (double) side[i] * side[i] - (double) side[i - 1] * side[i + 1];
            lo = std::min (lo, e);
            hi = std::max (hi, e);
        }
        wobble[hq] = 10.0 * std::log10 (hi / lo);
    }
    INFO ("12 kHz level wobble through a drifting double: Hermite " << wobble[0] << " dB, Lagrange " << wobble[1] << " dB");
    CHECK (wobble[1] < 0.6 * wobble[0]);
    CHECK (wobble[1] < 0.6);
}

TEST_CASE ("Double: two runs are identical", "[double]")
{
    const double fs = 48000.0;
    const auto v = voice (fs, 3.0).mono;
    const auto a = run ({ v, v }, fs, doubleSettings (70.0, 80.0));
    const auto b = run ({ v, v }, fs, doubleSettings (70.0, 80.0));
    CHECK (bitIdentical (a.left, b.left));
    CHECK (bitIdentical (a.right, b.right));

    CHECK_FALSE (bitIdentical (a.left, v));
}

TEST_CASE ("Double: the output doesn't depend on the block size", "[double][blocks]")
{
    const double fs = 48000.0;
    const auto v = voice (fs, 1.5).mono;
    const Stereo x { v, scaled (v, 0.7) };
    for (bool hq : { false, true })
    {
        const auto ref = run (x, fs, doubleSettings (70.0, 80.0, hq), 64);
        for (int block : { 1, 37, 4096 })
        {
            const auto y = run (x, fs, doubleSettings (70.0, 80.0, hq), block);
            INFO ("block " << block << (hq ? ", high" : ", normal"));
            CHECK (bitIdentical (y.left, ref.left));
            CHECK (bitIdentical (y.right, ref.right));
        }
    }
}

TEST_CASE ("Double: a copied instance carries on exactly where the original is", "[double][copy]")
{
    const double fs = 48000.0;
    const auto v = voice (fs, 2.0).mono;
    const auto s = doubleSettings (80.0, 90.0);
    voc::Doubler a, b;
    for (auto* m : { &a, &b })
    {
        m->set (s);
        m->prepare (fs, 64);
    }
    Signal la = v, ra = scaled (v, 0.8);
    const size_t half = v.size() / 2 + 13;
    for (size_t at = 0; at < half; at += 64)
        a.process (la.data() + at, ra.data() + at, (int) std::min<size_t> (64, half - at), {});
    const auto rings = a.rings();
    for (size_t i = 0; i < rings.size(); ++i) b.rings()[i]->copyAll (*rings[i]);
    b.copySmallState (a);
    Signal lb = la, rb = ra;
    for (size_t at = half; at < v.size(); at += 64)
    {
        const int n = (int) std::min<size_t> (64, v.size() - at);
        a.process (la.data() + at, ra.data() + at, n, {});
        b.process (lb.data() + at, rb.data() + at, n, {});
    }
    CHECK (a.engaged());
    CHECK (bitIdentical (Signal (la.begin() + (std::ptrdiff_t) half, la.end()), Signal (lb.begin() + (std::ptrdiff_t) half, lb.end())));
    CHECK (bitIdentical (Signal (ra.begin() + (std::ptrdiff_t) half, ra.end()), Signal (rb.begin() + (std::ptrdiff_t) half, rb.end())));
}

TEST_CASE ("Double: knob and width sweeps, power and quality toggles never click", "[double][click]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 6.0;
    o.plosives = false;
    o.sibilanceDb = -12.0;
    const auto v = synth::make (o).mono;
    const double dryP = worstClickP (v, fs);
    voc::Doubler m;
    const auto y = run ({ v, v }, fs, doubleSettings (0.0), 64, [&] (size_t at, voc::Settings& s) {
        const double t = (double) at / fs;
        s[P::doubler] = (float) (t < 2.0 ? std::clamp (100.0 * (t - 0.4), 0.0, 100.0)             // a sweep up
                                         : (t > 4.6 ? 100.0 * (5.6 - t) : 100.0));                // and down
        s[P::doubleWidth] = (float) (t > 2.0 && t < 4.0 ? 50.0 + 50.0 * std::cos (2.0 * pi * 0.8 * (t - 2.0)) : 100.0);
        s[P::doubleOn] = (t > 2.4 && t < 2.8) ? 0.f : 1.f;                                         // power off and on
        s[P::quality] = (t > 3.2 && t < 3.9) ? 1.f : 0.f;                                          // quality up and down
        if (t > 4.2 && t < 4.4) s[P::doubler] = 0.f;                                               // a jump to 0 and back
    }, m);
    const double pl = worstClickP (y.left, fs), pr = worstClickP (y.right, fs);

    // Control: the same output with its side cut off at once mid-vowel (where
    // the knob is at 100 %) must read as a click.
    const auto mid = midOf (y), side = sideOf (y);
    size_t cut = (size_t) (1.9 * fs);
    for (const auto& g : synth::make (o).of (synth::Seg::vowel))
        if (g.start > (size_t) (1.5 * fs) && g.end > g.start + (size_t) (0.1 * fs)) { cut = g.start + (size_t) (0.05 * fs); break; }
    Signal torn (y.size());
    for (size_t i = 0; i < torn.size(); ++i) torn[i] = mid[i] + (i < cut ? side[i] : 0.f);
    const double pc = worstClickP (torn, fs);
    INFO ("worst click P: L " << pl << ", R " << pr << " (the dry voice: " << dryP << "); control, the side cut at "
          << (double) cut / fs << " s: " << pc);
    CHECK (std::max (pl, pr) < std::max (4.0, 1.5 * dryP));
    CHECK (pc > 4.0);
    CHECK ((allFinite (y.left) && allFinite (y.right)));
}

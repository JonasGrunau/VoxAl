// RADIO: rest, latency, the band's corners, its response against the analog
// design (level, phase, delay), alias, a gentle saturation, loudness across
// the knob, Tone and voices, silence, block size, state copies and clicks.
#include "SynthVoice.h"
#include "TestUtil.h"
#include "fx/Radio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace voc::testutil;
using voc::P;

namespace
{
    voc::Settings radioSettings (double amount, double tone = 0.0)
    {
        voc::Settings s;
        s[P::radio] = (float) amount;
        s[P::radioTone] = (float) tone;
        return s;
    }

    // x through a fresh RADIO in blocks; `each` may change the settings at
    // a block's first sample.
    template <typename Each>
    Stereo run (const Stereo& x, double fs, voc::Settings s, int block, Each&& each, voc::Radio& m)
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

    Stereo run (const Stereo& x, double fs, const voc::Settings& s, int block = 64, int* latency = nullptr)
    {
        voc::Radio m;
        auto y = run (x, fs, s, block, [] (size_t, voc::Settings&) {}, m);
        if (latency != nullptr) *latency = m.latency();
        return y;
    }

    Signal aligned (const Signal& y, int lat) { return Signal (y.begin() + lat, y.end()); }

    // The transfer function, engaged and settled, from a small impulse
    // (-60 dBFS: the shaper is linear there to -80 dB), on n bins up to
    // Nyquist, the stated latency taken out.
    struct Response
    {
        std::vector<cplx> h;
        double fs = 48000.0;
        cplx at (double hz) const
        {
            const double k = hz / fs * 2.0 * (double) (h.size() - 1);
            const auto i = (size_t) k;
            return h[i] + (k - (double) i) * (h[i + 1] - h[i]);
        }
    };

    Response measure (double fs, const voc::Settings& s)
    {
        const size_t pre = (size_t) (0.25 * fs), len = 1 << 15, n = 1 << 17;
        const double amp = 1e-3;
        Signal x (pre + len, 0.f);
        x[pre] = (float) amp;
        voc::Radio m;
        const auto y = run ({ x, x }, fs, s, 64, [] (size_t, voc::Settings&) {}, m);
        std::vector<cplx> z (n);
        for (size_t i = 0; i < len; ++i) z[i] = y.left[pre + i] / amp;
        fft (z);
        const double delay = m.latency();
        Response r;
        r.fs = fs;
        r.h.resize (n / 2 + 1);
        for (size_t k = 0; k <= n / 2; ++k) r.h[k] = z[k] * std::polar (1.0, 2.0 * pi * (double) k * delay / (double) n);
        return r;
    }

    // Where the response falls 3 dB under the passband level, below and
    // above the band's centre.
    std::pair<double, double> corners (const Response& r, double passbandDb, double centreHz)
    {
        const double level = passbandDb - 3.0103;
        const auto dbAt = [&] (double hz) { return db (std::abs (r.at (hz))); };
        double lo = centreHz, hi = centreHz;
        while (lo > 10.0 && dbAt (lo) > level) lo /= 1.0005;
        while (hi < 0.49 * r.fs && dbAt (hi) > level) hi *= 1.0005;
        return { lo, hi };
    }

    double loudnessChange (const synth::Voice& v, double fs, const voc::Settings& s)
    {
        int lat = 0;
        const auto y = run ({ v.mono, v.mono }, fs, s, 64, &lat);
        const auto out = aligned (y.left, lat);
        return maxMomentaryLufs (out, out, fs) - maxMomentaryLufs (v.mono, v.mono, fs);
    }
} // namespace

TEST_CASE ("Radio: at rest the voice passes delayed, bit for bit", "[radio][rest]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto x = Stereo { noise ((size_t) (0.5 * fs), 4), noise ((size_t) (0.5 * fs), 5) };
    auto off = radioSettings (80.0, 40.0);
    off[P::radioOn] = 0.f;
    for (const auto& s : { voc::Settings {}, off, radioSettings (0.0, -70.0) })
    {
        int lat = 0;
        const auto y = run (x, fs, s, 64, &lat);
        INFO ("fs " << fs << ", latency " << lat);
        CHECK (bitIdentical (y.left, delayedBy (x.left, (size_t) lat)));
        CHECK (bitIdentical (y.right, delayedBy (x.right, (size_t) lat)));
    }
}

TEST_CASE ("Radio: the latency is the oversampler's round trip, fixed per rate", "[radio][latency]")
{
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        std::vector<int> lats;
        for (const auto& s : { voc::Settings {}, radioSettings (100.0), radioSettings (35.0, 80.0) })
            for (int hq = 0; hq < 2; ++hq)
            {
                auto t = s;
                t[P::quality] = (float) hq;
                voc::Radio m;
                m.set (t);
                m.prepare (fs, 256);
                lats.push_back (m.latency());
                // the inner rate stays at 176-192 kHz
                CHECK ((fs * m.oversampling() >= 176000.0 && fs * m.oversampling() <= 192000.0));
            }
        INFO ("fs " << fs << ": " << lats.front() << " samples");
        CHECK (std::all_of (lats.begin(), lats.end(), [&] (int l) { return l == lats.front(); }));
        CHECK (lats.front() / fs < 0.0025);
    }
}

TEST_CASE ("Radio: at 100 % the band's -3 dB corners are 300 Hz and 3.4 kHz", "[radio]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto d = voc::Radio::design (1.0, 0.0);
    CHECK (std::abs (d.lowHz - 300.0) < 1e-9);
    CHECK (std::abs (d.highHz - 3400.0) < 1e-9);
    const auto r = measure (fs, radioSettings (100.0));
    const auto [lo, hi] = corners (r, d.makeupDb, std::sqrt (d.lowHz * d.highHz));
    double peakHz = 0.0, peakDb = -1e9;
    for (double f = 100.0; f < 10000.0; f *= 1.002)
        if (const double l = db (std::abs (r.at (f))) - d.makeupDb; l > peakDb) { peakDb = l; peakHz = f; }
    INFO ("fs " << fs << ": measured corners " << lo << " Hz / " << hi << " Hz (stated 300 / 3400), resonance +" << peakDb
                << " dB at " << peakHz << " Hz");
    CHECK (std::abs (lo / 300.0 - 1.0) <= 0.05);
    CHECK (std::abs (hi / 3400.0 - 1.0) <= 0.05);
}

TEST_CASE ("Radio: Tone moves the band, and its corners land where it says", "[radio]")
{
    const double fs = 48000.0;
    for (double tone : { -100.0, -50.0, 50.0, 100.0 })
    {
        const auto d = voc::Radio::design (1.0, tone / 100.0);
        const auto [lo, hi] = corners (measure (fs, radioSettings (100.0, tone)), d.makeupDb, std::sqrt (d.lowHz * d.highHz));
        INFO ("Tone " << tone << ": stated " << d.lowHz << " / " << d.highHz << " Hz, measured " << lo << " / " << hi << " Hz");
        CHECK (std::abs (lo / d.lowHz - 1.0) <= 0.05);
        CHECK (std::abs (hi / d.highHz - 1.0) <= 0.05);
    }
    // the anchors: dark 250 Hz - 2.8 kHz, bright 500 Hz - 4.5 kHz
    CHECK (std::abs (voc::Radio::design (1.0, -1.0).lowHz - 250.0) < 1e-6);
    CHECK (std::abs (voc::Radio::design (1.0, -1.0).highHz - 2800.0) < 1e-6);
    CHECK (std::abs (voc::Radio::design (1.0, 1.0).lowHz - 500.0) < 1e-6);
    CHECK (std::abs (voc::Radio::design (1.0, 1.0).highHz - 4500.0) < 1e-6);
}

TEST_CASE ("Radio: the measured response is the analog design, in level and phase", "[radio]")
{
    // The level within 0.1 dB wherever the design is above -60 dB. The
    // phase, where the design is above -20 dB, is the analog phase and a
    // pure delay: the delay (least squares) within half a sample of the
    // stated latency (the inner low-passes run a little ahead of the analog
    // ones, the shaper 1.5 inner samples behind), and the rest within 0.02
    // rad (a whole sample would be 0.4 rad at 3 kHz).
    const double fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    for (double knob : { 30.0, 70.0, 100.0 })
    {
        const auto d = voc::Radio::design (knob / 100.0, 0.0);
        const auto r = measure (fs, radioSettings (knob));
        std::vector<std::pair<double, double>> phase;   // (Hz, measured - designed)
        double magErr = 0.0, sfp = 0.0, sff = 0.0;
        for (double f = 40.0; f < std::min (16000.0, 0.45 * fs); f *= 1.01)
        {
            const cplx want = voc::Radio::response (d, f), got = r.at (f);
            const double level = db (std::abs (want)) - d.makeupDb;
            if (level > -60.0) magErr = std::max (magErr, std::abs (db (std::abs (got)) - db (std::abs (want))));
            if (level <= -20.0) continue;
            phase.push_back ({ f, std::arg (got / want) });
            sfp += f * phase.back().second;
            sff += f * f;
        }
        const double slope = sfp / sff, delay = -slope * fs / (2.0 * pi);   // samples later than the latency
        double phaseErr = 0.0;
        for (const auto& [f, p] : phase) phaseErr = std::max (phaseErr, std::abs (p - slope * f));
        INFO ("fs " << fs << ", Radio " << knob << " %: worst level error " << magErr << " dB; delay latency + " << delay
                    << " samples, then phase within " << phaseErr << " rad");
        CHECK (magErr <= 0.1);
        CHECK (std::abs (delay) <= 0.5);
        CHECK (phaseErr <= 0.02);
    }
}

TEST_CASE ("Radio: a 3 kHz sine at -6 dBFS through 100 % has no aliases", "[radio][alias]")
{
    const double fs = GENERATE (44100.0, 48000.0);
    const auto x = sine (3000.0, fs, (size_t) (1.6 * fs), 0.5);
    int lat = 0;
    const auto yOut = run ({ x, x }, fs, radioSettings (100.0), 64, &lat);   // run () sets lat: read it after, not in the same call
    const auto y = aligned (yOut.left, lat);
    const Signal tail (y.begin() + (std::ptrdiff_t) (0.6 * fs), y.end());
    const double alias = worstAliasDb (tail, fs, 3000.0, 20000.0);
    INFO ("fs " << fs << ": worst non-harmonic " << alias << " dB re the fundamental, THD " << thdDb (tail, 3000.0, fs) << " dB");
    CHECK (alias < -85.0);

    // Where the band lets the harmonics through (a lower knob, higher tones).
    for (auto [hz, knob] : { std::pair { 5000.0, 60.0 }, { 7000.0, 40.0 }, { 9000.0, 30.0 }, { 4100.0, 100.0 } })
    {
        const auto xs = sine (hz, fs, (size_t) (1.6 * fs), 0.5);
        const auto ysOut = run ({ xs, xs }, fs, radioSettings (knob), 64, &lat);   // run () sets lat: read it after, not in the same call
        const auto ys = aligned (ysOut.left, lat);
        const double a = worstAliasDb (Signal (ys.begin() + (std::ptrdiff_t) (0.6 * fs), ys.end()), fs, hz, 20000.0);
        INFO ("fs " << fs << ", " << hz << " Hz at " << knob << " %: worst non-harmonic " << a << " dB");
        CHECK (a < -85.0);
    }
}

TEST_CASE ("Radio: the saturation is gentle, mostly 2nd harmonic, and grows with the knob", "[radio]")
{
    // A 1 kHz sine at -12 dBFS (a loud vowel's peak level) and -6 dBFS.
    const double fs = 48000.0;
    const auto distortion = [fs] (double knob, double amp, double& h2, double& h3) {
        const auto x = sine (1000.0, fs, (size_t) (1.6 * fs), amp);
        int lat = 0;
        const auto yOut = run ({ x, x }, fs, radioSettings (knob), 64, &lat);   // run () sets lat: read it after, not in the same call
        const auto y = aligned (yOut.left, lat);
        const Signal tail (y.begin() + (std::ptrdiff_t) (0.6 * fs), y.end());
        const double f = toneLevel (tail, 1000.0, fs);
        h2 = db (toneLevel (tail, 2000.0, fs) / f);
        h3 = db (toneLevel (tail, 3000.0, fs) / f);
        return thdDb (tail, 1000.0, fs);
    };
    double prev = -200.0, h2 = 0.0, h3 = 0.0;
    for (double knob : { 30.0, 50.0, 70.0, 100.0 })
    {
        const double thd = distortion (knob, 0.25, h2, h3);
        INFO ("Radio " << knob << " %, -12 dBFS: THD " << thd << " dB (H2 " << h2 << ", H3 " << h3 << ")");
        CHECK (thd > prev + 3.0);
        prev = thd;
    }
    CHECK ((prev > -40.0 && prev < -32.0));   // 1-2.5 % at full
    CHECK (h2 > h3 + 6.0);
    const double loud = distortion (100.0, 0.5, h2, h3);
    INFO ("Radio 100 %, -6 dBFS: THD " << loud << " dB (H2 " << h2 << ", H3 " << h3 << ")");
    CHECK (loud < -26.0);   // under 5 %
}

TEST_CASE ("Radio: the voice's loudness holds across the knob and Tone", "[radio][loudness]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 6.0;
    const auto v = synth::make (o);
    for (double knob : { 5.0, 15.0, 30.0, 50.0, 70.0, 85.0, 100.0 })
    {
        const double change = loudnessChange (v, fs, radioSettings (knob));
        INFO ("Radio " << knob << " %: " << change << " LU");
        CHECK (std::abs (change) <= 1.0);
    }
    for (double tone : { -100.0, -50.0, 50.0, 100.0 })
    {
        const double change = loudnessChange (v, fs, radioSettings (100.0, tone));
        INFO ("Radio 100 %, Tone " << tone << ": " << change << " LU");
        CHECK (std::abs (change) <= 1.0);
    }

    // The makeup is one law for every voice (a speech spectrum's), so other
    // voices land around it: a higher voice (formants +15 %, 220 Hz) and a
    // low one with softer esses, within 2 dB.
    for (int which = 0; which < 2; ++which)
    {
        synth::Options p = o;
        p.female = which == 0;
        p.f0 = which == 0 ? 220.0 : 110.0;
        p.sibilanceDb = which == 0 ? -4.0 : -14.0;
        p.seed = which == 0 ? 5 : 9;
        const auto w = synth::make (p);
        for (double tone : { -100.0, 0.0, 100.0 })
            for (double knob : { 50.0, 100.0 })
            {
                const double change = loudnessChange (w, fs, radioSettings (knob, tone));
                INFO ((which == 0 ? "higher" : "lower") << " voice, Radio " << knob << " %, Tone " << tone << ": " << change << " LU");
                CHECK (std::abs (change) <= 2.0);
            }
    }
}

TEST_CASE ("Radio: no noise is added, silence stays silence", "[radio]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const Signal z ((size_t) (0.3 * fs), 0.f);
        const auto y = run ({ z, z }, fs, radioSettings (100.0, 30.0));
        INFO ("fs " << fs);
        CHECK (bitIdentical (y.left, z));
        CHECK (bitIdentical (y.right, z));
    }
}

TEST_CASE ("Radio: the output doesn't depend on the block size", "[radio][blocks]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 1.0;
    const auto v = synth::make (o).mono;
    const Stereo x { v, scaled (v, -0.7) };
    const auto ref = run (x, fs, radioSettings (80.0, -30.0), 64);
    for (int block : { 1, 37, 4096 })
    {
        const auto y = run (x, fs, radioSettings (80.0, -30.0), block);
        INFO ("block " << block);
        CHECK (bitIdentical (y.left, ref.left));
        CHECK (bitIdentical (y.right, ref.right));
    }
}

TEST_CASE ("Radio: a copied instance carries on exactly where the original is", "[radio][copy]")
{
    const double fs = GENERATE (48000.0, 96000.0);
    synth::Options o;
    o.fs = fs;
    o.seconds = 1.2;
    const auto v = synth::make (o).mono;
    const auto s = radioSettings (90.0, 25.0);
    voc::Radio a, b;
    for (auto* m : { &a, &b })
    {
        m->set (s);
        m->prepare (fs, 64);
    }
    Signal la = v, ra = scaled (v, 0.5);
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
    INFO ("fs " << fs);
    CHECK (a.engaged());
    CHECK (bitIdentical (Signal (la.begin() + (std::ptrdiff_t) half, la.end()), Signal (lb.begin() + (std::ptrdiff_t) half, lb.end())));
    CHECK (bitIdentical (Signal (ra.begin() + (std::ptrdiff_t) half, ra.end()), Signal (rb.begin() + (std::ptrdiff_t) half, rb.end())));
}

TEST_CASE ("Radio: knob and Tone sweeps and power never click", "[radio][click]")
{
    const double fs = 48000.0;
    synth::Options o;
    o.fs = fs;
    o.seconds = 6.0;
    o.plosives = false;
    const auto voice = synth::make (o);
    const auto& v = voice.mono;
    const double dryP = worstClickP (v, fs);
    voc::Radio m;
    const auto y = run ({ v, v }, fs, radioSettings (0.0), 64, [&] (size_t at, voc::Settings& s) {
        const double t = (double) at / fs;
        s[P::radio] = (float) std::clamp (t < 3.5 ? 100.0 * (t - 0.3) : 100.0 - 100.0 * (t - 4.6), 0.0, 100.0);   // up, then down to 0
        s[P::radioTone] = (float) (100.0 * std::sin (2.0 * pi * 0.4 * t));
        s[P::radioOn] = (t > 2.0 && t < 2.6) ? 0.f : 1.f;   // power off and on at full
    }, m);
    const double p = worstClickP (y.left, fs);

    // The control: switching to the full sound at once, mid-vowel, is caught.
    int lat = 0;
    const auto full = run ({ v, v }, fs, radioSettings (100.0), 64, &lat).left;
    const auto dry = delayedBy (v, (size_t) lat);
    const auto vowel = voice.of (synth::Seg::vowel)[3];
    const size_t cut = (vowel.start + vowel.end) / 2 + (size_t) lat;
    Signal hard = dry;
    std::copy (full.begin() + (std::ptrdiff_t) cut, full.end(), hard.begin() + (std::ptrdiff_t) cut);
    const double control = worstClickP (hard, fs), bound = std::max (4.0, 1.5 * dryP);
    INFO ("worst click P " << p << " (the dry voice: " << dryP << "; an unfaded switch: " << control << ")");
    CHECK (p < bound);
    CHECK (control > 2.0 * bound);
    CHECK (allFinite (y.left));
}

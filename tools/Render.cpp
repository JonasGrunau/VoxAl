// vocanal_render: a WAV through the chain, offline, for before/after files.
//   vocanal_render in.wav out.wav [preset="Psy Lead"] [id=value ...] [order=clean,breath,...]
//                  [render=0|1] [bpm=140] [tail=0]
// A factory preset first (Presets.h), then ids and plain values as in the
// parameter table (Params.h) over it, e.g.
//   shift_mix=100 shift_pitch=-12 shift_formant=0
// The output is 32-bit float with the chain's latency removed, so it lines
// up with the input, plus `tail` seconds for the echoes and the reverb to
// ring out. render=1 (the default) runs at Render Quality, as a bounce
// would; the transport plays at `bpm` from the first sample. Core only, no
// JUCE: mp3s go through afconvert first.
#include "fx/Chain.h"
#include "fx/Presets.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    struct Audio
    {
        std::vector<float> left, right;
        int fs = 48000;
    };

    uint32_t u32 (const uint8_t* p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }
    uint16_t u16 (const uint8_t* p) { return (uint16_t) (p[0] | p[1] << 8); }

    bool readWav (const char* path, Audio& a)
    {
        FILE* f = std::fopen (path, "rb");
        if (f == nullptr) return false;
        std::vector<uint8_t> d;
        uint8_t buf[65536];
        for (size_t n; (n = std::fread (buf, 1, sizeof buf, f)) > 0;) d.insert (d.end(), buf, buf + n);
        std::fclose (f);
        if (d.size() < 12 || std::memcmp (d.data(), "RIFF", 4) != 0 || std::memcmp (d.data() + 8, "WAVE", 4) != 0) return false;
        int format = 0, channels = 0, bits = 0;
        for (size_t at = 12; at + 8 <= d.size();)
        {
            const uint32_t len = u32 (&d[at + 4]);
            const uint8_t* c = &d[at + 8];
            if (std::memcmp (&d[at], "fmt ", 4) == 0)
            {
                format = u16 (c);
                channels = u16 (c + 2);
                a.fs = (int) u32 (c + 4);
                bits = u16 (c + 14);
                if (format == 0xFFFE && len >= 26) format = u16 (c + 24);   // extensible: the sub-format
            }
            else if (std::memcmp (&d[at], "data", 4) == 0 && channels > 0)
            {
                const size_t bytes = (size_t) bits / 8, frames = std::min<size_t> (len, d.size() - at - 8) / (bytes * (size_t) channels);
                a.left.resize (frames);
                a.right.resize (frames);
                for (size_t i = 0; i < frames; ++i)
                    for (int ch = 0; ch < std::min (channels, 2); ++ch)
                    {
                        const uint8_t* s = c + (i * (size_t) channels + (size_t) ch) * bytes;
                        float v = 0.f;
                        if (format == 3 && bits == 32) std::memcpy (&v, s, 4);
                        else if (format == 1 && bits == 16) v = (float) (int16_t) u16 (s) / 32768.f;
                        else if (format == 1 && bits == 24) v = (float) ((int32_t) (u32 (s) << 8) >> 8) / 8388608.f;
                        else if (format == 1 && bits == 32) v = (float) (int32_t) u32 (s) / 2147483648.f;
                        else return false;
                        (ch == 0 ? a.left : a.right)[i] = v;
                    }
                if (channels == 1) a.right = a.left;
                return true;
            }
            at += 8 + len + (len & 1);
        }
        return false;
    }

    bool writeWav (const char* path, const Audio& a)
    {
        FILE* f = std::fopen (path, "wb");
        if (f == nullptr) return false;
        const auto put32 = [f] (uint32_t v) { const uint8_t b[4] = { (uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24) }; std::fwrite (b, 1, 4, f); };
        const auto put16 = [f] (uint16_t v) { const uint8_t b[2] = { (uint8_t) v, (uint8_t) (v >> 8) }; std::fwrite (b, 1, 2, f); };
        const auto frames = (uint32_t) a.left.size();
        std::fwrite ("RIFF", 1, 4, f); put32 (36 + frames * 8); std::fwrite ("WAVEfmt ", 1, 8, f);
        put32 (16); put16 (3); put16 (2); put32 ((uint32_t) a.fs); put32 ((uint32_t) a.fs * 8); put16 (8); put16 (32);
        std::fwrite ("data", 1, 4, f); put32 (frames * 8);
        for (uint32_t i = 0; i < frames; ++i)
        {
            std::fwrite (&a.left[i], 4, 1, f);
            std::fwrite (&a.right[i], 4, 1, f);
        }
        return std::fclose (f) == 0;
    }
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: vocanal_render in.wav out.wav [preset=name] [id=value ...] [order=...] [render=0|1] [bpm=140] [tail=0]\n");
        return 2;
    }
    Audio in;
    if (! readWav (argv[1], in))
    {
        std::fprintf (stderr, "can't read %s (PCM 16/24/32 or float WAV)\n", argv[1]);
        return 1;
    }
    voc::Settings s;
    s.render = true;
    voc::Order order;
    voc::Transport transport;
    transport.bpm = 140.0;
    transport.tempoKnown = transport.playing = true;
    double tail = 0.0;
    for (int i = 3; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto eq = arg.find ('=');
        if (eq == std::string::npos) continue;
        const auto key = arg.substr (0, eq), value = arg.substr (eq + 1);
        if (key == "preset")
        {
            const int k = voc::factoryIndexOf (value);
            if (k < 0)
            {
                std::fprintf (stderr, "no factory preset %s\n", value.c_str());
                return 2;
            }
            const auto& p = voc::factoryPresets()[(size_t) k];
            const auto v = voc::valuesOf (p);
            for (size_t j = 0; j < voc::kNumParams; ++j)
                if (voc::kParams[j].inPresets()) s.v[j] = v[j];
            order = voc::orderOf (p);
        }
        else if (key == "order") order = voc::Order::parse (value);
        else if (key == "render") s.render = value != "0";
        else if (key == "bpm") transport.bpm = std::stod (value);
        else if (key == "tail") tail = std::max (0.0, std::stod (value));
        else if (const int p = voc::indexOf (key); p >= 0) s.v[(size_t) p] = voc::clampToRange ((voc::P) p, std::stof (value));
        else
        {
            std::fprintf (stderr, "unknown parameter %s\n", key.c_str());
            return 2;
        }
    }

    voc::Chain chain;
    chain.set (s, order);
    chain.prepare (in.fs, 512);
    const auto lat = (size_t) chain.latencySamples();
    Audio out;
    out.fs = in.fs;
    const size_t n = in.left.size() + lat + (size_t) (tail * in.fs);
    std::vector<float> l (n, 0.f), r (n, 0.f);
    std::copy (in.left.begin(), in.left.end(), l.begin());
    std::copy (in.right.begin(), in.right.end(), r.begin());
    for (size_t at = 0; at < n; at += 512)
    {
        chain.set (s, order);
        transport.ppq = (double) at / in.fs * transport.bpm / 60.0;
        chain.process (l.data() + at, r.data() + at, (int) std::min<size_t> (512, n - at), nullptr, nullptr, transport);
    }
    out.left.assign (l.begin() + (std::ptrdiff_t) lat, l.end());
    out.right.assign (r.begin() + (std::ptrdiff_t) lat, r.end());
    if (! writeWav (argv[2], out)) return 1;
    std::printf ("%s: %zu samples at %d Hz, latency %zu removed\n", argv[2], out.left.size(), in.fs, lat);
    return 0;
}

// mrio — CoreAudio test rig for MixerReturn / SuperRack verification.
//
// Deliberately outside the CMake build: it is macOS-only, it talks to CoreAudio directly,
// and it exists to measure the *host*, not this plugin. Build it by hand:
//
//   clang++ -std=c++20 -O2 -o mrio tools/mrio.cpp \
//     -framework CoreAudio -framework AudioToolbox -framework CoreFoundation -framework Accelerate
//
// Typical use, with SuperRack's I/O set to the same loopback device:
//
//   ./mrio list                                   # find a device
//   ./mrio probe "Pro Tools Audio Bridge 32"      # prove it loops back 1:1 before trusting it
//   ./mrio run --dev "Pro Tools Audio Bridge 32" --gen 1,2,3,4 --cap 1,9,10,11,12,13 \
//              --secs 3 --amp 0.05 --ref 1 --wav sum.wav
//   python3 tools/resid.py sum.wav                # is the sum bit-exact against the senders?
//
// `run` reports a gain and a lag for every (generator, capture) pair. Its residual figure is
// only meaningful when one generator lands on one capture channel — where several generators
// sum into one channel, independent noise is not perfectly orthogonal over a finite window,
// so the per-pair fit leaves cross-terms behind. Use resid.py for those: it compares the sum
// against the captured senders themselves and is exact.
//
// Three jobs:
//   list                     enumerate devices, channel counts, rates
//   probe <dev>              play a distinct tone on every output channel, measure which
//                            input channels receive it -> loopback routing matrix
//   run <opts>               drive N output channels with identifiable signals, capture
//                            M input channels, report level / frequency / delay
//
// Analysis is Goertzel at integer frequencies with an integer number of periods in the
// window, so every tone sits exactly on a bin and leakage is not a factor.

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Accelerate/Accelerate.h>
#include <unistd.h>
#include <random>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

// ---------------------------------------------------------------------------------------
// HAL property helpers
// ---------------------------------------------------------------------------------------

static bool getProp (AudioObjectID obj, AudioObjectPropertySelector sel,
                     AudioObjectPropertyScope scope, void* out, UInt32& size,
                     UInt32 element = kAudioObjectPropertyElementMain)
{
    AudioObjectPropertyAddress a { sel, scope, element };
    return AudioObjectGetPropertyData (obj, &a, 0, nullptr, &size, out) == noErr;
}

static UInt32 propSize (AudioObjectID obj, AudioObjectPropertySelector sel,
                        AudioObjectPropertyScope scope,
                        UInt32 element = kAudioObjectPropertyElementMain)
{
    AudioObjectPropertyAddress a { sel, scope, element };
    UInt32 s = 0;
    if (AudioObjectGetPropertyDataSize (obj, &a, 0, nullptr, &s) != noErr) return 0;
    return s;
}

static std::string cfToStd (CFStringRef s)
{
    if (s == nullptr) return {};
    char buf[512] {};
    CFStringGetCString (s, buf, sizeof (buf), kCFStringEncodingUTF8);
    return std::string (buf);
}

static std::string deviceName (AudioObjectID dev)
{
    CFStringRef name = nullptr;
    UInt32 sz = sizeof (name);
    if (! getProp (dev, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &name, sz))
        return "<unnamed>";
    auto s = cfToStd (name);
    if (name) CFRelease (name);
    return s;
}

static std::string deviceUID (AudioObjectID dev)
{
    CFStringRef uid = nullptr;
    UInt32 sz = sizeof (uid);
    if (! getProp (dev, kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, &uid, sz))
        return {};
    auto s = cfToStd (uid);
    if (uid) CFRelease (uid);
    return s;
}

static int channelCount (AudioObjectID dev, AudioObjectPropertyScope scope)
{
    UInt32 sz = propSize (dev, kAudioDevicePropertyStreamConfiguration, scope);
    if (sz == 0) return 0;
    std::vector<uint8_t> raw (sz);
    auto* bl = reinterpret_cast<AudioBufferList*> (raw.data());
    if (! getProp (dev, kAudioDevicePropertyStreamConfiguration, scope, bl, sz)) return 0;
    int n = 0;
    for (UInt32 i = 0; i < bl->mNumberBuffers; ++i) n += (int) bl->mBuffers[i].mNumberChannels;
    return n;
}

static double nominalRate (AudioObjectID dev)
{
    Float64 r = 0;
    UInt32 sz = sizeof (r);
    getProp (dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &r, sz);
    return (double) r;
}

static bool setNominalRate (AudioObjectID dev, double rate)
{
    AudioObjectPropertyAddress a { kAudioDevicePropertyNominalSampleRate,
                                   kAudioObjectPropertyScopeGlobal,
                                   kAudioObjectPropertyElementMain };
    Float64 r = (Float64) rate;
    return AudioObjectSetPropertyData (dev, &a, 0, nullptr, sizeof (r), &r) == noErr;
}

static std::vector<AudioObjectID> allDevices()
{
    UInt32 sz = propSize (kAudioObjectSystemObject, kAudioHardwarePropertyDevices,
                          kAudioObjectPropertyScopeGlobal);
    std::vector<AudioObjectID> devs (sz / sizeof (AudioObjectID));
    if (devs.empty()) return devs;
    getProp (kAudioObjectSystemObject, kAudioHardwarePropertyDevices,
             kAudioObjectPropertyScopeGlobal, devs.data(), sz);
    devs.resize (sz / sizeof (AudioObjectID));
    return devs;
}

// Match by exact name, then by case-insensitive substring, then by UID.
static AudioObjectID findDevice (const std::string& query)
{
    auto devs = allDevices();
    for (auto d : devs) if (deviceName (d) == query) return d;
    for (auto d : devs) if (deviceUID (d) == query) return d;

    std::string lq = query;
    std::transform (lq.begin(), lq.end(), lq.begin(), ::tolower);
    for (auto d : devs)
    {
        auto n = deviceName (d);
        std::string ln = n;
        std::transform (ln.begin(), ln.end(), ln.begin(), ::tolower);
        if (ln.find (lq) != std::string::npos) return d;
    }
    return kAudioObjectUnknown;
}

// ---------------------------------------------------------------------------------------
// Goertzel: magnitude of an exact-bin frequency in a block of samples.
// ---------------------------------------------------------------------------------------
static double goertzel (const float* x, int n, double freq, double sr)
{
    const double k = freq * n / sr;
    const double w = 2.0 * M_PI * k / n;
    const double c = 2.0 * std::cos (w);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) { double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    const double real = s1 - s2 * std::cos (w);
    const double imag = s2 * std::sin (w);
    return 2.0 * std::sqrt (real * real + imag * imag) / n;   // amplitude estimate
}

static double rms (const float* x, int n)
{
    double a = 0;
    for (int i = 0; i < n; ++i) a += (double) x[i] * x[i];
    return std::sqrt (a / std::max (1, n));
}

static double peak (const float* x, int n)
{
    double p = 0;
    for (int i = 0; i < n; ++i) p = std::max (p, (double) std::fabs (x[i]));
    return p;
}

static double dbfs (double amp) { return amp > 1e-12 ? 20.0 * std::log10 (amp) : -240.0; }

// ---------------------------------------------------------------------------------------
// The IO engine. One device, one IOProc, generating on outputs and recording inputs.
// ---------------------------------------------------------------------------------------

struct ToneSpec { int channel; double freq; double amp; };

struct Engine
{
    AudioObjectID dev = kAudioObjectUnknown;
    AudioDeviceIOProcID procID = nullptr;
    double sr = 48000.0;
    int nOut = 0, nIn = 0;

    std::vector<ToneSpec> tones;            // what to emit (tone mode)

    // Playback mode: a precomputed sequence per output channel. Precomputed rather than
    // generated in the callback so the emitted signal is known exactly, sample for sample,
    // and can be correlated against what comes back.
    std::vector<int> playChan;                 // output channel index per sequence
    std::vector<std::vector<float>> playData;  // the sequences themselves

    std::vector<std::vector<float>> capture; // [slot][sample]
    std::vector<int> captureMap;             // slot -> device input channel (empty = identity)
    std::atomic<long long> framesOut { 0 };
    std::atomic<long long> framesIn  { 0 };
    long long captureFrames = 0;
    std::atomic<bool> capturing { false };
    std::atomic<bool> overrun { false };
    // Absolute emitted-frame index that capture sample 0 lines up with, before any device
    // or host latency. Lags measured against this are round-trip latencies.
    std::atomic<long long> capStartOut { -1 };

    // Phase accumulators live across callbacks so the tone is continuous.
    std::vector<double> phase;

    // Optional: mark a sync impulse at capture start on a designated output channel.
    int impulseChannel = -1;
    std::atomic<long long> impulseAt { -1 };

    static OSStatus ioProc (AudioObjectID, const AudioTimeStamp*,
                            const AudioBufferList* inData, const AudioTimeStamp*,
                            AudioBufferList* outData, const AudioTimeStamp*, void* ctx)
    {
        return static_cast<Engine*> (ctx)->io (inData, outData);
    }

    // Non-interleaved Float32 is what the HAL hands us for these devices, but handle
    // interleaved too rather than producing silently wrong results on a device that differs.
    static float* chanPtr (AudioBufferList* bl, int ch, int& stride, int& frames)
    {
        int seen = 0;
        for (UInt32 b = 0; b < bl->mNumberBuffers; ++b)
        {
            const int nc = (int) bl->mBuffers[b].mNumberChannels;
            if (ch < seen + nc)
            {
                stride = nc;
                frames = (int) (bl->mBuffers[b].mDataByteSize / sizeof (float) / (UInt32) nc);
                return static_cast<float*> (bl->mBuffers[b].mData) + (ch - seen);
            }
            seen += nc;
        }
        stride = 0; frames = 0;
        return nullptr;
    }

    static const float* chanPtr (const AudioBufferList* bl, int ch, int& stride, int& frames)
    {
        return chanPtr (const_cast<AudioBufferList*> (bl), ch, stride, frames);
    }

    OSStatus io (const AudioBufferList* in, AudioBufferList* out)
    {
        // ---- output ----
        int nFrames = 0;
        if (out != nullptr && out->mNumberBuffers > 0)
        {
            for (UInt32 b = 0; b < out->mNumberBuffers; ++b)
                std::memset (out->mBuffers[b].mData, 0, out->mBuffers[b].mDataByteSize);

            int stride = 0, frames = 0;
            chanPtr (out, 0, stride, frames);
            nFrames = frames;

            const long long start = framesOut.load();

            for (size_t t = 0; t < tones.size(); ++t)
            {
                const auto& s = tones[t];
                if (s.channel < 0 || s.channel >= nOut) continue;
                float* p = chanPtr (out, s.channel, stride, frames);
                if (p == nullptr) continue;
                const double inc = 2.0 * M_PI * s.freq / sr;
                double ph = phase[t];
                for (int i = 0; i < frames; ++i)
                {
                    p[i * stride] += (float) (s.amp * std::sin (ph));
                    ph += inc;
                    if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
                }
                phase[t] = ph;
            }

            // Playback of precomputed sequences. `start` is the absolute frame index, so a
            // captured block can be lined up against the emitted samples exactly.
            for (size_t s = 0; s < playChan.size(); ++s)
            {
                const int ch = playChan[s];
                if (ch < 0 || ch >= nOut) continue;
                float* p = chanPtr (out, ch, stride, frames);
                if (p == nullptr) continue;
                const auto& seq = playData[s];
                for (int i = 0; i < frames; ++i)
                {
                    const long long idx = start + i;
                    if (idx >= 0 && idx < (long long) seq.size())
                        p[i * stride] += seq[(size_t) idx];
                }
            }

            // Sync impulse: one full-scale sample at the first frame after capture starts.
            const long long imp = impulseAt.load();
            if (impulseChannel >= 0 && impulseChannel < nOut && imp >= start && imp < start + nFrames)
            {
                float* p = chanPtr (out, impulseChannel, stride, frames);
                if (p != nullptr) p[(int) (imp - start) * stride] = 1.0f;
            }

            framesOut.fetch_add (nFrames);
        }

        // ---- input ----
        if (in != nullptr && in->mNumberBuffers > 0 && capturing.load())
        {
            int stride = 0, frames = 0;
            chanPtr (in, 0, stride, frames);
            const long long pos = framesIn.load();
            if (pos == 0 && capStartOut.load() < 0)
                capStartOut.store (framesOut.load() - nFrames);
            if (pos < captureFrames)
            {
                const int n = (int) std::min<long long> (frames, captureFrames - pos);
                for (int c = 0; c < (int) capture.size(); ++c)
                {
                    const int devCh = captureMap.empty() ? c : captureMap[(size_t) c];
                    if (devCh < 0 || devCh >= nIn) continue;
                    const float* p = chanPtr (in, devCh, stride, frames);
                    if (p == nullptr) continue;
                    for (int i = 0; i < n; ++i) capture[(size_t) c][(size_t) (pos + i)] = p[i * stride];
                }
                framesIn.fetch_add (n);
            }
        }
        return noErr;
    }

    bool open (AudioObjectID d, double wantRate)
    {
        dev = d;
        nOut = channelCount (dev, kAudioDevicePropertyScopeOutput);
        nIn  = channelCount (dev, kAudioDevicePropertyScopeInput);
        if (wantRate > 0 && std::fabs (nominalRate (dev) - wantRate) > 0.5)
        {
            if (! setNominalRate (dev, wantRate))
                std::fprintf (stderr, "warning: could not set sample rate to %.0f\n", wantRate);
            usleep (400000);
        }
        sr = nominalRate (dev);
        if (AudioDeviceCreateIOProcID (dev, ioProc, this, &procID) != noErr) return false;
        return true;
    }

    bool start() { return AudioDeviceStart (dev, procID) == noErr; }
    void stop()
    {
        if (procID != nullptr) { AudioDeviceStop (dev, procID); AudioDeviceDestroyIOProcID (dev, procID); procID = nullptr; }
    }
};

// ---------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------

static int cmdList()
{
    auto devs = allDevices();
    std::printf ("%-42s %5s %5s %9s  %s\n", "NAME", "IN", "OUT", "RATE", "UID");
    std::printf ("%s\n", std::string (110, '-').c_str());
    for (auto d : devs)
    {
        const int in = channelCount (d, kAudioDevicePropertyScopeInput);
        const int out = channelCount (d, kAudioDevicePropertyScopeOutput);
        std::printf ("%-42s %5d %5d %9.0f  %s\n", deviceName (d).c_str(), in, out,
                     nominalRate (d), deviceUID (d).c_str());
    }
    return 0;
}

// Play a distinct tone on every output channel simultaneously, then work out which input
// channel each landed on. One pass, no per-channel serialisation.
static int cmdProbe (const std::string& devName, double rate, int maxCh, double seconds)
{
    AudioObjectID dev = findDevice (devName);
    if (dev == kAudioObjectUnknown) { std::fprintf (stderr, "device not found: %s\n", devName.c_str()); return 2; }

    Engine e;
    if (! e.open (dev, rate)) { std::fprintf (stderr, "could not open device\n"); return 3; }
    std::printf ("device: %s  (%d in / %d out @ %.0f Hz)\n", deviceName (dev).c_str(), e.nIn, e.nOut, e.sr);

    const int nOut = maxCh > 0 ? std::min (maxCh, e.nOut) : e.nOut;
    const int nIn  = maxCh > 0 ? std::min (maxCh, e.nIn)  : e.nIn;
    if (nOut == 0 || nIn == 0) { std::fprintf (stderr, "device has no %s\n", nOut == 0 ? "outputs" : "inputs"); e.stop(); return 4; }

    // Integer frequencies, and an analysis window of an integer number of seconds, so every
    // tone is exactly on a bin.
    for (int c = 0; c < nOut; ++c)
        e.tones.push_back ({ c, 400.0 + 37.0 * c, 0.2 });
    e.phase.assign (e.tones.size(), 0.0);

    const long long capFrames = (long long) (seconds * e.sr);
    e.captureFrames = capFrames;
    e.capture.assign ((size_t) nIn, std::vector<float> ((size_t) capFrames, 0.0f));

    if (! e.start()) { std::fprintf (stderr, "could not start device\n"); return 5; }
    usleep (500000);                      // let the stream settle before capturing
    e.capturing.store (true);
    while (e.framesIn.load() < capFrames) usleep (20000);
    e.capturing.store (false);
    e.stop();

    std::printf ("\nloopback matrix (dBFS of out-channel tone measured on each in-channel;\n"
                 "blank = below -80 dBFS)\n\n");
    std::printf ("%6s", "in\\out");
    for (int c = 0; c < nOut; ++c) std::printf ("%7d", c + 1);
    std::printf ("\n");

    int direct = 0, offDiag = 0;
    for (int i = 0; i < nIn; ++i)
    {
        std::printf ("%6d", i + 1);
        for (int c = 0; c < nOut; ++c)
        {
            const double a = goertzel (e.capture[(size_t) i].data(), (int) capFrames,
                                       400.0 + 37.0 * c, e.sr);
            const double db = dbfs (a);
            if (db < -80.0) std::printf ("%7s", ".");
            else
            {
                std::printf ("%7.1f", db);
                if (i == c) ++direct; else ++offDiag;
            }
        }
        std::printf ("\n");
    }
    std::printf ("\n%d channels loop back 1:1, %d off-diagonal leaks\n", direct, offDiag);
    std::printf ("%s\n", direct == std::min (nIn, nOut) && offDiag == 0
                 ? "VERDICT: clean 1:1 loopback device"
                 : "VERDICT: not a clean 1:1 loopback (see matrix)");
    return 0;
}

// ---------------------------------------------------------------------------------------
// Multi-channel WAV writer (32-bit float), so a run can be listened to or re-analysed.
// ---------------------------------------------------------------------------------------
static bool writeWav (const std::string& path, const std::vector<std::vector<float>>& chans,
                      double sr)
{
    if (chans.empty()) return false;
    const uint32_t nch = (uint32_t) chans.size();
    const uint32_t n   = (uint32_t) chans[0].size();
    const uint32_t dataBytes = n * nch * 4;

    FILE* f = std::fopen (path.c_str(), "wb");
    if (f == nullptr) return false;

    auto u32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto u16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };

    std::fwrite ("RIFF", 1, 4, f); u32 (36 + dataBytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); u32 (16); u16 (3); u16 ((uint16_t) nch);
    u32 ((uint32_t) sr); u32 ((uint32_t) sr * nch * 4); u16 ((uint16_t) (nch * 4)); u16 (32);
    std::fwrite ("data", 1, 4, f); u32 (dataBytes);

    std::vector<float> row (nch);
    for (uint32_t i = 0; i < n; ++i)
    {
        for (uint32_t c = 0; c < nch; ++c) row[c] = chans[c][i];
        std::fwrite (row.data(), 4, nch, f);
    }
    std::fclose (f);
    return true;
}

// ---------------------------------------------------------------------------------------
// Least-squares fit of one captured channel against one emitted sequence, searched over
// lag. Returns the best lag, the gain at that lag, and the normalised correlation.
//
// Correlating noise rather than tones is what makes the lag unambiguous: a tone correlates
// equally well at every whole period, which is exactly the wrong property when the question
// is "is the delay uniform across senders".
// ---------------------------------------------------------------------------------------
struct FitResult { int lag = 0; double gain = 0; double corr = 0; };

static FitResult fitLag (const float* cap, int capLen, const float* gen, int genLen,
                         int lagMin, int lagMax, int window)
{
    FitResult best;
    const int w = std::min ({ window, capLen, genLen });
    if (w <= 0) return best;

    // Energy of the capture window is fixed; energy of the generator window varies with lag
    // only at the edges, but compute it per lag anyway — it is cheap next to the dot product.
    float capEnergy = 0;
    vDSP_svesq (cap, 1, &capEnergy, (vDSP_Length) w);

    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        const int genStart = lag;
        if (genStart < 0 || genStart + w > genLen) continue;

        float dot = 0, genEnergy = 0;
        vDSP_dotpr (cap, 1, gen + genStart, 1, &dot, (vDSP_Length) w);
        vDSP_svesq (gen + genStart, 1, &genEnergy, (vDSP_Length) w);
        if (genEnergy <= 0) continue;

        const double corr = (double) dot / std::sqrt ((double) genEnergy * (double) capEnergy + 1e-30);
        if (std::fabs (corr) > std::fabs (best.corr))
        {
            best.corr = corr;
            best.lag  = lag;
            best.gain = (double) dot / (double) genEnergy;
        }
    }
    return best;
}

struct GenSpec { int channel; double amp; unsigned seed; std::vector<float> data; };

static int cmdRun (int argc, char** argv)
{
    std::string devName = "Pro Tools Audio Bridge 32";
    std::string wavPath;
    double rate = 48000.0, seconds = 3.0;
    std::vector<int> genChans, capChans;
    double amp = 0.1;
    int lagMax = 16384;
    int refCap = -1;   // capture channel used as the zero-latency reference

    for (int i = 0; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&] () -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        auto parseList = [] (const std::string& s, std::vector<int>& out)
        {
            size_t p = 0;
            while (p < s.size())
            {
                size_t q = s.find (',', p);
                if (q == std::string::npos) q = s.size();
                out.push_back (std::atoi (s.substr (p, q - p).c_str()) - 1);   // 1-based in
                p = q + 1;
            }
        };
        if (a == "--dev") devName = next();
        else if (a == "--gen") parseList (next(), genChans);
        else if (a == "--cap") parseList (next(), capChans);
        else if (a == "--secs") seconds = std::atof (next().c_str());
        else if (a == "--rate") rate = std::atof (next().c_str());
        else if (a == "--amp")  amp = std::atof (next().c_str());
        else if (a == "--wav")  wavPath = next();
        else if (a == "--lagmax") lagMax = std::atoi (next().c_str());
        else if (a == "--ref")  refCap = std::atoi (next().c_str()) - 1;
    }

    if (genChans.empty() || capChans.empty())
    { std::fprintf (stderr, "run needs --gen and --cap (1-based channel lists)\n"); return 1; }

    AudioObjectID dev = findDevice (devName);
    if (dev == kAudioObjectUnknown) { std::fprintf (stderr, "device not found: %s\n", devName.c_str()); return 2; }

    Engine e;
    if (! e.open (dev, rate)) { std::fprintf (stderr, "could not open device\n"); return 3; }
    std::printf ("device: %s  (%d in / %d out @ %.0f Hz)\n\n",
                 deviceName (dev).c_str(), e.nIn, e.nOut, e.sr);

    // Emitted sequences: independent white noise, one per generator channel, long enough to
    // cover pre-roll + capture + the widest lag we will search.
    const long long capFrames = (long long) (seconds * e.sr);
    // Long enough to cover pre-roll, the capture, and a lag search window on both sides of
    // the nominal alignment point.
    const long long seqLen    = capFrames + 2 * lagMax + (long long) (6.0 * e.sr);

    std::vector<GenSpec> gens;
    for (size_t g = 0; g < genChans.size(); ++g)
    {
        GenSpec s { genChans[g], amp, (unsigned) (0x9E3779B9u * (g + 1) + 12345u), {} };
        std::mt19937 rng (s.seed);
        std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        s.data.resize ((size_t) seqLen);
        for (auto& v : s.data) v = (float) (amp * d (rng));
        gens.push_back (std::move (s));
    }

    for (auto& g : gens) { e.playChan.push_back (g.channel); e.playData.push_back (g.data); }

    e.captureFrames = capFrames;
    e.capture.assign (capChans.size(), std::vector<float> ((size_t) capFrames, 0.0f));
    // Capture channel c of `capture` maps to device input channel capChans[c]; remap by
    // giving the engine a full-width capture and slicing after, which keeps the callback
    // trivial. Simpler: capture only the channels asked for, via an index map.
    e.captureMap = capChans;

    if (! e.start()) { std::fprintf (stderr, "could not start device\n"); return 4; }
    usleep (700000);                        // let the host settle before the window we analyse
    e.capturing.store (true);
    int guard = 0;
    while (e.framesIn.load() < capFrames && guard++ < 2000) usleep (10000);
    e.capturing.store (false);
    e.stop();

    if (e.framesIn.load() < capFrames)
    { std::fprintf (stderr, "capture did not complete (%lld/%lld frames)\n", e.framesIn.load(), capFrames); return 5; }

    const long long capStart = e.capStartOut.load();
    std::printf ("captured %lld frames; capture sample 0 == emitted frame %lld\n\n", capFrames, capStart);

    if (capStart < lagMax)
    { std::fprintf (stderr, "pre-roll (%lld) shorter than the lag search (%d); raise it\n", capStart, lagMax); return 6; }

    if (! wavPath.empty() && writeWav (wavPath, e.capture, e.sr))
        std::printf ("wrote %s\n\n", wavPath.c_str());

    // -------- per-capture-channel summary --------
    std::printf ("%-8s %9s %9s   %s\n", "cap ch", "peak dBFS", "rms dBFS", "contributions (gen ch: gain dB @ lag samples, corr)");
    std::printf ("%s\n", std::string (118, '-').c_str());

    const int window = (int) std::min<long long> (capFrames - lagMax - 1, 131072);

    std::vector<std::vector<FitResult>> fits (capChans.size());

    for (size_t c = 0; c < capChans.size(); ++c)
    {
        const auto& cap = e.capture[c];
        std::printf ("%-8d %9.1f %9.1f   ", capChans[c] + 1,
                     dbfs (peak (cap.data(), (int) capFrames)),
                     dbfs (rms (cap.data(), (int) capFrames)));

        bool any = false;
        for (size_t g = 0; g < gens.size(); ++g)
        {
            // CoreAudio hands an IOProc input that was captured slightly *before* the
            // output block written in the same callback, so the true lag can be negative.
            // Search a window centred on capStart by biasing the base pointer back by
            // lagMax and reporting the lag relative to that bias.
            const float* genBase = gens[g].data.data() + (capStart - lagMax);
            const int genAvail = (int) (gens[g].data.size() - (size_t) (capStart - lagMax));
            FitResult f = fitLag (cap.data(), (int) capFrames, genBase, genAvail, 0, 2 * lagMax, window);
            f.lag -= lagMax;
            fits[c].push_back (f);
            if (std::fabs (f.corr) > 0.02)
            {
                if (any) std::printf (", ");
                std::printf ("%d: %+.2f dB @ %d (%.3f)", gens[g].channel + 1,
                             dbfs (std::fabs (f.gain)), f.lag, f.corr);
                any = true;
            }
        }
        if (! any) std::printf ("(nothing correlated)");
        std::printf ("\n");
    }

    // -------- residual: what is present that no generator explains --------
    std::printf ("\n%-8s %12s %12s\n", "cap ch", "resid dBFS", "resid/total");
    std::printf ("%s\n", std::string (36, '-').c_str());
    for (size_t c = 0; c < capChans.size(); ++c)
    {
        std::vector<float> r = e.capture[c];
        for (size_t g = 0; g < gens.size(); ++g)
        {
            const auto& f = fits[c][g];
            if (std::fabs (f.corr) <= 0.02) continue;
            const float* genBase = gens[g].data.data() + capStart + f.lag;
            const long long avail = (long long) gens[g].data.size() - (capStart + f.lag);
            const long long n = std::min<long long> (capFrames, avail);
            for (long long i = 0; i < n; ++i) r[(size_t) i] -= (float) (f.gain * genBase[i]);
        }
        const double res = rms (r.data(), (int) capFrames);
        const double tot = rms (e.capture[c].data(), (int) capFrames);
        std::printf ("%-8d %12.1f %12.1f\n", capChans[c] + 1, dbfs (res), dbfs (res / (tot + 1e-30)));
    }

    // -------- relative delay, which is the property that actually matters --------
    if (refCap >= 0)
    {
        int refIdx = -1;
        for (size_t c = 0; c < capChans.size(); ++c) if (capChans[c] == refCap) refIdx = (int) c;
        if (refIdx >= 0)
        {
            int refLag = INT32_MAX;
            for (auto& f : fits[(size_t) refIdx])
                if (std::fabs (f.corr) > 0.02) refLag = std::min (refLag, f.lag);
            if (refLag != INT32_MAX)
            {
                // A more negative lag means the captured audio was emitted longer ago, i.e.
                // more delay. Report it as positive added delay so it reads the obvious way.
                std::printf ("\nadded delay vs reference capture channel %d (%d samples = device round trip):\n",
                             refCap + 1, -refLag);
                for (size_t c = 0; c < capChans.size(); ++c)
                    for (size_t g = 0; g < gens.size(); ++g)
                        if (std::fabs (fits[c][g].corr) > 0.02)
                            std::printf ("  cap %-3d <- gen %-3d : %+d samples (%.2f ms)\n",
                                         capChans[c] + 1, gens[g].channel + 1,
                                         refLag - fits[c][g].lag,
                                         1000.0 * (refLag - fits[c][g].lag) / e.sr);
            }
        }
    }
    return 0;
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr,
            "usage:\n"
            "  mrio list\n"
            "  mrio probe <device> [rate] [maxch] [secs]\n"
            "  mrio run --dev <name> --gen 1,2,3 --cap 9,10,13 [--secs 3] [--amp 0.1]\n"
            "           [--wav out.wav] [--ref <capch>] [--lagmax 16384]\n");
        return 1;
    }
    const std::string cmd = argv[1];
    if (cmd == "list") return cmdList();
    if (cmd == "run")  return cmdRun (argc - 2, argv + 2);
    if (cmd == "probe")
    {
        if (argc < 3) { std::fprintf (stderr, "probe needs a device name\n"); return 1; }
        const double rate = argc > 3 ? std::atof (argv[3]) : 48000.0;
        const int maxch   = argc > 4 ? std::atoi (argv[4]) : 8;
        const double secs = argc > 5 ? std::atof (argv[5]) : 1.0;
        return cmdProbe (argv[2], rate, maxch, secs);
    }
    std::fprintf (stderr, "unknown command: %s\n", cmd.c_str());
    return 1;
}

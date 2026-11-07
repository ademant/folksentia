// folksentia -- audio feature extractor built on Essentia.
//
// Usage: folksentia [options] FILE...
// Prints one JSON object per file (JSON array if several files).
// The JSON layout is documented in README.md (schema version 1).

#include <essentia/algorithmfactory.h>
#include <essentia/essentia.h>
#include <essentia/essentiamath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace essentia;
using essentia::standard::Algorithm;
using essentia::standard::AlgorithmFactory;

namespace {

constexpr int kSchema = 1;

struct Options {
  Real sampleRate = 44100.;
  std::string rhythmMethod = "multifeature";
  bool beats = false;
  bool downbeats = false;
  int waveformBins = 0;  // 0 = no waveform
  std::vector<std::string> files;
};

struct Features {
  Real duration = 0;
  Real bpm = 0, bpmConfidence = 0;
  std::vector<Real> ticks;
  std::string key, scale;
  Real keyStrength = 0;
  Real tuningFrequency = 0, tuningCents = 0;
  Real replayGain = 0, loudness = 0, dynamicComplexity = 0;

  // Silence at start and end; unset when the file has no non-silent frame.
  std::optional<Real> leadMs, tailMs;

  // EBU R128 integrated loudness and true peak (dBTP, max over channels).
  std::optional<Real> integratedLufs, truePeakDbtp;

  // Waveform peaks 0..255, one per bin; empty unless requested.
  std::vector<int> waveform;

  // Indexes into ticks that start a bar; unset when unknown.
  std::optional<std::vector<int>> downbeatIndexes;
};

using AlgoPtr = std::unique_ptr<Algorithm>;

template <typename... Args>
AlgoPtr make(const std::string& name, Args&&... args) {
  return AlgoPtr(AlgorithmFactory::create(name, std::forward<Args>(args)...));
}

std::string jsonString(const std::string& s) {
  std::ostringstream o;
  o << '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"':  o << "\\\""; break;
      case '\\': o << "\\\\"; break;
      case '\n': o << "\\n"; break;
      case '\r': o << "\\r"; break;
      case '\t': o << "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          o << buf;
        } else {
          o << c;
        }
    }
  }
  o << '"';
  return o.str();
}

// JSON number, or null when the value is not finite (NaN, +-inf).
std::string jsonNum(Real v) {
  if (!std::isfinite(v)) return "null";
  std::ostringstream o;
  o << std::setprecision(6) << v;
  return o.str();
}

std::string jsonNum(const std::optional<Real>& v) {
  return v ? jsonNum(*v) : "null";
}

// Text value, or null when empty (unknown).
std::string jsonText(const std::string& s) {
  return s.empty() ? "null" : jsonString(s);
}

// Silence at start and end: time before the first and after the last
// non-silent 512-sample frame (frames advance by 256 samples).
void measureSilence(const std::vector<Real>& audio, Real sr, Features& f) {
  const size_t frame = 512, hop = 256, n = audio.size();
  bool found = false;
  size_t first = 0, last = 0;
  for (size_t s = 0; s < n; s += hop) {
    const size_t e = std::min(n, s + frame);
    std::vector<Real> seg(audio.begin() + s, audio.begin() + e);
    if (isSilent(seg)) continue;
    if (!found) first = s;
    found = true;
    last = e;
  }
  if (!found) return;  // all silent: unknown
  f.leadMs = Real(first) / sr * 1000.;
  f.tailMs = Real(n - last) / sr * 1000.;
}

// Peak per bin, 0..255, normalised to the loudest bin.
std::vector<int> measureWaveform(const std::vector<Real>& audio, int bins) {
  const size_t n = audio.size();
  std::vector<Real> peak(bins, 0.);
  Real top = 0;
  for (int b = 0; b < bins; ++b) {
    const size_t s = size_t(b) * n / bins;
    size_t e = size_t(b + 1) * n / bins;
    if (e <= s) e = s + 1;
    for (size_t i = s; i < e && i < n; ++i) peak[b] = std::max(peak[b], std::abs(audio[i]));
    top = std::max(top, peak[b]);
  }
  std::vector<int> out(bins, 0);
  if (top <= 0) return out;
  for (int b = 0; b < bins; ++b) {
    out[b] = std::clamp(int(std::lround(peak[b] / top * 255.)), 0, 255);
  }
  return out;
}

// Bar starts from the beat ticks. Each beat is scored by the energy of the
// bass (below 150 Hz) just after it. Each of the four possible bar phases is
// scored by how much louder its beats are than the others; the loudest phase
// wins if it is clearly louder. Returns nothing when there is no clear phase.
std::optional<std::vector<int>> measureDownbeats(const std::vector<Real>& audio,
                                                 const std::vector<Real>& ticks,
                                                 Real sr) {
  constexpr int kMeter = 4;
  constexpr Real kMinContrast = 1.2;  // phase energy / energy of other beats
  if (ticks.size() < size_t(2 * kMeter) || audio.empty()) return std::nullopt;

  std::vector<Real> bass;
  {
    auto lp = make("LowPass", "sampleRate", sr, "cutoffFrequency", 150.0);
    lp->input("signal").set(audio);
    lp->output("signal").set(bass);
    lp->compute();
  }

  // Window after each beat: half the interval to the next beat (or the
  // previous interval for the last beat).
  std::vector<Real> accent(ticks.size(), 0.);
  for (size_t i = 0; i < ticks.size(); ++i) {
    Real interval;
    if (i + 1 < ticks.size()) interval = ticks[i + 1] - ticks[i];
    else interval = ticks[i] - ticks[i - 1];
    if (!(interval > 0)) return std::nullopt;

    const size_t s = size_t(std::max<Real>(0, std::round(ticks[i] * sr)));
    const size_t e = std::min(bass.size(), size_t(std::round((ticks[i] + 0.5 * interval) * sr)));
    if (e <= s) continue;
    Real energy = 0;
    for (size_t k = s; k < e; ++k) energy += bass[k] * bass[k];
    accent[i] = energy / Real(e - s);
  }

  Real bestContrast = 0;
  int bestPhase = -1;
  for (int p = 0; p < kMeter; ++p) {
    Real sumIn = 0, sumOut = 0;
    size_t nIn = 0, nOut = 0;
    for (size_t i = 0; i < accent.size(); ++i) {
      if (int(i % kMeter) == p) { sumIn += accent[i]; ++nIn; }
      else { sumOut += accent[i]; ++nOut; }
    }
    if (nIn == 0 || nOut == 0) continue;
    const Real meanIn = sumIn / nIn, meanOut = sumOut / nOut;
    const Real contrast = meanOut > 0 ? meanIn / meanOut : (meanIn > 0 ? 1e9 : 0);
    if (contrast > bestContrast) { bestContrast = contrast; bestPhase = p; }
  }
  if (bestPhase < 0 || bestContrast < kMinContrast) return std::nullopt;

  std::vector<int> out;
  for (size_t i = bestPhase; i < ticks.size(); i += kMeter) out.push_back(int(i));
  return out;
}

Features analyze(const std::string& file, const Options& opt) {
  Features f;
  const Real sr = opt.sampleRate;

  // Mono analysis signal at the analysis rate.
  std::vector<Real> audio;
  {
    auto loader = make("MonoLoader", "filename", file, "sampleRate", sr);
    loader->output("audio").set(audio);
    loader->compute();
  }
  if (audio.empty()) throw EssentiaException("no audio decoded from ", file);
  f.duration = audio.size() / sr;

  // Tempo + beats. RhythmExtractor2013 assumes 44.1 kHz input.
  {
    auto rhythm = make("RhythmExtractor2013", "method", opt.rhythmMethod);
    std::vector<Real> estimates, intervals;
    rhythm->input("signal").set(audio);
    rhythm->output("bpm").set(f.bpm);
    rhythm->output("ticks").set(f.ticks);
    rhythm->output("confidence").set(f.bpmConfidence);
    rhythm->output("estimates").set(estimates);
    rhythm->output("bpmIntervals").set(intervals);
    rhythm->compute();
  }

  // Key / mode.
  {
    auto key = make("KeyExtractor", "sampleRate", sr);
    key->input("audio").set(audio);
    key->output("key").set(f.key);
    key->output("scale").set(f.scale);
    key->output("strength").set(f.keyStrength);
    key->compute();
  }

  // Tuning frequency (interesting for non-440 Hz folk instruments).
  {
    const int frameSize = 4096, hopSize = 2048;
    auto cutter = make("FrameCutter", "frameSize", frameSize, "hopSize", hopSize,
                       "startFromZero", true);
    auto window = make("Windowing", "type", "blackmanharris62");
    auto spectrum = make("Spectrum", "size", frameSize);
    auto peaks = make("SpectralPeaks", "sampleRate", sr, "maxPeaks", 60,
                      "minFrequency", 40.0, "maxFrequency", 5000.0,
                      "orderBy", "magnitude", "magnitudeThreshold", 1e-5);
    auto tuning = make("TuningFrequency");

    std::vector<Real> frame, windowed, spec, freqs, mags;
    cutter->input("signal").set(audio);
    cutter->output("frame").set(frame);
    window->input("frame").set(frame);
    window->output("frame").set(windowed);
    spectrum->input("frame").set(windowed);
    spectrum->output("spectrum").set(spec);
    peaks->input("spectrum").set(spec);
    peaks->output("frequencies").set(freqs);
    peaks->output("magnitudes").set(mags);
    tuning->input("frequencies").set(freqs);
    tuning->input("magnitudes").set(mags);
    tuning->output("tuningFrequency").set(f.tuningFrequency);
    tuning->output("tuningCents").set(f.tuningCents);

    for (;;) {
      cutter->compute();
      if (frame.empty()) break;
      if (isSilent(frame)) continue;
      window->compute();
      spectrum->compute();
      peaks->compute();
      tuning->compute();  // accumulates; output is the running estimate
    }
  }

  // Loudness.
  {
    auto rg = make("ReplayGain", "sampleRate", sr);
    rg->input("signal").set(audio);
    rg->output("replayGain").set(f.replayGain);
    rg->compute();

    auto dc = make("DynamicComplexity", "sampleRate", sr);
    dc->input("signal").set(audio);
    dc->output("dynamicComplexity").set(f.dynamicComplexity);
    dc->output("loudness").set(f.loudness);
    dc->compute();
  }

  // EBU R128 and true peak need stereo at the file's own sample rate, so the
  // file is decoded a second time here.
  {
    std::vector<StereoSample> stereo;
    Real nativeRate = 0;
    int channels = 0;
    auto loader = make("AudioLoader", "filename", file);
    loader->output("audio").set(stereo);
    loader->output("sampleRate").set(nativeRate);
    loader->output("numberChannels").set(channels);
    loader->compute();
    if (stereo.empty() || nativeRate <= 0) throw EssentiaException("no stereo audio decoded from ", file);

    if (channels == 1) {  // mono: play it as dual mono
      for (auto& s : stereo) s.second = s.first;
    }

    auto ebu = make("LoudnessEBUR128", "sampleRate", nativeRate);
    ebu->input("signal").set(stereo);
    ebu->output("integratedLoudness").set(f.integratedLufs.emplace());
    ebu->compute();

    std::vector<Real> left, right;
    left.reserve(stereo.size());
    right.reserve(stereo.size());
    for (const auto& s : stereo) { left.push_back(s.first); right.push_back(s.second); }
    stereo.clear();
    stereo.shrink_to_fit();

    Real peak = 0;
    for (const auto* ch : {&left, &right}) {
      auto tp = make("TruePeakDetector", "sampleRate", nativeRate);
      std::vector<Real> out, locations;
      tp->input("signal").set(*ch);
      tp->output("output").set(out);
      tp->output("peakLocations").set(locations);
      tp->compute();
      for (Real v : out) peak = std::max(peak, std::abs(v));
    }
    if (peak > 0) f.truePeakDbtp = 20. * std::log10(peak);
    else f.truePeakDbtp.reset();  // digital silence: unknown
  }

  // Silence at start and end.
  measureSilence(audio, sr, f);

  // Waveform.
  if (opt.waveformBins > 0) f.waveform = measureWaveform(audio, opt.waveformBins);

  // Downbeats (needs the beat ticks).
  if (opt.downbeats) f.downbeatIndexes = measureDownbeats(audio, f.ticks, sr);

  return f;
}

// Indentation: `indent` is the outer indent; members use indent + 2 spaces.
void printFile(std::ostream& o, const std::string& file, const Features& f,
               const Options& opt, const std::string& indent) {
  const std::string in = indent + "  ";
  const std::string in2 = in + "  ";
  o << indent << "{\n"
    << in << "\"schema\": " << kSchema << ",\n"
    << in << "\"file\": " << jsonString(file) << ",\n"
    << in << "\"duration\": " << jsonNum(f.duration) << ",\n"
    << in << "\"bpm\": " << jsonNum(f.bpm) << ",\n"
    << in << "\"bpm_confidence\": " << jsonNum(f.bpmConfidence) << ",\n"
    << in << "\"key\": " << jsonText(f.key) << ",\n"
    << in << "\"scale\": " << jsonText(f.scale) << ",\n"
    << in << "\"key_strength\": " << jsonNum(f.keyStrength) << ",\n"
    << in << "\"tuning_frequency\": " << jsonNum(f.tuningFrequency) << ",\n"
    << in << "\"tuning_cents\": " << jsonNum(f.tuningCents) << ",\n"
    << in << "\"replay_gain_db\": " << jsonNum(f.replayGain) << ",\n"
    << in << "\"loudness_db\": " << jsonNum(f.loudness) << ",\n"
    << in << "\"dynamic_complexity\": " << jsonNum(f.dynamicComplexity) << ",\n"
    << in << "\"silence\": {\n"
    << in2 << "\"schema\": " << kSchema << ",\n"
    << in2 << "\"leadMs\": " << jsonNum(f.leadMs) << ",\n"
    << in2 << "\"tailMs\": " << jsonNum(f.tailMs) << "\n"
    << in << "},\n"
    << in << "\"loudness\": {\n"
    << in2 << "\"schema\": " << kSchema << ",\n"
    << in2 << "\"integratedLufs\": " << jsonNum(f.integratedLufs) << ",\n"
    << in2 << "\"truePeakDbtp\": " << jsonNum(f.truePeakDbtp) << "\n"
    << in << "}";
  if (opt.waveformBins > 0) {
    o << ",\n" << in << "\"waveform\": {\n"
      << in2 << "\"schema\": " << kSchema << ",\n"
      << in2 << "\"bins\": " << opt.waveformBins << ",\n"
      << in2 << "\"peaks\": [";
    for (size_t i = 0; i < f.waveform.size(); ++i) o << (i ? ", " : "") << f.waveform[i];
    o << "]\n" << in << "}";
  }
  if (opt.beats) {
    o << ",\n" << in << "\"beats\": [";
    for (size_t i = 0; i < f.ticks.size(); ++i) o << (i ? ", " : "") << jsonNum(f.ticks[i]);
    o << "]";
  }
  if (opt.downbeats) {
    o << ",\n" << in << "\"downbeatIndexes\": ";
    if (f.downbeatIndexes) {
      o << "[";
      for (size_t i = 0; i < f.downbeatIndexes->size(); ++i)
        o << (i ? ", " : "") << (*f.downbeatIndexes)[i];
      o << "]";
    } else {
      o << "null";
    }
  }
  o << "\n" << indent << "}";
}

void printError(std::ostream& o, const std::string& file, const std::string& error,
                const std::string& indent) {
  const std::string in = indent + "  ";
  o << indent << "{\n"
    << in << "\"schema\": " << kSchema << ",\n"
    << in << "\"file\": " << jsonString(file) << ",\n"
    << in << "\"error\": " << jsonString(error) << "\n"
    << indent << "}";
}

void usage(std::ostream& o) {
  o << "Usage: folksentia [options] FILE...\n"
       "\n"
       "Extract tempo, key, tuning, loudness and silence from audio files; prints JSON.\n"
       "The JSON layout is documented in README.md.\n"
       "\n"
       "Options:\n"
       "  -b, --beats            include beat positions (seconds)\n"
       "  -d, --downbeats        include downbeatIndexes (indexes into beats); implies --beats\n"
       "  -w, --waveform[=BINS]  include BINS waveform peaks, 0..255 (default 150)\n"
       "  -f, --fast             use the faster 'degara' beat tracker\n"
       "  -r, --sample-rate HZ   analysis sample rate (default 44100)\n"
       "  -h, --help             show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") { usage(std::cout); return 0; }
    else if (a == "-b" || a == "--beats") opt.beats = true;
    else if (a == "-d" || a == "--downbeats") { opt.downbeats = true; opt.beats = true; }
    else if (a == "-w" || a == "--waveform") opt.waveformBins = 150;
    else if (a.rfind("--waveform=", 0) == 0) {
      try {
        opt.waveformBins = std::stoi(a.substr(11));
      } catch (const std::exception&) {
        opt.waveformBins = 0;
      }
      if (opt.waveformBins < 1 || opt.waveformBins > 100000) {
        std::cerr << "folksentia: --waveform needs BINS between 1 and 100000\n";
        return 2;
      }
    }
    else if (a == "-f" || a == "--fast") opt.rhythmMethod = "degara";
    else if ((a == "-r" || a == "--sample-rate") && i + 1 < argc) opt.sampleRate = std::stod(argv[++i]);
    else if (a == "--") { for (++i; i < argc; ++i) opt.files.push_back(argv[i]); }
    else if (!a.empty() && a[0] == '-') { std::cerr << "unknown option: " << a << "\n"; usage(std::cerr); return 2; }
    else opt.files.push_back(a);
  }
  if (opt.files.empty()) { usage(std::cerr); return 2; }

  essentia::warningLevelActive = false;
  essentia::infoLevelActive = false;
  essentia::init();

  const bool many = opt.files.size() > 1;
  std::cout << std::setprecision(6);
  if (many) std::cout << "[\n";
  int rc = 0;
  bool first = true;
  for (const auto& file : opt.files) {
    if (!first) std::cout << ",\n";
    first = false;
    const std::string indent = many ? "  " : "";
    try {
      Features f = analyze(file, opt);
      printFile(std::cout, file, f, opt, indent);
    } catch (const std::exception& e) {
      std::cerr << "folksentia: " << file << ": " << e.what() << "\n";
      printError(std::cout, file, e.what(), indent);
      rc = 1;
    }
  }
  std::cout << (many ? "\n]\n" : "\n");

  essentia::shutdown();
  return rc;
}

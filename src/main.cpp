// folksentia -- audio feature extractor built on Essentia.
//
// Usage: folksentia [options] FILE...
// Prints one JSON object per file (JSON array if several files).

#include <essentia/algorithmfactory.h>
#include <essentia/essentia.h>
#include <essentia/essentiamath.h>

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace essentia;
using essentia::standard::Algorithm;
using essentia::standard::AlgorithmFactory;

namespace {

struct Options {
  Real sampleRate = 44100.;
  std::string rhythmMethod = "multifeature";
  bool beats = false;
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

Features analyze(const std::string& file, const Options& opt) {
  Features f;
  const Real sr = opt.sampleRate;

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

  return f;
}

void printJson(std::ostream& o, const std::string& file, const Features& f,
               const Options& opt, const std::string& indent) {
  const std::string in = indent + "  ";
  o << indent << "{\n"
    << in << "\"file\": " << jsonString(file) << ",\n"
    << in << "\"duration\": " << f.duration << ",\n"
    << in << "\"bpm\": " << f.bpm << ",\n"
    << in << "\"bpm_confidence\": " << f.bpmConfidence << ",\n"
    << in << "\"key\": " << jsonString(f.key) << ",\n"
    << in << "\"scale\": " << jsonString(f.scale) << ",\n"
    << in << "\"key_strength\": " << f.keyStrength << ",\n"
    << in << "\"tuning_frequency\": " << f.tuningFrequency << ",\n"
    << in << "\"tuning_cents\": " << f.tuningCents << ",\n"
    << in << "\"replay_gain_db\": " << f.replayGain << ",\n"
    << in << "\"loudness_db\": " << f.loudness << ",\n"
    << in << "\"dynamic_complexity\": " << f.dynamicComplexity;
  if (opt.beats) {
    o << ",\n" << in << "\"beats\": [";
    for (size_t i = 0; i < f.ticks.size(); ++i) o << (i ? ", " : "") << f.ticks[i];
    o << "]";
  }
  o << "\n" << indent << "}";
}

void usage(std::ostream& o) {
  o << "Usage: folksentia [options] FILE...\n"
       "\n"
       "Extract tempo, key, tuning and loudness from audio files; prints JSON.\n"
       "\n"
       "Options:\n"
       "  -b, --beats            include beat positions (seconds)\n"
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
    try {
      Features f = analyze(file, opt);
      if (!first) std::cout << ",\n";
      printJson(std::cout, file, f, opt, many ? "  " : "");
      first = false;
    } catch (const std::exception& e) {
      std::cerr << "folksentia: " << file << ": " << e.what() << "\n";
      rc = 1;
    }
  }
  std::cout << (many ? "\n]\n" : "\n");

  essentia::shutdown();
  return rc;
}

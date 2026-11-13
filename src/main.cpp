// folksentia -- audio feature extractor built on Essentia.
//
// Usage: folksentia [options] FILE...
// Prints one JSON object per file (JSON array if several files).
// The JSON layout is documented in README.md (schema version 1).

#include <essentia/algorithmfactory.h>
#include <essentia/essentia.h>
#include <essentia/essentiamath.h>

#include <chromaprint.h>

#include "version.h"  // generated: FOLKSENTIA_VERSION_INFO

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <iterator>
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

// Sample rate RhythmExtractor2013 is designed for.
constexpr Real kRhythmRate = 44100;

// Fingerprint the first kFingerprintSeconds of audio, like fpcalc's default.
// Fixed so that fingerprints from different runs compare.
constexpr Real kFingerprintSeconds = 120;

// Essentia's Chromaprinter uses CHROMAPRINT_ALGORITHM_DEFAULT. Chromaprint
// names that algorithm TEST2; it is the one fpcalc and AcoustID use, and we
// report it as version 2. Fingerprints from other algorithms don't compare.
static_assert(CHROMAPRINT_ALGORITHM_DEFAULT == CHROMAPRINT_ALGORITHM_TEST2,
              "libchromaprint default algorithm changed; update fingerprint version");
constexpr int kFingerprintVersion = 2;

struct Options {
  Real sampleRate = 44100.;
  Real silenceThresholdDb = -100;  // frame power below this is silence
  std::string rhythmMethod = "multifeature";
  bool beats = false;
  bool downbeats = false;
  int waveformBins = 0;  // 0 = no waveform
  bool fingerprint = false;
  bool array = false;  // always print a JSON array
  int meter = 4;       // beats per bar, for downbeats
  std::vector<std::string> files;
};

struct Features {
  Real duration = 0;
  // Tempo, key and tuning are unset when they could not be measured (digital
  // silence, no tempo found, no frame for the tuning estimate).
  std::optional<Real> bpm, bpmConfidence;
  std::vector<Real> ticks;
  std::string key, scale;
  std::optional<Real> keyStrength;
  std::optional<Real> tuningFrequency, tuningCents;
  Real replayGain = 0, loudness = 0, dynamicComplexity = 0;

  // Silence at start and end; unset when the file has no non-silent frame.
  std::optional<Real> leadMs, tailMs;

  // EBU R128 integrated loudness and true peak (dBTP, max over channels).
  std::optional<Real> integratedLufs, truePeakDbtp;

  // Waveform peaks 0..255, one per bin; empty unless requested.
  std::vector<int> waveform;

  // Indexes into ticks that start a bar; unset when unknown.
  std::optional<std::vector<int>> downbeatIndexes;

  // Chromaprint fingerprint (compressed, base64) and the seconds of audio it
  // covers; unset when not requested or when none could be computed.
  std::optional<std::string> fingerprint;
  Real fingerprintSeconds = 0;
};

using AlgoPtr = std::unique_ptr<Algorithm>;

template <typename... Args>
AlgoPtr make(const std::string& name, Args&&... args) {
  return AlgoPtr(AlgorithmFactory::create(name, std::forward<Args>(args)...));
}

// Length of the valid UTF-8 sequence starting at s[i], or 0 if the bytes
// there are not valid UTF-8 (RFC 3629: no overlong forms, no surrogates,
// nothing above U+10FFFF).
size_t utf8SequenceLength(const std::string& s, size_t i) {
  const auto at = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned char c = at(i);
  if (c < 0x80) return 1;
  size_t len;
  unsigned char lo = 0x80, hi = 0xBF;  // allowed range of the second byte
  if (c >= 0xC2 && c <= 0xDF) len = 2;
  else if (c == 0xE0) { len = 3; lo = 0xA0; }
  else if (c >= 0xE1 && c <= 0xEC) len = 3;
  else if (c == 0xED) { len = 3; hi = 0x9F; }
  else if (c >= 0xEE && c <= 0xEF) len = 3;
  else if (c == 0xF0) { len = 4; lo = 0x90; }
  else if (c >= 0xF1 && c <= 0xF3) len = 4;
  else if (c == 0xF4) { len = 4; hi = 0x8F; }
  else return 0;
  if (i + len > s.size()) return 0;
  if (at(i + 1) < lo || at(i + 1) > hi) return 0;
  for (size_t k = 2; k < len; ++k) {
    if (at(i + k) < 0x80 || at(i + k) > 0xBF) return 0;
  }
  return len;
}

bool isValidUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const size_t len = utf8SequenceLength(s, i);
    if (len == 0) return false;
    i += len;
  }
  return true;
}

// JSON string literal. Output is always valid UTF-8: each byte that is not
// part of a valid UTF-8 sequence becomes U+FFFD.
std::string jsonString(const std::string& s) {
  std::ostringstream o;
  o << '"';
  for (size_t i = 0; i < s.size();) {
    const size_t len = utf8SequenceLength(s, i);
    if (len == 0) {
      o << "\\ufffd";  // U+FFFD REPLACEMENT CHARACTER
      ++i;
      continue;
    }
    if (len > 1) {
      o.write(s.data() + i, std::streamsize(len));
      i += len;
      continue;
    }
    const unsigned char c = static_cast<unsigned char>(s[i++]);
    switch (c) {
      case '"':  o << "\\\""; break;
      case '\\': o << "\\\\"; break;
      case '\n': o << "\\n"; break;
      case '\r': o << "\\r"; break;
      case '\t': o << "\\t"; break;
      default:
        if (c < 0x20 || c == 0x7F) {
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

std::string base64(const std::string& bytes) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const size_t n = std::min<size_t>(3, bytes.size() - i);
    unsigned v = 0;
    for (size_t k = 0; k < 3; ++k) {
      v = (v << 8) | (k < n ? static_cast<unsigned char>(bytes[i + k]) : 0u);
    }
    for (size_t k = 0; k < 4; ++k) {
      out += k <= n ? kTable[(v >> (18 - 6 * k)) & 0x3F] : '=';
    }
  }
  return out;
}

// "file" member, plus "file_base64" (the exact path bytes) when the path is
// not valid UTF-8 and "file" therefore had to replace bytes with U+FFFD.
std::string jsonFileMembers(const std::string& file, const std::string& in) {
  std::string out = in + "\"file\": " + jsonString(file) + ",\n";
  if (!isValidUtf8(file)) out += in + "\"file_base64\": \"" + base64(file) + "\",\n";
  return out;
}

// JSON number, or null when the value is not finite (NaN, +-inf).
std::string jsonNum(Real v) {
  if (!std::isfinite(v)) return "null";
  std::ostringstream o;
  o << std::setprecision(6) << v;
  return o.str();
}

// Time in seconds with fixed millisecond resolution, or null when not finite.
// (Significant digits would lose resolution as times grow.)
std::string jsonSeconds(Real v) {
  if (!std::isfinite(v)) return "null";
  std::ostringstream o;
  o << std::fixed << std::setprecision(3) << v;
  return o.str();
}

std::string jsonNum(const std::optional<Real>& v) {
  return v ? jsonNum(*v) : "null";
}

// Text value, or null when empty (unknown).
std::string jsonText(const std::string& s) {
  return s.empty() ? "null" : jsonString(s);
}

// Start of the first and end of the last non-silent 512-sample frame (frames
// advance by 256 samples). A frame is silent when its mean power is below
// `cutoff`. Unset when every frame is silent.
std::optional<std::pair<size_t, size_t>> soundSpan(const std::vector<Real>& audio, Real cutoff) {
  const size_t frame = 512, hop = 256, n = audio.size();
  std::optional<std::pair<size_t, size_t>> span;
  for (size_t s = 0; s < n; s += hop) {
    const size_t e = std::min(n, s + frame);
    Real energy = 0;
    for (size_t k = s; k < e; ++k) energy += audio[k] * audio[k];
    if (energy / Real(e - s) < cutoff) continue;
    if (!span) span.emplace(s, e);
    span->second = e;
  }
  return span;
}

// Digital silence: no frame above Essentia's silence cutoff (as isSilent()).
bool isDigitalSilence(const std::vector<Real>& audio) {
  return !soundSpan(audio, SILENCE_CUTOFF);
}

// Silence at start and end: time before the first and after the last frame
// whose mean power is at least `thresholdDb` dBFS.
void measureSilence(const std::vector<Real>& audio, Real sr, Real thresholdDb, Features& f) {
  const auto span = soundSpan(audio, Real(std::pow(10., thresholdDb / 10.)));
  if (!span) return;  // all silent: unknown
  f.leadMs = Real(span->first) / sr * 1000.;
  f.tailMs = Real(audio.size() - span->second) / sr * 1000.;
}

// `signal` resampled from rate `from` to rate `to`.
std::vector<Real> resample(std::vector<Real> signal, Real from, Real to) {
  if (from == to) return signal;
  std::vector<Real> out;
  auto rs = make("Resample", "inputSampleRate", from, "outputSampleRate", to);
  rs->input("signal").set(signal);
  rs->output("signal").set(out);
  rs->compute();
  return out;
}

// EBU R128 integrated loudness and true peak of the stereo signal at the
// file's own sample rate.
void measureLoudness(const std::vector<StereoSample>& stereo, Real rate, Features& f) {
  {
    auto ebu = make("LoudnessEBUR128", "sampleRate", rate);
    std::vector<Real> momentary, shortTerm;
    Real loudnessRange = 0;
    ebu->input("signal").set(stereo);
    ebu->output("momentaryLoudness").set(momentary);
    ebu->output("shortTermLoudness").set(shortTerm);
    ebu->output("integratedLoudness").set(f.integratedLufs.emplace());
    ebu->output("loudnessRange").set(loudnessRange);
    ebu->compute();
    // Nothing passed the -70 LUFS absolute gate (e.g. digital silence): Essentia
    // reports the gate itself, but BS.1770 integrated loudness is undefined.
    if (*f.integratedLufs <= -70) f.integratedLufs.reset();
  }

  // TruePeakDetector holds the whole 4x oversampled signal (and copies it), so
  // each channel goes through it in blocks. The resampler is stateless; each
  // block is extended by kMargin samples on both sides, and the oversampled
  // samples of those margins, distorted by the cut, are left out.
  constexpr size_t kBlock = size_t(1) << 19, kMargin = 4096;
  constexpr int kOversampling = 4;  // TruePeakDetector's default
  const size_t n = stereo.size();
  auto tp = make("TruePeakDetector", "sampleRate", rate, "oversamplingFactor", kOversampling);
  Real peak = 0;
  std::vector<Real> block, out, locations;
  for (int c = 0; c < 2; ++c) {
    for (size_t s = 0; s < n; s += kBlock) {
      const size_t a = s > kMargin ? s - kMargin : 0;
      const size_t e = std::min(n, s + kBlock);
      const size_t b = std::min(n, e + kMargin);
      block.resize(b - a);
      for (size_t i = a; i < b; ++i) block[i - a] = c ? stereo[i].second : stereo[i].first;
      out.clear();
      locations.clear();
      tp->input("signal").set(block);
      tp->output("output").set(out);
      tp->output("peakLocations").set(locations);
      tp->compute();
      const size_t from = (s - a) * kOversampling;
      const size_t to = std::min(out.size(), (e - a) * kOversampling);
      for (size_t i = from; i < to; ++i) peak = std::max(peak, std::abs(out[i]));
    }
  }
  if (peak > 0) f.truePeakDbtp = 20. * std::log10(peak);
  else f.truePeakDbtp.reset();  // digital silence: unknown
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

// Chromaprint fingerprint of the first kFingerprintSeconds. Unset when
// Chromaprint gives no fingerprint (e.g. a file only a few seconds long); the
// caller skips digital silence.
void measureFingerprint(const std::vector<Real>& audio, Real sr, Features& f) {
  const size_t n = std::min(audio.size(), size_t(kFingerprintSeconds * sr));
  // Chromaprinter casts samples * 2^15 to int16 without clamping, so clip to
  // the int16 range first (decoded lossy audio can exceed full scale).
  const Real top = Real(32767) / Real(32768);
  std::vector<Real> clip(audio.begin(), audio.begin() + n);
  for (Real& v : clip) v = std::clamp(v, Real(-1), top);
  try {
    std::string fp;
    auto cp = make("Chromaprinter", "sampleRate", sr, "maxLength", 0.);
    cp->input("signal").set(clip);
    cp->output("fingerprint").set(fp);
    cp->compute();
    // A too-short signal gives a header with no items ("AQAAAA"): treat as
    // none. Decoding also confirms the algorithm actually used.
    uint32_t* items = nullptr;
    int size = 0, algorithm = -1;
    if (!chromaprint_decode_fingerprint(fp.data(), int(fp.size()), &items, &size, &algorithm, 1)) return;
    chromaprint_dealloc(items);
    if (size <= 0 || algorithm != CHROMAPRINT_ALGORITHM_TEST2) return;
    f.fingerprint = fp;
    f.fingerprintSeconds = Real(n) / sr;
  } catch (const std::exception&) {
    // no fingerprint: reported as null, the rest of the analysis stands
  }
}

// Bar starts from the beat ticks. Each beat is scored by the energy of the
// bass (below 150 Hz) just after it. Each of the `meter` possible bar phases is
// scored by how much louder its beats are than the others; the loudest phase
// wins if it is clearly louder. Returns nothing when there is no clear phase.
std::optional<std::vector<int>> measureDownbeats(const std::vector<Real>& audio,
                                                 const std::vector<Real>& ticks,
                                                 Real sr, int meter) {
  constexpr Real kMinContrast = 1.2;  // phase energy / energy of other beats
  if (ticks.size() < size_t(2 * meter) || audio.empty()) return std::nullopt;

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
  for (int p = 0; p < meter; ++p) {
    Real sumIn = 0, sumOut = 0;
    size_t nIn = 0, nOut = 0;
    for (size_t i = 0; i < accent.size(); ++i) {
      if (int(i % meter) == p) { sumIn += accent[i]; ++nIn; }
      else { sumOut += accent[i]; ++nOut; }
    }
    if (nIn == 0 || nOut == 0) continue;
    const Real meanIn = sumIn / nIn, meanOut = sumOut / nOut;
    const Real contrast = meanOut > 0 ? meanIn / meanOut : (meanIn > 0 ? 1e9 : 0);
    if (contrast > bestContrast) { bestContrast = contrast; bestPhase = p; }
  }
  if (bestPhase < 0 || bestContrast < kMinContrast) return std::nullopt;

  std::vector<int> out;
  for (size_t i = bestPhase; i < ticks.size(); i += meter) out.push_back(int(i));
  return out;
}

Features analyze(const std::string& file, const Options& opt) {
  Features f;
  const Real sr = opt.sampleRate;

  // Decode once, at the file's own sample rate.
  std::vector<StereoSample> stereo;
  Real nativeRate = 0;
  int channels = 0;
  {
    int bitRate = 0;
    std::string md5, codec;
    // Standard-mode algorithms require every output to be bound.
    auto loader = make("AudioLoader", "filename", file);
    loader->output("audio").set(stereo);
    loader->output("sampleRate").set(nativeRate);
    loader->output("numberChannels").set(channels);
    loader->output("md5").set(md5);
    loader->output("bit_rate").set(bitRate);
    loader->output("codec").set(codec);
    loader->compute();
  }
  if (stereo.empty() || nativeRate <= 0) throw EssentiaException("no audio decoded from ", file);
  if (channels == 1) {  // mono: play it as dual mono
    for (auto& s : stereo) s.second = s.first;
  }

  measureLoudness(stereo, nativeRate, f);

  // Mono analysis signal at the analysis rate, as MonoLoader makes it.
  // RhythmExtractor2013 assumes 44.1 kHz input, so it gets its own copy at
  // that rate when the analysis rate differs.
  std::vector<Real> audio, rhythmAudio;
  {
    std::vector<Real> mono;
    auto mixer = make("MonoMixer");
    mixer->input("audio").set(stereo);
    mixer->input("numberChannels").set(channels);
    mixer->output("audio").set(mono);
    mixer->compute();
    stereo.clear();
    stereo.shrink_to_fit();
    if (sr != kRhythmRate) rhythmAudio = resample(mono, nativeRate, kRhythmRate);
    audio = resample(std::move(mono), nativeRate, sr);
  }
  if (audio.empty()) throw EssentiaException("no audio decoded from ", file);
  f.duration = audio.size() / sr;

  // Digital silence has no tempo, key or tuning: leave them unknown.
  const bool silent = isDigitalSilence(audio);

  // Tempo + beats, at 44.1 kHz.
  if (!silent) {
    auto rhythm = make("RhythmExtractor2013", "method", opt.rhythmMethod);
    Real bpm = 0, confidence = 0;
    std::vector<Real> estimates, intervals;
    rhythm->input("signal").set(sr == kRhythmRate ? audio : rhythmAudio);
    rhythm->output("bpm").set(bpm);
    rhythm->output("ticks").set(f.ticks);
    rhythm->output("confidence").set(confidence);
    rhythm->output("estimates").set(estimates);
    rhythm->output("bpmIntervals").set(intervals);
    rhythm->compute();
    if (bpm > 0) {  // 0: no tempo found
      f.bpm = bpm;
      f.bpmConfidence = confidence;
    }
  }
  rhythmAudio.clear();
  rhythmAudio.shrink_to_fit();

  // Key / mode.
  if (!silent) {
    auto key = make("KeyExtractor", "sampleRate", sr);
    key->input("audio").set(audio);
    key->output("key").set(f.key);
    key->output("scale").set(f.scale);
    key->output("strength").set(f.keyStrength.emplace());
    key->compute();
  }

  // Tuning frequency (interesting for non-440 Hz folk instruments).
  if (!silent) {
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
    Real frequency = 0, cents = 0;
    tuning->output("tuningFrequency").set(frequency);
    tuning->output("tuningCents").set(cents);
    bool measured = false;

    for (;;) {
      cutter->compute();
      if (frame.empty()) break;
      if (isSilent(frame)) continue;
      window->compute();
      spectrum->compute();
      peaks->compute();
      tuning->compute();  // accumulates; output is the running estimate
      measured = true;
    }
    if (measured) {
      f.tuningFrequency = frequency;
      f.tuningCents = cents;
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

  // Silence at start and end.
  measureSilence(audio, sr, opt.silenceThresholdDb, f);

  // Waveform.
  if (opt.waveformBins > 0) f.waveform = measureWaveform(audio, opt.waveformBins);

  // Downbeats (needs the beat ticks).
  if (opt.downbeats) f.downbeatIndexes = measureDownbeats(audio, f.ticks, sr, opt.meter);

  // Fingerprint; none for digital silence.
  if (opt.fingerprint && !isDigitalSilence(audio)) measureFingerprint(audio, sr, f);

  return f;
}

// Indentation: `indent` is the outer indent; members use indent + 2 spaces.
void printFile(std::ostream& o, const std::string& file, const Features& f,
               const Options& opt, const std::string& indent) {
  const std::string in = indent + "  ";
  const std::string in2 = in + "  ";
  o << indent << "{\n"
    << in << "\"schema\": " << kSchema << ",\n"
    << jsonFileMembers(file, in)
    << in << "\"duration\": " << jsonSeconds(f.duration) << ",\n"
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
    << in2 << "\"thresholdDb\": " << jsonNum(opt.silenceThresholdDb) << ",\n"
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
    for (size_t i = 0; i < f.ticks.size(); ++i) o << (i ? ", " : "") << jsonSeconds(f.ticks[i]);
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
  if (opt.fingerprint) {
    o << ",\n" << in << "\"fingerprint\": ";
    if (f.fingerprint) {
      o << "{\n"
        << in2 << "\"schema\": " << kSchema << ",\n"
        << in2 << "\"algorithm\": \"chromaprint\",\n"
        << in2 << "\"version\": " << kFingerprintVersion << ",\n"
        << in2 << "\"durationSec\": " << jsonSeconds(f.fingerprintSeconds) << ",\n"
        << in2 << "\"value\": " << jsonString(*f.fingerprint) << "\n"
        << in << "}";
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
    << jsonFileMembers(file, in)
    << in << "\"error\": " << jsonString(error) << "\n"
    << indent << "}";
}

// The whole of `s` as a finite number, or nothing.
std::optional<double> parseNumber(const std::string& s) {
  try {
    size_t used = 0;
    const double v = std::stod(s, &used);
    if (used != s.size() || !std::isfinite(v)) return std::nullopt;
    return v;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void printVersion(std::ostream& o) {
  constexpr const char* kInfo[] = {FOLKSENTIA_VERSION_INFO};  // version, build date
  const auto keys = AlgorithmFactory::keys();
  const bool chromaprint = std::find(keys.begin(), keys.end(), "Chromaprinter") != keys.end();
  o << "folksentia " << kInfo[0] << "\n"
    << "built " << kInfo[1] << "\n"
    << "Essentia " << essentia::version << " (" << essentia::version_git_sha << ")\n"
    << "Chromaprint " << (chromaprint ? "yes" : "no") << "\n";
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
       "  -m, --meter N          beats per bar for --downbeats, 2..12 (default 4)\n"
       "  -w, --waveform[=BINS]  include BINS waveform peaks, 0..255 (default 150)\n"
       "  -p, --fingerprint      include a Chromaprint fingerprint of the first 120 s\n"
       "  -a, --array            print a JSON array even for a single file\n"
       "  -s, --silence-threshold DB\n"
       "                         frame power (dBFS) below which a frame is silence (default -100)\n"
       "  -f, --fast             use the faster 'degara' beat tracker\n"
       "  -r, --sample-rate HZ   analysis sample rate: 8000, 16000, 32000, 44100\n"
       "                         or 48000 (default 44100)\n"
       "  -V, --version          show version, build date and Essentia version\n"
       "  -h, --help             show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  bool showVersion = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") { usage(std::cout); return 0; }
    else if (a == "-V" || a == "--version") showVersion = true;
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
    else if (a == "-p" || a == "--fingerprint") opt.fingerprint = true;
    else if (a == "-a" || a == "--array") opt.array = true;
    else if (a == "-s" || a == "--silence-threshold") {
      const auto db = i + 1 < argc ? parseNumber(argv[++i]) : std::nullopt;
      if (!db || *db < -150 || *db > 0) {
        std::cerr << "folksentia: --silence-threshold needs DB between -150 and 0\n";
        return 2;
      }
      opt.silenceThresholdDb = Real(*db);
    }
    else if (a == "-m" || a == "--meter") {
      const auto n = i + 1 < argc ? parseNumber(argv[++i]) : std::nullopt;
      if (!n || *n != std::floor(*n) || *n < 2 || *n > 12) {
        std::cerr << "folksentia: --meter needs N, a whole number of beats per bar from 2 to 12\n";
        return 2;
      }
      opt.meter = int(*n);
    }
    else if (a == "-f" || a == "--fast") opt.rhythmMethod = "degara";
    else if (a == "-r" || a == "--sample-rate") {
      const auto hz = i + 1 < argc ? parseNumber(argv[++i]) : std::nullopt;
      // The rates ReplayGain's EqualLoudness filter supports.
      constexpr double kRates[] = {8000, 16000, 32000, 44100, 48000};
      if (!hz || std::find(std::begin(kRates), std::end(kRates), *hz) == std::end(kRates)) {
        std::cerr << "folksentia: --sample-rate needs HZ of 8000, 16000, 32000, 44100 or 48000\n";
        return 2;
      }
      opt.sampleRate = Real(*hz);
    }
    else if (a == "--") { for (++i; i < argc; ++i) opt.files.push_back(argv[i]); }
    else if (!a.empty() && a[0] == '-') { std::cerr << "unknown option: " << a << "\n"; usage(std::cerr); return 2; }
    else opt.files.push_back(a);
  }
  if (opt.files.empty() && !showVersion) { usage(std::cerr); return 2; }

  essentia::warningLevelActive = false;
  essentia::infoLevelActive = false;
  essentia::init();

  if (showVersion) {
    printVersion(std::cout);
    essentia::shutdown();
    return 0;
  }

  if (opt.fingerprint) {
    const auto keys = AlgorithmFactory::keys();
    if (std::find(keys.begin(), keys.end(), "Chromaprinter") == keys.end()) {
      std::cerr << "folksentia: --fingerprint: Essentia was built without Chromaprint;\n"
                   "  install libchromaprint-dev and run scripts/build-essentia.sh again\n";
      essentia::shutdown();
      return 2;
    }
  }

  const bool many = opt.array || opt.files.size() > 1;
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

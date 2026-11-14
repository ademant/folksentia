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
#include <array>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace essentia;
using essentia::standard::Algorithm;
using essentia::standard::AlgorithmFactory;

namespace {

constexpr int kSchema = 1;

// Tuning is also measured per segment of this length, for its spread.
constexpr Real kTuningSegmentSec = 30;

// Keys weaker than this are reported as unknown. Reliable keys on folk
// recordings came out at 0.85-0.95; a cappella singing drifting by a quarter
// tone gave 0.54 (see README).
constexpr Real kMinKeyStrength = 0.6;

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

// Pulse, bar and accents from the onset envelope (measureRhythm).
struct Rhythm {
  Real pulseSec = 0, barSec = 0, barScore = 0;
  int barPulses = 0;
  struct Alternative { int pulses; Real sec, score; };
  std::vector<Alternative> alternatives;
  std::vector<Real> accentEnergy, accentBass;  // per pulse of the bar, 0..1
  struct Point { Real startSec, barsPerMin; };
  std::vector<Point> tempoCurve;
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
  // Spread of the tuning across segments (cents); unset with fewer than two.
  std::optional<Real> tuningSpreadCents;
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

  // Unset for silence, short files, or when no cycle can be measured.
  std::optional<Rhythm> rhythm;
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

// Spread of the segments' tuning around `overall`, in cents: 10th to 90th
// percentile. Tuning repeats every semitone, so each segment's deviation is
// wrapped into [-50, 50) cents. Unset with fewer than two segments.
std::optional<Real> tuningSpread(const std::vector<Real>& segments, Real overall) {
  if (segments.size() < 2 || !(overall > 0)) return std::nullopt;
  std::vector<Real> cents;
  for (Real hz : segments) {
    Real c = 1200 * std::log2(hz / overall);
    c -= 100 * std::floor((c + 50) / 100);
    cents.push_back(c);
  }
  std::sort(cents.begin(), cents.end());
  const auto at = [&](Real q) {  // linear interpolation between ranks
    const Real r = q * Real(cents.size() - 1);
    const size_t i = size_t(r);
    return i + 1 < cents.size() ? cents[i] + (r - Real(i)) * (cents[i + 1] - cents[i]) : cents[i];
  };
  return at(0.9) - at(0.1);
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

  // True peak as ITU-R BS.1770-4 Annex 2 measures it: 4x oversampling by a
  // polyphase FIR interpolator, here a Hann-windowed sinc of 49 taps as in
  // libebur128, then the largest absolute sample. It streams over the input
  // and holds no oversampled copy; Essentia's TruePeakDetector runs a general
  // sinc resampler instead and took most of the analysis time.
  constexpr int kOversampling = 4, kTaps = 49;
  constexpr int kPerPhase = (kTaps + kOversampling - 1) / kOversampling;
  static const auto kPhases = [] {
    std::array<std::array<double, kPerPhase>, kOversampling> h{};
    for (int j = 0; j < kTaps; ++j) {
      const double m = j - (kTaps - 1) / 2.0, x = m * M_PI / kOversampling;
      const double sinc = std::abs(m) < 1e-9 ? 1.0 : std::sin(x) / x;
      h[j % kOversampling][j / kOversampling] = sinc * 0.5 * (1 - std::cos(2 * M_PI * j / (kTaps - 1)));
    }
    return h;
  }();
  const size_t n = stereo.size();
  double peak = 0;
  for (int c = 0; c < 2; ++c) {
    std::array<double, kPerPhase> z{};  // the last kPerPhase samples, newest first
    for (size_t i = 0; i < n + kPerPhase; ++i) {  // and run the filter out past the end
      std::copy_backward(z.begin(), z.end() - 1, z.end());
      z[0] = i < n ? (c ? stereo[i].second : stereo[i].first) : 0;
      for (const auto& h : kPhases) {
        double y = 0;
        for (int k = 0; k < kPerPhase; ++k) y += h[k] * z[k];
        peak = std::max(peak, std::abs(y));
      }
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

// ---- Rhythm: pulse, bar, accents and tempo curve (README, "rhythm") --------
//
// Computed from a spectral-flux onset envelope rather than from the beat
// tracker, whose beats land on different note values from tune to tune. The
// pulse is the period whose multiples best match the envelope's
// autocorrelation; the bar is the cycle of pulses with the most structure
// that repeats from cycle to cycle and is not explained by a shorter cycle,
// or, when no cycle stands out, the cycle with the strongest autocorrelation.

constexpr Real kRhythmWindowSec = 30;       // bars are measured per window
constexpr Real kRhythmMinSec = 10;          // shorter files get no rhythm
constexpr Real kPulseMinSec = 0.10, kPulseMaxSec = 0.60, kPulseStepSec = 0.002;
constexpr Real kPulseSpanSec = 3.0;         // pulse multiples scored up to here
constexpr Real kBarMinSec = 0.8, kBarMaxSec = 4.5;
constexpr int kMaxBarPulses = 16;
constexpr Real kBassHz = 150;
constexpr Real kPrimaryMargin = 2;          // structure score lead over the runner-up and noise
constexpr Real kPrimaryMinAc = 0.4;         // and its autocorrelation vs. the best cycle's

struct Envelopes {
  Real rate = 0;                       // frames per second
  std::vector<Real> onsets, energy, bass;
};

// Onset (log spectral flux, local mean removed), energy and bass energy per
// frame of about 93 ms, hop 1/8 frame.
Envelopes rhythmEnvelopes(const std::vector<Real>& audio, Real sr) {
  const int frameSize = 1 << int(std::lround(std::log2(0.0929 * sr)));
  const int hop = frameSize / 8;
  Envelopes e;
  e.rate = sr / hop;
  auto window = make("Windowing", "type", "hann", "normalized", false, "zeroPhase", false);
  auto spectrum = make("Spectrum", "size", frameSize);
  std::vector<Real> frame(frameSize), windowed, spec, logPrev, logCur;
  window->input("frame").set(frame);
  window->output("frame").set(windowed);
  spectrum->input("frame").set(windowed);
  spectrum->output("spectrum").set(spec);
  const size_t bassBins = size_t(std::ceil(kBassHz * frameSize / sr));
  for (size_t s = 0; s + frameSize <= audio.size(); s += hop) {
    std::copy(audio.begin() + s, audio.begin() + s + frameSize, frame.begin());
    window->compute();
    spectrum->compute();
    logCur.resize(spec.size());
    Real energy = 0, bass = 0;
    for (size_t k = 0; k < spec.size(); ++k) {
      logCur[k] = std::log1p(100 * spec[k]);
      const Real power = spec[k] * spec[k];
      energy += power;
      if (k < bassBins) bass += power;
    }
    if (!logPrev.empty()) {
      Real flux = 0;
      for (size_t k = 0; k < logCur.size(); ++k) flux += std::max(Real(0), logCur[k] - logPrev[k]);
      e.onsets.push_back(flux);
      e.energy.push_back(energy);
      e.bass.push_back(bass);
    }
    std::swap(logPrev, logCur);
  }
  // Subtract the local mean (32 frames, offsets -16..+15) and keep the rises.
  const std::vector<Real> raw = e.onsets;
  const long n = long(raw.size());
  for (long i = 0; i < n; ++i) {
    Real sum = 0;
    for (long j = std::max(0L, i - 16); j <= std::min(n - 1, i + 15); ++j) sum += raw[j];
    e.onsets[i] = std::max(Real(0), raw[i] - sum / 32);
  }
  return e;
}

// Normalised autocorrelation of f[from, to) for lags 0..maxLag.
std::vector<Real> autocorrelation(const std::vector<Real>& f, size_t from, size_t to, size_t maxLag) {
  const size_t n = to - from;
  double mean = 0;
  for (size_t i = from; i < to; ++i) mean += f[i];
  mean /= double(std::max<size_t>(n, 1));
  std::vector<double> d(n);
  for (size_t i = 0; i < n; ++i) d[i] = f[from + i] - mean;
  std::vector<Real> a(std::min(maxLag + 1, n), 0);
  for (size_t lag = 0; lag < a.size(); ++lag) {
    double s = 0;
    for (size_t i = 0; i + lag < n; ++i) s += d[i] * d[i + lag];
    a[lag] = Real(s);
  }
  if (!a.empty() && a[0] > 0) {
    const Real a0 = a[0];
    for (Real& v : a) v /= a0;
  }
  return a;
}

// Largest value of a within `radius` lags of lag i.
Real peakNear(const std::vector<Real>& a, long i, long radius) {
  Real best = -1;
  for (long j = std::max(0L, i - radius); j <= std::min(long(a.size()) - 1, i + radius); ++j) best = std::max(best, a[j]);
  return best;
}

// Mean autocorrelation at the multiples of period p up to kPulseSpanSec.
Real pulseScore(const std::vector<Real>& a, Real p, Real rate) {
  const int count = int(kPulseSpanSec / p);
  Real sum = 0;
  for (int k = 1; k <= count; ++k) sum += peakNear(a, std::lround(k * p * rate), 1);
  return count > 0 ? sum / count : 0;
}

// The autocorrelation peak within t*(1 +- rel), in seconds, interpolated.
Real refinePeriod(const std::vector<Real>& a, Real t, Real rel, Real rate) {
  const long lo = long(t * (1 - rel) * rate);
  const long hi = std::min(long(a.size()), long(t * (1 + rel) * rate) + 1);
  if (lo < 0 || lo >= hi) return t;
  long i = lo;
  for (long j = lo; j < hi; ++j) if (a[j] > a[i]) i = j;
  // Parabolic interpolation, only at a real local maximum (not at the edge of
  // the search range), by at most half a frame.
  Real offset = 0;
  if (i > 0 && i + 1 < long(a.size()) && a[i] >= a[i - 1] && a[i] >= a[i + 1]) {
    const Real denom = a[i - 1] - 2 * a[i] + a[i + 1];
    if (denom < 0) offset = std::clamp((a[i - 1] - a[i + 1]) / (2 * denom), Real(-0.5), Real(0.5));
  }
  return (Real(i) + offset) / rate;
}

// Frame ranges of the analysis windows: whole windows, plus the last
// kRhythmWindowSec when the remainder is at least half a window.
struct RhythmWindow { size_t from, to; Real startSec; };
std::vector<RhythmWindow> rhythmWindows(size_t frames, Real rate) {
  const Real secs = frames / rate;
  if (secs < kRhythmWindowSec) return {{0, frames, 0}};
  std::vector<RhythmWindow> out;
  const int whole = int(secs / kRhythmWindowSec);
  for (int w = 0; w < whole; ++w)
    out.push_back({size_t(w * kRhythmWindowSec * rate), size_t((w + 1) * kRhythmWindowSec * rate),
                   w * kRhythmWindowSec});
  if (secs - whole * kRhythmWindowSec >= kRhythmWindowSec / 2)
    out.push_back({size_t((secs - kRhythmWindowSec) * rate), frames, secs - kRhythmWindowSec});
  return out;
}

// Mean of f per pulse of a cycle of `pulses` pulses lasting T seconds, over
// frames [from, to); `parity` 0 or 1 keeps only even or odd cycles, -1 all.
// Empty when a pulse has no frame.
std::vector<Real> foldCycle(const std::vector<Real>& f, size_t from, size_t to, Real rate, Real T,
                            int pulses, int parity) {
  if (!(T > 0) || pulses < 1) return {};
  std::vector<Real> sum(pulses, 0);
  std::vector<int> count(pulses, 0);
  for (size_t i = from; i < to; ++i) {
    const Real t = (i - from) / rate;
    if (parity >= 0 && long(t / T) % 2 != parity) continue;
    const int k = int(t / T * pulses) % pulses;
    sum[k] += f[i];
    ++count[k];
  }
  for (int k = 0; k < pulses; ++k) {
    if (count[k] == 0) return {};
    sum[k] /= count[k];
  }
  return sum;
}

// Structure of a cycle that repeats from one cycle to the next and is not
// periodic with a shorter cycle dividing it: the cross-spectrum of the even-
// and odd-cycle profiles over the harmonics coprime with the cycle length.
Real cycleStructure(const std::vector<Real>& even, const std::vector<Real>& odd) {
  const int n = int(even.size());
  Real meanA = 0, meanB = 0;
  for (int j = 0; j < n; ++j) { meanA += even[j]; meanB += odd[j]; }
  meanA /= n;
  meanB /= n;
  Real score = 0;
  for (int k = 1; k < n; ++k) {
    if (std::gcd(k, n) != 1) continue;
    Real ar = 0, ai = 0, br = 0, bi = 0;
    for (int j = 0; j < n; ++j) {
      const Real phase = -2 * M_PI * j * k / n;
      ar += (even[j] - meanA) * std::cos(phase);
      ai += (even[j] - meanA) * std::sin(phase);
      br += (odd[j] - meanB) * std::cos(phase);
      bi += (odd[j] - meanB) * std::sin(phase);
    }
    score += ar * br + ai * bi;  // Re(A * conj(B))
  }
  return score / (Real(n) * n);
}

Real median(std::vector<Real> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const size_t m = v.size() / 2;
  return v.size() % 2 ? v[m] : (v[m - 1] + v[m]) / 2;
}

// Rotation r of each profile (roll left by r) that best matches the running
// average, starting from the first profile; two passes.
std::vector<int> alignRotations(const std::vector<std::vector<Real>>& profiles) {
  const int n = int(profiles[0].size());
  std::vector<Real> ref = profiles[0];
  std::vector<int> rot(profiles.size(), 0);
  for (int pass = 0; pass < 2; ++pass) {
    Real refMean = 0;
    for (Real v : ref) refMean += v;
    refMean /= n;
    for (size_t w = 0; w < profiles.size(); ++w) {
      Real best = -1e30;
      for (int s = 0; s < n; ++s) {
        Real dot = 0;
        for (int j = 0; j < n; ++j) dot += (profiles[w][(j + s) % n] - 1) * (ref[j] - refMean);
        if (dot > best) { best = dot; rot[w] = s; }
      }
    }
    std::fill(ref.begin(), ref.end(), 0);
    for (size_t w = 0; w < profiles.size(); ++w)
      for (int j = 0; j < n; ++j) ref[j] += profiles[w][(j + rot[w]) % n] / profiles.size();
  }
  return rot;
}

std::optional<Rhythm> measureRhythm(const std::vector<Real>& audio, Real sr) {
  if (audio.size() < kRhythmMinSec * sr) return std::nullopt;
  const Envelopes e = rhythmEnvelopes(audio, sr);
  const Real rate = e.rate;
  const size_t maxLag = size_t(kBarMaxSec * 1.05 * rate) + 4;
  const std::vector<Real> whole = autocorrelation(e.onsets, 0, e.onsets.size(), maxLag);
  if (whole.size() <= maxLag || !(whole[0] > 0)) return std::nullopt;

  // Pulse: the period whose multiples line up best with the autocorrelation.
  Real pulse = 0, bestScore = -1;
  for (int j = 0;; ++j) {
    const Real p = kPulseMinSec + j * kPulseStepSec;
    if (p >= kPulseMaxSec) break;
    const Real s = pulseScore(whole, p, rate);
    if (s > bestScore) { bestScore = s; pulse = p; }
  }

  // Candidate cycles of N pulses: structure score and period per window.
  const auto windows = rhythmWindows(e.onsets.size(), rate);
  std::vector<std::vector<Real>> windowAc;
  for (const auto& w : windows) windowAc.push_back(autocorrelation(e.onsets, w.from, w.to, maxLag));
  struct Candidate { int pulses; Real structure, ac; std::vector<Real> periods; };
  std::vector<Candidate> cands;
  for (int n = 2; n <= kMaxBarPulses; ++n) {
    if (n * pulse < kBarMinSec || n * pulse > kBarMaxSec) continue;
    Candidate c{n, 0, peakNear(whole, std::lround(n * pulse * rate), 2), {}};
    std::vector<Real> scores;
    for (size_t w = 0; w < windows.size(); ++w) {
      const size_t from = windows[w].from, to = windows[w].to;
      const Real p = refinePeriod(windowAc[w], pulse, 0.05, rate);
      const Real T = refinePeriod(windowAc[w], n * p, 0.03, rate);
      c.periods.push_back(T);
      Real mean = 0;
      for (size_t i = from; i < to; ++i) mean += e.onsets[i];
      mean /= Real(to - from);
      const auto even = foldCycle(e.onsets, from, to, rate, T, n, 0);
      const auto odd = foldCycle(e.onsets, from, to, rate, T, n, 1);
      if (even.empty() || odd.empty() || !(mean > 0)) continue;
      scores.push_back(cycleStructure(even, odd) / (mean * mean));
    }
    if (scores.empty()) continue;
    c.structure = median(scores);
    cands.push_back(std::move(c));
  }
  if (cands.empty()) return std::nullopt;

  // The bar: the cycle with clearly the most structure of its own, provided it
  // also recurs (autocorrelation), else the cycle with the strongest
  // autocorrelation.
  std::vector<size_t> byStructure(cands.size());
  for (size_t i = 0; i < cands.size(); ++i) byStructure[i] = i;
  std::sort(byStructure.begin(), byStructure.end(),
            [&](size_t a, size_t b) { return cands[a].structure > cands[b].structure; });
  // Real structure can't make the even and odd cycles anti-correlate, so the
  // most negative score is a measure of the noise.
  const Real top = cands[byStructure[0]].structure;
  const Real second = cands.size() > 1 ? cands[byStructure[1]].structure : 0;
  const Real noise = std::max(Real(0), -cands[byStructure.back()].structure);
  size_t strongest = 0;
  for (size_t i = 0; i < cands.size(); ++i) if (cands[i].ac > cands[strongest].ac) strongest = i;
  size_t bar = byStructure[0];
  if (!(top > 0 && top >= kPrimaryMargin * std::max({second, noise, Real(1e-12)}) &&
        cands[bar].ac >= kPrimaryMinAc * cands[strongest].ac))
    bar = strongest;
  const Candidate& c = cands[bar];

  Rhythm r;
  r.barPulses = c.pulses;
  r.barSec = median(c.periods);
  r.pulseSec = r.barSec / c.pulses;
  r.barScore = c.ac;
  std::vector<size_t> others;
  for (size_t i = 0; i < cands.size(); ++i) if (i != bar) others.push_back(i);
  std::sort(others.begin(), others.end(), [&](size_t a, size_t b) { return cands[a].ac > cands[b].ac; });
  for (size_t i = 0; i < others.size() && i < 2; ++i)
    r.alternatives.push_back({cands[others[i]].pulses, cands[others[i]].pulses * r.pulseSec, cands[others[i]].ac});
  for (size_t w = 0; w < windows.size(); ++w)
    r.tempoCurve.push_back({windows[w].startSec, 60 / c.periods[w]});

  // Accents: energy and bass per pulse of the bar, windows aligned on energy,
  // rotated so the strongest pulse comes first, relative to it.
  std::vector<std::vector<Real>> energy, bass;
  for (size_t w = 0; w < windows.size(); ++w) {
    auto en = foldCycle(e.energy, windows[w].from, windows[w].to, rate, c.periods[w], c.pulses, -1);
    auto bs = foldCycle(e.bass, windows[w].from, windows[w].to, rate, c.periods[w], c.pulses, -1);
    Real me = 0, mb = 0;
    for (Real v : en) me += v;
    for (Real v : bs) mb += v;
    if (en.empty() || !(me > 0) || !(mb > 0)) continue;
    for (Real& v : en) v /= me / c.pulses;
    for (Real& v : bs) v /= mb / c.pulses;
    energy.push_back(std::move(en));
    bass.push_back(std::move(bs));
  }
  if (!energy.empty()) {
    const int n = c.pulses;
    const auto rot = alignRotations(energy);
    std::vector<Real> en(n, 0), bs(n, 0);
    for (size_t w = 0; w < energy.size(); ++w)
      for (int j = 0; j < n; ++j) {
        en[j] += energy[w][(j + rot[w]) % n];
        bs[j] += bass[w][(j + rot[w]) % n];
      }
    const int first = int(std::max_element(en.begin(), en.end()) - en.begin());
    const Real maxE = en[first], maxB = *std::max_element(bs.begin(), bs.end());
    for (int j = 0; j < n; ++j) {
      r.accentEnergy.push_back(std::round(en[(j + first) % n] / maxE * 100) / 100);
      r.accentBass.push_back(std::round(bs[(j + first) % n] / maxB * 100) / 100);
    }
  }
  return r;
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

  // Tuning frequency (interesting for non-440 Hz folk instruments), over the
  // whole file and per kTuningSegmentSec segment for its spread.
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
    auto segmentTuning = make("TuningFrequency");

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
    Real frequency = 0, cents = 0, segmentFrequency = 0, segmentCents = 0;
    for (auto* t : {tuning.get(), segmentTuning.get()}) {
      t->input("frequencies").set(freqs);
      t->input("magnitudes").set(mags);
    }
    tuning->output("tuningFrequency").set(frequency);
    tuning->output("tuningCents").set(cents);
    segmentTuning->output("tuningFrequency").set(segmentFrequency);
    segmentTuning->output("tuningCents").set(segmentCents);

    const size_t framesPerSegment = size_t(kTuningSegmentSec * sr / hopSize);
    const size_t minSegmentFrames = framesPerSegment / 3;  // enough sound to count
    std::vector<Real> segments;  // tuning of each segment, Hz
    size_t index = 0, measured = 0, segmentMeasured = 0;
    const auto endSegment = [&] {
      if (segmentMeasured >= minSegmentFrames) segments.push_back(segmentFrequency);
      segmentTuning->reset();
      segmentMeasured = 0;
    };
    for (;; ++index) {
      cutter->compute();
      if (frame.empty()) break;
      if (index > 0 && index % framesPerSegment == 0) endSegment();
      if (isSilent(frame)) continue;
      window->compute();
      spectrum->compute();
      peaks->compute();
      tuning->compute();  // accumulates; output is the running estimate
      segmentTuning->compute();
      ++measured;
      ++segmentMeasured;
    }
    endSegment();
    if (measured > 0) {
      f.tuningFrequency = frequency;
      f.tuningCents = cents;
      f.tuningSpreadCents = tuningSpread(segments, frequency);
    }
  }

  // Key / mode, on the measured tuning. A weak key is reported as unknown.
  if (!silent) {
    auto key = make("KeyExtractor", "sampleRate", sr,
                    "tuningFrequency", f.tuningFrequency.value_or(Real(440)));
    key->input("audio").set(audio);
    key->output("key").set(f.key);
    key->output("scale").set(f.scale);
    key->output("strength").set(f.keyStrength.emplace());
    key->compute();
    if (*f.keyStrength < kMinKeyStrength) {
      f.key.clear();
      f.scale.clear();
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

  // Pulse, bar, accents and tempo curve.
  if (!silent) f.rhythm = measureRhythm(audio, sr);

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

void printNumbers(std::ostream& o, const std::vector<Real>& v) {
  o << "[";
  for (size_t i = 0; i < v.size(); ++i) o << (i ? ", " : "") << jsonNum(v[i]);
  o << "]";
}

// The "rhythm" member's value; `in` is the indent of the member.
void printRhythm(std::ostream& o, const std::optional<Rhythm>& r, const std::string& in) {
  if (!r) { o << "null"; return; }
  const std::string in2 = in + "  ", in3 = in2 + "  ";
  o << "{\n"
    << in2 << "\"schema\": " << kSchema << ",\n"
    << in2 << "\"pulseSec\": " << jsonSeconds(r->pulseSec) << ",\n"
    << in2 << "\"barPulses\": " << r->barPulses << ",\n"
    << in2 << "\"barSec\": " << jsonSeconds(r->barSec) << ",\n"
    << in2 << "\"barsPerMin\": " << jsonNum(Real(std::round(600 / r->barSec) / 10)) << ",\n"
    << in2 << "\"barScore\": " << jsonNum(Real(std::round(r->barScore * 100) / 100)) << ",\n"
    << in2 << "\"alternatives\": [";
  for (size_t i = 0; i < r->alternatives.size(); ++i) {
    const auto& a = r->alternatives[i];
    o << (i ? ", " : "") << "{\"pulses\": " << a.pulses << ", \"sec\": " << jsonSeconds(a.sec)
      << ", \"score\": " << jsonNum(Real(std::round(a.score * 100) / 100)) << "}";
  }
  o << "],\n"
    << in2 << "\"accents\": {\n" << in3 << "\"energy\": ";
  printNumbers(o, r->accentEnergy);
  o << ",\n" << in3 << "\"bass\": ";
  printNumbers(o, r->accentBass);
  o << "\n" << in2 << "},\n"
    << in2 << "\"tempoCurve\": [";
  for (size_t i = 0; i < r->tempoCurve.size(); ++i)
    o << (i ? ", " : "") << "{\"startSec\": " << jsonSeconds(r->tempoCurve[i].startSec)
      << ", \"barsPerMin\": " << jsonNum(Real(std::round(r->tempoCurve[i].barsPerMin * 10) / 10)) << "}";
  o << "]\n" << in << "}";
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
    << in << "\"tuning_spread_cents\": " << jsonNum(f.tuningSpreadCents) << ",\n"
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
    << in << "},\n"
    << in << "\"rhythm\": ";
  printRhythm(o, f.rhythm, in);
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

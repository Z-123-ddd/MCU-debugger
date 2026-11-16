#pragma once

#include <QVector>

namespace mrd {

/// Spectrum analysis, backed by KFR (third_party/kfr).
///
/// This used to be a hand-written radix-2 FFT. KFR brings a tuned implementation
/// (SIMD kernels, split-radix, cached plans) and, more importantly, removes a piece of
/// numerical code this project would otherwise have to maintain and validate forever.
///
/// The interface deliberately stays in plain QVector<double> / Hz / dB: callers care
/// about "which frequencies and how loud", not about KFR's types.
class Fft
{
public:
    /// Single-sided amplitude spectrum in dB, length = fftSize / 2.
    ///
    /// A Hann window is applied before the transform — without it a non-integer number
    /// of cycles leaks energy into every bin and the peak reads wider than it is.
    /// NaN samples are treated as 0 so one broken channel cannot poison the spectrum.
    static QVector<double> magnitudeSpectrum(const QVector<double> &input);

    /// Point count actually used: the input length rounded up to a power of two.
    static int fftSize(int inputSize);

    /// Frequency of bin \a index, for the axis labels.
    static double binFrequency(double sampleRateHz, int fftPoints, int index);
};

} // namespace mrd

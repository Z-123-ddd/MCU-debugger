#include "core/Fft.h"

#include <kfr/base.hpp>
#include <kfr/dft.hpp>

#include <cmath>
#include <limits>

namespace mrd {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// Below this the dB conversion is clamped, so silence reads as a floor instead of -inf.
constexpr double kFloorAmplitude = 1e-12;
constexpr double kFloorDb = -240.0;

int nextPowerOfTwo(int n)
{
    int v = 1;
    while (v < n)
        v <<= 1;
    return v;
}

} // namespace

int Fft::fftSize(int inputSize)
{
    return inputSize <= 0 ? 0 : nextPowerOfTwo(inputSize);
}

double Fft::binFrequency(double sampleRateHz, int fftPoints, int index)
{
    if (fftPoints <= 0 || sampleRateHz <= 0.0)
        return 0.0;
    return sampleRateHz * double(index) / double(fftPoints);
}

QVector<double> Fft::magnitudeSpectrum(const QVector<double> &input)
{
    QVector<double> spectrum;
    if (input.isEmpty())
        return spectrum;

    const int n = nextPowerOfTwo(input.size());
    const int half = n / 2;

    // Zero-pad to a power of two. KFR handles any size, but a power of two keeps the
    // bin-to-frequency mapping trivial and matches what the axis labels assume.
    kfr::univector<kfr::complex<double>> buffer(n);
    const int size = input.size();
    for (int i = 0; i < size; ++i) {
        const double value = std::isfinite(input.at(i)) ? input.at(i) : 0.0;
        // Hann window: a raised cosine tapers both ends so a truncated cycle does not
        // smear across the whole spectrum.
        const double window = size > 1
            ? 0.5 * (1.0 - std::cos(2.0 * kPi * double(i) / double(size - 1)))
            : 1.0;
        buffer[i] = kfr::complex<double>(value * window, 0.0);
    }
    for (int i = size; i < n; ++i)
        buffer[i] = kfr::complex<double>(0.0, 0.0);

    const auto transformed = kfr::dft(buffer);

    spectrum.resize(half);
    // KFR returns the unnormalised sum, so a full-scale sine reads N/2 in its bin;
    // 2/N puts it at 1.0, i.e. 0 dB.
    const double invN = 2.0 / double(n);
    for (int i = 0; i < half; ++i) {
        const double amp = std::abs(transformed[i]) * invN;
        spectrum[i] = amp > kFloorAmplitude ? 20.0 * std::log10(amp) : kFloorDb;
    }
    return spectrum;
}

} // namespace mrd

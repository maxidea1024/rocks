#include "TextureBuildSteps.h"

#include <DeterministicMath/DeterministicMath.h>

#include <math.h>

// mip 필터 커널이다(Docs/Design/AssetSystem 12.18 TP1 1·2 — UE 5.8 TextureCompressorModule.cpp
// FImageKernel2D 174–389, TextureLODSettings.cpp GetMipGenSettings 395–448). 식과 연산 순서를 UE와
// 같게 두고 float로 계산한다. exp는 DeterministicMath Exp2(x · log2 e), sqrt는 IEEE 기본 연산이다
// (TP-D3).
namespace rocks::texture_build_detail {

namespace build_kernel {

namespace {

constexpr double Log2E = 1.4426950408889634073599246810019;
constexpr float KernelPi = 3.1415926535897932f;
// UE GetMipGenSettings: Sharpen n의 sharpen = n × 0.2, 크기 8.
constexpr float SharpenStep = 0.2f;
constexpr uint32 SharpenKernelSize = 8;

float ExpFloat(float value) noexcept {
    return static_cast<float>(deterministic::Exp2(static_cast<double>(value) * Log2E));
}

float SqrtFloat(float value) noexcept {
    return sqrtf(value);
}

// UE NormalDistribution(x, 분산) = exp(−x² / (2 분산)) / (√분산 · √(2π)).
float NormalDistribution(float x, float variance) noexcept {
    const float deviation = SqrtFloat(variance);
    const float exponent = -(x * x) / (2.0f * variance);
    const float scale = deviation * SqrtFloat(2.0f * KernelPi);
    return ExpFloat(exponent) / scale;
}

// UE BuildGaussian1D: 가운데 = 크기 × 0.5 − 0.5, 합이 sum이 되게 정규화(역수 곱).
void BuildGaussian1D(float* table, uint32 size, float sum, float variance) noexcept {
    const float center = static_cast<float>(size) * 0.5f - 0.5f;
    float current = 0.0f;
    for (uint32 index = 0; index < size; ++index) {
        const float actual = NormalDistribution(static_cast<float>(index) - center, variance);
        table[index] = actual;
        current += actual;
    }

    const float inverse = sum / current;
    for (uint32 index = 0; index < size; ++index) {
        table[index] *= inverse;
    }
}

// UE BuildFilterTable1DBase: 가운데 두 칸 0.5 × sum, 나머지 0.
void BuildBase1D(float* table, uint32 size, float sum) noexcept {
    const float inner = 0.5f * sum;
    const uint32 center = size / 2;
    for (uint32 index = 0; index < size; ++index) {
        table[index] = index == center || index == center - 1 ? inner : 0.0f;
    }
}

// UE BlurFilterTable1D: 이웃 셋(끝은 둘)의 합 / 3을 times번.
void Blur1D(float* table, uint32 size, uint32 times) noexcept {
    float intermediate[MaxKernelSize];
    for (uint32 pass = 0; pass < times; ++pass) {
        for (uint32 index = 0; index < size; ++index) {
            intermediate[index] = table[index];
        }

        for (uint32 index = 0; index < size; ++index) {
            float sum = intermediate[index];
            if (index != 0) {
                sum += intermediate[index - 1];
            }

            if (index + 1 < size) {
                sum += intermediate[index + 1];
            }

            table[index] = sum / 3.0f;
        }
    }
}

void Build2D(MipKernel& kernel) noexcept {
    for (uint32 y = 0; y < kernel.size; ++y) {
        for (uint32 x = 0; x < kernel.size; ++x) {
            kernel.weights2D[x + y * kernel.size] = kernel.weights1D[y] * kernel.weights1D[x];
        }
    }
}

} // namespace

MipKernel BuildMipKernel(uint32 size, float sharpen) noexcept {
    MipKernel kernel;
    kernel.size = size < MaxKernelSize ? size : MaxKernelSize;
    float* table = kernel.weights1D;
    if (kernel.size == 2) {
        // SimpleAverage(sharpen 무시).
        table[0] = 0.5f;
        table[1] = 0.5f;
        for (float& weight : kernel.weights2D) {
            weight = 0.0f;
        }

        kernel.weights2D[0] = 0.25f;
        kernel.weights2D[1] = 0.25f;
        kernel.weights2D[2] = 0.25f;
        kernel.weights2D[3] = 0.25f;
        return kernel;
    }

    if (sharpen < 0.0f) {
        // Blur — Gauss(분산 = −sharpen).
        BuildGaussian1D(table, kernel.size, 1.0f, -sharpen);
        Build2D(kernel);
        return kernel;
    }

    // 4·6·8탭 sharpen(UE는 그 밖 크기를 check로 막는다 — Rocks는 MipFilterFor가 만들지 않는다).
    float negative[MaxKernelSize] = {};
    if (kernel.size == 8) {
        const float doubled = sharpen * 2.0f;
        BuildBase1D(table, kernel.size, 1.0f + doubled);
        Blur1D(table, kernel.size, 1);
        BuildBase1D(negative, kernel.size, -doubled);
        Blur1D(negative, kernel.size, 3);
    } else {
        BuildBase1D(table, kernel.size, 1.0f + sharpen);
        BuildBase1D(negative, kernel.size, -sharpen);
        Blur1D(negative, kernel.size, kernel.size == 4 ? 1 : 2);
    }

    for (uint32 index = 0; index < kernel.size; ++index) {
        table[index] += negative[index];
    }

    Build2D(kernel);
    return kernel;
}

MipFilter MipFilterFor(TextureMipGenSettings mipGen,
                       TextureCompressionSettings compression) noexcept {
    MipFilter filter;
    // 색 이동 보정은 법선·마스크가 아닐 때만이다(UE 403).
    filter.sharpenWithoutColorShift = compression != TextureCompressionSettings::Normalmap &&
                                      compression != TextureCompressionSettings::Masks;
    const uint8 value = static_cast<uint8>(mipGen);
    const uint8 sharpen0 = static_cast<uint8>(TextureMipGenSettings::Sharpen0);
    const uint8 sharpen10 = static_cast<uint8>(TextureMipGenSettings::Sharpen10);
    const uint8 blur1 = static_cast<uint8>(TextureMipGenSettings::Blur1);
    const uint8 blur5 = static_cast<uint8>(TextureMipGenSettings::Blur5);
    if (value >= sharpen0 && value <= sharpen10) {
        filter.sharpen = static_cast<float>(value - sharpen0) * SharpenStep;
        filter.kernelSize = SharpenKernelSize;
    } else if (value >= blur1 && value <= blur5) {
        const int32 factor = static_cast<int32>(value) + 1 - static_cast<int32>(blur1);
        filter.sharpen = static_cast<float>(-factor * 2);
        filter.kernelSize = static_cast<uint32>(2 + 2 * factor);
        filter.downsampleWithAverage = false;
        filter.sharpenWithoutColorShift = false;
        filter.borderColorBlack = true;
    }

    return filter;
}

} // namespace build_kernel

} // namespace rocks::texture_build_detail

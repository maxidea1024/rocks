#include "TextureBuildSteps.h"

#include "TextureBlockParallel.h"

#include <Base/NumericLimits.h>
#include <Platform/Memory.h>

#include <math.h>

// 텍스처 빌드의 mip 생성이다(Docs/Design/AssetSystem 12.5절 mip 규칙, 12.18 TP1 — UE 5.8
// TextureCompressorModule.cpp GenerateMipChain 1799–2098과 그 아래 mip 함수 391–1243을 같은 식과
// 연산 순서로 옮겼다). 작업 평면은 Level의 (layer, z) 평면을 차례로 센 번호다(Volume은 깊이 slice,
// 그 밖은 layer — UE FImage slice와 같은 배치). 곱과 합은 따로 써서 FMA로 합쳐지지 않게 한다.
namespace rocks::texture_build_detail {

namespace build_mips {

namespace {

using build_kernel::MipKernel;

// UE GenerateSharpenedMipB8G8R8A8의 ScaleFactor다(mip 생성은 늘 2).
constexpr uint32 MipScaleFactor = 2;
// UE ComputeAlphaScale 이분 탐색 횟수·범위다(TCM 550–605).
constexpr uint32 AlphaScaleSteps = 8;
constexpr float AlphaScaleMaximum = 4.0f;
constexpr float KindaSmallNumber = 1.e-4f;
// 색 이동 보정의 휘도 가중치·문턱이다(UE FLinearColor::GetLuminance — TCM 812–830).
constexpr float LuminanceRed = 0.3f;
constexpr float LuminanceGreen = 0.59f;
constexpr float LuminanceBlue = 0.11f;
constexpr float LuminanceMinimum = 0.001f;

// 행 batch의 최소 texel 수다(12.18 TP1b — UE NormalizeMipChain이 작은 mip을 한 작업으로 묶는 기준,
// TCM 4670–4677).
constexpr uint64 MipMinBatchTexels = 16384;

// 행 rows개를 batch로 나눈 계획이다(batch마다 최소 MipMinBatchTexels texel).
struct RowPlan {
    uint64 rows = 0;
    uint64 texelsPerRow = 1;
    uint64 rowsPerBatch = 1;
    uint64 batches = 0;
};

RowPlan PlanRows(uint64 rows, uint64 texelsPerRow) noexcept {
    RowPlan plan;
    plan.rows = rows;
    plan.texelsPerRow = texelsPerRow == 0 ? 1 : texelsPerRow;
    plan.rowsPerBatch = (MipMinBatchTexels + plan.texelsPerRow - 1) / plan.texelsPerRow;
    plan.rowsPerBatch = plan.rowsPerBatch == 0 ? 1 : plan.rowsPerBatch;
    plan.batches = (rows + plan.rowsPerBatch - 1) / plan.rowsPerBatch;
    return plan;
}

// 계획대로 행 [begin, end)를 body로 실행한다(압축 단계 RunBlockRows — batch마다 결정적 FP,
// scheduler가 없으면 호출 스레드 혼자). batch 번호는 begin / rowsPerBatch다.
Result<void> RunRows(const TextureBuildExecution& execution, const RowPlan& plan, void* context,
                     texture_block_detail::BlockRowBody body) noexcept {
    TextureBlockOptions options;
    options.scheduler = execution.scheduler;
    options.scratch = execution.scratch;
    options.minBatchBlocks = static_cast<uint32>(plan.rowsPerBatch * plan.texelsPerRow);
    return texture_block_detail::RunBlockRows(options, plan.rows, plan.texelsPerRow, 0, context,
                                              body);
}

// 한 사슬이 되풀이해 쓰는 작업 버퍼다(batch별 분리 커널 행 버퍼·coverage 개수, Volume 두 번째 slice
// 결과 평면 — 미리 할당해 worker 안에서는 할당하지 않는다).
struct MipScratch {
    explicit MipScratch(Allocator& allocator) noexcept
        : rows(allocator), counts(allocator), second(allocator) {
    }

    Array<float> rows;
    Array<int64> counts;
    Array<float> second;
};

// 한 평면의 보기다(UE FImageView2D).
struct PlaneView {
    const float* texels = nullptr;
    int64 width = 0;
    int64 height = 0;

    const float* At(int64 x, int64 y) const noexcept {
        return texels + (x + y * width) * static_cast<int64>(Channels);
    }
};

struct MutablePlane {
    float* texels = nullptr;
    int64 width = 0;
    int64 height = 0;

    float* At(int64 x, int64 y) const noexcept {
        return texels + (x + y * width) * static_cast<int64>(Channels);
    }
};

usize PlaneCount(const Level& level) noexcept {
    return usize{level.layers} * level.depth;
}

PlaneView ViewOf(const Level& level, usize plane) noexcept {
    return {level.texels.Data() + plane * level.PlaneTexels() * Channels, level.width,
            level.height};
}

MutablePlane MutableViewOf(Level& level, usize plane) noexcept {
    return {level.texels.Data() + plane * level.PlaneTexels() * Channels, level.width,
            level.height};
}

bool IsPowerOfTwo(int64 value) noexcept {
    return value > 0 && (value & (value - 1)) == 0;
}

int64 ClampIndex(int64 value, int64 maximum) noexcept {
    return value < 0 ? 0 : (value < maximum ? value : maximum);
}

constexpr float BorderBlack[Channels] = {0.0f, 0.0f, 0.0f, 0.0f};

// UE LookupSourceMip(2D — TCM 103–138).
const float* Lookup(const PlaneView& view, MipAddress address, int64 x, int64 y) noexcept {
    if (address == MipAddress::Wrap) {
        x = static_cast<int64>(static_cast<uint32>(x) & static_cast<uint32>(view.width - 1));
        y = static_cast<int64>(static_cast<uint32>(y) & static_cast<uint32>(view.height - 1));
    } else if (address == MipAddress::Clamp) {
        x = ClampIndex(x, view.width - 1);
        y = ClampIndex(y, view.height - 1);
    } else if (static_cast<uint32>(x) >= static_cast<uint32>(view.width) ||
               static_cast<uint32>(y) >= static_cast<uint32>(view.height)) {
        return BorderBlack;
    }

    return view.At(x, y);
}

// UE LookupSourceMip(1D — TCM 140–171).
const float* Lookup1D(const float* row, int64 size, MipAddress address, int64 x) noexcept {
    if (address == MipAddress::Wrap) {
        x = static_cast<int64>(static_cast<uint32>(x) & static_cast<uint32>(size - 1));
    } else if (address == MipAddress::Clamp) {
        x = ClampIndex(x, size - 1);
    } else if (static_cast<uint32>(x) >= static_cast<uint32>(size)) {
        return BorderBlack;
    }

    return row + x * static_cast<int64>(Channels);
}

// c = c + w × s (UE VectorAdd(Color, VectorMultiply(Weight, Sample))).
void Accumulate(float* color, float weight, const float* sample) noexcept {
    for (usize channel = 0; channel < Channels; ++channel) {
        const float product = weight * sample[channel];
        color[channel] = color[channel] + product;
    }
}

// ((A + B) + C) + D) × 0.25다.
void Average4(const float* a, const float* b, const float* c, const float* d, float* out) noexcept {
    for (usize channel = 0; channel < Channels; ++channel) {
        const float ab = a[channel] + b[channel];
        const float abc = ab + c[channel];
        const float sum = abc + d[channel];
        out[channel] = sum * 0.25f;
    }
}

// UE DetermineScaledThreshold(TCM 391–447): x × scale ≥ threshold와 같은 x ≥ 결과인 가장 작은
// float.
float NextTowardZero(float value) noexcept {
    return platform::BitCast<float>(platform::BitCast<uint32>(value) - 1u);
}

float NextAwayFromZero(float value) noexcept {
    return platform::BitCast<float>(platform::BitCast<uint32>(value) + 1u);
}

float ScaledThreshold(float threshold, float scale) noexcept {
    float scaled = threshold / scale;
    const float down = NextTowardZero(scaled);
    const float downProduct = down * scale;
    const float product = scaled * scale;
    if (downProduct >= threshold) {
        scaled = down;
    } else if (product < threshold) {
        scaled = NextAwayFromZero(scaled);
    }

    return scaled;
}

struct CoverageJob {
    const PlaneView* view;
    const float* scaled;
    bool alphaOnly;
    int64* counts;
    uint64 rowsPerBatch;
};

void CoverageRows(void* context, uint64 begin, uint64 end) noexcept {
    const CoverageJob& job = *static_cast<const CoverageJob*>(context);
    int64* counts = job.counts + (begin / job.rowsPerBatch) * Channels;
    for (int64 y = static_cast<int64>(begin); y < static_cast<int64>(end); ++y) {
        for (int64 x = 0; x < job.view->width; ++x) {
            const float* texel = job.view->At(x, y);
            for (usize channel = job.alphaOnly ? Alpha : 0; channel < Channels; ++channel) {
                counts[channel] += texel[channel] >= job.scaled[channel] ? 1 : 0;
            }
        }
    }
}

// UE ComputeAlphaCoverage(TCM 449–548): 채널마다 값 ≥ 비교 임계값인 texel 비율(임계값 0 채널은 0).
// 행 batch마다 센 정수를 batch 순서로 더한다(분할과 무관).
Result<void> AlphaCoverage(const float* thresholds, const float* scales, const PlaneView& view,
                           const TextureBuildExecution& execution, MipScratch& scratch,
                           float* coverage) noexcept {
    float scaled[Channels];
    for (usize channel = 0; channel < Channels; ++channel) {
        scaled[channel] = thresholds[channel] == 0.0f
                              ? NumericLimits<float>::Max()
                              : ScaledThreshold(thresholds[channel], scales[channel]);
    }

    // 알파 임계값만 있으면 UE는 알파만 센다(TCM 461–486 — 다른 채널은 0, 무한대 texel도 세지 않음).
    const bool alphaOnly =
        thresholds[Red] == 0.0f && thresholds[Green] == 0.0f && thresholds[Blue] == 0.0f;
    const RowPlan plan =
        PlanRows(static_cast<uint64>(view.height), static_cast<uint64>(view.width));
    if (auto sized = scratch.counts.TryResize(static_cast<usize>(plan.batches) * Channels);
        !sized) {
        return sized;
    }

    for (int64& count : scratch.counts) {
        count = 0;
    }

    CoverageJob job{&view, scaled, alphaOnly, scratch.counts.Data(), plan.rowsPerBatch};
    if (auto ran = RunRows(execution, plan, &job, &CoverageRows); !ran) {
        return ran;
    }

    int64 counts[Channels] = {};
    for (usize batch = 0; batch < plan.batches; ++batch) {
        for (usize channel = 0; channel < Channels; ++channel) {
            counts[channel] += scratch.counts[batch * Channels + channel];
        }
    }

    const float texels = static_cast<float>(view.width * view.height);
    for (usize channel = 0; channel < Channels; ++channel) {
        coverage[channel] = static_cast<float>(counts[channel]) / texels;
    }

    return Result<void>::Success();
}

// UE ComputeAlphaScale(TCM 550–605): 8단계 이분 탐색(채널별 건너뜀은 엄격 <, 끝 판정은 Equals <=).
Result<void> AlphaScale(const float* coverages, const float* thresholds, const PlaneView& view,
                        const TextureBuildExecution& execution, MipScratch& scratch,
                        float* scales) noexcept {
    float minimum[Channels] = {0.0f, 0.0f, 0.0f, 0.0f};
    float maximum[Channels] = {AlphaScaleMaximum, AlphaScaleMaximum, AlphaScaleMaximum,
                               AlphaScaleMaximum};
    for (usize channel = 0; channel < Channels; ++channel) {
        scales[channel] = 1.0f;
    }

    for (uint32 step = 0; step < AlphaScaleSteps; ++step) {
        float computed[Channels];
        if (auto counted = AlphaCoverage(thresholds, scales, view, execution, scratch, computed);
            !counted) {
            return counted;
        }

        for (usize channel = 0; channel < Channels; ++channel) {
            const float difference = computed[channel] - coverages[channel];
            if (thresholds[channel] == 0.0f || fabsf(difference) < KindaSmallNumber) {
                continue;
            }

            if (computed[channel] < coverages[channel]) {
                minimum[channel] = scales[channel];
            } else if (computed[channel] > coverages[channel]) {
                maximum[channel] = scales[channel];
            }

            const float sum = minimum[channel] + maximum[channel];
            scales[channel] = sum * 0.5f;
        }

        bool equal = true;
        for (usize channel = 0; channel < Channels; ++channel) {
            const float difference = computed[channel] - coverages[channel];
            equal = equal && fabsf(difference) <= KindaSmallNumber;
        }

        if (equal) {
            break;
        }
    }

    return Result<void>::Success();
}

void Scale(float* color, const float* scales) noexcept {
    for (usize channel = 0; channel < Channels; ++channel) {
        color[channel] = color[channel] * scales[channel];
    }
}

// 한 생성(UE GenerateSharpenedMipB8G8R8A8Templ — TCM 937–1031)의 입력이다.
struct MipStep {
    MipAddress address = MipAddress::Clamp;
    const MipKernel* kernel = nullptr;
    bool coverage = false;
    const float* coverages = nullptr;
    const float* thresholds = nullptr;
    bool sharpenWithoutColorShift = false;
    bool unfiltered = false;
    bool useNewMipFilter = true;
};

// 분리 커널 mip(UE GenerateMipSharpenedSeparable — TCM 855–935). row는 원본 너비의 행 버퍼다.
void SeparableMip(const PlaneView& source, const MutablePlane& dest, const MipStep& step,
                  const float* scales, float* row, int64 rowBegin, int64 rowEnd) noexcept {
    const int64 size = step.kernel->size;
    const int64 center = (size - 1) / 2;
    for (int64 destY = rowBegin; destY < rowEnd; ++destY) {
        const int64 sourceY = destY * MipScaleFactor;
        for (int64 sourceX = 0; sourceX < source.width; ++sourceX) {
            float color[Channels] = {};
            for (int64 ky = 0; ky < size; ++ky) {
                const float* sample = Lookup(source, step.address, sourceX, sourceY + ky - center);
                Accumulate(color, step.kernel->weights1D[ky], sample);
            }

            float* stored = row + sourceX * static_cast<int64>(Channels);
            for (usize channel = 0; channel < Channels; ++channel) {
                stored[channel] = color[channel];
            }
        }

        for (int64 destX = 0; destX < dest.width; ++destX) {
            const int64 sourceX = destX * MipScaleFactor;
            float color[Channels] = {};
            for (int64 kx = 0; kx < size; ++kx) {
                const float* sample =
                    Lookup1D(row, source.width, step.address, sourceX + kx - center);
                Accumulate(color, step.kernel->weights1D[kx], sample);
            }

            Scale(color, scales);
            float* target = dest.At(destX, destY);
            for (usize channel = 0; channel < Channels; ++channel) {
                target[channel] = color[channel];
            }
        }
    }
}

float Luminance(const float* color) noexcept {
    const float red = color[Red] * LuminanceRed;
    const float green = color[Green] * LuminanceGreen;
    const float blue = color[Blue] * LuminanceBlue;
    const float redGreen = red + green;
    return redGreen + blue;
}

// 2D 커널 mip(UE GenerateMipSharpened — TCM 763–852), 색 이동 보정 포함.
void SharpenedMip(const PlaneView& source, const MutablePlane& dest, const MipStep& step,
                  const float* scales, int64 rowBegin, int64 rowEnd) noexcept {
    const int64 size = step.kernel->size;
    const int64 center = (size - 1) / 2;
    for (int64 destY = rowBegin; destY < rowEnd; ++destY) {
        for (int64 destX = 0; destX < dest.width; ++destX) {
            const int64 sourceX = destX * MipScaleFactor;
            const int64 sourceY = destY * MipScaleFactor;
            float color[Channels] = {};
            for (int64 ky = 0; ky < size; ++ky) {
                for (int64 kx = 0; kx < size; ++kx) {
                    const float* sample =
                        Lookup(source, step.address, sourceX + kx - center, sourceY + ky - center);
                    Accumulate(color, step.kernel->weights2D[kx + ky * size], sample);
                }
            }

            float* target = dest.At(destX, destY);
            if (step.sharpenWithoutColorShift) {
                const float newLuminance = Luminance(color);
                float filtered[Channels];
                Average4(Lookup(source, step.address, sourceX, sourceY),
                         Lookup(source, step.address, sourceX + 1, sourceY),
                         Lookup(source, step.address, sourceX, sourceY + 1),
                         Lookup(source, step.address, sourceX + 1, sourceY + 1), filtered);
                const float oldLuminance = Luminance(filtered);
                if (oldLuminance > LuminanceMinimum) {
                    const float ratio = newLuminance / oldLuminance;
                    for (usize channel = 0; channel < Channels; ++channel) {
                        filtered[channel] = filtered[channel] * ratio;
                    }
                }

                filtered[Alpha] = color[Alpha];
                Scale(filtered, scales);
                for (usize channel = 0; channel < Channels; ++channel) {
                    target[channel] = filtered[channel];
                }
            } else {
                Scale(color, scales);
                for (usize channel = 0; channel < Channels; ++channel) {
                    target[channel] = color[channel];
                }
            }
        }
    }
}

// 2×2 Simple 경로인지다(UE GenerateSharpenedMipB8G8R8A8Templ 966–975 — alpha scale을 구하기 전).
bool IsSimple(const PlaneView& source, const MutablePlane& dest, const MipStep& step) noexcept {
    return step.kernel->size == 2 && dest.width * 2 == source.width &&
           dest.height * 2 == source.height && !step.coverage && !step.unfiltered;
}

// 한 평면의 대상 행 [rowBegin, rowEnd)를 만든다(UE GenerateSharpenedMipB8G8R8A8Templ — scales는
// 미리 구했다).
void GenerateRows(const PlaneView& source, const MutablePlane& dest, const MipStep& step,
                  const float* scales, float* row, int64 rowBegin, int64 rowEnd) noexcept {
    const uint32 size = step.kernel->size;
    if (IsSimple(source, dest, step)) {
        // UE GenerateMip2x2Simple(색 이동 보정 무시).
        for (int64 y = rowBegin; y < rowEnd; ++y) {
            for (int64 x = 0; x < dest.width; ++x) {
                Average4(source.At(2 * x, 2 * y), source.At(2 * x + 1, 2 * y),
                         source.At(2 * x, 2 * y + 1), source.At(2 * x + 1, 2 * y + 1),
                         dest.At(x, y));
            }
        }

        return;
    }

    if (step.unfiltered) {
        // UE GenerateMipUnfiltered.
        for (int64 y = rowBegin; y < rowEnd; ++y) {
            for (int64 x = 0; x < dest.width; ++x) {
                const float* sample =
                    Lookup(source, step.address, x * MipScaleFactor, y * MipScaleFactor);
                float* target = dest.At(x, y);
                for (usize channel = 0; channel < Channels; ++channel) {
                    target[channel] = sample[channel] * scales[channel];
                }
            }
        }

        return;
    }

    if (size == 2) {
        // UE GenerateMip2x2.
        for (int64 y = rowBegin; y < rowEnd; ++y) {
            for (int64 x = 0; x < dest.width; ++x) {
                const int64 sx = x * MipScaleFactor;
                const int64 sy = y * MipScaleFactor;
                float* target = dest.At(x, y);
                Average4(Lookup(source, step.address, sx, sy),
                         Lookup(source, step.address, sx + 1, sy),
                         Lookup(source, step.address, sx, sy + 1),
                         Lookup(source, step.address, sx + 1, sy + 1), target);
                Scale(target, scales);
            }
        }

        return;
    }

    if (step.useNewMipFilter) {
        SeparableMip(source, dest, step, scales, row, rowBegin, rowEnd);
    } else {
        SharpenedMip(source, dest, step, scales, rowBegin, rowEnd);
    }
}

struct StepJob {
    PlaneView source;
    MutablePlane dest;
    const MipStep* step;
    const float* scales;
    float* rows;
    usize rowFloats;
    uint64 rowsPerBatch;
};

void StepRows(void* context, uint64 begin, uint64 end) noexcept {
    const StepJob& job = *static_cast<const StepJob*>(context);
    float* row =
        job.rows == nullptr ? nullptr : job.rows + (begin / job.rowsPerBatch) * job.rowFloats;
    GenerateRows(job.source, job.dest, *job.step, job.scales, row, static_cast<int64>(begin),
                 static_cast<int64>(end));
}

// 한 평면 생성(UE GenerateSharpenedMipB8G8R8A8Templ): alpha scale을 구한 뒤 대상 행을 batch로 나눠
// 만든다(12.18 TP1b). 오류: 할당 실패, 병렬 실행 오류.
Result<void> GenerateStep(const PlaneView& source, const MutablePlane& dest, const MipStep& step,
                          const TextureBuildExecution& execution, MipScratch& scratch) noexcept {
    float scales[Channels] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (!IsSimple(source, dest, step) && step.coverage) {
        if (auto scaled =
                AlphaScale(step.coverages, step.thresholds, source, execution, scratch, scales);
            !scaled) {
            return scaled;
        }
    }

    const RowPlan plan =
        PlanRows(static_cast<uint64>(dest.height), static_cast<uint64>(dest.width));
    const bool separable = !IsSimple(source, dest, step) && !step.unfiltered &&
                           step.kernel->size != 2 && step.useNewMipFilter;
    const usize rowFloats = static_cast<usize>(source.width) * Channels;
    if (separable) {
        if (auto sized = scratch.rows.TryResize(static_cast<usize>(plan.batches) * rowFloats);
            !sized) {
            return sized;
        }
    }

    StepJob job{
        source,           dest, &step, scales, separable ? scratch.rows.Data() : nullptr, rowFloats,
        plan.rowsPerBatch};
    return RunRows(execution, plan, &job, &StepRows);
}

// UE GenerateMipBorder(TCM 1110–1178): 테두리 texel을 직전 mip 2×2 발자국 안의 원본 테두리 texel
// 평균으로 다시 쓴다(합에 1/개수를 곱함 — FLinearColor::operator/=, 원본이 2의 거듭제곱이면 Wrap
// 조회, 아니면 Clamp).
void RedrawBorder(const PlaneView& source, const MutablePlane& dest) noexcept {
    const bool pow2 = IsPowerOfTwo(source.width) && IsPowerOfTwo(source.height);
    const MipAddress address = pow2 ? MipAddress::Wrap : MipAddress::Clamp;
    for (int64 y = 0; y < dest.height; ++y) {
        for (int64 x = 0; x < dest.width; ++x) {
            if (x != 0 && y != 0 && x + 1 != dest.width && y + 1 != dest.height) {
                continue;
            }

            float sum[Channels] = {};
            float weight = 0.0f;
            for (int64 ky = 0; ky < 2; ++ky) {
                for (int64 kx = 0; kx < 2; ++kx) {
                    const int64 sx = x * 2 + kx;
                    const int64 sy = y * 2 + ky;
                    if (sx == 0 || sx == source.width - 1 || sy == 0 || sy == source.height - 1) {
                        const float* sample = Lookup(source, address, sx, sy);
                        for (usize channel = 0; channel < Channels; ++channel) {
                            sum[channel] = sum[channel] + sample[channel];
                        }

                        weight = weight + 1.0f;
                    }
                }
            }

            const float inverse = 1.0f / weight;
            float* target = dest.At(x, y);
            for (usize channel = 0; channel < Channels; ++channel) {
                target[channel] = sum[channel] * inverse;
            }
        }
    }
}

bool IsLast(uint32 width, uint32 height, uint32 planes, bool volume) noexcept {
    return width == 1 && height == 1 && (!volume || planes == 1);
}

} // namespace

uint32 Half(uint32 n) noexcept {
    return n / 2 > 0 ? n / 2 : 1;
}

MipChainParams MipChainParamsFor(const TextureBuildSettings& settings) noexcept {
    MipChainParams params;
    params.filter = build_kernel::MipFilterFor(settings.mipGenSettings, settings.compression);
    params.volume = settings.dimension == TextureDimension::Volume;
    params.cube = settings.dimension == TextureDimension::TextureCube ||
                  settings.dimension == TextureDimension::TextureCubeArray;
    params.unfiltered = settings.mipGenSettings == TextureMipGenSettings::Unfiltered;
    params.useNewMipFilter = settings.useNewMipFilter;
    params.preserveBorder = settings.preserveBorder;
    params.addressX = settings.addressX;
    params.addressY = settings.addressY;
    params.alphaCoverage = settings.alphaCoverage;
    params.thresholds[0] = settings.alphaCoverageThresholds.x;
    params.thresholds[1] = settings.alphaCoverageThresholds.y;
    params.thresholds[2] = settings.alphaCoverageThresholds.z;
    params.thresholds[3] = settings.alphaCoverageThresholds.w;
    return params;
}

MipAddress MipAddressFor(const MipChainParams& params, uint32 width, uint32 height) noexcept {
    if (params.preserveBorder) {
        return params.filter.borderColorBlack ? MipAddress::BorderBlack : MipAddress::Clamp;
    }

    if (params.cube && params.useNewMipFilter) {
        return MipAddress::Clamp;
    }

    if (!IsPowerOfTwo(width) || !IsPowerOfTwo(height)) {
        return MipAddress::Clamp;
    }

    if (params.useNewMipFilter && params.addressX != TextureAddress::Wrap &&
        params.addressY != TextureAddress::Wrap) {
        return MipAddress::Clamp;
    }

    return MipAddress::Wrap;
}

uint32 MipChainLength(const MipChainParams& params, uint32 width, uint32 height,
                      uint32 planes) noexcept {
    uint32 count = 0;
    while (!IsLast(width, height, planes, params.volume)) {
        width = Half(width);
        height = Half(height);
        planes = params.volume ? Half(planes) : planes;
        ++count;
    }

    return count;
}

Result<void> GenerateMipChain(Allocator& allocator, const MipChainParams& params, const Level& base,
                              uint32 depth, Array<Level>& out,
                              const TextureBuildExecution& execution) noexcept {
    const uint32 basePlanes = static_cast<uint32>(PlaneCount(base));
    uint32 remaining = MipChainLength(params, base.width, base.height, basePlanes);
    remaining = remaining < depth ? remaining : depth;
    if (remaining == 0) {
        return Result<void>::Success();
    }

    // base가 out 안에 있을 수 있으므로 늘리지 않는다 — 호출자가 자리를 마련한다.
    if (out.Capacity() - out.Size() < remaining) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const MipKernel average = build_kernel::BuildMipKernel(2, 0.0f);
    const MipKernel downsample =
        build_kernel::BuildMipKernel(params.filter.kernelSize, params.filter.sharpen);
    // 커널이 2×2면 평균 사슬을 따로 두지 않는다(UE 1831–1838).
    const bool withAverage = params.filter.downsampleWithAverage && params.filter.kernelSize != 2;
    const MipAddress address = MipAddressFor(params, base.width, base.height);
    const bool redrawBorder = params.preserveBorder && !params.filter.borderColorBlack;

    MipScratch scratch(allocator);
    // 목표 coverage는 base의 평면 0에서 한 번 구한다(UE 1898–1907).
    float coverages[Channels] = {};
    if (params.alphaCoverage) {
        const float ones[Channels] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (auto counted = AlphaCoverage(params.thresholds, ones, ViewOf(base, 0), execution,
                                         scratch, coverages);
            !counted) {
            return counted;
        }
    }

    MipStep step;
    step.address = address;
    step.coverage = params.alphaCoverage;
    step.coverages = coverages;
    step.thresholds = params.thresholds;
    step.sharpenWithoutColorShift = params.filter.sharpenWithoutColorShift;
    step.unfiltered = params.unfiltered;
    step.useNewMipFilter = params.useNewMipFilter;

    Level averages[2] = {Level(allocator), Level(allocator)};
    usize nextAverage = 0;
    const Level* source = &base;
    for (uint32 made = 0; made < remaining; ++made) {
        const uint32 planes = static_cast<uint32>(PlaneCount(*source));
        const uint32 width = Half(source->width);
        const uint32 height = Half(source->height);
        const uint32 destPlanes = params.volume ? Half(planes) : planes;
        const uint32 layers = params.volume ? 1 : source->layers;
        const uint32 depthOut = params.volume ? destPlanes : 1;
        Level dest(allocator);
        if (auto made2 = dest.Allocate(width, height, depthOut, layers); !made2) {
            return made2;
        }

        Level* averageDest = withAverage ? &averages[nextAverage] : nullptr;
        if (averageDest != nullptr) {
            if (auto made2 = averageDest->Allocate(width, height, depthOut, layers); !made2) {
                return made2;
            }
        }

        if (params.volume) {
            if (auto sized = scratch.second.TryResize(usize{width} * height * Channels); !sized) {
                return sized;
            }
        }

        for (uint32 plane = 0; plane < destPlanes; ++plane) {
            const usize sourcePlane = params.volume ? usize{plane} * 2 : plane;
            const PlaneView view = ViewOf(*source, sourcePlane);
            auto make = [&](Level& target, const MipKernel& kernel) noexcept -> Result<void> {
                MipStep current = step;
                current.kernel = &kernel;
                const MutablePlane destView = MutableViewOf(target, plane);
                if (auto made = GenerateStep(view, destView, current, execution, scratch); !made) {
                    return made;
                }

                if (params.volume && !params.unfiltered) {
                    const usize nextPlane =
                        sourcePlane + 1 < planes ? sourcePlane + 1 : usize{planes} - 1;
                    const MutablePlane other{scratch.second.Data(), width, height};
                    if (auto made = GenerateStep(ViewOf(*source, nextPlane), other, current,
                                                 execution, scratch);
                        !made) {
                        return made;
                    }

                    const usize count = usize{width} * height * Channels;
                    for (usize index = 0; index < count; ++index) {
                        const float sum = destView.texels[index] + scratch.second[index];
                        destView.texels[index] = sum * 0.5f;
                    }
                }

                if (redrawBorder) {
                    RedrawBorder(view, destView);
                }

                return Result<void>::Success();
            };
            if (auto made = make(dest, downsample); !made) {
                return made;
            }

            if (averageDest != nullptr) {
                if (auto made = make(*averageDest, average); !made) {
                    return made;
                }
            }
        }

        (void)out.TryPushBack(rocks::Move(dest));
        if (averageDest != nullptr) {
            source = averageDest;
            nextAverage ^= 1;
        } else {
            source = &out[out.Size() - 1];
        }
    }

    return Result<void>::Success();
}

// 법선 재정규화다(UE NormalizeMip — float v × 2 − 1을 double로, 제곱합 1은 그대로, 1e−8 미만은
// (0, 0, 1), 그 밖 1 / sqrt(제곱합)을 곱해 float(v × 0.5 + 0.5)로, 알파는 그대로).
constexpr double NormalSmallNumber = 1.0e-8;

namespace {

void NormalizeTexels(float* texels, usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        float* texel = texels + index * Channels;
        double v[3];
        for (usize channel = 0; channel < 3; ++channel) {
            const float scaled = texel[channel] * 2.0f;
            v[channel] = static_cast<double>(scaled - 1.0f);
        }

        const double xx = v[0] * v[0];
        const double yy = v[1] * v[1];
        const double zz = v[2] * v[2];
        const double square = xx + yy + zz;
        if (square == 1.0) {
            // 그대로(UE GetSafeNormal).
        } else if (square < NormalSmallNumber) {
            v[0] = 0.0;
            v[1] = 0.0;
            v[2] = 1.0;
        } else {
            const double scale = 1.0 / sqrt(square);
            for (double& component : v) {
                component *= scale;
            }
        }

        for (usize channel = 0; channel < 3; ++channel) {
            const double halved = v[channel] * 0.5;
            texel[channel] = static_cast<float>(halved + 0.5);
        }
    }
}

struct NormalizeJob {
    float* texels;
    usize width;
};

void NormalizeRows(void* context, uint64 begin, uint64 end) noexcept {
    const NormalizeJob& job = *static_cast<const NormalizeJob*>(context);
    NormalizeTexels(job.texels + static_cast<usize>(begin) * job.width * Channels,
                    static_cast<usize>(end - begin) * job.width);
}

} // namespace

Result<void> NormalizeNormals(Level& level, const TextureBuildExecution& execution) noexcept {
    // (평면, 행) 단위 batch다(텍스셀마다 독립 — 12.18 TP1b).
    const RowPlan plan = PlanRows(uint64{level.layers} * level.depth * level.height, level.width);
    NormalizeJob job{level.texels.Data(), level.width};
    return RunRows(execution, plan, &job, &NormalizeRows);
}

} // namespace build_mips

} // namespace rocks::texture_build_detail

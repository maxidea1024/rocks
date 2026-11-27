#include "BlockGoldenImages.h"
#include "Speed.h"

#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Clock.h>
#include <Platform/Thread.h>
#include <Task/WorkerScheduler.h>
#include <Texture/TextureBuildSettings.h>
#include <TextureTools/TextureBlockEncode.h>
#include <TextureTools/TextureBuild.h>

#include <stdio.h>

// 텍스처 압축 속도 측정이다(Docs/Design/AssetSystem 12.17.9 6 — TC5b). 인코더 형식마다 직렬·병렬
// blocks/s와 BC 최소 batch 후보별 시간, 그리고 BuildTexture의 준비 단계 몫(비압축 출력)과 형식별
// 전체 시간을 잰다. 결과는 표로 출력하고 판정하지 않는다(스펙에 기록한다).
namespace rocks::texture_codec_speed {

namespace {

namespace golden = test::block_golden;

constexpr uint32 Channels = 4;
// 인코더 측정 그림 한 변이다(1024² — BC 65536블록, ASTC 6×6 29241블록).
constexpr uint32 EncoderSize = 1024;
// BuildTexture 측정 그림 한 변이다(2048² — mip 사슬 포함).
constexpr uint32 BuildSize = 2048;
// EAC 판단 그림 한 변이다(12.17.9 6 — 잡음 4096² Alpha가 압축 scheduler로 1분 안인지).
constexpr uint32 EacJudgeSize = 4096;
constexpr double NanosecondsPerSecond = 1e9;

double SecondsSince(uint64 start) noexcept {
    return static_cast<double>(platform::MonotonicNanoseconds() - start) / NanosecondsPerSecond;
}

// 값 잡음 RGBA8 그림이다(BlockGoldenImages 생성기를 큰 크기로).
bool Noise(uint32 size, uint32 seed, Array<uint8>& out) noexcept {
    if (!out.TryResize(usize{size} * size * Channels)) {
        return false;
    }

    const golden::Case one{"speed", golden::ValueNoise, size, size, seed};
    golden::Generate(one, out.Data());
    return true;
}

// format 입력으로 바꾼다(BC6H는 RGBA16F half, EAC는 R16 — R·G byte).
bool InputFor(TextureBlockFormat format, const Array<uint8>& rgba, Array<uint8>& out) noexcept {
    const usize texels = rgba.Size() / Channels;
    const usize texelBytes = TextureBlockSourceTexelBytes(format);
    if (!out.TryResize(texels * texelBytes)) {
        return false;
    }

    for (usize i = 0; i < texels; ++i) {
        const uint8* texel = rgba.Data() + i * Channels;
        uint8* target = out.Data() + i * texelBytes;
        if (format == TextureBlockFormat::Bc6h) {
            for (uint32 c = 0; c < Channels; ++c) {
                const uint16 half = Float16::FromFloat(static_cast<float>(texel[c]) / 64.0f).bits;
                target[c * 2] = static_cast<uint8>(half & 0xFF);
                target[c * 2 + 1] = static_cast<uint8>(half >> 8);
            }
        } else if (format == TextureBlockFormat::EacR11) {
            target[0] = texel[1];
            target[1] = texel[0];
        } else {
            for (uint32 c = 0; c < Channels; ++c) {
                target[c] = texel[c];
            }
        }
    }

    return true;
}

struct FormatRow {
    const char* name;
    TextureBlockFormat format;
};

constexpr FormatRow Formats[] = {
    {"BC1", TextureBlockFormat::Bc1},
    {"BC3", TextureBlockFormat::Bc3},
    {"BC4", TextureBlockFormat::Bc4},
    {"BC5", TextureBlockFormat::Bc5},
    {"BC6H", TextureBlockFormat::Bc6h},
    {"BC7", TextureBlockFormat::Bc7},
    {"EAC R11", TextureBlockFormat::EacR11},
    {"ASTC 4x4", TextureBlockFormat::Astc4x4},
    {"ASTC 6x6", TextureBlockFormat::Astc6x6},
    {"ASTC 8x8", TextureBlockFormat::Astc8x8},
    {"ASTC 12x12", TextureBlockFormat::Astc12x12},
};

// 한 번 압축하고 걸린 초를 돌려준다(실패면 음수).
double EncodeOnce(TextureBlockFormat format, const Array<uint8>& input, uint32 size,
                  const TextureBlockOptions& options, Array<uint8>& out) noexcept {
    auto bytes = TextureBlockOutputBytes(format, size, size, 1);
    if (!bytes || !out.TryResize(static_cast<usize>(bytes.Value()))) {
        return -1.0;
    }

    TextureBlockImage image;
    image.rgba = input.View();
    image.sizeX = size;
    image.sizeY = size;
    const uint64 start = platform::MonotonicNanoseconds();
    if (!EncodeTextureBlocks(format, image, options, out.View())) {
        return -1.0;
    }

    return SecondsSince(start);
}

void MeasureEncoders(Allocator& allocator, WorkerScheduler& scheduler, uint32 workers) noexcept {
    Array<uint8> rgba(allocator);
    if (!Noise(EncoderSize, 7, rgba)) {
        printf("cannot make the encoder image\n");
        return;
    }

    printf("Encoders (%u x %u value noise, Release, blocks/s; parallel = %u workers + caller)\n",
           EncoderSize, EncoderSize, workers);
    printf("  %-11s %12s %12s %8s\n", "format", "serial", "parallel", "speedup");
    for (const FormatRow& row : Formats) {
        if ((row.format == TextureBlockFormat::Bc6h || row.format == TextureBlockFormat::Bc7) &&
            IspcBcEncoderIdentity().IsEmpty()) {
            continue;
        }

        Array<uint8> input(allocator);
        Array<uint8> out(allocator);
        if (!InputFor(row.format, rgba, input)) {
            continue;
        }

        const uint64 blocks =
            uint64{(EncoderSize + TextureBlockWidth(row.format) - 1) /
                   TextureBlockWidth(row.format)} *
            ((EncoderSize + TextureBlockHeight(row.format) - 1) / TextureBlockHeight(row.format));
        const double serial =
            EncodeOnce(row.format, input, EncoderSize, TextureBlockOptions{}, out);
        TextureBlockOptions options;
        options.scheduler = &scheduler;
        options.scratch = &allocator;
        const double parallel = EncodeOnce(row.format, input, EncoderSize, options, out);
        printf("  %-11s %12.0f %12.0f %7.2fx\n", row.name, static_cast<double>(blocks) / serial,
               static_cast<double>(blocks) / parallel, serial / parallel);
    }

    // BC 최소 batch 후보(12.17.9 2 — 기본 2048).
    printf("BC1 minimum batch candidates (parallel seconds)\n");
    Array<uint8> out(allocator);
    for (const uint32 batch : {128u, 512u, 2048u, 8192u, 32768u}) {
        TextureBlockOptions options;
        options.scheduler = &scheduler;
        options.scratch = &allocator;
        options.minBatchBlocks = batch;
        printf("  %6u blocks: %.4f s\n", batch,
               EncodeOnce(TextureBlockFormat::Bc1, rgba, EncoderSize, options, out));
    }
}

struct BuildRow {
    const char* name;
    TextureCompressionSettings compression;
    bool none;
    graphics::GraphicsPlatform platform;
};

double BuildOnce(Allocator& allocator, const TextureBuildSettings& settings,
                 const Array<uint8>& pixels, const TextureBuildExecution& execution) noexcept {
    TextureBuildResult result(allocator);
    const uint64 start = platform::MonotonicNanoseconds();
    if (!BuildTexture(allocator, settings, pixels.View(), result, execution)) {
        return -1.0;
    }

    return SecondsSince(start);
}

TextureBuildSettings BuildSettings(uint32 size, const BuildRow& row) noexcept {
    TextureBuildSettings settings;
    settings.format = TextureSourceFormat::RGBA8;
    settings.sizeX = size;
    settings.sizeY = size;
    settings.numSlices = 1;
    settings.compression = row.compression;
    settings.srgb = row.compression != TextureCompressionSettings::Alpha;
    settings.compressionNone = row.none;
    settings.mipGenSettings = TextureMipGenSettings::SimpleAverage;
    settings.cookPlatform = static_cast<uint8>(row.platform);
    return settings;
}

void MeasureBuilds(Allocator& allocator, WorkerScheduler& scheduler) noexcept {
    Array<uint8> pixels(allocator);
    if (!Noise(BuildSize, 11, pixels)) {
        printf("cannot make the build image\n");
        return;
    }

    using graphics::GraphicsPlatform;
    const BuildRow rows[] = {
        {"prepare only (uncompressed BGRA8)", TextureCompressionSettings::Default, true,
         GraphicsPlatform::Windows},
        {"Windows Default (BC1)", TextureCompressionSettings::Default, false,
         GraphicsPlatform::Windows},
        {"Windows BC7", TextureCompressionSettings::BC7, false, GraphicsPlatform::Windows},
        {"Android Default (ASTC 6x6)", TextureCompressionSettings::Default, false,
         GraphicsPlatform::Android},
        {"Android Alpha (EAC R11)", TextureCompressionSettings::Alpha, false,
         GraphicsPlatform::Android},
    };
    printf("BuildTexture (%u x %u value noise with mips, seconds)\n", BuildSize, BuildSize);
    printf("  %-34s %9s %9s\n", "case", "serial", "parallel");
    TextureBuildExecution parallel{&scheduler, &allocator};
    for (const BuildRow& row : rows) {
        if (row.compression == TextureCompressionSettings::BC7 &&
            IspcBcEncoderIdentity().IsEmpty()) {
            continue;
        }

        const TextureBuildSettings settings = BuildSettings(BuildSize, row);
        printf("  %-34s %9.3f %9.3f\n", row.name,
               BuildOnce(allocator, settings, pixels, TextureBuildExecution{}),
               BuildOnce(allocator, settings, pixels, parallel));
    }
}

void JudgeEac(Allocator& allocator, WorkerScheduler& scheduler) noexcept {
    Array<uint8> pixels(allocator);
    if (!pixels.TryResize(usize{EacJudgeSize} * EacJudgeSize * Channels)) {
        printf("cannot make the EAC image\n");
        return;
    }

    // 잡음(무작위 byte) — 가장 느린 경우다.
    golden::Random random(97);
    for (usize i = 0; i < pixels.Size(); ++i) {
        pixels[i] = random.Byte();
    }

    const BuildRow row{"Android Alpha", TextureCompressionSettings::Alpha, false,
                       graphics::GraphicsPlatform::Android};
    const TextureBuildSettings settings = BuildSettings(EacJudgeSize, row);
    const double seconds =
        BuildOnce(allocator, settings, pixels, TextureBuildExecution{&scheduler, &allocator});
    printf("EAC judgement: %u x %u noise Alpha (Android, mips) parallel %.1f s (limit 60 s)\n",
           EacJudgeSize, EacJudgeSize, seconds);
}

} // namespace

int RunSpeed(bool judgeEac) noexcept {
    SystemAllocator allocator;
    const uint32 logical = platform::LogicalProcessorCount();
    // asset-cook과 같은 worker 수다(min(논리 − 1, 64) — 12.17.9 4).
    uint32 workers = logical > 1 ? logical - 1 : 1;
    workers = workers < 64 ? workers : 64;
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = workers;
    if (!scheduler.Start(config)) {
        printf("cannot start the worker scheduler\n");
        return 1;
    }

    printf("logical processors %u\n", logical);
    MeasureEncoders(allocator, scheduler, workers);
    MeasureBuilds(allocator, scheduler);
    if (judgeEac) {
        JudgeEac(allocator, scheduler);
    }

    (void)scheduler.Shutdown();
    return 0;
}

} // namespace rocks::texture_codec_speed

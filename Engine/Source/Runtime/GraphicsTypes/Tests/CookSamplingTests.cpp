#include <GraphicsTypes/CookSampling.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>
#include <stdio.h>

using namespace rocks;
namespace cook_sampling_tests_file {

using namespace rocks::graphics;

namespace {

// 쿠킹 대상 조합이다(Docs/Design/PixelFormat 7.1절 형식 표의 열 순서).
struct Target {
    Backend backend;
    GraphicsPlatform platform;
};

constexpr Target Targets[] = {
    {Backend::D3D12, GraphicsPlatform::Windows},  {Backend::D3D11, GraphicsPlatform::Windows},
    {Backend::Vulkan, GraphicsPlatform::Windows}, {Backend::Vulkan, GraphicsPlatform::Android},
    {Backend::OpenGL, GraphicsPlatform::Android}, {Backend::Metal, GraphicsPlatform::Mac},
};

constexpr uint32 TargetCount = sizeof(Targets) / sizeof(Targets[0]);

// 7.1절 형식 표를 시험 쪽에 따로 옮긴 것이다. cells는 열마다 한 글자다: F = 샘플 + 선형
// 필터(API 최소), S = 샘플만(API 최소), E = 샘플 + 선형 필터(엔진 요구), - = 보장 없음. family는
// 대체 계열이다: L = 8비트 선형, R = 8비트 sRGB, H = 고정밀, - = 없음.
struct Expected {
    const char* name;
    const char* cells;
    char family;
};

constexpr Expected Table[] = {
    {"R8Unorm", "FFFFFF", 'L'},
    {"R8UnormSrgb", "------", 'R'},
    {"R8Uint", "------", '-'},
    {"R8Sint", "------", '-'},
    {"A8Unorm", "FFFFFF", 'L'},
    {"RG8Unorm", "FFFFFF", 'L'},
    {"RG8UnormSrgb", "------", 'R'},
    {"RG8Snorm", "FFFFFF", 'H'},
    {"RG8Uint", "------", '-'},
    {"RGB8Unorm", "----F-", 'L'},
    {"RGB8UnormSrgb", "----F-", 'R'},
    {"RGBA8Unorm", "FFFFFF", 'L'},
    {"RGBA8UnormSrgb", "FFFFFF", 'R'},
    {"RGBA8Snorm", "FFFFFF", 'H'},
    {"RGBA8Uint", "------", '-'},
    {"BGRA8Unorm", "FFFF-F", 'L'},
    {"BGRA8UnormSrgb", "FFFF-F", 'R'},
    {"R16Unorm", "FFE--F", 'H'},
    {"R16Uint", "------", '-'},
    {"R16Sint", "------", '-'},
    {"R16Float", "FFFFFF", 'H'},
    {"RG16Unorm", "FFE--F", 'H'},
    {"RG16Snorm", "FF---F", 'H'},
    {"RG16Uint", "------", '-'},
    {"RG16Sint", "------", '-'},
    {"RG16Float", "FFFFFF", 'H'},
    {"RGBA16Unorm", "FFE--F", 'H'},
    {"RGBA16Snorm", "FF---F", 'H'},
    {"RGBA16Uint", "------", '-'},
    {"RGBA16Sint", "------", '-'},
    {"RGBA16Float", "FFFFFF", 'H'},
    {"R32Uint", "------", '-'},
    {"R32Sint", "------", '-'},
    {"R32Float", "FFESSE", 'H'},
    {"RG32Uint", "------", '-'},
    {"RG32Float", "FFESSE", 'H'},
    {"RGB32Uint", "------", '-'},
    {"RGB32Sint", "------", '-'},
    {"RGB32Float", "----S-", 'H'},
    {"RGBA32Uint", "------", '-'},
    {"RGBA32Float", "FFESSE", 'H'},
    {"R64Uint", "------", '-'},
    {"R5G6B5Unorm", "FFFFF-", 'L'},
    {"B5G5R5A1Unorm", "FFFF--", 'L'},
    {"RGB10A2Unorm", "FFFFFF", 'H'},
    {"RG11B10Ufloat", "FFFFFF", 'H'},
    {"RGB9E5Ufloat", "FFFFFF", 'H'},
    {"D16Unorm", "------", '-'},
    {"D24Unorm", "------", '-'},
    {"D32Float", "------", '-'},
    {"D24UnormS8Uint", "------", '-'},
    {"D32FloatS8Uint", "------", '-'},
    {"BC1Unorm", "FFE--E", 'L'},
    {"BC1UnormSrgb", "FFE--E", 'R'},
    {"BC2Unorm", "FFE--E", 'L'},
    {"BC2UnormSrgb", "FFE--E", 'R'},
    {"BC3Unorm", "FFE--E", 'L'},
    {"BC3UnormSrgb", "FFE--E", 'R'},
    {"BC4Unorm", "FFE--E", 'L'},
    {"BC5Unorm", "FFE--E", 'L'},
    {"BC6HUfloat", "FFE--E", 'H'},
    {"BC7Unorm", "FFE--E", 'L'},
    {"BC7UnormSrgb", "FFE--E", 'R'},
    {"ETC2RGB8Unorm", "---EF-", 'L'},
    {"ETC2RGB8UnormSrgb", "---EF-", 'R'},
    {"ETC2RGBA8Unorm", "---EF-", 'L'},
    {"ETC2RGBA8UnormSrgb", "---EF-", 'R'},
    {"EACR11Unorm", "---EF-", 'L'},
    {"EACRG11Unorm", "---EF-", 'L'},
    {"ASTC4x4Unorm", "---EF-", 'L'},
    {"ASTC4x4UnormSrgb", "---EF-", 'R'},
    {"ASTC4x4Hdr", "------", 'H'},
    {"ASTC6x6Unorm", "---EF-", 'L'},
    {"ASTC6x6UnormSrgb", "---EF-", 'R'},
    {"ASTC6x6Hdr", "------", 'H'},
    {"ASTC8x8Unorm", "---EF-", 'L'},
    {"ASTC8x8UnormSrgb", "---EF-", 'R'},
    {"ASTC8x8Hdr", "------", 'H'},
    {"ASTC10x10Unorm", "---EF-", 'L'},
    {"ASTC10x10UnormSrgb", "---EF-", 'R'},
    {"ASTC10x10Hdr", "------", 'H'},
    {"ASTC12x12Unorm", "---EF-", 'L'},
    {"ASTC12x12UnormSrgb", "---EF-", 'R'},
    {"ASTC12x12Hdr", "------", 'H'},
    {"NV12", "------", '-'},
    {"P010", "------", '-'},
};

constexpr uint32 TableCount = sizeof(Table) / sizeof(Table[0]);

PixelFormat FormatOf(const Expected& row) noexcept {
    auto parsed = ParsePixelFormatName(StringView(row.name, platform::CStringLength(row.name)));
    return parsed ? parsed.Value() : PixelFormat::Unknown;
}

PixelFormatCaps CapsOf(char cell) noexcept {
    switch (cell) {
    case 'F':
    case 'E':
        return PixelFormatCaps::Sampled | PixelFormatCaps::Filterable;
    case 'S':
        return PixelFormatCaps::Sampled;
    default:
        return PixelFormatCaps::None;
    }
}

CookSamplingBasis BasisOf(char cell) noexcept {
    switch (cell) {
    case 'F':
    case 'S':
        return CookSamplingBasis::ApiMinimum;
    case 'E':
        return CookSamplingBasis::EngineRequirement;
    default:
        return CookSamplingBasis::None;
    }
}

bool IsTarget(Backend backend, GraphicsPlatform platform) noexcept {
    for (const Target& target : Targets) {
        if (target.backend == backend && target.platform == platform) {
            return true;
        }
    }

    return false;
}

// 모든 칸을 갖춘 장치 능력 배열이다.
struct FullCaps {
    PixelFormatCaps caps[MaxPixelFormatNumber + 1];

    FullCaps() noexcept {
        for (uint32 number = 0; number <= MaxPixelFormatNumber; ++number) {
            caps[number] = CookSamplingCaps;
        }
    }

    Span<const PixelFormatCaps> View() const noexcept {
        return Span<const PixelFormatCaps>(caps);
    }
};

// 각 조합에서 번호 순으로 첫 보장 형식이다.
PixelFormat FirstGuaranteed(const Target& target, uint32 fromNumber) noexcept {
    for (uint32 number = fromNumber; number <= MaxPixelFormatNumber; ++number) {
        const auto format = static_cast<PixelFormat>(number);
        if (CookSamplingGuarantee(target.backend, target.platform, format) !=
            PixelFormatCaps::None) {
            return format;
        }
    }

    return PixelFormat::Unknown;
}

} // namespace

TEST(CookSampling_TableCoversEveryPixelFormatOnce) {
    CHECK(TableCount == PixelFormatCount);
    for (const Expected& row : Table) {
        const PixelFormat format = FormatOf(row);
        CHECK(IsValidPixelFormat(format));
        CHECK(platform::CStringLength(row.cells) == TargetCount);
    }

    for (uint32 number = 1; number <= MaxPixelFormatNumber; ++number) {
        const auto format = static_cast<PixelFormat>(number);
        if (!IsValidPixelFormat(format)) {
            continue;
        }

        uint32 rows = 0;
        for (const Expected& row : Table) {
            rows += FormatOf(row) == format ? 1u : 0u;
        }

        CHECK(rows == 1);
    }
}

TEST(CookSampling_GuaranteeMatchesTheSpecTable) {
    for (const Expected& row : Table) {
        const PixelFormat format = FormatOf(row);
        for (uint32 column = 0; column < TargetCount; ++column) {
            const Target& target = Targets[column];
            const char cell = row.cells[column];
            const PixelFormatCaps caps =
                CookSamplingGuarantee(target.backend, target.platform, format);
            if (caps != CapsOf(cell)) {
                printf("  mismatch: %s on %s/%s\n", row.name, BackendName(target.backend),
                       GraphicsPlatformName(target.platform));
            }

            CHECK(caps == CapsOf(cell));
            CHECK(CookSamplingGuaranteeBasis(target.backend, target.platform, format) ==
                  BasisOf(cell));
            // 표의 값은 늘 CookSamplingCaps의 부분집합이고, 선형 필터는 샘플을 포함한다.
            CHECK(HasPixelFormatCaps(CookSamplingCaps, caps));
            CHECK(!HasPixelFormatCaps(caps, PixelFormatCaps::Filterable) ||
                  HasPixelFormatCaps(caps, PixelFormatCaps::Sampled));
        }
    }
}

TEST(CookSampling_OnlySixCombinationsAreTargets) {
    uint32 targets = 0;
    for (uint32 backendValue = 0; backendValue < BackendCount; ++backendValue) {
        for (uint32 platformValue = 0; platformValue < GraphicsPlatformCount; ++platformValue) {
            const auto backend = static_cast<Backend>(backendValue);
            const auto platform = static_cast<GraphicsPlatform>(platformValue);
            const bool expected = IsTarget(backend, platform);
            CHECK(IsCookSamplingTarget(backend, platform) == expected);
            targets += expected ? 1u : 0u;
            if (expected) {
                continue;
            }

            // 대상이 아닌 조합(OpenGL·Windows 미리보기 등)은 모든 형식이 보장 없음이다.
            for (uint32 number = 0; number <= MaxPixelFormatNumber; ++number) {
                const auto format = static_cast<PixelFormat>(number);
                CHECK(CookSamplingGuarantee(backend, platform, format) == PixelFormatCaps::None);
                CHECK(CookSamplingGuaranteeBasis(backend, platform, format) ==
                      CookSamplingBasis::None);
            }
        }
    }

    CHECK(targets == TargetCount);
    CHECK(!IsCookSamplingTarget(Backend::OpenGL, GraphicsPlatform::Windows));
    CHECK(!IsCookSamplingTarget(Backend::Vulkan, GraphicsPlatform::Mac));
    CHECK(!IsCookSamplingTarget(Backend::D3D12, GraphicsPlatform::Android));
}

TEST(CookSampling_InvalidInputsHaveNoGuarantee) {
    const auto badBackend = static_cast<Backend>(BackendCount);
    const auto badPlatform = static_cast<GraphicsPlatform>(GraphicsPlatformCount);
    CHECK(!IsCookSamplingTarget(badBackend, GraphicsPlatform::Windows));
    CHECK(!IsCookSamplingTarget(Backend::D3D12, badPlatform));
    CHECK(CookSamplingGuarantee(badBackend, GraphicsPlatform::Windows, PixelFormat::RGBA8Unorm) ==
          PixelFormatCaps::None);
    CHECK(CookSamplingGuarantee(Backend::D3D12, badPlatform, PixelFormat::RGBA8Unorm) ==
          PixelFormatCaps::None);
    for (const Target& target : Targets) {
        CHECK(CookSamplingGuarantee(target.backend, target.platform, PixelFormat::Unknown) ==
              PixelFormatCaps::None);
        // 번호표의 빈 번호와 범위 밖 번호다.
        const uint32 holes[] = {48, 63, 69, 79, 91, 102, 127, 159, MaxPixelFormatNumber + 1, 255};
        for (const uint32 hole : holes) {
            const auto format = static_cast<PixelFormat>(hole);
            CHECK(CookSamplingGuarantee(target.backend, target.platform, format) ==
                  PixelFormatCaps::None);
            CHECK(CookSamplingGuaranteeBasis(target.backend, target.platform, format) ==
                  CookSamplingBasis::None);
        }
    }
}

TEST(CookSampling_ListGuaranteeIsTheIntersection) {
    const Backend windows[] = {Backend::D3D12, Backend::D3D11, Backend::Vulkan};
    const Span<const Backend> list(windows);
    const GraphicsPlatform platform = GraphicsPlatform::Windows;
    for (uint32 number = 0; number <= 255; ++number) {
        const auto format = static_cast<PixelFormat>(number);
        PixelFormatCaps expected = CookSamplingCaps;
        for (const Backend backend : windows) {
            expected = expected & CookSamplingGuarantee(backend, platform, format);
        }

        CHECK(CookSamplingGuarantee(list, platform, format) == expected);
    }

    CHECK(CookSamplingGuarantee(list, platform, PixelFormat::BC7UnormSrgb) == CookSamplingCaps);
    CHECK(CookSamplingGuarantee(list, platform, PixelFormat::RGBA32Float) == CookSamplingCaps);
    CHECK(CookSamplingGuarantee(list, platform, PixelFormat::RG16Snorm) == PixelFormatCaps::None);
    CHECK(CookSamplingGuarantee(list, platform, PixelFormat::ASTC6x6Unorm) ==
          PixelFormatCaps::None);
    // 빈 목록과 대상이 아닌 조합이 낀 목록은 보장 없음이다.
    CHECK(CookSamplingGuarantee(Span<const Backend>(), platform, PixelFormat::RGBA8Unorm) ==
          PixelFormatCaps::None);
    const Backend withPreview[] = {Backend::D3D12, Backend::OpenGL};
    CHECK(CookSamplingGuarantee(Span<const Backend>(withPreview), platform,
                                PixelFormat::RGBA8Unorm) == PixelFormatCaps::None);
    // Android 두 backend의 교집합: BGRA8은 OpenGL ES가, 32F는 둘 다 샘플만이다.
    const Backend android[] = {Backend::OpenGL, Backend::Vulkan};
    const Span<const Backend> androidList(android);
    CHECK(CookSamplingGuarantee(androidList, GraphicsPlatform::Android, PixelFormat::BGRA8Unorm) ==
          PixelFormatCaps::None);
    CHECK(CookSamplingGuarantee(androidList, GraphicsPlatform::Android, PixelFormat::RGBA32Float) ==
          PixelFormatCaps::Sampled);
    CHECK(CookSamplingGuarantee(androidList, GraphicsPlatform::Android,
                                PixelFormat::ASTC4x4UnormSrgb) == CookSamplingCaps);
    CHECK(CookSamplingGuarantee(androidList, GraphicsPlatform::Android, PixelFormat::RGB8Unorm) ==
          PixelFormatCaps::None);
}

TEST(CookSampling_FallbackIsAGuaranteedUncompressedFormat) {
    // 조합 하나씩, Windows 셋, Android 둘의 목록이다.
    const Backend windows[] = {Backend::D3D12, Backend::D3D11, Backend::Vulkan};
    const Backend android[] = {Backend::OpenGL, Backend::Vulkan};
    struct List {
        Span<const Backend> backends;
        GraphicsPlatform platform;
    };

    List lists[TargetCount + 2] = {};
    for (uint32 column = 0; column < TargetCount; ++column) {
        lists[column] = {Span<const Backend>(&Targets[column].backend, 1),
                         Targets[column].platform};
    }

    lists[TargetCount] = {Span<const Backend>(windows), GraphicsPlatform::Windows};
    lists[TargetCount + 1] = {Span<const Backend>(android), GraphicsPlatform::Android};
    const PixelFormatCaps requirements[] = {CookSamplingCaps, PixelFormatCaps::Sampled};
    for (const List& list : lists) {
        for (const PixelFormatCaps required : requirements) {
            for (const Expected& row : Table) {
                const PixelFormat format = FormatOf(row);
                const PixelFormat fallback =
                    CookSamplingFallback(list.backends, list.platform, format, required);
                if (row.family == '-') {
                    CHECK(fallback == PixelFormat::Unknown);
                    continue;
                }

                // 계열이 있으면 늘 답이 있고, 비압축 색 형식이며, 목록 모두에서 보장이다.
                REQUIRE(fallback != PixelFormat::Unknown);
                const PixelFormatInfo& info = GetPixelFormatInfo(fallback);
                CHECK(info.kind == PixelFormatKind::Color);
                CHECK(HasPixelFormatCaps(
                    CookSamplingGuarantee(list.backends, list.platform, fallback), required));
                const bool keeps =
                    GetPixelFormatInfo(format).kind == PixelFormatKind::Color &&
                    HasPixelFormatCaps(CookSamplingGuarantee(list.backends, list.platform, format),
                                       required);
                if (keeps) {
                    CHECK(fallback == format);
                } else if (row.family == 'L') {
                    CHECK(fallback == PixelFormat::BGRA8Unorm ||
                          fallback == PixelFormat::RGBA8Unorm);
                } else if (row.family == 'R') {
                    CHECK(fallback == PixelFormat::BGRA8UnormSrgb ||
                          fallback == PixelFormat::RGBA8UnormSrgb);
                } else {
                    CHECK(fallback == PixelFormat::RGBA16Float);
                }
            }
        }
    }
}

TEST(CookSampling_FallbackPrefersBgra8AndFallsBackToRgba8) {
    const Backend windows[] = {Backend::D3D12, Backend::D3D11, Backend::Vulkan};
    const Span<const Backend> windowsList(windows);
    const Backend gles[] = {Backend::OpenGL};
    const Span<const Backend> glesList(gles);
    const GraphicsPlatform win = GraphicsPlatform::Windows;
    const GraphicsPlatform android = GraphicsPlatform::Android;
    CHECK(CookSamplingFallback(windowsList, win, PixelFormat::BC7UnormSrgb) ==
          PixelFormat::BGRA8UnormSrgb);
    CHECK(CookSamplingFallback(windowsList, win, PixelFormat::BC1Unorm) == PixelFormat::BGRA8Unorm);
    CHECK(CookSamplingFallback(windowsList, win, PixelFormat::BC6HUfloat) ==
          PixelFormat::RGBA16Float);
    CHECK(CookSamplingFallback(windowsList, win, PixelFormat::R8UnormSrgb) ==
          PixelFormat::BGRA8UnormSrgb);
    CHECK(CookSamplingFallback(windowsList, win, PixelFormat::R16Unorm) == PixelFormat::R16Unorm);
    CHECK(CookSamplingFallback(windowsList, win, PixelFormat::RGBA32Float) ==
          PixelFormat::RGBA32Float);
    CHECK(CookSamplingFallback(windowsList, win, PixelFormat::RGB8Unorm) ==
          PixelFormat::BGRA8Unorm);
    // OpenGL ES에는 BGRA8 보장이 없다.
    CHECK(CookSamplingFallback(glesList, android, PixelFormat::BC7UnormSrgb) ==
          PixelFormat::RGBA8UnormSrgb);
    CHECK(CookSamplingFallback(glesList, android, PixelFormat::BGRA8Unorm) ==
          PixelFormat::RGBA8Unorm);
    CHECK(CookSamplingFallback(glesList, android, PixelFormat::ASTC6x6Unorm) ==
          PixelFormat::RGBA8Unorm);
    CHECK(CookSamplingFallback(glesList, android, PixelFormat::R16Unorm) ==
          PixelFormat::RGBA16Float);
    CHECK(CookSamplingFallback(glesList, android, PixelFormat::ASTC6x6Hdr) ==
          PixelFormat::RGBA16Float);
    CHECK(CookSamplingFallback(glesList, android, PixelFormat::RGB8Unorm) ==
          PixelFormat::RGB8Unorm);
    // 32F: 필터가 필요하면 16F, 샘플만이면 그대로다(12.16.1 HDR_F32).
    CHECK(CookSamplingFallback(glesList, android, PixelFormat::RGBA32Float) ==
          PixelFormat::RGBA16Float);
    CHECK(CookSamplingFallback(glesList, android, PixelFormat::RGBA32Float,
                               PixelFormatCaps::Sampled) == PixelFormat::RGBA32Float);
}

TEST(CookSampling_FallbackRejectsBadArguments) {
    const Backend windows[] = {Backend::D3D12};
    const Span<const Backend> list(windows);
    const GraphicsPlatform win = GraphicsPlatform::Windows;
    CHECK(CookSamplingFallback(list, win, PixelFormat::BC1Unorm, PixelFormatCaps::None) ==
          PixelFormat::Unknown);
    CHECK(CookSamplingFallback(list, win, PixelFormat::BC1Unorm, PixelFormatCaps::Filterable) ==
          PixelFormat::Unknown);
    CHECK(CookSamplingFallback(list, win, PixelFormat::BC1Unorm,
                               CookSamplingCaps | PixelFormatCaps::RenderTarget) ==
          PixelFormat::Unknown);
    CHECK(CookSamplingFallback(Span<const Backend>(), win, PixelFormat::BC1Unorm) ==
          PixelFormat::Unknown);
    const Backend preview[] = {Backend::OpenGL};
    CHECK(CookSamplingFallback(Span<const Backend>(preview), win, PixelFormat::BC1Unorm) ==
          PixelFormat::Unknown);
    CHECK(CookSamplingFallback(list, win, PixelFormat::Unknown) == PixelFormat::Unknown);
    CHECK(CookSamplingFallback(list, win, static_cast<PixelFormat>(48)) == PixelFormat::Unknown);
    CHECK(CookSamplingFallback(list, win, PixelFormat::R32Uint) == PixelFormat::Unknown);
    CHECK(CookSamplingFallback(list, win, PixelFormat::D32Float) == PixelFormat::Unknown);
    CHECK(CookSamplingFallback(list, win, PixelFormat::NV12) == PixelFormat::Unknown);
}

TEST(CookSampling_FindMissingReportsTheFirstLackingFormat) {
    const FullCaps full;
    for (const Target& target : Targets) {
        CHECK(FindMissingCookSampling(target.backend, target.platform, full.View()) ==
              PixelFormat::Unknown);
        // 빈 배열은 첫 보장 형식에서 멈춘다(모든 대상에서 R8Unorm).
        CHECK(FindMissingCookSampling(target.backend, target.platform, {}) == PixelFormat::R8Unorm);
        // 짧은 배열은 그 뒤 번호의 능력을 None으로 본다.
        const Span<const PixelFormatCaps> shortCaps(full.caps, 50);
        CHECK(FindMissingCookSampling(target.backend, target.platform, shortCaps) ==
              FirstGuaranteed(target, 50));
        // 보장 칸 하나씩 빼면 그 형식을 돌려준다. 샘플만 보장인 칸은 Sampled만 있어도 된다.
        for (uint32 number = 1; number <= MaxPixelFormatNumber; ++number) {
            const auto format = static_cast<PixelFormat>(number);
            const PixelFormatCaps guaranteed =
                CookSamplingGuarantee(target.backend, target.platform, format);
            if (guaranteed == PixelFormatCaps::None) {
                continue;
            }

            FullCaps lacking;
            lacking.caps[number] = PixelFormatCaps::None;
            CHECK(FindMissingCookSampling(target.backend, target.platform, lacking.View()) ==
                  format);
            lacking.caps[number] = PixelFormatCaps::Sampled | PixelFormatCaps::RenderTarget;
            const PixelFormat expected = HasPixelFormatCaps(PixelFormatCaps::Sampled, guaranteed)
                                             ? PixelFormat::Unknown
                                             : format;
            CHECK(FindMissingCookSampling(target.backend, target.platform, lacking.View()) ==
                  expected);
        }
    }

    // 대상이 아닌 조합은 대조하지 않는다(OpenGL ES 미리보기).
    CHECK(FindMissingCookSampling(Backend::OpenGL, GraphicsPlatform::Windows, {}) ==
          PixelFormat::Unknown);
}

TEST(CookSampling_RejectionTextNamesFormatBackendAndPlatform) {
    char text[256];
    WriteCookSamplingRejection(Span<char>(text), Backend::Vulkan, GraphicsPlatform::Windows,
                               PixelFormat::BC7UnormSrgb);
    const StringView view(text, platform::CStringLength(text));
    CHECK(view.Contains(StringView("BC7UnormSrgb"), StringComparison::CaseSensitive));
    CHECK(view.Contains(StringView("Vulkan"), StringComparison::CaseSensitive));
    CHECK(view.Contains(StringView("Windows"), StringComparison::CaseSensitive));
    CHECK(view.Contains(StringView("linearly filter"), StringComparison::CaseSensitive));
    CHECK(view.Contains(StringView("engine requirement"), StringComparison::CaseSensitive));
    WriteCookSamplingRejection(Span<char>(text), Backend::OpenGL, GraphicsPlatform::Android,
                               PixelFormat::RGBA32Float);
    const StringView sampled(text, platform::CStringLength(text));
    CHECK(!sampled.Contains(StringView("filter"), StringComparison::CaseSensitive));
    CHECK(sampled.Contains(StringView("API minimum"), StringComparison::CaseSensitive));
    // 작은 칸은 잘리고 NUL로 끝난다. 빈 칸은 쓰지 않는다.
    char small[8];
    for (char& slot : small) {
        slot = 'x';
    }

    WriteCookSamplingRejection(Span<char>(small), Backend::D3D12, GraphicsPlatform::Windows,
                               PixelFormat::R8Unorm);
    CHECK(platform::CStringLength(small) < sizeof(small));
    WriteCookSamplingRejection(Span<char>(), Backend::D3D12, GraphicsPlatform::Windows,
                               PixelFormat::R8Unorm);
}

TEST(CookSampling_BackendAndPlatformNamesRoundTrip) {
    for (uint32 value = 0; value < BackendCount; ++value) {
        const auto backend = static_cast<Backend>(value);
        const char* name = BackendName(backend);
        auto parsed = ParseBackendName(StringView(name, platform::CStringLength(name)));
        REQUIRE(parsed);
        CHECK(parsed.Value() == backend);
    }

    for (uint32 value = 0; value < GraphicsPlatformCount; ++value) {
        const auto platform = static_cast<GraphicsPlatform>(value);
        const char* name = GraphicsPlatformName(platform);
        auto parsed = ParseGraphicsPlatformName(StringView(name, platform::CStringLength(name)));
        REQUIRE(parsed);
        CHECK(parsed.Value() == platform);
    }

    // 매니페스트 backend 어휘와 같은 철자다(AssetSystem 10.3절 1항).
    CHECK(platform::CStringCompare(BackendName(Backend::D3D12), "D3D12") == 0);
    CHECK(platform::CStringCompare(BackendName(Backend::OpenGL), "OpenGL") == 0);
    CHECK(platform::CStringCompare(BackendName(static_cast<Backend>(BackendCount)), "Unknown") ==
          0);
    CHECK(platform::CStringCompare(
              GraphicsPlatformName(static_cast<GraphicsPlatform>(GraphicsPlatformCount)),
              "Unknown") == 0);
    CHECK(ParseBackendName(StringView("d3d12")).Error() == ErrorCode::NotFound);
    CHECK(ParseBackendName(StringView("Unknown")).Error() == ErrorCode::NotFound);
    CHECK(ParseBackendName(StringView("")).Error() == ErrorCode::NotFound);
    CHECK(ParseGraphicsPlatformName(StringView("windows")).Error() == ErrorCode::NotFound);
    CHECK(ParseGraphicsPlatformName(StringView("Linux")).Error() == ErrorCode::NotFound);
    CHECK(ParseGraphicsPlatformName(StringView("Unknown")).Error() == ErrorCode::NotFound);
    // 실행 파일 플랫폼 이름(HostConfig HostPlatformName과 같은 철자)과 맞는다.
    const GraphicsPlatform expected = IsWindows ? GraphicsPlatform::Windows
                                      : IsMac   ? GraphicsPlatform::Mac
                                                : GraphicsPlatform::Android;
    CHECK(HostGraphicsPlatform() == expected);
}

} // namespace cook_sampling_tests_file

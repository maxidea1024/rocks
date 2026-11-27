#include <Core/Text/StringBuilder.h>
#include <GraphicsTypes/CookSampling.h>

namespace rocks::graphics {

namespace {

// 쿠킹 대상 조합의 열 번호다(Docs/Design/PixelFormat 7.1절 형식 표의 열 순서).
enum class CookTarget : uint8 {
    D3D12Windows,
    D3D11Windows,
    VulkanWindows,
    VulkanAndroid,
    OpenGLAndroid,
    MetalMac
};

// 쿠킹 대상 조합 수(표의 열 수)와 "대상 아님" 표지다.
constexpr uint32 CookTargetCount = 6;
constexpr uint32 NoCookTarget = CookTargetCount;

// 표 칸 하나다.
enum class CookCell : uint8 {
    // 보장 없음
    None,
    // 샘플만(API 최소)
    Sampled,
    // 샘플 + 선형 필터(API 최소)
    Filtered,
    // 샘플 + 선형 필터(엔진 요구 — 장치 생성이 대조)
    EngineFiltered
};

// 보장 비압축 대체 계열이다(7.1절 "보장 비압축 대체").
enum class CookFamily : uint8 {
    // 쿠킹 텍스처 형식이 아니다(정수·깊이·비디오)
    None,
    // 8비트 선형
    Linear8,
    // 8비트 sRGB
    Srgb8,
    // 고정밀(snorm·16비트·float·묶음 고정밀·BC6H·ASTC HDR)
    HighPrecision
};

struct CookRow {
    PixelFormat format;
    CookFamily family;
    CookCell cells[CookTargetCount];
};

constexpr CookCell CkN = CookCell::None;
constexpr CookCell CkS = CookCell::Sampled;
constexpr CookCell CkF = CookCell::Filtered;
constexpr CookCell CkE = CookCell::EngineFiltered;
constexpr CookFamily FmNone = CookFamily::None;
constexpr CookFamily FmL8 = CookFamily::Linear8;
constexpr CookFamily FmS8 = CookFamily::Srgb8;
constexpr CookFamily FmHp = CookFamily::HighPrecision;

// 형식 표다(7.1절 표와 행마다 같다, 번호 순). 열: D3D12·Windows, D3D11·Windows, Vulkan·Windows,
// Vulkan·Android, OpenGL·Android, Metal·Mac. 근거는 7.1절 "backend별 보장과 출처"다.
constexpr CookRow CookRows[] = {
    {PixelFormat::R8Unorm, FmL8, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::R8UnormSrgb, FmS8, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::R8Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::R8Sint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::A8Unorm, FmL8, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::RG8Unorm, FmL8, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::RG8UnormSrgb, FmS8, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::RG8Snorm, FmHp, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::RG8Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::RGB8Unorm, FmL8, {CkN, CkN, CkN, CkN, CkF, CkN}},
    {PixelFormat::RGB8UnormSrgb, FmS8, {CkN, CkN, CkN, CkN, CkF, CkN}},
    {PixelFormat::RGBA8Unorm, FmL8, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::RGBA8UnormSrgb, FmS8, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::RGBA8Snorm, FmHp, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::RGBA8Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::BGRA8Unorm, FmL8, {CkF, CkF, CkF, CkF, CkN, CkF}},
    {PixelFormat::BGRA8UnormSrgb, FmS8, {CkF, CkF, CkF, CkF, CkN, CkF}},
    {PixelFormat::R16Unorm, FmHp, {CkF, CkF, CkE, CkN, CkN, CkF}},
    {PixelFormat::R16Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::R16Sint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::R16Float, FmHp, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::RG16Unorm, FmHp, {CkF, CkF, CkE, CkN, CkN, CkF}},
    {PixelFormat::RG16Snorm, FmHp, {CkF, CkF, CkN, CkN, CkN, CkF}},
    {PixelFormat::RG16Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::RG16Sint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::RG16Float, FmHp, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::RGBA16Unorm, FmHp, {CkF, CkF, CkE, CkN, CkN, CkF}},
    {PixelFormat::RGBA16Snorm, FmHp, {CkF, CkF, CkN, CkN, CkN, CkF}},
    {PixelFormat::RGBA16Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::RGBA16Sint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::RGBA16Float, FmHp, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::R32Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::R32Sint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::R32Float, FmHp, {CkF, CkF, CkE, CkS, CkS, CkE}},
    {PixelFormat::RG32Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::RG32Float, FmHp, {CkF, CkF, CkE, CkS, CkS, CkE}},
    {PixelFormat::RGB32Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::RGB32Sint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::RGB32Float, FmHp, {CkN, CkN, CkN, CkN, CkS, CkN}},
    {PixelFormat::RGBA32Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::RGBA32Float, FmHp, {CkF, CkF, CkE, CkS, CkS, CkE}},
    {PixelFormat::R64Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::R5G6B5Unorm, FmL8, {CkF, CkF, CkF, CkF, CkF, CkN}},
    {PixelFormat::B5G5R5A1Unorm, FmL8, {CkF, CkF, CkF, CkF, CkN, CkN}},
    {PixelFormat::RGB10A2Unorm, FmHp, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::RG11B10Ufloat, FmHp, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::RGB9E5Ufloat, FmHp, {CkF, CkF, CkF, CkF, CkF, CkF}},
    {PixelFormat::D16Unorm, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::D24Unorm, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::D32Float, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::D24UnormS8Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::D32FloatS8Uint, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::BC1Unorm, FmL8, {CkF, CkF, CkE, CkN, CkN, CkE}},
    {PixelFormat::BC1UnormSrgb, FmS8, {CkF, CkF, CkE, CkN, CkN, CkE}},
    {PixelFormat::BC2Unorm, FmL8, {CkF, CkF, CkE, CkN, CkN, CkE}},
    {PixelFormat::BC2UnormSrgb, FmS8, {CkF, CkF, CkE, CkN, CkN, CkE}},
    {PixelFormat::BC3Unorm, FmL8, {CkF, CkF, CkE, CkN, CkN, CkE}},
    {PixelFormat::BC3UnormSrgb, FmS8, {CkF, CkF, CkE, CkN, CkN, CkE}},
    {PixelFormat::BC4Unorm, FmL8, {CkF, CkF, CkE, CkN, CkN, CkE}},
    {PixelFormat::BC5Unorm, FmL8, {CkF, CkF, CkE, CkN, CkN, CkE}},
    {PixelFormat::BC6HUfloat, FmHp, {CkF, CkF, CkE, CkN, CkN, CkE}},
    {PixelFormat::BC7Unorm, FmL8, {CkF, CkF, CkE, CkN, CkN, CkE}},
    {PixelFormat::BC7UnormSrgb, FmS8, {CkF, CkF, CkE, CkN, CkN, CkE}},
    {PixelFormat::ETC2RGB8Unorm, FmL8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ETC2RGB8UnormSrgb, FmS8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ETC2RGBA8Unorm, FmL8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ETC2RGBA8UnormSrgb, FmS8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::EACR11Unorm, FmL8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::EACRG11Unorm, FmL8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ASTC4x4Unorm, FmL8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ASTC4x4UnormSrgb, FmS8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ASTC4x4Hdr, FmHp, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::ASTC6x6Unorm, FmL8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ASTC6x6UnormSrgb, FmS8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ASTC6x6Hdr, FmHp, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::ASTC8x8Unorm, FmL8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ASTC8x8UnormSrgb, FmS8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ASTC8x8Hdr, FmHp, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::ASTC10x10Unorm, FmL8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ASTC10x10UnormSrgb, FmS8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ASTC10x10Hdr, FmHp, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::ASTC12x12Unorm, FmL8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ASTC12x12UnormSrgb, FmS8, {CkN, CkN, CkN, CkE, CkF, CkN}},
    {PixelFormat::ASTC12x12Hdr, FmHp, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::NV12, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
    {PixelFormat::P010, FmNone, {CkN, CkN, CkN, CkN, CkN, CkN}},
};

// 행이 유효 형식마다 하나씩, 번호 순인지다(번호표 자체와의 대조는 시험이 한다).
constexpr bool CookRowsAreOrdered() noexcept {
    for (usize index = 1; index < sizeof(CookRows) / sizeof(CookRows[0]); ++index) {
        if (PixelFormatNumber(CookRows[index - 1].format) >=
            PixelFormatNumber(CookRows[index].format)) {
            return false;
        }
    }

    return true;
}

static_assert(sizeof(CookRows) / sizeof(CookRows[0]) == PixelFormatCount,
              "The cook sampling table needs exactly one row per pixel format");
static_assert(CookRowsAreOrdered(), "Cook sampling rows must be in pixel format number order");

// 형식 번호 → 행 번호 표다(없으면 행 수).
struct CookRowIndex {
    uint8 rows[MaxPixelFormatNumber + 1];
};

constexpr CookRowIndex MakeCookRowIndex() noexcept {
    CookRowIndex index{};
    for (uint32 number = 0; number <= MaxPixelFormatNumber; ++number) {
        index.rows[number] = static_cast<uint8>(PixelFormatCount);
    }

    for (uint32 row = 0; row < PixelFormatCount; ++row) {
        index.rows[PixelFormatNumber(CookRows[row].format)] = static_cast<uint8>(row);
    }

    return index;
}

constexpr CookRowIndex CookRowsByNumber = MakeCookRowIndex();

// format의 행이다. 표에 없으면(Unknown·번호표 밖) nullptr이다.
const CookRow* FindCookRow(PixelFormat format) noexcept {
    const uint32 number = PixelFormatNumber(format);
    if (number > MaxPixelFormatNumber) {
        return nullptr;
    }

    const uint32 row = CookRowsByNumber.rows[number];
    return row < PixelFormatCount ? &CookRows[row] : nullptr;
}

// (backend, platform)의 열 번호다. 대상이 아니면 NoCookTarget이다.
uint32 CookTargetOf(Backend backend, GraphicsPlatform platform) noexcept {
    struct Pair {
        Backend backend;
        GraphicsPlatform platform;
        CookTarget target;
    };

    // 열 순서와 같은 대상 조합 표다.
    constexpr Pair Targets[] = {
        {Backend::D3D12, GraphicsPlatform::Windows, CookTarget::D3D12Windows},
        {Backend::D3D11, GraphicsPlatform::Windows, CookTarget::D3D11Windows},
        {Backend::Vulkan, GraphicsPlatform::Windows, CookTarget::VulkanWindows},
        {Backend::Vulkan, GraphicsPlatform::Android, CookTarget::VulkanAndroid},
        {Backend::OpenGL, GraphicsPlatform::Android, CookTarget::OpenGLAndroid},
        {Backend::Metal, GraphicsPlatform::Mac, CookTarget::MetalMac},
    };
    static_assert(sizeof(Targets) / sizeof(Targets[0]) == CookTargetCount,
                  "Every cook target column needs a backend and platform");
    for (const Pair& pair : Targets) {
        if (pair.backend == backend && pair.platform == platform) {
            return static_cast<uint32>(pair.target);
        }
    }

    return NoCookTarget;
}

CookCell CookCellOf(Backend backend, GraphicsPlatform platform, PixelFormat format) noexcept {
    const uint32 target = CookTargetOf(backend, platform);
    const CookRow* row = FindCookRow(format);
    if (target == NoCookTarget || row == nullptr) {
        return CookCell::None;
    }

    return row->cells[target];
}

PixelFormatCaps CookCellCaps(CookCell cell) noexcept {
    switch (cell) {
    case CookCell::None:
        return PixelFormatCaps::None;
    case CookCell::Sampled:
        return PixelFormatCaps::Sampled;
    case CookCell::Filtered:
    case CookCell::EngineFiltered:
        return CookSamplingCaps;
    }

    return PixelFormatCaps::None;
}

// 대체 계열의 후보다(앞에서부터 본다).
constexpr PixelFormat CookLinear8Candidates[] = {PixelFormat::BGRA8Unorm, PixelFormat::RGBA8Unorm};
constexpr PixelFormat CookSrgb8Candidates[] = {PixelFormat::BGRA8UnormSrgb,
                                               PixelFormat::RGBA8UnormSrgb};
constexpr PixelFormat CookHighPrecisionCandidates[] = {PixelFormat::RGBA16Float};

Span<const PixelFormat> CookFamilyCandidates(CookFamily family) noexcept {
    switch (family) {
    case CookFamily::None:
        return {};
    case CookFamily::Linear8:
        return Span<const PixelFormat>(CookLinear8Candidates);
    case CookFamily::Srgb8:
        return Span<const PixelFormat>(CookSrgb8Candidates);
    case CookFamily::HighPrecision:
        return Span<const PixelFormat>(CookHighPrecisionCandidates);
    }

    return {};
}

} // namespace

bool IsCookSamplingTarget(Backend backend, GraphicsPlatform platform) noexcept {
    return CookTargetOf(backend, platform) != NoCookTarget;
}

PixelFormatCaps CookSamplingGuarantee(Backend backend, GraphicsPlatform platform,
                                      PixelFormat format) noexcept {
    return CookCellCaps(CookCellOf(backend, platform, format));
}

PixelFormatCaps CookSamplingGuarantee(Span<const Backend> backends, GraphicsPlatform platform,
                                      PixelFormat format) noexcept {
    if (backends.Size() == 0) {
        return PixelFormatCaps::None;
    }

    PixelFormatCaps caps = CookSamplingCaps;
    for (usize index = 0; index < backends.Size(); ++index) {
        caps = caps & CookSamplingGuarantee(backends[index], platform, format);
    }

    return caps;
}

CookSamplingBasis CookSamplingGuaranteeBasis(Backend backend, GraphicsPlatform platform,
                                             PixelFormat format) noexcept {
    switch (CookCellOf(backend, platform, format)) {
    case CookCell::None:
        return CookSamplingBasis::None;
    case CookCell::Sampled:
    case CookCell::Filtered:
        return CookSamplingBasis::ApiMinimum;
    case CookCell::EngineFiltered:
        return CookSamplingBasis::EngineRequirement;
    }

    return CookSamplingBasis::None;
}

PixelFormat CookSamplingFallback(Span<const Backend> backends, GraphicsPlatform platform,
                                 PixelFormat format, PixelFormatCaps required) noexcept {
    const CookRow* row = FindCookRow(format);
    const bool validRequired = HasPixelFormatCaps(required, PixelFormatCaps::Sampled) &&
                               HasPixelFormatCaps(CookSamplingCaps, required);
    if (row == nullptr || row->family == CookFamily::None || !validRequired) {
        return PixelFormat::Unknown;
    }

    const PixelFormatInfo& info = GetPixelFormatInfo(format);
    if (info.kind == PixelFormatKind::Color &&
        HasPixelFormatCaps(CookSamplingGuarantee(backends, platform, format), required)) {
        return format;
    }

    const Span<const PixelFormat> candidates = CookFamilyCandidates(row->family);
    for (usize index = 0; index < candidates.Size(); ++index) {
        if (HasPixelFormatCaps(CookSamplingGuarantee(backends, platform, candidates[index]),
                               required)) {
            return candidates[index];
        }
    }

    return PixelFormat::Unknown;
}

PixelFormat FindMissingCookSampling(Backend backend, GraphicsPlatform platform,
                                    Span<const PixelFormatCaps> capsByNumber) noexcept {
    if (!IsCookSamplingTarget(backend, platform)) {
        return PixelFormat::Unknown;
    }

    for (uint32 row = 0; row < PixelFormatCount; ++row) {
        const PixelFormat format = CookRows[row].format;
        const uint32 number = PixelFormatNumber(format);
        const PixelFormatCaps caps =
            number < capsByNumber.Size() ? capsByNumber[number] : PixelFormatCaps::None;
        if (!HasPixelFormatCaps(caps, CookSamplingGuarantee(backend, platform, format))) {
            return format;
        }
    }

    return PixelFormat::Unknown;
}

void WriteCookSamplingRejection(Span<char> out, Backend backend, GraphicsPlatform platform,
                                PixelFormat missing) noexcept {
    if (out.Size() == 0) {
        return;
    }

    const bool filtered = HasPixelFormatCaps(CookSamplingGuarantee(backend, platform, missing),
                                             PixelFormatCaps::Filterable);
    const bool engine = CookSamplingGuaranteeBasis(backend, platform, missing) ==
                        CookSamplingBasis::EngineRequirement;
    FixedStringBuilder text{out};
    (void)text.TryAppendFormat(
        "the device cannot {} {} textures, which the engine requires of every {} device on {} "
        "({}; Docs/Design/PixelFormat section 7.1)",
        filtered ? "sample and linearly filter" : "sample", PixelFormatName(missing),
        BackendName(backend), GraphicsPlatformName(platform),
        engine ? "engine requirement beyond the API minimum" : "API minimum guarantee");
}

} // namespace rocks::graphics

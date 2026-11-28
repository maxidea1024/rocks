#pragma once

#include "TextureBuildInternal.h"

#include <TextureTools/TextureBuild.h>
#include <TextureTools/TextureFormatSelect.h>

// 텍스처 빌드 단계의 파일 사이 경계다(Docs/Design/AssetSystem 12.18 TP-D6): 선형화·재표본
// (TextureBuildResize.cpp), mip 생성(TextureBuildMips.cpp), 출력(TextureBuildEncode.cpp)을 빌드
// 본문 (TextureBuild.cpp)이 부른다. 각 파일의 나머지 함수는 그 파일의 namespace 안에만 쓴다(유니티
// 빌드 묶음 충돌 정책 — UnityBuildPch 9절).
namespace rocks::texture_build_detail {

// 출력 부호화 선택이다(12.17.5 3).
struct EncodeOptions {
    // BC7이 알파 프로필을 쓰는지다(12.17.6 4 — TextureFormatChoice::alphaProfile).
    bool alphaProfile = false;
    // ASTC 채널 배치다(12.17.8 3 — TextureFormatChoice::astcMode).
    TextureAstcMode astcMode = TextureAstcMode::Rgba;
    // 블록 압축 실행 방법이다(12.17.9 4 — BuildTexture 인자 그대로).
    TextureBuildExecution execution{};
    // norm16 대체(G16 → R16F)는 Unorm 규칙대로 [0, 1]로 자르고 NaN → 0이다(12.14절).
    bool clampUnit = false;
};

namespace build_resize {

// settings의 원본 정규 픽셀을 선형 float로 푼다(12.2절 1번). 오류: byte 수 다름, 할당 실패.
Result<void> DecodeSource(const TextureBuildSettings& settings, Span<const uint8> pixels,
                          Level& out) noexcept;
// value 이상인 가장 작은 2의 거듭제곱이다.
uint32 RoundUpPowerOfTwo32(uint32 value) noexcept;
// 각 평면을 (targetX, targetY)로 재표본한다(12.5절 크기 조정 필터). 오류: 할당 실패.
Result<void> Resample(Allocator& allocator, Level& level, uint32 targetX, uint32 targetY) noexcept;
// 각 평면을 늘린다(12.5절 3번 Pad). 오류: 할당 실패.
Result<void> Pad(Allocator& allocator, Level& level, uint32 targetX, uint32 targetY, uint32 targetZ,
                 bool borderColor, const float* fill) noexcept;

} // namespace build_resize

namespace build_kernel {

// 커널 한 변의 상한이다(UE FImageKernel2D MaxKernelExtend).
inline constexpr uint32 MaxKernelSize = 12;

// mip 필터 커널이다(UE FImageKernel2D — 1D 가중치와 그 바깥곱 2D 가중치, 앞 size·size² 칸만 뜻이
// 있다).
struct MipKernel {
    uint32 size = 0;
    float weights1D[MaxKernelSize] = {};
    float weights2D[MaxKernelSize * MaxKernelSize] = {};
};

// UE BuildSeparatableGaussWithSharpen이다(12.18 TP1 1): 2는 평균, sharpen < 0이면 Gauss, 4·6·8은
// sharpen lobe.
ROCKS_TEXTURETOOLS_API MipKernel BuildMipKernel(uint32 size, float sharpen) noexcept;

// mip 필터 값이다(UE GetMipGenSettings가 내는 다섯 값).
struct MipFilter {
    float sharpen = 0.0f;
    uint32 kernelSize = 2;
    bool downsampleWithAverage = true;
    bool sharpenWithoutColorShift = true;
    bool borderColorBlack = false;
};

// mipGen(해석 뒤 — FromTextureGroup·Angular는 이미 바뀐 값)과 compression에서 필터 값을 낸다(12.18
// TP1 2).
ROCKS_TEXTURETOOLS_API MipFilter MipFilterFor(TextureMipGenSettings mipGen,
                                              TextureCompressionSettings compression) noexcept;

} // namespace build_kernel

namespace build_mips {

// mip 생성의 원본 밖 처리다(UE EMipGenAddressMode).
enum class MipAddress : uint8 {
    Wrap,
    Clamp,
    BorderBlack,
};

// mip 사슬 생성 입력이다(TextureBuildSettings에서 MipChainParamsFor가 만든다).
struct MipChainParams {
    build_kernel::MipFilter filter;
    bool volume = false;
    bool cube = false;
    bool unfiltered = false;
    bool useNewMipFilter = true;
    bool preserveBorder = false;
    TextureAddress addressX = TextureAddress::Wrap;
    TextureAddress addressY = TextureAddress::Wrap;
    bool alphaCoverage = false;
    float thresholds[Channels] = {};
};

// 다음 mip 길이 max(1, n >> 1)이다.
uint32 Half(uint32 n) noexcept;
ROCKS_TEXTURETOOLS_API MipChainParams
MipChainParamsFor(const TextureBuildSettings& settings) noexcept;
// base 크기의 주소 방식이다(UE ComputeAddressMode — 12.18 TP1 5).
MipAddress MipAddressFor(const MipChainParams& params, uint32 width, uint32 height) noexcept;
// (width, height, 평면 수)에서 1×1(Volume은 ×1)까지 만들 mip 수다.
ROCKS_TEXTURETOOLS_API uint32 MipChainLength(const MipChainParams& params, uint32 width,
                                             uint32 height, uint32 planes) noexcept;
// base 다음 mip을 최대 depth개(1×1에서 멈춤) out 뒤에 더한다(UE GenerateMipChain — 평균 사슬·alpha
// coverage·Volume 두 slice·테두리 다시 쓰기, 12.18 TP1 3–7). base는 out 안에 있어도 되므로 out을
// 늘리지 않는다 — 호출자가 더할 mip 수만큼 자리를 예약해 둔다(모자라면 InvalidArgument). 오류: 할당
// 실패. 대상 행·coverage 행을 execution으로 나눠 돈다(결과는 분할과 무관 — 12.18 TP1b).
ROCKS_TEXTURETOOLS_API Result<void>
GenerateMipChain(Allocator& allocator, const MipChainParams& params, const Level& base,
                 uint32 depth, Array<Level>& out,
                 const TextureBuildExecution& execution = {}) noexcept;
// 법선을 정규화한다(UE NormalizeMip — (평면, 행) batch 병렬, 12.18 TP1b). 오류: 병렬 실행 오류.
Result<void> NormalizeNormals(Level& level, const TextureBuildExecution& execution = {}) noexcept;

} // namespace build_mips

namespace build_encode {

// [0, 1]로 자르고 NaN은 0이다.
float Clamp01NansTo0(float value) noexcept;
// level을 choice 형식으로 부호화한다(12.17.5 3). 오류: 할당 실패, 인코더 오류.
Result<void> Encode(Allocator& allocator, const Level& level, const TextureFormatChoice& choice,
                    const EncodeOptions& options, Array<uint8>& out) noexcept;
// 채널을 복제한다(12.17절 TD6).
void Replicate(Level& level, TextureChannelReplicate replicate) noexcept;
// 투명 알파가 있는지다(UE DetectAlphaChannel 8비트 문턱).
bool HasTransparentAlpha(const Level& level) noexcept;

} // namespace build_encode

} // namespace rocks::texture_build_detail

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

namespace build_mips {

// 다음 mip 길이 max(1, n >> 1)이다.
uint32 Half(uint32 n) noexcept;
// source의 다음 mip을 out에 만든다(12.5절 mip 규칙). 오류: 할당 실패.
Result<void> NextMip(const Level& source, bool volume, bool unfiltered, Level& out) noexcept;
// 테두리 texel을 다시 쓴다(PreserveBorder).
void RedrawBorder(const Level& source, bool volume, Level& mip) noexcept;
// 법선을 정규화한다(UE NormalizeMip).
void NormalizeNormals(Level& level) noexcept;

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

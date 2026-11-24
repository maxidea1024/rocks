#pragma once

#include <AssetCooker/CookProcessor.h>
#include <Core/Containers/Span.h>
#include <Texture/Texture.h>
#include <TextureTools/TextureBuild.h>

// 텍스처 쿠킹 처리기다(Docs/Design/AssetSystem 12.14절 — A5j1). Texture 파생 다섯 클래스의 쿠킹
// 패키지를 만든다: 설정 판정(12.8절) → 유효 설정(텍스처 + 프로파일 LOD 그룹 — 12.13절) → 너겟
// 조회(bucket texture, SCC Record — 9.1절) → 미적중이면 빌드(A5i)·Put → TexturePlatformData 채우기
// (12.5.1절 상주 꼬리·비스트리밍, 샘플러) → 기본 처리기처럼 쿠킹 패키지 쓰기 → 에디터 객체의
// platformData를 기본값으로 되돌리기(성공·실패 모두).
//
// 사용 예(호스트 — 명령 동안 처리기를 쥔다):
//   TextureCookProcessor textureCook;
//   context.cookProcessors = {&RegisterTextureCookProcessors, &textureCook};
namespace rocks {

// 스트리밍 텍스처의 최소 상주 mip 수다(UE NUM_INLINE_DERIVED_MIPS — 12.5.1절 2번). 바꾸면 쿠킹
// 결과가 바뀌므로 처리기 버전 Guid를 바꾼다.
inline constexpr uint32 TextureMinResidentMipCount = 7;

// 너겟 값 머리(part 0)의 형식 버전이다(배치가 바뀌면 올린다 — 처리기 버전 Guid도 바꾼다).
inline constexpr uint32 TextureNuggetHeaderVersion = 1;

// 너겟 머리가 담을 수 있는 mip 수 상한이다(2^31 축 길이의 mip 사슬 32개 — 넘으면 손상으로 본다).
inline constexpr uint32 TextureNuggetMaxMips = 32;

// 처리기 정체성이다(출력 배치 — 값·머리 형식·플랫폼 데이터 채우기 — 가 바뀌면 버전 Guid를 바꾼다).
inline constexpr NuggetProcessorId TextureCookProcessorId{
    Guid{0x524f434b54455843ull, 0x0000000000000001ull},
    Guid{0x524f434b54455856ull, 0x0000000000000001ull}};

// 쿠킹 mip 하나의 크기와 byte다(너겟 머리 부호화·플랫폼 데이터 채우기가 쓴다 — byte는 빌림).
struct TextureCookMip {
    uint32 sizeX = 0;
    uint32 sizeY = 0;
    uint32 sizeZ = 1;
    Span<const uint8> bytes{};
};

// 너겟 머리의 mip 항목이다(값 byte는 part 1..N — 여기는 크기와 byte 수만).
struct TextureNuggetMip {
    uint32 sizeX = 0;
    uint32 sizeY = 0;
    uint32 sizeZ = 1;
    uint64 byteSize = 0;
};

// 너겟 머리를 해석한 값이다.
struct TextureNuggetHeader {
    explicit TextureNuggetHeader(Allocator& allocator) noexcept : mips(allocator) {
    }

    TextureBuildOutputInfo output;
    uint32 numSlices = 0;
    Array<TextureNuggetMip> mips;
};

// 너겟 머리(part 0)를 out에 쓴다(out을 비우고 채운다 — little-endian 고정 배치: 버전 u32,
// PixelFormat 번호 u32, srgb u8, swizzle u8 넷, numSlices u32, mip 수 u32, mip마다 sizeX·sizeY·
// sizeZ u32와 byte 수 u64). 오류: mip 수가 0이거나 TextureNuggetMaxMips를 넘음 InvalidArgument,
// 할당 실패.
ROCKS_TEXTURETOOLS_API Result<void> WriteTextureNuggetHeader(const TextureBuildOutputInfo& output,
                                                             uint32 numSlices,
                                                             Span<const TextureCookMip> mips,
                                                             Array<uint8>& out) noexcept;

// 너겟 머리를 해석한다(out을 채운다). 오류: 크기·버전·mip 수·PixelFormat 번호·srgb·swizzle 값이
// 맞지 않음 InvalidArgument(적중을 미적중으로 다룬다 — 12.14절 6번), 할당 실패.
ROCKS_TEXTURETOOLS_API Result<void> ReadTextureNuggetHeader(Span<const uint8> bytes,
                                                            TextureNuggetHeader& out) noexcept;

// 상주 꼬리 mip 수 R이다(12.5.1절 2번 — 비압축이라 블록 규칙·형식 mip tail이 없다): 비스트리밍
// (streaming false)이거나 numMips ≤ 1이면 numMips, 아니면 min(numMips, TextureMinResidentMipCount).
ROCKS_TEXTURETOOLS_API uint32 TextureResidentTailMips(uint32 numMips, bool streaming) noexcept;

// 쿠킹 샘플러 필터다(12.14절 8번): 텍스처 filter가 Default면 그룹 MinMagFilter·MipFilter 조합(UE
// SetupGroup), 아니면 Nearest → Point, Bilinear → Bilinear, Trilinear → Trilinear.
ROCKS_TEXTURETOOLS_API TextureSamplerFilter
TextureCookSamplerFilter(TextureFilter filter, const TextureLodGroup& group) noexcept;

// 텍스처 쿠킹 처리기다(TypeIdOf<Texture>()에 등록 — 파생 다섯 클래스에 쓰인다). 한 스레드에서
// 쓴다(쿠킹 실행기와 같다). Cook 오류: 설정 판정 이슈 → 첫 이슈의 코드(UnsupportedSetting은
// Unsupported, InvalidSetting은 InvalidArgument — detail = 이슈 수), 유효 설정·빌드의 오류(진단은
// 빌드 실패 그대로), 빈 원본 InvalidSource(InvalidArgument), 원본 bulk 읽기 오류, verify 불일치
// NuggetMismatch(InvalidArgument), required 너겟 캐시 실패(진단 None — 원인 오류), 쓰기 오류,
// 할당 실패 OutOfMemory.
class ROCKS_TEXTURETOOLS_API TextureCookProcessor final : public CookProcessor {
public:
    NuggetProcessorId Identity() const noexcept override {
        return TextureCookProcessorId;
    }

    Result<void> Cook(CookContext& context) const noexcept override;

    // 빌드(BuildTexture)를 부른 수다(시험 — 너겟 적중이면 오르지 않는다).
    uint64 Builds() const noexcept {
        return builds_;
    }

    // 마지막 Cook이 만든 SCC key다(시험 — 너겟 캐시가 없었으면 앞 값 그대로).
    const Sha256Digest& LastNuggetKey() const noexcept {
        return lastKey_;
    }

private:
    mutable uint64 builds_ = 0;
    mutable Sha256Digest lastKey_{};
};

// 쿠킹 처리기 등록이다(AssetCookProcessorSetup — state = TextureCookProcessor*, 명령 동안 산다).
// 오류: null state InvalidArgument, CookProcessorTable::Register의 오류.
ROCKS_TEXTURETOOLS_API Result<void>
RegisterTextureCookProcessors(void* state, CookProcessorTable& table) noexcept;

} // namespace rocks

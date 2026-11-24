#pragma once

#include <AssetDatabase/AssetImportArguments.h>
#include <AssetDatabase/AssetImporter.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <Texture/Texture.h>
#include <Texture/TextureImportOptions.h>

// 텍스처 임포트다(Docs/Design/AssetSystem 12.12절 — A5h): 임포터 9개(원본 구성 12.3절), 새 임포트
// 기본값(UE UTextureFactory·ApplyDefaultsForNewlyImportedTextures), 설정 정규화(UE
// UTexture::ValidateSettingsAfterImportOrEdit), 임포트 요청(TextureImportSpec)과 asset-import 예약
// 인자 번역기. 원본 디코딩은 ImageCodec이 한다.
//
// 사용 예(제품 호스트 — 세션마다 표에 건다):
//   TextureImporterSet textureImporters;
//   context.importers = {&RegisterTextureImporters, &textureImporters};
//   context.typeHooks = {&RegisterTextureTypeHooks, nullptr};
//   context.importTranslator = {&TranslateTextureImportArguments, nullptr};
namespace rocks {

// 임포터 종류다(12.12절 표 순서).
enum class TextureImportKind : uint8 {
    // 2D 한 파일(`texture` — 확장자 기본).
    Texture2D,
    // 6면 파일(`texture_cube` — 역할 px nx py ny pz nz).
    Cube,
    // 한 파일 Cube(`texture_cube_file` — DDS Cube 또는 배치).
    CubeFile,
    // slice 파일들(`texture_array` — 역할 slice:<i>).
    Array,
    // DDS 2DArray 한 파일(`texture_array_file`).
    ArrayFile,
    // cube 파일들(`texture_cube_array` — 역할 cube:<i>.<면>).
    CubeArray,
    // DDS CubeArray 한 파일(`texture_cube_array_file`).
    CubeArrayFile,
    // slice 파일들(`texture_volume` — 역할 slice:<i>).
    Volume,
    // 한 파일 Volume(`texture_volume_file` — DDS Volume 또는 타일).
    VolumeFile,
};

inline constexpr usize TextureImportKindCount = 9;

// 임포터 이름이다(`--importer=` 값·기록 importerName — kind 순서).
inline constexpr StringView TextureImporterNames[TextureImportKindCount] = {
    "texture",
    "texture_cube",
    "texture_cube_file",
    "texture_array",
    "texture_array_file",
    "texture_cube_array",
    "texture_cube_array_file",
    "texture_volume",
    "texture_volume_file"};

// Cube 면 수와 면 역할 이름이다(D3D 순서 +X −X +Y −Y +Z −Z — 12.3절).
inline constexpr uint32 TextureCubeFaces = 6;
inline constexpr StringView TextureCubeFaceRoles[TextureCubeFaces] = {"px", "nx", "py",
                                                                      "ny", "pz", "nz"};
// 가변 원본 역할 접두다(2DArray·Volume slice `slice:<i>`, CubeArray `cube:<i>.<면>`).
inline constexpr StringView TextureSliceRolePrefix = "slice:";
inline constexpr StringView TextureCubeRolePrefix = "cube:";

// 텍스처 임포터 하나다(kind가 정보·구성 규칙을 정한다). 수명: 표 등록 토큰보다 오래 산다.
class ROCKS_TEXTURETOOLS_API TextureImporter final : public AssetImporter {
public:
    explicit TextureImporter(TextureImportKind kind) noexcept;

    const AssetImporterInfo& Info() const noexcept override {
        return info_;
    }

    TextureImportKind Kind() const noexcept {
        return kind_;
    }

    Result<void> Import(AssetImportContext& context) noexcept override;

private:
    TextureImportKind kind_;
    AssetImporterInfo info_;
};

// 임포터 9개의 묶음이다(호스트가 명령 동안 쥔다 — RegisterTextureImporters의 state).
class ROCKS_TEXTURETOOLS_API TextureImporterSet {
public:
    TextureImporterSet() noexcept;

    TextureImporter& At(TextureImportKind kind) noexcept {
        return importers_[static_cast<usize>(kind)];
    }

private:
    TextureImporter importers_[TextureImportKindCount];
};

// state(TextureImporterSet*)의 임포터 9개를 table에 등록하고 토큰을 tokens에 둔다
// (commandlet_host::AssetImporterSetup 모양). 오류: null state InvalidArgument, 등록 오류(타입이
// 등록되지 않았으면 NotFound), 할당 실패.
ROCKS_TEXTURETOOLS_API Result<void>
RegisterTextureImporters(void* state, AssetImporterTable& table,
                         Array<AssetImporterRegistration>& tokens) noexcept;

// Texture의 설정 정규화 hook을 table에 등록한다(AssetTypeHookSetup 모양 — state는 쓰지 않는다).
// 오류: 등록 오류, 할당 실패.
ROCKS_TEXTURETOOLS_API Result<void>
RegisterTextureTypeHooks(void* state, AssetTypeHookTable& table,
                         Array<AssetTypeHookRegistration>& tokens) noexcept;

// texture의 설정을 UE와 같은 규칙으로 정규화한다(12.12절 설정 정규화 — UE PostEditChangeProperty의
// LODGroup 처리(assignedFields에 lodGroup이 있을 때), ValidateSettingsAfterImportOrEdit).
// isTexture2D는 출력 클래스가 Texture2D인지다(Resize 규칙). 두 번 불러도 결과가 같다.
ROCKS_TEXTURETOOLS_API void
NormalizeTextureSettings(Texture& texture, bool isTexture2D,
                         Span<const StringView> assignedFields) noexcept;

// 텍스처 임포트 요청이다(서비스 API 구조 — 에디터·도구가 명령과 같은 규칙으로 쓴다).
struct TextureImportSpec {
    TextureImportKind kind = TextureImportKind::Texture2D;
    // 원본 파일이다: Texture2D·*File은 하나, Cube는 면 순서 6개, Array·Volume은 slice 순(1개 이상),
    // CubeArray는 cube 순 × 면 순(6의 배수).
    Span<const StringView> files{};
    // CubeFile의 배치(None이면 DDS Cube)다.
    TextureCubeLayout cubeLayout = TextureCubeLayout::None;
    // VolumeFile의 타일(0×0이면 DDS Volume)이다.
    uint32 volumeTileX = 0;
    uint32 volumeTileY = 0;
    // PNG infill(ArrayFile·CubeArrayFile은 옵션이 없어 기본값만 받는다)이다.
    TextureImportPngInfill pngInfill = TextureImportPngInfill::OnlyOnBinaryTransparency;
};

// spec을 임포터 이름·원본 역할 목록·옵션 대입으로 바꾼다(out — 기본값과 같은 옵션은 넣지 않는다).
// 반환: spec 규칙(파일 수·타일 한쪽만 0·옵션 없는 임포터의 infill) 위반이면 false. 오류: 할당
// 실패.
ROCKS_TEXTURETOOLS_API Result<bool> MakeTextureImportRequest(const TextureImportSpec& spec,
                                                             AssetImportTranslation& out) noexcept;

// asset-import 예약 인자 번역기다(AssetImportTranslator 모양 — 12.12절 명령 문법 표). 예약 인자가
// 없고 원본 하나가 DDS면 머리의 차원으로 임포터를 고른다(파일을 읽지 못하면 바꾸지 않는다 — 실패는
// 임포트가 낸다). state는 쓰지 않는다.
ROCKS_TEXTURETOOLS_API Result<bool>
TranslateTextureImportArguments(void* state, Allocator& allocator,
                                Span<const AssetImportReservedArgument> reserved,
                                Span<const AssetImportSourceInput> sources, StringView importer,
                                AssetImportTranslation& out) noexcept;

} // namespace rocks

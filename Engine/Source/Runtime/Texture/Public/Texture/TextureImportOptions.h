#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 텍스처 임포트 옵션이다(Docs/Design/AssetSystem 12.12절 — TextureTools 임포터의 옵션 struct, 기록
// ImportRecord.options에 남아 재임포트가 같은 옵션을 쓴다). 에셋 명령 세션이 이미 등록하는 Texture
// 모듈에 둔다(UE ETextureImportPNGInfill도 런타임 폴더의 TextureUtilitiesCommon에 있다). 필드
// 이름은 텍스처 설정 이름·명령 예약 이름과 겹치지 않는다(옵션과 설정을 첫 이름으로 나눈다).
namespace rocks {

// PNG의 투명 흰색 texel 채우기다(UE ETextureImportPNGInfill — 값이 같다, 12.4절 원칙). UE의
// Default(0 — 프로젝트 설정을 따름)는 Rocks에 그 설정이 없어 두지 않는다.
ROCKS_ENUM()
enum class TextureImportPngInfill : uint8 {
    // 채우지 않는다(파일 그대로).
    Never = 1,
    // 알파가 0과 최대값뿐일 때만 알파 0 흰색 texel을 채운다(기본 — UE와 같다).
    OnlyOnBinaryTransparency = 2,
    // 알파 0 흰색 texel을 늘 채운다.
    Always = 3,
};

// 한 파일 Cube의 면 배치다(12.3절 표 — None이면 DDS Cube).
ROCKS_ENUM()
enum class TextureCubeLayout : uint8 {
    None = 0,
    // 4×3 칸 십자.
    HorizontalCross = 1,
    // 3×4 칸 십자(−Z는 180° 돌려 담는다).
    VerticalCross = 2,
    // 가로 6칸(면 순서 +X −X +Y −Y +Z −Z).
    Strip6 = 3,
};

// 원본 하나·여러 원본 임포터의 옵션이다(texture·texture_cube·texture_array·texture_cube_array·
// texture_volume).
ROCKS_STRUCT()
struct TextureImportOptions {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY()
    TextureImportPngInfill pngInfill = TextureImportPngInfill::OnlyOnBinaryTransparency;
};

// 한 파일 Cube 임포터(texture_cube_file)의 옵션이다.
ROCKS_STRUCT()
struct TextureCubeFileOptions {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() TextureCubeLayout cubeLayout = TextureCubeLayout::None;
    ROCKS_PROPERTY()
    TextureImportPngInfill pngInfill = TextureImportPngInfill::OnlyOnBinaryTransparency;
};

// 한 파일 Volume 임포터(texture_volume_file)의 옵션이다. 타일이 0×0이면 DDS Volume, 아니면 2D
// 그림을 타일(한 칸 volumeTileX × volumeTileY texel)로 나눈다.
ROCKS_STRUCT()
struct TextureVolumeFileOptions {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 volumeTileX = 0;
    ROCKS_PROPERTY() uint32 volumeTileY = 0;
    ROCKS_PROPERTY()
    TextureImportPngInfill pngInfill = TextureImportPngInfill::OnlyOnBinaryTransparency;
};

} // namespace rocks

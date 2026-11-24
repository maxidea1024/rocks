#pragma once

#include <Base/Types.h>

// 쿠킹된 셰이더 byte(라이브러리 v1, 바인딩 layout, GLES 바인딩 머리)를 읽은 결과다
// (Docs/Design/ShaderSystem 12.2.1절 SSD7·SSD26). 판정 이름은 AssetSystem 쿠킹 패키지 판정
// (CheckCookedPackageFile)과 같다: 형식 버전이 다르면 UnsupportedVersion, 그 밖의 위반(길이·magic·
// SHA·정렬·범위·0이 아닌 빈 byte)은 CorruptPackage다.
//
// 사용 예:
//   ShaderLibraryView library;
//   if (const ShaderReadStatus opened = library.Open(bytes); !opened) {
//       ROCKS_LOG(LogRender, Error, "shader library: {} ({})", ShaderReadErrorName(opened.error),
//                 opened.reason);
//   }
namespace rocks::shader {

enum class ShaderReadError : uint8 {
    None,
    UnsupportedVersion,
    CorruptPackage
};

struct [[nodiscard]] ShaderReadStatus {
    ShaderReadError error = ShaderReadError::None;
    // 위반 설명(영어 정적 문구)이다. 성공이면 빈 문자열이다.
    const char* reason = "";

    explicit operator bool() const noexcept {
        return error == ShaderReadError::None;
    }

    static ShaderReadStatus Success() noexcept {
        return ShaderReadStatus{};
    }

    static ShaderReadStatus Corrupt(const char* why) noexcept {
        return ShaderReadStatus{ShaderReadError::CorruptPackage, why};
    }

    static ShaderReadStatus Unsupported(const char* why) noexcept {
        return ShaderReadStatus{ShaderReadError::UnsupportedVersion, why};
    }
};

// 판정 이름("None"·"UnsupportedVersion"·"CorruptPackage", 정적 수명)이다. 범위 밖이면 "unknown".
ROCKS_SHADERCORE_API const char* ShaderReadErrorName(ShaderReadError error) noexcept;

} // namespace rocks::shader

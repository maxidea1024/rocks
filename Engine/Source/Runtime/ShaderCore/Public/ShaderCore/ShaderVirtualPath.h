#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// 셰이더 가상 경로 문법이다(Docs/Design/ShaderSystem 11.1절 SC2, UE CheckVirtualShaderFilePath
// 대응). 셰이더 타입의 원문 경로 검사(ShaderCore 등록부)와 소스 트리·전처리기(ShaderCompiler)가 이
// 규칙 하나를 쓴다.
//
// 가상 경로 규칙:
// - 루트 아래 파일이다: /Engine/…, /Project/…, /Plugin/<이름>/….
// - 성분은 비지 않은 ASCII [A-Za-z0-9_.-]이고 "."·".."이 아니다('\\'·빈 성분 거부).
// - 확장자: <루트>/Shared/ 아래는 .h(C++·HLSL 공용 헤더)만, 그 밖은 .rsf(엔트리)·.rsh(include).
// - 전체 길이는 MaxShaderVirtualPathBytes 미만이다.
// 가상 디렉터리(mount 이름)는 "/Engine", "/Project", "/Plugin/<이름>"이다(이름은 비지 않은 ASCII
// [A-Za-z0-9_-], MaxShaderPluginNameBytes 이하).
//
// 사용 예:
//   ShaderVirtualPathInfo info;
//   if (const ShaderVirtualPathCheck checked = CheckShaderVirtualPath(path, info); !checked) {
//       Log::Error(..., checked.reason);           // 영어 문구, 정적 수명
//   } else if (info.root == ShaderVirtualRoot::Plugin) { ... info.pluginName ... }
namespace rocks::shader {

// 가상 경로의 최대 byte 수(NUL 포함)다. 오류 위치 버퍼도 이 크기다.
inline constexpr usize MaxShaderVirtualPathBytes = 256;
// 플러그인 이름(/Plugin/<이름>)의 최대 byte 수(NUL 제외)다.
inline constexpr usize MaxShaderPluginNameBytes = 64;

// 가상 루트와 예약 디렉터리 이름이다.
inline constexpr char EngineShaderVirtualRoot[] = "/Engine";
inline constexpr char ProjectShaderVirtualRoot[] = "/Project";
inline constexpr char PluginShaderVirtualPrefix[] = "/Plugin/";
inline constexpr char GeneratedShaderDirectoryName[] = "Generated";
inline constexpr char SharedShaderDirectoryName[] = "Shared";

// 가상 루트 종류다.
enum class ShaderVirtualRoot : uint8 {
    Engine,
    Project,
    Plugin
};

enum class ShaderVirtualPathError : uint8 {
    None,
    // 경로 문법 위반(루트·성분·문자·길이).
    InvalidPath,
    // 확장자 규칙 위반.
    InvalidExtension,
    // 가상 디렉터리(mount 이름) 규칙 위반.
    InvalidDirectory
};

// 검사 결과다. reason은 영어 세부 문구(정적 수명)이고 성공이면 빈 문자열이다. pathTooLong이면 오류
// 위치에 경로를 싣지 않는다(버퍼에 들어가지 않는다).
struct [[nodiscard]] ShaderVirtualPathCheck {
    ShaderVirtualPathError error = ShaderVirtualPathError::None;
    const char* reason = "";
    bool pathTooLong = false;

    explicit operator bool() const noexcept {
        return error == ShaderVirtualPathError::None;
    }
};

// 가상 경로를 나눈 결과다. view는 검사한 경로를 가리킨다.
struct ShaderVirtualPathInfo {
    ShaderVirtualRoot root = ShaderVirtualRoot::Engine;
    // root가 Plugin이면 플러그인 이름이다.
    StringView pluginName;
    // 마지막 성분의 확장자("rsf"·"rsh"·"h")다.
    StringView extension;
    // <루트>/Shared/ 아래인가.
    bool shared = false;
};

// 가상 파일 경로를 검사하고 나눈다(위 규칙). mount 여부는 보지 않는다. 오류는 InvalidPath·
// InvalidExtension이다. 실패하면 info는 정해지지 않는다.
ROCKS_SHADERCORE_API ShaderVirtualPathCheck
CheckShaderVirtualPath(StringView path, ShaderVirtualPathInfo& info) noexcept;

// 가상 디렉터리(mount 이름)를 검사한다. 오류는 InvalidDirectory다.
ROCKS_SHADERCORE_API ShaderVirtualPathCheck
CheckShaderVirtualDirectory(StringView directory) noexcept;

} // namespace rocks::shader

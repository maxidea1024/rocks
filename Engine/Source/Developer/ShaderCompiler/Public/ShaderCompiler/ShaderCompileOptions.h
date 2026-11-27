#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Meta/Annotations.h>
#include <Object/ModuleRegistration.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>

// 셰이더 컴파일 옵션이다(Docs/Design/ShaderSystem 11.1절 SC9). job 키 6행에 리플렉션 기록
// (WriteReflected — 기본값 포함 전체, 필드 이름이 키와 trace에 들어간다)으로 들어간다. 명령
// (`shader-compile --option NAME=VALUE`)과 API는 같은 이름=값 파서를 쓴다.
//
// 사용 예:
//   ShaderCompileOptions options = DefaultShaderCompileOptions(format);
//   const StringView given[] = {"optimization=O1", "warningsAsErrors=false"};
//   if (auto parsed = ParseShaderCompileOptions(given, options); !parsed) {
//       // parsed.code, parsed.index(몇 번째 항목), parsed.name
//   }
//
// 스레드: 모든 함수는 재진입 가능하며 전역 상태를 쓰지 않는다.
namespace rocks::shader {

// 최적화 수준이다(DXC -O0–-O3, FXC /O0–/O3).
ROCKS_ENUM()
enum class ShaderOptimization : uint8 {
    O0 = 0,
    O1 = 1,
    O2 = 2,
    O3 = 3
};

// 행렬 저장 순서다. 7.2절 6항으로 행 우선 하나로 고정한다(값을 더하면 키 기록이 바뀐다).
ROCKS_ENUM()
enum class ShaderMatrixLayout : uint8 {
    Row = 0
};

// job 하나의 컴파일 옵션이다. 필드를 더하면 모든 셰이더 키가 바뀐다(리플렉션 기록).
ROCKS_STRUCT()
struct ShaderCompileOptions {
    ROCKS_GENERATED_BODY()

    // HLSL 2021(결정 2). 기본은 형식 정의 language.hlsl2021이다(DefaultShaderCompileOptions).
    ROCKS_PROPERTY() bool hlsl2021 = false;
    ROCKS_PROPERTY() ShaderOptimization optimization = ShaderOptimization::O3;
    // 디버그 심볼(값 3 — ShaderSystem 16.3절 SSD73). 켜면 형식이 심볼을 만들 수 있을 때 값 3이
    // 생긴다 (D3D PDB·SPIR-V debug module, 텍스트 형식은 없음).
    ROCKS_PROPERTY() bool debugSymbols = false;
    ROCKS_PROPERTY() ShaderMatrixLayout matrixLayout = ShaderMatrixLayout::Row;
    // 경고를 오류로 다룬다(ShaderSystem 16.4절 SSD79 — DXC -WX, FXC
    // D3DCOMPILE_WARNINGS_ARE_ERRORS). 엔진 셰이더는 true(결정 7), 머티리얼 job은 false를 준다
    // (UE r.Shaders.WarningsAsErrors 기본 0과 같다). false면 경고가 job을 실패시키지 않고 결과
    // 진단(ShaderCompileJobResult::diagnostics)에 남는다(너겟 적중 결과에는 없다).
    ROCKS_PROPERTY() bool warningsAsErrors = true;
};

// 옵션 이름의 최대 byte 수(NUL 포함, 오류 보고 버퍼)다.
inline constexpr usize MaxShaderOptionNameBytes = 64;

enum class ShaderOptionError : uint8 {
    None,
    // "이름=값" 모양이 아니다('=' 없음, 빈 이름).
    InvalidSyntax,
    // 모르는 옵션 이름이다.
    UnknownName,
    // 그 옵션이 받지 않는 값이다(bool은 true·false, enum은 열거자 이름).
    InvalidValue,
    // 같은 옵션을 두 번 주었다.
    Duplicate
};

// 진단용 열거자 이름이다(정적 수명). 범위 밖이면 "unknown".
ROCKS_SHADERCOMPILER_API const char* ShaderOptionErrorName(ShaderOptionError code) noexcept;

// 결과다. index는 문제 항목의 위치(0부터), name은 그 옵션 이름(잘릴 수 있다)이다.
struct [[nodiscard]] ShaderOptionStatus {
    ShaderOptionError code = ShaderOptionError::None;
    usize index = 0;
    char name[MaxShaderOptionNameBytes] = {};

    explicit operator bool() const noexcept {
        return code == ShaderOptionError::None;
    }
};

// 형식의 기본 옵션이다(hlsl2021은 형식 정의 값, 나머지는 struct 기본값).
ROCKS_SHADERCOMPILER_API ShaderCompileOptions
DefaultShaderCompileOptions(const ShaderFormatDefinition& format) noexcept;

// "이름=값" 목록을 options에 적용한다(주지 않은 옵션은 그대로). 이름은 필드 이름 그대로
// (hlsl2021·optimization·debugSymbols·matrixLayout·warningsAsErrors), 값은 bool `true`·`false`,
// enum 열거자 이름
// (`O0`–`O3`, `Row`)이고 공백을 받지 않는다. 오류: InvalidSyntax, UnknownName, InvalidValue,
// Duplicate. 실패하면 options는 바뀌지 않는다.
ROCKS_SHADERCOMPILER_API ShaderOptionStatus ParseShaderCompileOptions(
    Span<const StringView> assignments, ShaderCompileOptions& options) noexcept;

// 값이 범위 안인가(optimization O0–O3, matrixLayout Row — 밖이면 InvalidValue).
ROCKS_SHADERCOMPILER_API ShaderOptionStatus
ValidateShaderCompileOptions(const ShaderCompileOptions& options) noexcept;

// ShaderCompiler 리플렉션 타입(ShaderCompileOptions와 그 enum)을 등록한다(생성 집계 함수
// generated::ShaderCompiler::RegisterModuleTypes). job 키를 계산하는 쪽(관리자·시험)이 그
// TypeRegistry에 한 번 부른다.
ROCKS_SHADERCOMPILER_API Result<void>
RegisterShaderCompilerTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks::shader

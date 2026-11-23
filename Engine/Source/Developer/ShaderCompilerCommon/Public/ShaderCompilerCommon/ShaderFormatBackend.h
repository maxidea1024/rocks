#pragma once

#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCompilerCommon/ShaderWorkerProtocol.h>

// 형식 컴파일 인터페이스다(Docs/Design/ShaderSystem 11.1절 SC1, UE IShaderFormat 대응). 워커
// (ShaderWorker)가 형식 정의의 처리기(module)마다 구현(ShaderFormatD3D·ShaderFormatVulkan …)을 골라
// job을 맡긴다. 도구 묶음(ShaderToolSet)은 워커 쪽 모듈만 정의를 본다 — 이 헤더는 이름만
// 쓴다(도구를 여는 것은 워커 쪽뿐이다, 정책 shader-tool-binary-load).
//
// 구현 규칙:
// - 결과는 result에 둔다: 성공이면 값(ValueId 오름차순, 값 0 Code는 늘 있다), 실패면 상태와 진단
//   (영어, 도구 원문). 실패한 job은 값을 두지 않는다.
// - 경고는 오류다(결정 7). 같은 입력은 같은 byte를 낸다(10절 결정성).
// - 스레드: 한 워커가 한 job씩 부른다.
namespace rocks::shader {

class ShaderToolSet;
struct MetalToolchain;

// 처리기 호출 문맥이다(11.1절 SH1e E1). 포인터·view는 Compile 동안 살아 있어야 한다.
struct ShaderBackendContext {
    // 열린 저장소 도구 묶음이다(열지 못했으면 null — 처리기는 ToolError를 둔다).
    const ShaderToolSet* tools = nullptr;
    // 찾은 Metal 도구(system 도구)다. 찾지 못했으면 null이거나 found가 false다.
    const MetalToolchain* metal = nullptr;
    // 처리기 임시 디렉터리(UTF-8 절대 경로, 이미 있음)다. 외부 프로그램과 파일을 주고받는 처리기만
    // 쓰고, job마다 같은 이름을 다시 쓴다(한 워커가 한 job씩 부른다).
    StringView scratchDirectory;
};

// 셰이더 프로필 이름의 최대 byte 수(NUL 포함, 예: "cs_6_6")다.
inline constexpr usize MaxShaderProfileBytes = 16;

// DXC·FXC 프로필 이름("vs_6_6"·"ps_5_0"·"cs_6_0")을 out에 쓴다. stage가 ShaderStageBits 한 비트가
// 아니면 false다.
ROCKS_SHADERCOMPILERCOMMON_API bool MakeShaderProfile(uint32 stage, ShaderVersion model,
                                                      char (&out)[MaxShaderProfileBytes]) noexcept;

// 결과에 실패를 둔다(값을 비우고 상태·진단 — 진단은 message 뒤에 detail).
ROCKS_SHADERCOMPILERCOMMON_API void FailShaderJob(ShaderWorkerJobResult& result,
                                                  ShaderWorkerJobStatus status, StringView message,
                                                  StringView detail = StringView()) noexcept;

class ShaderFormatBackend {
public:
    virtual ~ShaderFormatBackend() = default;

    // 이 구현이 맡는 처리기다(형식 정의 processor.module).
    virtual ShaderFormatModule Module() const noexcept = 0;

    // 처리기 버전이다(형식 정의 processor.version과 같아야 한다 — 다르면 워커는 BadFormatVersion).
    virtual uint32 ProcessorVersion() const noexcept = 0;

    // format의 job을 컴파일한다. context는 도구·임시 디렉터리다(위 ShaderBackendContext). 결과
    // 버퍼는 result.values의 할당자에서 나온다.
    virtual void Compile(const ShaderFormatDefinition& format, const ShaderWorkerJob& job,
                         const ShaderBackendContext& context,
                         ShaderWorkerJobResult& result) noexcept = 0;
};

} // namespace rocks::shader

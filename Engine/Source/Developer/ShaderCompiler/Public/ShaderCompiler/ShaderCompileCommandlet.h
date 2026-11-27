#pragma once

#include <Base/Types.h>
#include <Core/Commandlet/Commandlet.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

// shader-compile 명령이다(Docs/Design/ShaderSystem 11.1절 SC13, UE ShaderCompileWorker를 부르는
// `-run=ShaderCompile` 계열 대응). 셰이더 관리자(ShaderCompileManager)로 job을 컴파일하고 결과를
// 알린다. 명령 함수가 ShaderCompiler 모듈에 있어 EngineTests가 프로세스 안에서 부른다.
//
//   shader-compile [--project=<dir>] --format=<id> [--format=<id>]...
//                  [--define=<NAME>=<VALUE>]... [--option=<name>=<value>]... [--out=<dir>]
//                  [--workers=<n>] [--no-cache] [--keep-input]
//                  <virtual path>:<entry>:<vertex|pixel|compute>...
//
//   - 원문: `/Engine` = `<engine>/Shaders`, --project가 있으면 `/Project` = `<project>/Shaders`
//     (있을 때)다. 너겟 root와 워커 작업 디렉터리 뿌리(`<root>/Intermediate/ShaderWorkers`)는
//     project, 없으면 엔진 폴더다.
//   - 형식: --format으로 준 형식들(`<engine>/Config/ShaderFormats`)이다. 하나 이상 있어야 한다
//     (엔진 컴파일 프로필은 없앴다. 호스트 플랫폼 TargetProfile 기본값은 그 칸이 생긴 뒤 붙인다 —
//     ShaderSystem 12.3.1절 SSD24).
//   - 출력: job마다 표준 출력에 `shader-compile: <format> <path>:<entry>:<stage> <key> <hit|miss>
//     <status>[ <file>]` 한 줄, 진단은 표준 오류다. --out이 있으면 성공한 코드를
//     `<out>/<format>/<name>.<entry>.<stage>.<ext>`(name은 가상 경로의 `/`를 `+`로 바꾼 것, ext는
//     backend별 dxil·dxbc·spv·metallib·essl)에 쓴다. --option=debugSymbols=true면 값 3(ShaderSystem
//     16.3절 SSD73)을 같은 폴더에 심볼 이름(D3D PDB), 이름이 없으면 <name>.<entry>.<stage>.spv로
//     쓴다. 끝에 관리자 계수 요약 한 줄이다(진행 줄은 찍지 않는다 — 진행 표시는 Progress 모듈 몫,
//     진행 보고 12절).
//   - 종료 코드: 모든 job 성공 0, job 실패·실행 실패 1, 사용법 오류 2.
namespace rocks::shader {

// 명령 문맥이다(commandlet_host가 건다).
struct ShaderCompileCommandContext {
    // 여러 스레드의 동시 할당을 받는 할당자다.
    Allocator* allocator = nullptr;
    // 엔진 폴더다. 비면 FindEngineDirectory다.
    StringView engineDirectory{};
    // 워커 실행 파일이다. 비면 이 실행 파일 옆의 ShaderCompileWorker다.
    StringView workerExecutable{};
};

ROCKS_SHADERCOMPILER_API Result<int32> RunShaderCompileCommand(void* context,
                                                               Span<const StringView> args,
                                                               CommandletOutput& output) noexcept;

} // namespace rocks::shader

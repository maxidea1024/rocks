#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>

// `sparks` CLI의 명령 실행이다(Docs/Design/SparksTools.ko.md 2절 — LC8). 프로그램 `sparks`는 표준
// 스트림에 쓰는 출력 대상으로 이 함수를 부르고, 시험은 버퍼에 쓰는 출력 대상으로 같은 프로세스에서
// 부른다.
//
// 사용 예:
//   class Console final : public sparks::tools::CommandOutput { ... };
//   Console console;
//   const int32 code = sparks::tools::RunCommand(allocator, platform::PhysicalFileSystem(),
//                                                context.Arguments(), console);
namespace rocks::sparks::tools {

// 명령의 종료 코드다(T4).
inline constexpr int32 ExitSuccess = 0; // 진단 없음·시험 통과·서식 같음
inline constexpr int32 ExitFailure = 1; // 진단·시험 실패·서식 다름·작업 공간 배치 오류
inline constexpr int32 ExitUsage = 2;   // 명령·옵션 사용법 오류
inline constexpr int32 ExitError = 3;   // 입출력·할당 실패

// 명령 출력 대상이다. text는 UTF-8이며 호출 동안만 유효하다.
class CommandOutput {
public:
    virtual ~CommandOutput() = default;

    // 결과(진단·덤프·요약)를 쓴다. 실패는 입출력 오류다(명령은 ExitError로 끝난다).
    virtual Result<void> WriteOutput(StringView text) noexcept = 0;

    // 사용법·작업 공간 오류 문장을 쓴다.
    virtual Result<void> WriteError(StringView text) noexcept = 0;
};

// arguments(명령 이름부터 — 실행 파일 제외)를 실행하고 종료 코드를 돌려준다. 원문·schema는 files로
// 읽는다. 명령:
//   check [--schema <폴더>]... <경로>...      패키지를 검사해 진단을 쓴다
//   dump (--ast|--names|--types|--ir|--bytecode) [--schema <폴더>]... <경로>...
//                                             모듈마다 덤프(`== <경로>` 머리 줄)를 쓴다
// 경로는 폴더(아래의 `SparksPackage.json5`가 있는 폴더마다 패키지) 또는 `.spark` 파일(그 파일의
// 패키지 — dump는 그 모듈만)이다. 출력은 결정적이다(경로 byte 순, 진단은 SL 8.1절 순서).
ROCKS_SPARKSTOOLS_API int32 RunCommand(Allocator& allocator, platform::FileSystem& files,
                                       Span<const StringView> arguments,
                                       CommandOutput& output) noexcept;

} // namespace rocks::sparks::tools

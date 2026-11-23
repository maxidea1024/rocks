#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/StringView.h>

// 셰이더 컴파일 워커 본체다(Docs/Design/ShaderSystem.ko.md 11절, 11.1절 SC4–SC7). 워커 프로그램
// (Programs/ShaderCompileWorker)이 부른다.
//
// 명령줄: --work-dir <dir> --parent-pid <pid> --worker <n> [--keep-input] [--test-hooks]
//
// 제어(입력 pipe = 부모 lifeline, 출력 pipe = 제어 줄 전용):
// 1. 입력 줄 `<seq>`를 받으면 `<work-dir>/input.<seq>`를 읽고 지운다(--keep-input이면 남긴다).
// 2. 머리(메시지 버전·도구 표 SHA·툴체인 지문·결정적 수학 버전·처리기 버전)를 자기 것과 대조해
//    다르면 출력 없이 ShaderWorkerExit 코드로 끝난다.
// 3. 요청을 처리해 `output.<seq>.tmp`에 쓰고 no-replace 이동으로 `output.<seq>`에 둔 뒤
//    `done <seq>`를 쓴다.
// 입력 EOF(부모가 닫음·부모 종료)면 자기가 띄운 자식을 정리하고 Success로 끝난다. 로그는
// `<work-dir>/worker.log`다.
//
// 시험 hook(--test-hooks로 띄운 워커만, SC7): Hold(`held <seq>`를 쓰고 다음 입력 줄까지 막힘),
// SpawnChild(자식을 띄우고 `child <pid>`를 쓴 뒤 Hold), Crash(접근 위반), Assert(Fatal).
//
// 사용 예(워커 프로그램 본문):
//   ShaderWorkerArguments arguments;
//   if (!ParseShaderWorkerArguments(context.Arguments(), arguments)) {
//       return static_cast<int>(ShaderWorkerExit::BadArguments);
//   }
//   return RunShaderWorker(arguments, StandardShaderWorkerControl(), context.DefaultAllocator());
namespace rocks::shader {

// 작업 디렉터리 경로의 최대 byte 수(NUL 포함)다.
inline constexpr usize MaxShaderWorkerPathBytes = 4096;
// 제어 줄의 최대 byte 수(NUL 포함)다.
inline constexpr usize MaxShaderWorkerControlLineBytes = 64;

struct ShaderWorkerArguments {
    // 프로그램 인자를 빌린다(프로세스 수명).
    StringView workDirectory;
    uint32 parentProcessId = 0;
    uint32 worker = 0;
    bool keepInput = false;
    bool testHooks = false;
};

// 명령줄을 읽는다. 모르는 인자·값 없는 옵션·중복·필수 옵션 없음(--work-dir·--parent-pid·--worker)·
// 숫자가 아닌 값은 false다.
ROCKS_SHADERWORKER_API bool ParseShaderWorkerArguments(Span<const StringView> arguments,
                                                       ShaderWorkerArguments& out) noexcept;

// 제어 줄 통로다(표준 입력·출력. 시험은 바꿔 끼울 수 있다).
class ShaderWorkerControl {
public:
    virtual ~ShaderWorkerControl() = default;
    // 줄 하나를 읽어 끝 줄바꿈(\r\n·\n)을 뗀다. EOF·읽기 실패면 false다. 줄이 capacity보다 길면
    // 잘린 앞부분이고 나머지는 버린다.
    virtual bool ReadLine(char* buffer, usize capacity, usize& bytes) noexcept = 0;
    // text와 '\n'을 쓰고 비운다. 실패하면 false다.
    virtual bool WriteLine(StringView text) noexcept = 0;
};

// 표준 입력·출력 통로다(프로세스에 하나).
ROCKS_SHADERWORKER_API ShaderWorkerControl& StandardShaderWorkerControl() noexcept;

// 워커 본체다. 반환값은 프로세스 종료 코드(ShaderWorkerExit)다. allocator는 메시지·결과 버퍼에
// 쓴다.
ROCKS_SHADERWORKER_API int32 RunShaderWorker(const ShaderWorkerArguments& arguments,
                                             ShaderWorkerControl& control,
                                             Allocator& allocator) noexcept;

} // namespace rocks::shader

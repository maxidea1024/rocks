#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

namespace rocks {

enum class CommandletStream : uint8 {
    Output,
    Error
};

// 출력은 호출 중에만 빌리는 UTF-8 view다. sink는 내용을 보관하려면 직접 복사한다.
// sink/context는 Run이 끝날 때까지 유효해야 하며 실패는 명시적인 Result로 전달한다.
struct CommandletOutput {
    using WriteFn = Result<void> (*)(void*, CommandletStream, StringView) noexcept;

    WriteFn write = nullptr;
    void* context = nullptr;
};

// args는 명령 이름을 제외한 원래 인수의 읽기 전용 view이며 Run 호출 중에만 유효하다.
// callback/context의 수명은 Registry의 마지막 실행보다 길어야 한다. callback은 noexcept다.
// 정상 종료 코드는 0..125다. 실행 오류는 Result 실패로 반환하고 호출자가 프로세스 오류로 변환한다.
using CommandletCallback = Result<int32> (*)(void*, Span<const StringView>,
                                             CommandletOutput&) noexcept;

struct CommandletDefinition {
    StringView name;
    StringView description;
    CommandletCallback execute = nullptr;
    void* context = nullptr;
};

// 렌더러·window·에셋·로그 서비스에 의존하지 않는 단일 소유 스레드 명령 registry다.
// 이름과 설명은 등록 시 내부에 복사한다. callback 코드와 context는 소유하지 않는다.
// 등록 및 실행은 생성 스레드에서만 가능하다. sink/callback의 실행 재진입과 실행 중 등록은
// Busy다. Count는 실행 중에도 허용한다.
// 종료는 Run을 마친 뒤 registry와 caller가 소유한 context를 순서대로 파괴한다.
class CommandletRegistry {
public:
    static constexpr usize Capacity = 64;
    static constexpr usize NameCapacity = 63;
    static constexpr usize DescriptionCapacity = 191;
    static constexpr usize MaxArguments = 128;
    static constexpr usize MaxArgumentBytes = 4096;

    CommandletRegistry() noexcept;
    CommandletRegistry(const CommandletRegistry&) = delete;
    CommandletRegistry& operator=(const CommandletRegistry&) = delete;

    // 소문자 ASCII, 숫자와 '-'로 된 이름만 허용한다. help/list는 예약 이름이다.
    // 중복·유효하지 않은 UTF-8·길이 초과·용량 초과 시 기존 등록을 변경하지 않는다.
    Result<void> Register(const CommandletDefinition&) noexcept;

    // args는 실행 파일 이름을 제외한다. --help/--list는 인수 없이 단독으로만 사용할 수 있다.
    // 빈 인수/알 수 없는 명령은 오류 출력을 시도하고 프로세스 사용법 오류 2를 반환한다.
    // 출력 실패, 잘못된 UTF-8, 잘못된 콜백 반환 등 API 오류는 Result 실패로 반환한다.
    Result<int32> Run(Span<const StringView> args, CommandletOutput output) noexcept;

    // 소유 스레드 전용이다. 실행 중 callback에서 조회할 수 있다. 객체 파괴와 경쟁하지 않는다.
    Result<usize> Count() const noexcept;

private:
    struct Entry {
        char name[NameCapacity + 1]{};
        char description[DescriptionCapacity + 1]{};
        usize nameBytes = 0;
        usize descriptionBytes = 0;
        CommandletCallback execute = nullptr;
        void* context = nullptr;
    };

    uint64 owner_ = 0;
    usize count_ = 0;
    bool running_ = false;
    Entry entries_[Capacity]{};
};

} // namespace rocks

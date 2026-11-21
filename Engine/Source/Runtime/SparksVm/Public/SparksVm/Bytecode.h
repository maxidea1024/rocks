#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/StringView.h>
#include <SparksVm/Opcode.h>

// 모듈 하나의 bytecode다(Docs/Design/SparksBytecode.ko.md 4절, LC4b). 컴파일러가 만들고 `.sparkc`
// 읽기(LC4c)·verifier·interpreter(LC5)가 읽는 형식의 메모리 표현이다. 표는 모두 모듈 하나에 하나씩
// 있고(상수·문자열·참조·switch·await 지점) 함수는 그 구간을 가리킨다. 같은 입력이면 표의 내용과
// 순서가 같다(결정적).
//
// 수명·스레드: 표는 생성 allocator를 빌린다. 다 만든 모듈은 여러 스레드가 읽어도 된다.

namespace rocks::sparks {

// 레지스터 bank 번호다(배열 첨자).
enum class BytecodeBank : uint8 {
    Scalar,
    Ref,
    Buf
};

inline constexpr uint32 BytecodeBankCount = 3;

// 표 번호가 없음을 나타낸다.
inline constexpr uint32 NoBytecodeIndex = NumericLimits<uint32>::Max();

// 참조 표 항목의 종류다(실행 중 결정·연결하는 이름). LC4d가 안정 ID를 더한다.
enum class BytecodeRefKind : uint8 {
    Function,         // 같은 모듈 함수(detail = 함수 표 번호)
    ImportedFunction, // 다른 모듈의 Sparks 함수
    VirtualFunction,  // open·override·interface 메서드(실행 중 수신자 타입으로 결정)
    NativeFunction,   // 엔진 schema 함수
    LanguageFunction, // 언어 함수(부록 B — Math·Log·Delay…)
    RpcFunction,      // RPC 송신 대상
    Field,            // 객체(class·엔진 object) 필드
    StructField,      // struct 값의 필드
    Type,             // 타입(생성·`as?`·enum 이름)
    Event,            // 이벤트(멤버·확장·엔진)
    Context,          // 문맥 이름(`World`·`Time`…)
    Literal           // 검증 리터럴(name = 종류, detail = 글자의 문자열 번호)
};

const char* BytecodeRefKindName(BytecodeRefKind kind) noexcept;

struct BytecodeRef {
    BytecodeRefKind kind = BytecodeRefKind::Function;
    uint32 name = NoBytecodeIndex;   // 표시 이름(`Owner.Name`)의 문자열 번호
    uint32 detail = NoBytecodeIndex; // 종류별 보조 번호(위 설명)
};

// 명령 단어 하나의 원문 위치다(분리 디버그 정보 — 1부터 센 줄·열).
struct BytecodeLocation {
    uint32 line = 0;
    uint32 column = 0;
};

struct BytecodeSwitchCase {
    int64 value = 0;
    uint32 target = 0; // 함수 안 명령 주소
};

struct BytecodeSwitchTable {
    uint32 firstCase = 0;
    uint32 caseCount = 0;
    uint32 defaultTarget = 0;
};

// await 지점에서 살아 있는 레지스터 구간이다(재개 때 보존 — verifier·GC 경계).
struct BytecodeLiveRange {
    BytecodeBank bank = BytecodeBank::Scalar;
    uint16 first = 0;
    uint16 count = 0;
};

struct BytecodeAwaitPoint {
    uint32 address = 0; // AWAIT·AWAITEV 명령의 함수 안 주소
    uint32 firstLive = 0;
    uint32 liveCount = 0;
};

struct BytecodeFunction {
    uint32 name = NoBytecodeIndex; // 표시 이름의 문자열 번호
    uint32 firstWord = 0;
    uint32 wordCount = 0;
    // bank마다 할당한 레지스터 수(= 호출 창 base)와 frame 크기(할당 수 + 가장 큰 창, 256 이하).
    uint16 registers[BytecodeBankCount] = {};
    uint16 frame[BytecodeBankCount] = {};
    // bank마다 매개변수 slot 수(this 포함 — 0번부터).
    uint16 parameters[BytecodeBankCount] = {};
    uint32 firstSwitch = 0;
    uint32 switchCount = 0;
    uint32 firstAwait = 0;
    uint32 awaitCount = 0;
};

class BytecodeModule final {
public:
    explicit BytecodeModule(Allocator& allocator) noexcept
        : scalars(allocator), stringBytes(allocator), stringOffsets(allocator),
          stringLengths(allocator), refs(allocator), functions(allocator), words(allocator),
          locations(allocator), switches(allocator), cases(allocator), awaits(allocator),
          liveRanges(allocator) {
    }

    BytecodeModule(BytecodeModule&&) noexcept = default;
    BytecodeModule& operator=(BytecodeModule&&) noexcept = default;

    StringView String(uint32 index) const noexcept {
        return StringView(stringBytes.Data() + stringOffsets[index], stringLengths[index]);
    }

    Array<uint64> scalars;   // Scalar 상수(정수·double 비트·이름 id)
    Array<char> stringBytes; // 문자열 표(escape를 푼 UTF-8)
    Array<uint32> stringOffsets;
    Array<uint32> stringLengths;
    Array<BytecodeRef> refs;
    Array<BytecodeFunction> functions;
    Array<uint32> words;               // 모든 함수의 명령 단어(함수는 구간)
    Array<BytecodeLocation> locations; // words와 같은 번호
    Array<BytecodeSwitchTable> switches;
    Array<BytecodeSwitchCase> cases;
    Array<BytecodeAwaitPoint> awaits;
    Array<BytecodeLiveRange> liveRanges;
};

} // namespace rocks::sparks

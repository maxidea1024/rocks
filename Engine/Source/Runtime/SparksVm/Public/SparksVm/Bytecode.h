#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
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

// content·interface hash의 byte 수다(SHA-256).
inline constexpr usize BytecodeHashBytes = 32;

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

// 참조 표 항목이다. 이름은 표시 이름이며 다른 모듈의 Sparks 선언은 `<import 경로>:` 접두를 붙인다
// (같은 이름의 두 모듈 선언을 구별한다 — LC4d가 안정 ID를 더한다).
struct BytecodeRef {
    BytecodeRefKind kind = BytecodeRefKind::Function;
    uint32 name = NoBytecodeIndex; // 표시 이름(`Owner.Name`)의 문자열 번호
    // 종류별 보조 번호: Function은 함수 표 번호, Literal은 리터럴 글자의 문자열 번호, 타입 인자를
    // 단 호출(`Find<Hp>`)은 첫 타입 인자의 Type 참조 번호.
    uint32 detail = NoBytecodeIndex;
    // Literal: 이름 지정 인자 표(문자열 번호 — `이름=bank글자;…`, 창에 놓는 순서), 없으면 NoIndex.
    uint32 arguments = NoBytecodeIndex;
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

// TYPE 구역(형식 버전 2 — SparksBytecode 5절)의 타입 종류다.
enum class BytecodeTypeKind : uint8 {
    Struct,
    Enum
};

// 이 모듈이 참조하는 Sparks struct·enum의 배치다. struct 값 버퍼는 bank 묶음
// 순(Scalar·Ref·Buf)이다.
struct BytecodeType {
    uint32 name = NoBytecodeIndex; // Type 참조와 같은 표시 이름의 문자열 번호
    BytecodeTypeKind kind = BytecodeTypeKind::Struct;
    uint32 firstField = 0;
    uint32 fieldCount = 0;
    uint16 slots[BytecodeBankCount] = {}; // struct의 bank별 slot 수
};

// struct 필드(enum은 case) 하나다. offset은 struct 내용의 절대 slot 위치다(enum은 0).
struct BytecodeTypeField {
    uint32 name = NoBytecodeIndex;
    uint8 bank = 0;
    uint8 slots = 0;
    uint32 offset = 0;
    uint8 flags = 0; // CLASS 필드만(형식 v4): BytecodeFieldHeld
    // CLASS 필드만(형식 v5 — SparksReload K4): 타입 표기(정규 표시 이름 — 다른 모듈 선언은 타입
    // 인자까지 `<경로>:` 접두)와
    // `@formerly` 옛 이름의 문자열 번호(없으면 NoIndex). reload의 필드 대응이 쓴다.
    uint32 type = NoBytecodeIndex;
    uint32 formerName = NoBytecodeIndex;
};

// CLASS 필드 표지다(SparksRuntime 2.6절): 유지 자원 필드(Ref 1 slot — Disable·Detach가 해제).
inline constexpr uint8 BytecodeFieldHeld = 1;

// 수명·물리 hook 종류다(SL 5.1절 — 클래스당 종류마다 하나).
enum class BytecodeHookKind : uint8 {
    Awake,
    Enable,
    Start,
    Tick,
    FixedTick,
    LateTick,
    Disable,
    Destroy,
    TriggerEnter,
    TriggerExit,
    CollisionEnter,
    CollisionExit,
    Validate,
    Count
};

const char* BytecodeHookKindName(BytecodeHookKind kind) noexcept;

// handler의 재진입 정책이다(SL 6.1절 — await가 있는 handler만 표시).
enum class BytecodeReentry : uint8 {
    None,
    SkipWhileBusy,
    Restart
};

// CLASS 구역(형식 버전 3 — Docs/Design/SparksRuntime.ko.md 2.1절)의 클래스 하나다. 범위는 이
// 클래스가 선언한 것만이다(기반 사슬은 session이 잇는다).
struct BytecodeClass {
    uint32 name = NoBytecodeIndex;
    uint32 baseClass = NoBytecodeIndex; // 같은 모듈 기반 클래스 번호(앞선 클래스)
    // 다른 모듈 Sparks 기반(`<경로>:Class`) 또는 엔진 기반 타입 이름의 문자열 번호.
    uint32 baseName = NoBytecodeIndex;
    uint32 firstField = 0; // classFields 범위(위치는 자기 bank 구역 안)
    uint32 fieldCount = 0;
    uint32 firstHook = 0;
    uint32 hookCount = 0;
    uint32 firstHandler = 0;
    uint32 handlerCount = 0;
    uint32 firstEvent = 0;
    uint32 eventCount = 0;
    uint16 slots[BytecodeBankCount] = {}; // 자기 필드의 bank별 slot 수
    uint32 initializer = NoBytecodeIndex; // `<Class>.$init` 함수 번호
};

struct BytecodeHook {
    BytecodeHookKind kind = BytecodeHookKind::Awake;
    uint32 function = NoBytecodeIndex;
};

struct BytecodeHandler {
    uint32 path = NoBytecodeIndex;  // 경로 글자(빈 글자는 자기 이벤트 — 표시용)
    uint32 event = NoBytecodeIndex; // 이벤트 이름(표시용)
    uint32 function = NoBytecodeIndex;
    BytecodeReentry reentry = BytecodeReentry::None;
    uint8 awaits = 0;
    // 경로 참조(handlerPaths 범위 — 마디마다 Field·Context 참조, SparksRuntime 2.3절 R7). Changed는
    // 마지막 마디가 감시 필드다.
    uint32 firstPath = 0;
    uint32 pathCount = 0;
    // 이벤트 참조(Event — Changed와 풀지 못한 경로는 NoBytecodeIndex).
    uint32 eventRef = NoBytecodeIndex;
};

struct BytecodeEvent {
    uint32 name = NoBytecodeIndex;
    uint32 arguments = NoBytecodeIndex; // 인자 서술자(피연산자 부분), 인자가 없으면 NoIndex
};

struct BytecodeFunction {
    uint32 name = NoBytecodeIndex; // 표시 이름의 문자열 번호
    uint32 firstWord = 0;
    uint32 wordCount = 0;
    // bank마다 할당한 레지스터 수(= 호출 창 base)와 frame 크기(할당 수 + 가장 큰 창, 256 이하).
    uint16 registers[BytecodeBankCount] = {};
    uint16 frame[BytecodeBankCount] = {};
    // bank마다 매개변수 slot 수(this 포함 — 0번부터). struct 메서드의 this는 Buf 0번이다.
    uint16 parameters[BytecodeBankCount] = {};
    // 반환 값의 bank와 slot 수(값 없음은 slot 0). RETERR가 이 모양의 Result를 만든다.
    uint8 returnBank = 0;
    uint8 returnSlots = 0;
    // 본문 hash(위치 주석을 뺀 IR 덤프의 SHA-256 — 재컴파일 범위, SparksBytecode 6.2절).
    uint8 bodyHash[BytecodeHashBytes] = {};
    uint32 firstSwitch = 0;
    uint32 switchCount = 0;
    uint32 firstAwait = 0;
    uint32 awaitCount = 0;
    // 유지 자원 지역의 Ref 구간(형식 v4 — SparksRuntime 2.6절 R18): 언제나 0이거나 유지 자원
    // handle이다. 매개변수 뒤·할당 수 안.
    uint16 heldFirst = 0;
    uint16 heldCount = 0;
};

class BytecodeModule final {
public:
    explicit BytecodeModule(Allocator& allocator) noexcept
        : scalars(allocator), stringBytes(allocator), stringOffsets(allocator),
          stringLengths(allocator), refs(allocator), types(allocator), typeFields(allocator),
          classes(allocator), classFields(allocator), hooks(allocator), handlers(allocator),
          handlerPaths(allocator), events(allocator), functions(allocator), words(allocator),
          locations(allocator), switches(allocator), cases(allocator), awaits(allocator),
          liveRanges(allocator) {
    }

    BytecodeModule(BytecodeModule&&) noexcept = default;
    BytecodeModule& operator=(BytecodeModule&&) noexcept = default;

    StringView String(uint32 index) const noexcept {
        return StringView(stringBytes.Data() + stringOffsets[index], stringLengths[index]);
    }

    // 머리(`.sparkc` 5절): compiler version·모듈 import 경로(문자열 번호), 원문 content hash, 공개
    // 선언의 interface hash(LC4d 전에는 0), manifest profile(0 core, 1 full).
    uint32 compiler = NoBytecodeIndex;
    uint32 path = NoBytecodeIndex;
    uint8 contentHash[BytecodeHashBytes] = {};
    uint8 interfaceHash[BytecodeHashBytes] = {};
    uint8 profile = 0;

    Array<uint64> scalars;   // Scalar 상수(정수·double 비트·이름 id)
    Array<char> stringBytes; // 문자열 표(escape를 푼 UTF-8)
    Array<uint32> stringOffsets;
    Array<uint32> stringLengths;
    Array<BytecodeRef> refs;
    // TYPE 구역(형식 버전 2): Type 참조의 보조 번호가 types 번호, StructField 참조의 보조 번호가
    // typeFields 번호다.
    Array<BytecodeType> types;
    Array<BytecodeTypeField> typeFields;
    // CLASS 구역(형식 버전 3): 클래스 필드의 위치는 자기 bank 구역 안이다.
    Array<BytecodeClass> classes;
    Array<BytecodeTypeField> classFields;
    Array<BytecodeHook> hooks;
    Array<BytecodeHandler> handlers;
    Array<uint32> handlerPaths; // handler 경로 마디의 참조 번호
    Array<BytecodeEvent> events;
    Array<BytecodeFunction> functions;
    Array<uint32> words;               // 모든 함수의 명령 단어(함수는 구간)
    Array<BytecodeLocation> locations; // words와 같은 번호
    Array<BytecodeSwitchTable> switches;
    Array<BytecodeSwitchCase> cases;
    Array<BytecodeAwaitPoint> awaits;
    Array<BytecodeLiveRange> liveRanges;
};

// 모듈의 bytecode 덤프(Docs/Design/SparksBytecode.ko.md 4.6절 `.bc`)를 out에 붙인다:
// 상수·문자열·참조 표, 함수마다 머리 줄(bank별 할당 수/frame 크기, 매개변수 slot 수)·await
// 지점·switch 표·명령(주소, opcode 이름, 피연산자, 원문 위치). 결정적이다. 실패: out의 할당 실패는
// 그 오류.
Result<void> AppendBytecodeDump(StringBuilderBase& out, const BytecodeModule& module) noexcept;

} // namespace rocks::sparks

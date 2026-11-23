#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Source.h>

// Sparks 컴파일 진단이다(Docs/Design/SparksLanguage 8.1절).
//
// 진단은 오류뿐이다(경고 없음). 진단 하나는 코드(S0100–S1199), 원문 위치, 영어 메시지, 설명, 수정
// 예를 가진다. DiagnosticList가 모으고 8.1절 순서로 정렬하며, AppendDiagnostics가 8.1절 형식으로
// 쓴다.
//
// 사용 예:
//   DiagnosticList diagnostics(allocator);
//   auto added = diagnostics.TryAdd(DiagnosticCode::InvalidUtf8, id, SourceSpan{7, 8},
//                                   "Source is not valid UTF-8.", "explanation", "fix");
//   diagnostics.Sort(sources);
//   StringBuilder<256> out(allocator);
//   auto written = AppendDiagnostics(out, sources, diagnostics);
//
// 출력 형식(각 줄은 LF로 끝난다):
//   Door.spark:3:5: error S0101: Source is not valid UTF-8.
//       let x = ?;
//           ^
//     설명 줄(여러 줄이면 줄마다 2칸 들여쓰기)
//     fix: 수정 예
// 원문 위치가 없는 진단(파일 전체)은 "경로: error S코드: 메시지"와 설명·수정 예만 쓴다.

namespace rocks::sparks {

// 진단 코드다. 값이 표시 번호다(S0101 → 101). 대역 배정은 8.1절, 엔진 공통 등록부는
// Docs/Design/CompilerDiagnostics.ko.md "진단 코드 등록부". 한 번 정한 번호의 뜻은 바꾸지 않는다.
enum class DiagnosticCode : uint16 {
    InvalidUtf8 = 101,
    LoneCarriageReturn = 102,
    SourceTooLarge = 103,
    InvalidCharacter = 104,
    UnderscoreInIdentifier = 105,
    UnterminatedBlockComment = 106,
    UnterminatedString = 107,
    InvalidEscape = 108,
    IntegerLiteralTooLarge = 109,
    MalformedNumber = 110,
    StringInInterpolation = 111,
    RangeOperator = 112,
    FloatLiteralOutOfRange = 113,
    NumericSuffix = 114,
    ExpectedToken = 201,
    ExpectedExpression = 202,
    ExpectedDeclaration = 203,
    ExpectedMember = 204,
    ExpectedStatement = 205,
    ExpectedType = 206,
    ExpectedName = 207,
    MisplacedDocComment = 208,
    ChainedComparison = 209,
    ImportAfterDeclaration = 210,
    InvalidAssignmentTarget = 211,
    MisplacedAwait = 212,
    MisplacedExit = 213,
    MisplacedAttribute = 214,
    NestingTooDeep = 215,
    RangeOutsideFor = 216,
    MisplacedDefault = 217,
    MixedCollectionLiteral = 218,
    EmptyEnum = 219,
    UnknownModule = 301,
    UndeclaredDependency = 302,
    DuplicateImport = 303,
    SelfImport = 304,
    RocksImport = 305,
    NamingRule = 306,
    ReservedName = 307,
    HookName = 308,
    DuplicateDeclaration = 309,
    NameConflict = 310,
    ImportConflict = 311,
    MemberTypeName = 312,
    HidesInheritedMember = 313,
    OverrideWithoutTarget = 314,
    OverrideSignatureMismatch = 315,
    MisplacedOpen = 316,
    UnknownName = 317,
    UnknownType = 318,
    NotAType = 319,
    TypeArgumentCount = 320,
    ContextNameUnavailable = 321,
    InvalidBase = 322,
    InheritanceCycle = 323,
    InterfaceNotImplemented = 324,
    InvalidService = 325,
    UnknownMember = 326,
    TypeMismatch = 401,
    NotCallable = 402,
    ArgumentCount = 403,
    ArgumentForm = 404,
    NotAValue = 405,
    InvalidOperator = 406,
    NotComparable = 407,
    ConditionNotBool = 408,
    MissingReturn = 409,
    ReturnValue = 410,
    TypeArgumentRule = 411,
    MayBeNull = 412,
    LevelScriptRequiredObject = 413,
    NotOptional = 414,
    SwitchCoverage = 415,
    DuplicateCase = 416,
    NoExpectedType = 417,
    MixedCollectionTypes = 418,
    InterpolationType = 419,
    InvalidCast = 420,
    AwaitTryOperand = 421,
    Unawaited = 422,
    ThisMisuse = 423,
    ValidatedLiteralForm = 424,
    StaticInstanceMismatch = 425,
    RecursiveStruct = 426,
    InitializerTooDeep = 427,
    ObjectFieldNeedsOptional = 428,
    StructDefaultCycle = 429,
    IntegerDivision = 501,
    FloatToIntConversion = 502,
    ConstantOverflow = 503,
    ConstantDivisionByZero = 504,
    NonFiniteConstant = 505,
    UnsignedNegation = 506,
    MixedNumericTypes = 507,
    InvalidConversion = 508,
    FloatConstantInInteger = 509,
    NotConstant = 510,
    ConstCycle = 511,
    ConstType = 512,
    AwaitOutsideAsync = 601,
    ContextEffect = 602,
    WhileWithoutWait = 603,
    UnknownAttribute = 620,
    AttributeNotAllowed = 621,
    DuplicateAttribute = 622,
    AttributeArgument = 623,
    LetWrite = 701,
    ParameterWrite = 702,
    ForeignFieldWrite = 703,
    ReadOnlyProperty = 704,
    StructSelfWrite = 705,
    NotAssignable = 706,
    UnusedLocal = 710,
    UnreadStore = 711,
    UnusedParameter = 712,
    DiscardedResult = 713,
    UnusedImport = 714,
    AssignToCallResult = 731,
    LevelScriptPhysicsHook = 801,
    LevelScriptCollider = 802,
    UnknownHook = 803,
    HookUnavailable = 804,
    HookSignature = 805,
    DuplicateHandler = 806,
    UnknownEventSource = 807,
    UnknownEvent = 808,
    ChangedUnavailable = 809,
    HandlerSignature = 810,
    EmitNotAllowed = 811,
    EventCall = 812,
    ExtensionEventTarget = 813,
    DuplicateExtensionEvent = 814,
    ExtensionEventNotImported = 815,
    MissingReentrancyPolicy = 816,
    MisplacedReentrancyPolicy = 817,
    DiscardedResource = 901,
    ResourcePlacement = 902,
    ResourceMoved = 903,
    ResourceDeclaration = 904,
    ResourceWithoutWait = 905,
    NotSaveable = 1001,
    NotReplicable = 1002,
    NetWriteOutsideAuthority = 1003,
    RpcCall = 1004,
    DataIdLost = 1005,
    InvalidIdManifest = 1006,
    IdNameReserved = 1007,
    IdClaimedTwice = 1008,
    ManifestSyntax = 1101,
    UnknownManifestKey = 1102,
    MissingManifestKey = 1103,
    InvalidManifestValue = 1104,
    UnsupportedLanguageVersion = 1105,
    MissingDependency = 1106,
    DependencyCycle = 1107,
    InvalidModulePath = 1108,
    ModulePathCaseConflict = 1109,
    ModuleFolderConflict = 1110,
    PackageFolderConflict = 1111,
    DuplicatePackage = 1112,
    FullProfileOnly = 1113,
    InvalidBindingSchema = 1120,
    FunctionTooLarge = 1150
};

// 진단 코드 표기의 숫자 자리 수다("S0412").
inline constexpr usize DiagnosticCodeDigits = 4;

// 표시 번호를 "S0412" 형태로 out에 붙인다. 실패: out의 할당 실패.
ROCKS_SPARKSCOMPILER_API Result<void> AppendDiagnosticCode(StringBuilderBase& out,
                                                           uint32 number) noexcept;

// 코드 표의 한 행이다. name은 kebab-case 영어 이름이며 사람이 코드를 찾을 때 쓴다.
struct DiagnosticCodeInfo {
    DiagnosticCode code;
    StringView name;
};

// 정의된 모든 코드다(번호 오름차순).
ROCKS_SPARKSCOMPILER_API Span<const DiagnosticCodeInfo> AllDiagnosticCodes() noexcept;

// 표시 번호로 코드를 찾는다. 없는 번호면 nullptr이다(.expect 해석 등 외부 입력용).
ROCKS_SPARKSCOMPILER_API const DiagnosticCodeInfo* FindDiagnosticCode(uint32 number) noexcept;

// 진단 하나다. DiagnosticList만 만든다.
class Diagnostic final {
public:
    Diagnostic(Diagnostic&&) noexcept = default;
    Diagnostic& operator=(Diagnostic&&) noexcept = default;
    Diagnostic(const Diagnostic&) = delete;
    Diagnostic& operator=(const Diagnostic&) = delete;
    ~Diagnostic() noexcept = default;

    DiagnosticCode Code() const noexcept {
        return code_;
    }

    SourceId Source() const noexcept {
        return source_;
    }

    // 원문 위치가 있는지다. 없으면(파일 전체 진단) Span()은 의미가 없다.
    bool HasSpan() const noexcept {
        return hasSpan_;
    }

    SourceSpan Span() const noexcept {
        return span_;
    }

    // 한 문장 영어 메시지다(마침표로 끝난다).
    StringView Message() const noexcept {
        return message_.View();
    }

    // 원인 설명이다. 비어 있을 수 있고 여러 줄이면 LF로 나뉜다.
    StringView Explanation() const noexcept {
        return explanation_.View();
    }

    // 수정 예다. 비어 있을 수 있다.
    StringView Fix() const noexcept {
        return fix_.View();
    }

private:
    friend class DiagnosticList;

    explicit Diagnostic(Allocator& allocator) noexcept;

    String message_;
    String explanation_;
    String fix_;
    SourceSpan span_;
    SourceId source_;
    uint32 sequence_ = 0;
    DiagnosticCode code_ = DiagnosticCode::InvalidUtf8;
    bool hasSpan_ = false;
};

// 한 번의 컴파일이 낸 진단 목록이다. 한 스레드에서 쓴다. allocator가 목록보다 오래 살아야 한다.
class ROCKS_SPARKSCOMPILER_API DiagnosticList final {
public:
    explicit DiagnosticList(Allocator& allocator) noexcept;

    DiagnosticList(const DiagnosticList&) = delete;
    DiagnosticList& operator=(const DiagnosticList&) = delete;
    DiagnosticList(DiagnosticList&&) = delete;
    DiagnosticList& operator=(DiagnosticList&&) = delete;
    ~DiagnosticList() noexcept = default;

    // 원문 위치가 있는 진단을 더한다. message는 비어 있으면 안 된다. explanation·fix는 비어 있으면
    // 출력에서 생략한다. message와 fix는 한 줄이고 explanation만 LF로 여러 줄을 쓸 수 있다.
    // 문자열은 복사한다. 실패: message가 비었거나, message·fix에 LF가 있거나, 문자열이 UTF-8이
    // 아니거나, span.begin > span.end면 InvalidArgument, 할당 실패는 그 오류. 실패하면 목록은
    // 바뀌지 않는다.
    Result<void> TryAdd(DiagnosticCode code, SourceId source, SourceSpan span, StringView message,
                        StringView explanation, StringView fix) noexcept;

    // 원문 위치가 없는 진단(파일 전체)을 더한다. 실패 규칙은 TryAdd와 같다.
    Result<void> TryAddFileLevel(DiagnosticCode code, SourceId source, StringView message,
                                 StringView explanation, StringView fix) noexcept;

    usize Count() const noexcept {
        return items_.Size();
    }

    bool IsEmpty() const noexcept {
        return items_.IsEmpty();
    }

    const Diagnostic& operator[](usize index) const noexcept {
        return items_[index];
    }

    Span<const Diagnostic> View() const noexcept {
        return items_.View();
    }

    // 8.1절 순서로 정렬한다: 파일 경로 byte 순 → 시작 위치(파일 전체 진단이 먼저) → 코드 → 더한
    // 순서. sources는 진단이 가리키는 파일을 모두 가진 SourceSet이어야 한다. 할당하지 않는다.
    void Sort(const SourceSet& sources) noexcept;

private:
    friend class SourceSet;

    // SourceSet::TryAdd가 원자적으로 쓰는 두 단계다. Prepare는 진단을 만들기만 하고(목록 불변),
    // Commit은 미리 확보한 용량에 넣는다(실패하지 않는다).
    Result<Diagnostic> TryPrepare(DiagnosticCode code, SourceId source, bool hasSpan,
                                  SourceSpan span, StringView message, StringView explanation,
                                  StringView fix) noexcept;
    Result<void> TryReserveMore(usize count) noexcept;
    void Commit(Diagnostic&& diagnostic) noexcept;

    Allocator* allocator_;
    Array<Diagnostic> items_;
    uint32 nextSequence_ = 0;
};

// 진단과 같은 형식(8.1절)의 보고 하나를 out에 붙인다: 첫 줄 `경로:줄:열: error <code>: <message>`
// (hasSpan이 거짓이면 `경로: error <code>: <message>`), 원문 줄과 밑줄, 설명 줄, `fix:` 줄. code는
// S 코드 표기 대신 쓰는 글자다(도구의 결과 — 예: `sparks test`의 `AssertFailed`,
// SparksTools 9.5절). message·fix는 한 줄, explanation은 LF로 여러 줄, 비면 그 줄을 생략한다.
// span은 source 원문 안이어야 한다(밖이면 Fatal — SourceFile::Locate). 실패: out의 할당 실패(out에
// 일부가 남을 수 있다).
ROCKS_SPARKSCOMPILER_API Result<void>
AppendSourceReport(StringBuilderBase& out, const SourceSet& sources, SourceId source, bool hasSpan,
                   SourceSpan span, StringView code, StringView message, StringView explanation,
                   StringView fix) noexcept;

// 진단 하나를 8.1절 형식으로 out에 붙인다. 원문 줄은 제어 문자를 공백으로, 잘못된 UTF-8 byte를
// U+FFFD로 바꿔 보인다(출력은 항상 UTF-8이다).
// 실패: out의 할당 실패. 실패하면 out에 일부가 남을 수 있다(호출자가 버린다).
Result<void> AppendDiagnostic(StringBuilderBase& out, const SourceSet& sources,
                              const Diagnostic& diagnostic) noexcept;

// 목록 전체를 지금 순서대로 붙인다(정렬은 하지 않는다 — 먼저 Sort를 부른다).
ROCKS_SPARKSCOMPILER_API Result<void> AppendDiagnostics(StringBuilderBase& out,
                                                        const SourceSet& sources,
                                                        const DiagnosticList& diagnostics) noexcept;

} // namespace rocks::sparks

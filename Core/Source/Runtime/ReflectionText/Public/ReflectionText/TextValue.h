#pragma once

#include <Object/PropertyPath.h>
#include <Object/TypeRegistry.h>
#include <ReflectionText/TextCodecOptions.h>
#include <ReflectionText/TextReference.h>

// 리플렉션 값의 명령행 표기다(리플렉션 B9i3b — Docs/Design/ReflectionExtensions.ko.md 10.4절). 필드
// 경로 하나(`Downscale.perPlatform[Android]`)를 자리(TextSlot)로 풀고, 그 자리의 값 텍스트 하나를
// 값(TextValue)으로 읽거나 값을 텍스트로 쓴다. 대입 묶음(`<경로>=<값>` 여러 개를 모아 게시 —
// PrepareAssignments)은 이 함수들 위에 있다(B9i3c).
//
// 표기(10.4절 표):
//   bool on·off·true·false(출력 true·false) / 정수 정규 10진 / 실수 10진·nan·inf·-inf(출력 최단
//   왕복) / enum 도구 이름(대소문자 무시·접두) / Flags `A+B`(입력 `A|B`도, 0은 빈 값) /
//   Guid·AssetId 36자 소문자 / Digest256 64자 소문자 / String 그대로 / Name 그대로(NFC 정규화 후
//   intern, None 출력 `None`) / ObjectRef·WeakRef·Soft resolver(null `none`) / ClassRef 타입 도구
//   이름·TypeId(null `none`) / Array·StaticArray·Set 쉼표 목록(빈 값 = 빈 컨테이너) / Map `k:v`
//   쉼표 목록 / Optional 내부 표기(struct 내부는 `{}` — unset은 대입 종류 Unset) / struct
//   `{}`(기본값) / InstancedStruct 타입 도구 이름(그 struct 기본 값으로 형 바꾸기 — 멤버는 경로
//   `<f>.<m>=v`), 비우기 `none`(리플렉션 B9j).
//
// 목록 escape 표(codec 전체에 하나 — TextListEscapeCharacters): 목록 원소 안의 `,`·`;`는 `\,`·`\;`,
// Map 항목은 처음 나오는 escape되지 않은 `:`에서 나누고 키 안의 `:`만 `\:`로 쓴다(값 안의 `C:\x`는
// escape 없이 쓴다). 그 밖의 `\`는 글자 그대로다. 목록이 아닌 단일 값 텍스트에는 escape가 없다.
// 목록 원소의 앞뒤 공백은 지우지 않는다(`a, b`의 둘째 원소는 ` b`다).
namespace rocks::reflection_text {

// 명령행 목록의 escape 글자다(Core StringSplit의 SplitOptions::escapeCharacters로 쓴다).
inline constexpr StringView TextListEscapeCharacters = ",;:";

// 경로가 가리키는 자리의 종류다.
enum class TextSlotPart : uint8 {
    // 필드 자체(최상위 필드·struct 멤버) — 값 텍스트는 필드 종류 전체의 표기(목록·`{}` 포함).
    Field,
    // Array·StaticArray 원소(`a[i]`).
    Element,
    // Map 항목의 값(`m[k]`).
    MapValue,
    // Set 원소(`s[k]`) — 값이 없다(대입 값으로 쓸 수 없다. 넣기·지우기는 대입 묶음이 한다).
    SetElement
};

// 경로 하나가 가리키는 자리다(DescribeTextPath·ResolveTextPath). field·enum·struct view는 해석에 쓴
// TypeLease의 수명을 따른다(lease를 쥔 동안만 쓴다).
struct TextSlot {
    // 경로 끝 단계의 필드(멤버) 스키마.
    const FieldSchema* field = nullptr;
    TextSlotPart part = TextSlotPart::Field;
    // 필드의 대상 enum(Enum 필드·Enum 원소·Map 값·Optional 내부 — TypeLease::FieldEnum).
    const EnumSchema* valueEnum = nullptr;
    // Map 키 enum(TypeLease::FieldKeyEnum).
    const EnumSchema* keyEnum = nullptr;
    // 필드의 대상 struct(struct 필드·struct 원소·Map 값 struct·Optional<struct> 내부).
    StructView structView{};
    // 경로 단계마다 자기 플래그의 합(OR)이다: 끝의 유효 플래그(PropagateFieldFlags)에 조상의
    // 전파되지 않는 플래그(DuplicateTransient 등)를 더한 값이라, 용도 판정(TextUse.h)을 이 값
    // 하나로 하면 경로의 어느 단계든 제외되는지 본다.
    uint32 flags = 0;
};

// 단일 값 하나의 형식이다(enumSchema는 type이 Enum일 때).
struct TextScalarType {
    ValueType type{};
    const EnumSchema* enumSchema = nullptr;
};

// 자리의 단일 값 형식이다: Field면 단일 값 필드의 형식(Optional은 내부, 컨테이너는 원소·Map 값),
// Element·MapValue·SetElement면 원소·값 형식이다. Map 키·Set 원소는 SlotKeyType이다.
TextScalarType SlotValueType(const TextSlot& slot) noexcept;

// Map 키(키 enum 포함)·Set 원소의 형식이다(그 밖의 필드는 Opaque).
TextScalarType SlotKeyType(const TextSlot& slot) noexcept;

// 값 텍스트 하나의 모양이다.
enum class TextValueShape : uint8 {
    // 값이 없다(초기값·해석 실패).
    Empty,
    // 단일 값 하나(single) — 단일 값 필드·원소·Map 값·Optional 내부.
    Single,
    // `{}` — struct·Optional<struct>·struct 원소·Map 값 struct를 기본값으로(B9i3c가 만들거나
    // 초기화).
    StructDefault,
    // 목록(elements) — Array·StaticArray·Set 필드.
    List,
    // Map 항목 목록(elements = 키, values = 값 — 같은 순서·같은 수).
    Entries,
    // InstancedStruct 자리의 형 바꾸기(리플렉션 B9j — 텍스트는 타입 도구 이름, single.guid = 그
    // struct ID로 그 struct 기본 값을 만든다) 또는 비우기(텍스트 `none`, single.guid = 0).
    InstancedType
};

// 값 텍스트 하나를 읽은 값, 또는 텍스트로 쓸 값이다. FieldValue는 allocator를 빌린다(값보다 오래
// 살아야 한다). 이동만 된다.
class TextValue {
public:
    explicit TextValue(Allocator& allocator) noexcept
        : single(allocator), elements(allocator), values(allocator) {
    }

    TextValue(TextValue&&) noexcept = default;
    TextValue& operator=(TextValue&&) noexcept = default;
    TextValue(const TextValue&) = delete;
    TextValue& operator=(const TextValue&) = delete;

    // 모양을 Empty로 하고 원소를 비운다(버퍼는 재사용한다).
    void Reset() noexcept {
        shape = TextValueShape::Empty;
        single.kind = ValueKind::Opaque;
        single.present = true;
        elements.Clear();
        values.Clear();
    }

    TextValueShape shape = TextValueShape::Empty;
    FieldValue single;
    Array<FieldValue> elements;
    Array<FieldValue> values;
};

// 텍스트 codec 호출 하나의 문맥이다: 옵션(용도·resolver·Name pool·한도), TypeRegistry(enum·ClassRef
// 조회), 진단 모음, 값·임시 버퍼 allocator, 입력 byte 누계, Soft·ClassRef 경로 key의 hook.
//
// 한도 셈법(TextCodecOptions):
//   - maxInputBytes: 이 문맥으로 해석한 경로 텍스트·값 텍스트 byte의 누계다(ResolveTextPath·
//     ParseTextValue·ParseTextScalar가 입력마다 더한다 — 대입 묶음은 Prepare와 그 Append가 문맥
//     하나를 쓰므로 묶음 전체의 누계다. JSON import는 문자열·키·숫자 토큰 byte를 더한다). 넘으면
//     그 호출이 CapacityExceeded이고 진단은 남기지 않는다.
//   - maxElements: 목록 텍스트 하나(쉼표 목록·Map 항목 목록)의 원소 수다. 원소를 만들기 전에 센다.
//     넘으면 진단 OutOfRange(필드 MaxElements 초과도 OutOfRange, StaticArray의 원소 수가 N이
//     아니면 InvalidValue).
//   - maxJsonDepth: JSON import의 중첩 깊이(B9i4).
//
// 수명: types·options·diagnostics·allocator와 options.resolver·names는 문맥보다 오래 살아야 한다.
// 옮길 수 없다(KeyHook이 문맥을 가리킨다). 스레드: 한 스레드에서 쓴다(resolver·TypeRegistry의
// 스레드 규칙도 따른다).
//
// 사용 예:
//   TextDiagnostics diagnostics(allocator);
//   TextCodecContext context(types, options, diagnostics, allocator);
//   TextPath path;
//   auto resolved = ResolveTextPath(context, lease, "SourceColorSettings.ColorSpace",
//                                   TextSource::Assignment(0), path);
//   TextValue value(allocator);
//   if (resolved && resolved.Value()) {
//       auto parsed = ParseTextValue(context, path.slot, "sRGB", TextSource::Assignment(0),
//                                    "SourceColorSettings.ColorSpace", value);
//   }
class TextCodecContext {
public:
    TextCodecContext(TypeRegistry& types, const TextCodecOptions& options,
                     TextDiagnostics& diagnostics, Allocator& allocator) noexcept;

    TextCodecContext(const TextCodecContext&) = delete;
    TextCodecContext& operator=(const TextCodecContext&) = delete;
    TextCodecContext(TextCodecContext&&) = delete;
    TextCodecContext& operator=(TextCodecContext&&) = delete;

    // 옵션을 검사하고(ValidateTextCodecOptions) 진단 모음 한도를 options.maxDiagnostics로 정한다.
    // 아래 함수들이 처음 쓸 때 부르므로 직접 부르지 않아도 된다(오류를 먼저 보려면 부른다). 오류:
    // InvalidArgument(옵션).
    Result<void> Start() noexcept;

    // 입력 byte를 누계에 더한다. 오류: 누계가 maxInputBytes를 넘으면 CapacityExceeded(누계는
    // 바뀌지 않는다).
    Result<void> ChargeInput(usize bytes) noexcept;

    uint64 ConsumedInputBytes() const noexcept {
        return consumed_;
    }

    TypeRegistry& Types() const noexcept {
        return *types_;
    }

    const TextCodecOptions& Options() const noexcept {
        return *options_;
    }

    TextDiagnostics& Diagnostics() const noexcept {
        return *diagnostics_;
    }

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    // Soft·ClassRef 경로 key 텍스트의 hook(ParsePropertyPath·FormatPropertyPath에 준다 — 리플렉션
    // B9f3). key 텍스트 규칙은 값 표기와 같다(ClassRef 타입 도구 이름·TypeId, Soft resolver).
    PathKeyTextHook& KeyHook() noexcept;

    // KeyHook의 마지막 해석 실패를 진단 종류로 돌려주고 지운다(없으면 InvalidValue).
    // ResolveTextPath가 key 오류를 보고할 때 쓴다.
    TextDiagnosticCode TakeKeyFailure() noexcept;

    // InstancedStruct 뒤 멤버 단계(`row.damage` — 리플렉션 B9j)를 풀 때 그 자리의 지금 담긴
    // struct를 주는 resolver다(없으면 nullptr — 그런 경로는 ResolveTextPath가 InvalidValue 진단).
    // 대입 묶음은 준비 상태(같은 묶음의 앞 대입이 바꾼 상자)를 보는 resolver를 대입마다 건다.
    // resolver는 이 문맥이 쓰는 동안 살아 있어야 한다.
    PathInstancedTypeResolver* InstancedTypes() const noexcept {
        return instancedTypes_;
    }

    void SetInstancedTypes(PathInstancedTypeResolver* resolver) noexcept {
        instancedTypes_ = resolver;
    }

private:
    class KeyTextHook final : public PathKeyTextHook {
    public:
        explicit KeyTextHook(TextCodecContext& owner) noexcept;

        Result<PathKeyValue> ParseKeyText(const ValueType& keyType,
                                          StringView text) noexcept override;
        Result<void> FormatKeyText(const ValueType& keyType, const PathKeyValue& value,
                                   String& out) noexcept override;

        TextCodecContext* owner;
        FieldValue parsed;
        bool failed = false;
        TextDiagnosticCode failure = TextDiagnosticCode::InvalidValue;
    };

    TypeRegistry* types_;
    const TextCodecOptions* options_;
    TextDiagnostics* diagnostics_;
    Allocator* allocator_;
    uint64 consumed_ = 0;
    bool started_ = false;
    KeyTextHook keyHook_;
    PathInstancedTypeResolver* instancedTypes_ = nullptr;
};

// 해석한 경로와 그 자리다(약 7 KB — PropertyPathBuffer).
struct TextPath {
    PropertyPathBuffer path;
    TextSlot slot;
};

// path(type 스키마로 해석한 경로 — 정규형)가 가리키는 자리를 out에 쓴다. 할당하지 않는다.
// 오류: 빈 path·정규형이 아닌 단계·없는 필드·모양 불일치(ParsePropertyPath가 만들지 않는 경로)는
// InvalidArgument다. instanced가 있으면 InstancedStruct 뒤 멤버 단계를 그 자리의 지금 담긴 struct로
// 푼다(리플렉션 B9j — 대입 묶음은 준비 상태를 보는 resolver를 넘긴다). 없으면 그런 단계는
// InvalidArgument다.
Result<void> DescribeTextPath(const TypeLease& type, PropertyPath path, TextSlot& out,
                              PathInstancedTypeResolver* instanced = nullptr) noexcept;

// 경로 텍스트를 type(클래스·struct lease)의 스키마로 풀어 out에 쓴다(ParsePropertyPath + KeyHook +
// DescribeTextPath). 텍스트 byte를 입력 누계에 더하고, 입력 용도의 판정을 경로 전체에 한다.
// 반환: true면 out이 유효하다. false면 진단을 남겼다(source·path = text):
//   UnknownProperty(모르는 이름), InvalidValue(문법·17단계 초과·key 텍스트가 키 종류의 값이 아님·
//   모양 불일치·Name key가 NFC 아님 등), UnknownType·Unresolved·Unsupported(Soft·ClassRef key —
//   KeyHook), HiddenEnumerator(숨김 열거자 key), ExcludedProperty(용도의 입력 제외 플래그가 경로의
//   어느 단계에 있음). ReadOnly는 판정하지 않는다(slot.flags — 대입 묶음이 대상·값으로 정한다).
// 오류: 용도가 입력을 받지 않음(Report)·옵션 InvalidArgument, 입력 한도 CapacityExceeded, 진단
// 할당 실패, 빈 lease InvalidArgument.
Result<bool> ResolveTextPath(TextCodecContext& context, const TypeLease& type, StringView text,
                             TextSource source, TextPath& out) noexcept;

// path를 텍스트로 out 뒤에 붙인다(FormatPropertyPath + KeyHook). 용도의 출력 제외 플래그가 경로의
// 어느 단계에 있으면 아무것도 쓰지 않고 false다. 오류: FormatPropertyPath·DescribeTextPath의 오류.
Result<bool> FormatTextPath(TextCodecContext& context, const TypeLease& type, PropertyPath path,
                            String& out) noexcept;

// 대입 텍스트 `<경로>=<값>`을 `[...]`와 key 따옴표 밖의 첫 `=`에서 나눈다(String 키 `a=b`는
// `m["a=b"]=1`로 쓴다 — 1.5절). `=`가 없으면 false다(path·value는 바뀌지 않는다). 따옴표 안의
// `\"`·`\\`는 건너뛴다. 할당하지 않는다.
bool SplitTextAssignment(StringView text, StringView& path, StringView& value) noexcept;

// 단일 값 텍스트 하나를 type의 값으로 out에 읽는다(목록 escape를 푼 원문 — 목록이 아니면 그대로).
// out의 kind·값 칸·type(enum ID)을 정하고 present는 true다. 텍스트 byte를 입력 누계에 더한다.
// 반환: true면 읽었다. false면 진단을 남겼다(source, path = pathText): InvalidValue(형식),
// OutOfRange(정수 폭·실수 범위), HiddenEnumerator, UnknownType(ClassRef 도구 이름·TypeId 미등록),
// Unresolved(resolver 실패), Unsupported(resolver 없는 non-null Hard·Weak, Name pool 없음, 텍스트로
// 쓸 수 없는 종류).
// 오류: 입력 한도 CapacityExceeded, 할당 실패, Name pool의 오류(CapacityExceeded·ShuttingDown 등).
Result<bool> ParseTextScalar(TextCodecContext& context, const TextScalarType& type, StringView text,
                             TextSource source, StringView pathText, FieldValue& out) noexcept;

// value(type의 값)의 텍스트를 out 뒤에 붙인다(ParseTextScalar와 왕복한다 — NaN은 `nan` 하나로
// 쓴다). 오류: 쓸 수 없는 값(이름 없는 enum 값·resolver가 쓰지 못한 Hard·Weak·경로 있는 Soft·Name
// pool 없음)은 Unsupported, value.kind가 type과 다르면 TypeMismatch, 붙이기 실패.
Result<void> FormatTextScalar(TextCodecContext& context, const TextScalarType& type,
                              const FieldValue& value, StringBuilderBase& out) noexcept;

// 자리(slot)의 값 텍스트를 out에 읽는다(out은 먼저 Reset한다). 모양: 단일 값 필드·원소·Map 값·
// Optional<단일 값> → Single, `{}`(struct·Optional<struct>·struct 원소·Map 값 struct) →
// StructDefault, Array·StaticArray·Set → List, Map → Entries. struct 원소 Array·Map은 빈
// 텍스트(비우기)만 받고 struct 원소 StaticArray는 통째 값을 받지 않는다(원소 경로로 쓴다). 텍스트
// byte를 입력 누계에 더한다(원소 텍스트는 다시 세지 않는다). 반환: true면 읽었다. false면 진단을
// 남겼다(원소 진단은 원소마다 — 한도 안에서 모두): ParseTextScalar의 진단, InvalidValue(`{}`·목록
// 모양·Map 항목에 `:` 없음·StaticArray 원소 수 ≠ N), OutOfRange(원소 수 > maxElements·필드
// MaxElements), DuplicateKey(Set 원소·Map 키 중복 — Name은 pool 비교 규칙, String은 byte),
// Unsupported(Set 원소 자리, 통째로 쓸 수 없는 필드). 오류: ParseTextScalar와 같다.
Result<bool> ParseTextValue(TextCodecContext& context, const TextSlot& slot, StringView text,
                            TextSource source, StringView pathText, TextValue& out) noexcept;

// value를 자리(slot)의 텍스트로 out 뒤에 붙인다(ParseTextValue와 왕복한다). 목록 원소는 `,`·`;`를,
// Map 키는 `:`도 escape한다. 오류: 모양이 자리와 맞지 않음 InvalidArgument, 목록으로 쓸 수 없는
// 원소(원소 하나뿐인 빈 문자열, 다음 구분자와 escape 쌍이 되는 `\`로 끝나는 원소·키)·
// FormatTextScalar의 Unsupported, 붙이기 실패. 실패하면 out에 앞부분이 남을 수 있다.
Result<void> FormatTextValue(TextCodecContext& context, const TextSlot& slot,
                             const TextValue& value, String& out) noexcept;

} // namespace rocks::reflection_text

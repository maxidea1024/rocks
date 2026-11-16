#pragma once

#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Meta/PropertyPathSchema.h>
#include <Object/NameField.h>
#include <Object/ObjectRegistry.h>
#include <Object/PathKey.h>
#include <Object/PathTarget.h>

// 리플렉션 속성 경로 접근이다(Docs/Design/ReflectionFoundations.ko.md 7·7.1절). 경로(PropertyPath —
// FieldId 체인 + 원소 index·Map key, MetaSchema)로 객체 하나 안의 단일 값을 읽고 쓰고, 배열 원소를
// 넣고 빼고 비우고, Map 항목·Set 원소를 key로 넣고 빼고 찾는다(리플렉션 B9f3 — Docs/Design/
// ReflectionExtensions.ko.md 1.5·7.3절). 문자열 경로("slots[2].material"·"perPlatform[Android]")는
// ParsePropertyPath가 타입 스키마로 PropertyPath를 만들고 FormatPropertyPath가 되돌린다. 실제
// 접근은 생성 경로 thunk(RuntimeFieldOps::path·RuntimeStructFieldOps::path — Object/
// GeneratedPathAccess.h)가 정적 타입으로 하고, 검증은 쓰기의 단일 지점(detail::ValidateFieldWrite·
// ValidateArrayEdit)을 지난다. Archive에 의존하지 않는다.
namespace rocks {

// 경로 연산이 실패한 사유다(PropertyPathFailure::reason). 같은 ErrorCode의 원인을 구별해 도구가
// 모르는 속성·범위 오류·값 오류를 나눠 보고하게 한다. 괄호는 함께 돌아오는 ErrorCode다.
//   None              — 사유가 없는 오류(문맥·수명·Busy·OutOfMemory·Overflow·Registry 대상
//                       검사·게시 직전 재확인), 단계 0개·정규형이 아닌 단계(InvalidArgument),
//                       유효 플래그가 ReadOnly가 아닌데 등록된 write가 낸 Unsupported
//   Syntax            — 문자열 경로 문법(InvalidArgument)
//   TooDeep           — MaxPropertyPathSteps(17) 단계 초과(InvalidArgument)
//   UnknownProperty   — 문자열 경로의 모르는 이름(NotFound — 도구가 AS 4.6절 UnknownProperty
//                       진단으로 바꾼다)
//   UnknownField      — FieldId 체인의 없는 필드(스키마가 바뀐 옛 경로, NotFound)
//   NotArray          — 배열(Array·StaticArray)이 아닌 필드에 index(selector Index)·컨테이너
//                       (Array·StaticArray·Map·Set)가 아닌 필드에 원소 수·비우기(TypeMismatch)
//   NotStruct         — struct가 아닌 값(index 없는 배열·key 없는 Map·단일 값 Optional·단일 값
//                       Map 항목·Set 원소 포함) 뒤의 단계(`.`)(TypeMismatch)
//   IndexRequired     — 배열 삽입·삭제의 마지막 단계가 index 단계가 아님(InvalidArgument)
//   IndexNotAllowed   — 원소 수·비우기의 마지막 단계에 index·key가 있음(InvalidArgument)
//   IndexOutOfRange   — 원소 index가 범위 밖(InvalidArgument)
//   InvalidValue      — 값 내용·종류(InvalidArgument — 값 종류나 enum ID가 필드와 다르면
//                       TypeMismatch), 삽입 값 누락, 단일 값 Map에 PathInsertKey(값은 PathWrite로
//                       넣는다)
//   ReadOnly          — 유효 플래그(바깥 struct·배열 필드에서 전파)의 ReadOnly(Unsupported)
//   AggregateValue    — struct·배열·Optional<값 struct>·Map·Set·Map의 값 struct 항목을 통째로
//                       읽기·쓰기, struct 원소 삽입에 값·제거에 removed, Map<K, 값 struct> 항목
//                       지우기에 removed(Unsupported, removed는 InvalidArgument)
//   NoPathAccess      — 경로 thunk(또는 최상위 read·write)가 없는 필드·멤버, Opaque 필드로 끝나는
//                       읽기·쓰기, previous를 요청했는데 최상위 필드에 read가 없음(Unsupported)
//   TooManyElements   — 삽입 뒤 길이가 필드 MaxElements 초과(CapacityExceeded)
//   StaticTypeMismatch — 생성 thunk의 정적 타입과 등록 스키마가 어긋남(TypeMismatch)
//   OptionalUnset     — unset인 Optional<struct> 필드의 멤버로 들어가는 단계(NotFound — 리플렉션
//                       B9e, 먼저 PathEmplaceOptional로 값을 만든다). 쓰기는 검증 지점이 thunk보다
//                       먼저라 유효 플래그가 ReadOnly인 자리면 ReadOnly가 먼저다(읽기는
//                       OptionalUnset)
//   NotOptional       — PathEmplaceOptional·PathResetOptional·PathIsOptionalSet의 끝이 Optional<값
//                       struct> 필드가 아님(TypeMismatch — 단일 값 Optional은 PathRead·PathWrite의
//                       present로 다룬다)
//   NotKeyed          — Map·Set이 아닌 필드에 key(selector Key)(TypeMismatch — 리플렉션 B9f3)
//   KeyRequired       — PathContains·PathInsertKey·PathEraseKey의 마지막 단계가 key 단계가
//                       아님(InvalidArgument)
//   KeyNotFound       — key의 Map 항목·Set 원소가 없음(NotFound — 읽기·지우기·`m[k].member`. Name
//                       키는 pool에 없는 이름이면 할당 없이 이것이다)
//   InvalidKey        — key 값: 종류가 필드의 키 종류와 다름(TypeMismatch), wire·문자열 경로 key
//                       텍스트가 정규형이 아님·미선언 enum 키·Soft 규칙·문자열 경로 key byte
//                       합계 초과(InvalidArgument), 문자열 경로로 쓸 수 없는 키 종류(Soft·ClassRef
//                       — Unsupported)
//   NotMap            — Set 원소 key 단계의 값 읽기·쓰기(TypeMismatch — Set 원소는 키라 값이 없다,
//                       PathContains·PathInsertKey·PathEraseKey를 쓴다)
//   FixedLength       — 정적 배열(T x[N] — 리플렉션 B9g)의 원소 삽입·삭제·비우기(Unsupported —
//   길이가
//                       N으로 고정이다. 원소는 [i] 쓰기로 바꾼다)
enum class PropertyPathError : uint8 {
    None,
    Syntax,
    TooDeep,
    UnknownProperty,
    UnknownField,
    NotArray,
    NotStruct,
    IndexRequired,
    IndexNotAllowed,
    IndexOutOfRange,
    InvalidValue,
    ReadOnly,
    AggregateValue,
    NoPathAccess,
    TooManyElements,
    StaticTypeMismatch,
    OptionalUnset,
    NotOptional,
    NotKeyed,
    KeyRequired,
    KeyNotFound,
    InvalidKey,
    NotMap,
    FixedLength,
    // 깊은 복사·무할당 교환을 할 수 없는 값이다(PathDetach·PathPrepareAttach — 리플렉션 B9i3c0).
    NotCopyable
};

// 경로 연산·문자열 해석의 실패 위치와 사유다. step은 실패한 단계의 index다: 경로 연산에서 문맥·
// 수명(AcquireType)과 Borrow 획득 오류는 0, 17단계 초과는 17이고, 고정·게시 직전 재확인·Registry
// 호출 오류는 그 연산을 하던 단계다. offset은 문자열 해석의 byte 위치다(경로 연산은 0). 연산을
// 시작할 때 비우고 실패할 때 채운다.
struct PropertyPathFailure {
    PropertyPathError reason = PropertyPathError::None;
    usize step = 0;
    usize offset = 0;
};

// 문자열 경로 해석이 key 하나 앞에 남기는 여백 byte다: 가장 긴 고정 prefix(Soft 키 — Guid 16 + 길이
// 8)다. key 텍스트를 이 여백 뒤에 풀고 그 자리에서 wire로 바꾸므로 별도 텍스트 버퍼가 없다.
inline constexpr usize PathKeyTextPrefixBytes = 24;

// 문자열 경로 하나가 담는 key wire byte 합계의 상한이다(PropertyPathBuffer 고정 버퍼). 최대 길이
// Name 키(MaxNameFieldBytes — Object/NameField.h)와 여백을 두 개 담는다: Map 값 struct 안의 Name 키
// Map(A5 PerPlatform 모양을 담은 Map)처럼 key 단계 둘이 모두 최대 길이여도 해석된다. 키 하나의
// 텍스트는 여백과 함께 남은 byte에 들어가야 하고, 넘으면 ParsePropertyPath가 InvalidArgument
// (InvalidKey)다. 더 긴 key(긴 String·Soft 경로 키)는 PropertyPath와 PathKey를 직접 만든다.
inline constexpr usize MaxPropertyPathKeyBytes = 2 * (MaxNameFieldBytes + PathKeyTextPrefixBytes);

namespace detail {

struct PropertyPathParser;

} // namespace detail

// 해석한 경로를 담는 고정 크기 버퍼다(할당 없음): 단계, key 단계의 키(PathKey — wire는 이 버퍼의
// key byte를 가리킨다). 키는 ParsePropertyPath만 채운다(키 칸은 읽기 전용 — 바깥 wire를 넣을 수
// 없게 감쌌다). 복사하면 사본의 key가 사본 버퍼를 가리키도록 옮긴다. View()는 이 버퍼가 사는
// 동안(바뀌지 않는 동안) 유효하다. 크기는 약 7 KB(대부분 key byte — MaxPropertyPathKeyBytes)이고
// ParsePropertyPath는 실패 원자성 때문에 같은 크기의 지역 버퍼를 하나 더 쓴다 — 작은 스택의
// worker에서 해석하면 그 몫을 잡는다.
//
// 사용 예:
//   PropertyPathBuffer path;
//   if (auto parsed = ParsePropertyPath(lease, "slots[2].material", path); !parsed) { ... }
//   auto written = PathWrite(registry, access, mesh, path.View(), value);
struct PropertyPathBuffer {
    PathStep steps[MaxPropertyPathSteps];
    usize count = 0;

    PropertyPathBuffer() noexcept = default;

    PropertyPathBuffer(const PropertyPathBuffer& other) noexcept {
        CopyFrom(other);
    }

    PropertyPathBuffer& operator=(const PropertyPathBuffer& other) noexcept {
        if (this != &other) {
            CopyFrom(other);
        }

        return *this;
    }

    // 담은 key 수와 index번째 key다(index < KeyCount()).
    usize KeyCount() const noexcept {
        return keyCount_;
    }

    const PathKey& Key(usize index) const noexcept {
        ROCKS_ASSERT(index < keyCount_);
        return keys_[index];
    }

    PropertyPath View() const noexcept {
        return PropertyPath{Span<const PathStep>(steps, count),
                            Span<const PathKey>(keys_, keyCount_)};
    }

private:
    friend struct detail::PropertyPathParser;

    // 단계·key·byte를 복사하고 key wire가 이 버퍼의 byte를 가리키게 한다(키 칸은
    // ParsePropertyPath만 채우므로 wire는 늘 keyBytes_ 안이다).
    void CopyFrom(const PropertyPathBuffer& other) noexcept {
        count = other.count;
        keyCount_ = other.keyCount_;
        keyByteCount_ = other.keyByteCount_;
        for (usize index = 0; index < count; ++index) {
            steps[index] = other.steps[index];
        }

        for (usize index = 0; index < keyByteCount_; ++index) {
            keyBytes_[index] = other.keyBytes_[index];
        }

        for (usize index = 0; index < keyCount_; ++index) {
            const usize offset =
                static_cast<usize>(other.keys_[index].wire.Data() - other.keyBytes_);
            keys_[index].kind = other.keys_[index].kind;
            keys_[index].wire =
                Span<const uint8>(keyBytes_ + offset, other.keys_[index].wire.Size());
        }
    }

    PathKey keys_[MaxPropertyPathSteps];
    usize keyCount_ = 0;
    uint8 keyBytes_[MaxPropertyPathKeyBytes];
    usize keyByteCount_ = 0;
};

// 문자열 경로 key 텍스트의 hook이다(리플렉션 B9f3 — Docs/Design/ReflectionExtensions.ko.md 7.3절).
// 문맥 없이 텍스트를 정할 수 없는 키 종류(SoftObjectReference는 논리 경로·resolver,
// ClassReference는 타입 도구 이름·TypeRegistry가 필요하다)의 key 텍스트를 호출자가 해석·형식화한다.
// ReflectionText(B9i3)가 같은 경로 문법(이름·index·따옴표·escape)을 다시 구현하지 않고 이 hook만
// 준다. 그 밖의 키 종류는 hook을 부르지 않는다. hook이 없으면 그 두 종류는
// Unsupported(InvalidKey)다.
//
// 사용 예:
//   struct ClassKeys final : PathKeyTextHook {
//       Result<PathKeyValue> ParseKeyText(const ValueType&, StringView) noexcept override;
//       Result<void> FormatKeyText(const ValueType&, const PathKeyValue&,
//                                  String&) noexcept override;
//   };
//   ClassKeys hook;
//   auto parsed = ParsePropertyPath(lease, "classNames[MapKeyDerived]", path, &failure, &hook);
class PathKeyTextHook {
public:
    // keyType(Soft·ClassRef 키 형식 — type은 Soft 기대 타입·ClassRef Base)의 텍스트 text(따옴표를
    // 벗기고 escape를 푼 원문)를 키 값으로 읽는다. 결과의 kind는 keyType.kind여야 하고(아니면
    // TypeMismatch), text(Soft 경로)는 다음 hook 호출 전까지 유효해야 한다(해석이 곧바로 경로
    // 버퍼에 정규형으로 복사한다). 오류는 ParsePropertyPath의 오류(사유 InvalidKey, offset은 key
    // 시작)가 된다.
    virtual Result<PathKeyValue> ParseKeyText(const ValueType& keyType,
                                              StringView text) noexcept = 0;

    // value(정규형 — key 해석을 마친 값)의 텍스트를 out(빈 String — FormatPropertyPath의 out
    // allocator)에 쓴다. 따옴표·escape는 FormatPropertyPath가 붙인다. 오류는 그대로
    // FormatPropertyPath의 오류다.
    virtual Result<void> FormatKeyText(const ValueType& keyType, const PathKeyValue& value,
                                       String& out) noexcept = 0;

protected:
    ~PathKeyTextHook() = default;
};

// 문자열 경로를 type(클래스 또는 struct lease)의 스키마로 해석해 out에 쓴다. 문법은
// `name ('.' name | '[' index ']' | '[' key ']')*`이다(`.`는 struct 값 — struct 필드·struct 원소·
// Optional<값 struct> 필드·Map<K, 값 struct> 항목 — 뒤에만 온다): 이름은 ASCII 식별자이고 필드의
// 실효 도구 이름(toolName, 없으면 C++ 이름 — toolName이 있으면 C++ 이름은 받지 않는다)과 ASCII
// 대소문자 무시로 비교하며 Opaque 필드는 찾지 않는다. index(배열 필드)는 정규 10진(앞자리 0은 `0`
// 하나만, 부호·공백·uint32 초과 거부)이다.
//
// key(Map·Set 필드 — 리플렉션 B9f3, Docs/Design/ReflectionExtensions.ko.md 1.5절)는 스키마의 키
// 종류로 해석하므로 따옴표 없이 쓴다(`perPlatform[Android]`). 따옴표 없는 key는 첫 `]`까지이고
// `,`·`=`·`"`·`\`·ASCII 공백·제어 문자를 담을 수 없다(Syntax — 그런 key는 따옴표로 쓴다, 표기가
// 하나가 되게). 따옴표 key(`ratios["a b"]`) 안에서는 UE ParseQuotedString의 escape 가운데 다섯
// (`\\`·`\"`·`\n`·`\r`·`\t`)만 받고 그 밖의 `\x`(UE의 `\'`·8진 포함)와 날 제어 문자는 Syntax다.
// 키 텍스트(Soft·ClassRef 말고는 값 표기와 같은 공용 규칙 — Object/ValueText.h ParseScalarText):
//   - String·Name: 그대로(Name은 정규화하지 않는다 — None은 `None`, 빈 key도 None으로 읽는다)
//   - bool: true·false·on·off(ASCII 대소문자 무시)
//   - 정수: 정규 10진(앞자리 0·`+`·`-0` 거부, 범위 밖 거부)
//   - enum: 열거자 도구 이름(FindEnumerator — 대소문자 무시·toolPrefix). Flags enum은 `A+B`·`A|B`
//     조합과 빈 값(0)도 받는다
//   - Guid·AssetId: 36자 소문자(AssetId::TryParse — 대문자·32자 거부)
//   - Digest256: 64자 16진(대소문자 무관)
//   - Soft·ClassRef: hook(PathKeyTextHook)이 해석한다(없으면 Unsupported(InvalidKey))
// 키는 out의 고정 버퍼에 PathKey 정규형으로 담는다(합계 MaxPropertyPathKeyBytes). 파생 클래스
// 스키마는 상속 필드를 같은 ID로 담으므로 base lease로 해석한 경로를 파생 객체에 쓸 수 있다.
// 할당하지 않고 스레드와 무관하다(lease만 있으면 된다 — hook의 할당·스레드는 hook의 몫이다).
//
// 오류(failure가 있으면 사유·단계·byte 위치를 채운다, out은 실패하면 바뀌지 않는다):
//   InvalidArgument — 빈 lease·enum lease(None), 문법(Syntax — 닫히지 않은 따옴표·`]` 없음·따옴표
//                     없는 key의 금지 글자·모르는 escape), 17단계 초과(TooDeep), key 텍스트가 키
//                     종류의 값이 아님·key byte 합계 초과(InvalidKey — offset은 key 시작)
//   NotFound        — 모르는 이름(UnknownProperty — offset은 이름 시작)
//   TypeMismatch    — 배열·Map·Set이 아닌 필드·원소 뒤의 `[`(NotArray), struct가 아닌 값 뒤의
//                     `.`(NotStruct)
//   Unsupported     — hook 없는 Soft·ClassRef 키(InvalidKey)
//   hook의 오류      — 그 코드(InvalidKey)
//
// 사용 예:
//   PropertyPathBuffer path;
//   PropertyPathFailure failure;
//   if (auto parsed = ParsePropertyPath(lease, text, path, &failure); !parsed) {
//       if (failure.reason == PropertyPathError::UnknownProperty) { ... }   // AS 4.6절 진단
//   }
Result<void> ParsePropertyPath(const TypeLease& type, StringView text, PropertyPathBuffer& out,
                               PropertyPathFailure* failure = nullptr,
                               PathKeyTextHook* hook = nullptr) noexcept;

// path를 type(클래스 또는 struct lease)의 스키마로 문자열 경로로 바꿔 out에 쓴다(실효 도구 이름 —
// ParsePropertyPath와 왕복한다). key는 ParsePropertyPath의 키 텍스트로 쓴다(bool은 true·false,
// enum은 열거자 실효 도구 이름 — Flags는 선언 순 `A+B`·0은 빈 값, Guid·AssetId는 36자 소문자,
// Digest256은 64자 소문자, Name None은 `None`, Soft·ClassRef는 hook). `]`·`,`·`=`·`"`·`\`·공백·
// 제어 문자가 있으면 큰따옴표로 감싸고 그 안에서 `\`·`"`·개행·CR·탭을 `\\`·`\"`·`\n`·`\r`·`\t`로
// 쓴다. hook이 없으면 out의 allocator로 한 번 할당하고(hook 키는 그 텍스트마다 더 할당한다)
// 실패하면 out은 바뀌지 않는다. 오류: 빈·enum lease, 단계 0개·17 초과·정규형이 아닌 단계, 정규형이
// 아닌 key wire는 InvalidArgument, 없는 필드는 NotFound, 경로 thunk가 닿을 수 없는 Opaque
// 필드·텍스트로 쓸 수 없는 key(hook 없는 Soft·ClassRef 키, 이름 있는 열거자(조합)로 나타낼 수 없는
// enum 키 값, 개행·CR·탭이 아닌 제어 문자를 담은 텍스트)는 Unsupported, 모양 불일치(배열이 아닌
// 필드의 index·Map·Set이 아닌 필드의 key·key 종류가 필드 키 종류와 다름·struct가 아닌 값 뒤의
// 단계)는 TypeMismatch, 할당 실패는 OutOfMemory, hook의 오류는 그 코드다.
//
// 사용 예:
//   String text(allocator);
//   if (auto formatted = FormatPropertyPath(lease, path.View(), text); formatted) { ... }
Result<void> FormatPropertyPath(const TypeLease& type, PropertyPath path, String& out,
                                PathKeyTextHook* hook = nullptr) noexcept;

// ---- 경로 연산(소유 스레드 전용) ----
//
// 공통 계약:
// - 경로는 객체 하나 안에서 끝난다(참조를 따라 다른 객체로 들어가지 않는다). target은 게시된 생존
//   객체(ObjectHandle에서 암묵 변환)이고 해석은 그 객체의 실제 타입 스키마로 한다. staged
//   객체·분리된 값(struct 값·DetachedValue 상자)에서 시작하는 차이는 Object/PathTarget.h(리플렉션
//   B9i3c0)다.
// - 끝 지점: 최상위 단일 값 필드는 등록된 read·write(FieldWriteOperation — 수동 함수표도 같다),
//   struct 멤버 단일 값(String·Soft·enum 포함)과 Array<E> 원소는 경로 thunk, 참조 멤버는
//   SetReference(StructRefArray 원소 안이면 SetElementReference), ObjectRefArray 원소는 Registry
//   컨테이너 API(SetReferenceAt·InsertReferenceAt·RemoveReferenceAt·ClearReferences),
//   StructRefArray는 InsertElementAt·RemoveElementAt·ClearElements(원소 멤버는 멤버 단위)다.
//   struct·배열을 통째로 읽거나 쓰면 Unsupported(AggregateValue)이고 Resize·원소 이동·복제는 없다.
//   Optional 필드(리플렉션 B9e — Docs/Design/ReflectionExtensions.ko.md 6절)는 단일 값 내부면
//   FieldValue(kind = 내부 종류, present = set 여부 — present = false 쓰기가 unset)로 읽고 쓰고,
//   값 struct 내부면 aggregate다: PathEmplaceOptional·PathResetOptional로 값을 만들고 비우고
//   PathIsOptionalSet으로 set 여부를 보며,
//   멤버 경로(`opt.member`)는 set일 때만 닿는다(unset이면 NotFound(OptionalUnset)). 빈 자리에
//   만드는 새 값(Optional의 String·struct)은 ObjectRegistry::ObjectAllocator로 만든다.
//   Map·Set 필드(리플렉션 B9f3 — Docs/Design/ReflectionExtensions.ko.md 7.3절)는 key 단계
//   (selector Key)로 항목을 가리킨다: Map의 단일 값 항목은 PathRead·PathWrite(없으면 끝에 넣고
//   있으면 자리를 지키며 값을 바꾼다 — Sparks `m[k] = v`), Map<K, 값 struct> 항목은 멤버 경로
//   (`m[k].member`)와 PathInsertKey(기본 값 넣기·초기화), Set 원소는 PathInsertKey, 둘 다
//   PathEraseKey·PathContains·PathCount·PathClear다. Map·Set 통째 읽기·쓰기는 AggregateValue다. 새
//   키·값(String·Soft·값 struct)은 그 컨테이너의 allocator(살아 있는 객체면 객체 allocator)로
//   만든다. Name 키 조회는 NamePool::TryFind다(pool에 없는 이름은 할당 없이 없음 — NotFound
//   (KeyNotFound)·PathContains 거짓). 넣기 준비는 키가 없을 때만 한다: 새로 넣는 Name 키는 내용
//   규칙(ValidateNameFieldText) 뒤 Registry pool에 intern하고(intern은 대상을 바꾸지 않으므로 그 뒤
//   연산이 실패해도 pool entry는 남는다 — 4절 2a와 같다), 새로 넣는 ClassRef 키는 등록·Base 파생을
//   본다. 있는 키의 교체·이미 있는 Set 원소 넣기는 intern·등록 검사를 하지 않는다(대소문자 변형
//   Name 키 교체가 pool을 늘리지 않고, 등록을 해제한 클래스 키의 값도 바꿀 수 있다). 순회(ForEach)
//   중인 컨테이너의 key 쓰기·넣기·지우기·비우기는 Busy다(조회·읽기는 된다).
// - 검증: 바깥 struct·배열 필드의 플래그를 PropagateFieldFlags로 누적한다 — ReadOnly는 멤버·원소
//   쓰기와 삽입·삭제·비우기를 모두 막고(Unsupported), EditorOnly·Transient는 막지 않는다. 값 쓰기·
//   단일 값 원소 삽입은 ValidateFieldWrite(Map 항목 값도 원소 모드), 구조 변경(Map·Set의 key 넣기·
//   지우기·비우기 포함)은 ValidateArrayEdit를 지난다. 삽입 뒤 길이는 필드 MaxElements를 넘을 수
//   없다(CapacityExceeded). 키 값은 key 단계 해석에서 본다: 종류(TypeMismatch(InvalidKey)), wire
//   정규형·미선언 enum 키·Soft 규칙(InvalidArgument(InvalidKey)). 새로 넣는 키만 ClassRef 등록·Base
//   파생(값과 같은 ValidateClassReference 규칙)과 Name 내용 규칙을 본다. 참조 값의 비정규
//   null(IsNull인 non-zero handle)은 ObjectHandle{}로 정규화한다.
// - 원자성: 쓰기는 ResolveForWrite로 Borrow를 얻는다. owner가 encode 고정 중이면 Busy,
//   DestroyRequested owner는 Destroyed다(정리 중 제거·비우기는 Registry API를 직접 쓴다).
//   경로가 직접 할당하는 끝 지점(String·Soft 쓰기, Array<E> 삽입, Map·Set 항목 넣기, 다른
//   allocator로의 옛 값 복사)은 할당부터 게시까지 owner를 encode 고정해 할당 콜백의 재진입 변경을
//   Busy로 막고, 게시 직전 owner 생존을 다시 확인한다(고정은 RequestDestroy를 막지 않는다 —
//   Destroyed면 게시하지 않는다). 고정은 게시 또는 실패 직후 풀리고, 옛 값·옛 버퍼와 실패한 연산의
//   임시 버퍼(새 값·옛 값 사본·새 원소·새 배열 버퍼)는 그 뒤에 반환한다. 최상위 String·Soft 필드는
//   등록된 생성 write(WriteGeneratedField)가 같은 고정을 스스로 쥔다 — 수동 함수표의 write
//   callback에는 이 고정이 없다(할당하는 수동 callback은 같은 규칙을 스스로 지킨다). 실패하면
//   대상은 바뀌지 않는다(Registry 컨테이너는 그 API의 실패 보장). 같은 배열을 C++ 코드로 직접
//   바꾸는 콜백은 계약 밖이다(Array.h의 재진입 계약). 참조 struct는 struct 단위 원자성이 없다(멤버
//   하나의 쓰기만 원자적이다). Map·Set 항목 넣기는 고정 아래 새 키·값을 만든 뒤 새 저장소만
//   준비하고(OrderedMap::PrepareReserve(Count() + 1) — 컨테이너는 그대로), owner를 다시 확인한
//   다음(파괴됐으면 준비한 저장소만 반환 — 컨테이너는 용량·주소까지 그대로) 무할당 CommitReserve와
//   Insert로 게시한다(옛 저장소는 새 저장소를 게시한 뒤 반환하므로 그 해제 콜백은 게시된 컨테이너를
//   본다). 지우기·비우기도 고정 아래 한다: 값을 지역으로 옮긴 뒤 Erase하고(키는 Erase가 항목을
//   논리적으로 지운 뒤 그 안에서 소멸한다 — 해제 콜백의 재진입 쓰기는 Busy, 읽기는 지운 뒤의 상태를
//   본다) 값은 고정을 푼 뒤 소멸한다. 비우기는 컨테이너를 지역으로 옮기고 고정을 푼 뒤 반환한다.
// - 오류 순서: 입력 형식(단계 0개·17 초과·정규형 위반·Opaque 값·index·key 유무) InvalidArgument →
//   문맥·수명(AcquireType) → 단계 순서의 해석(없는 ID NotFound, 모양 불일치 TypeMismatch, key 종류
//   TypeMismatch·key 값 InvalidArgument, Name 키 pool 조회) → 통째 접근·thunk 없음 Unsupported →
//   참조 값의 기대 타입·대상 lease(NotFound·StaleHandle) → ValidateFieldWrite와
//   ValidateArrayEdit(TypeMismatch → Unsupported(ReadOnly) → InvalidArgument) → Borrow와
//   고정(Busy, Destroyed) → 정적 타입 대조(TypeMismatch) → index InvalidArgument·key 없음 NotFound·
//   순회 중 Busy → 길이 CapacityExceeded → 새로 넣는 key의 ClassRef
//   lease(NotFound·InvalidArgument)· Name 내용 규칙과 intern → 정수 폭 Overflow → Registry 대상
//   검사(StaleHandle·TypeMismatch) → 할당 OutOfMemory → 게시 직전 owner 재확인 Destroyed.
// - failure가 있으면 실패 단계와 사유를 채운다(PropertyPathError).

// path의 단일 값을 out에 읽는다(out의 allocator로 복사). 읽기는 Resolve + encode 고정 아래 복사하고
// out을 게시하기 전에 owner를 다시 확인한다(String·Soft 복사의 할당 콜백이 원본을 바꾸면 Busy).
// 실패하면 out은 바뀌지 않는다.
//
// 사용 예:
//   FieldValue value(allocator);
//   auto read = PathRead(registry, access, mesh, path.View(), value);
Result<void> PathRead(ObjectRegistry& registry, const ObjectAccessContext& access,
                      const PathTarget& target, PropertyPath path, FieldValue& out,
                      PropertyPathFailure* failure = nullptr) noexcept;

// path의 단일 값(배열 원소 교체, Map 항목 넣기·교체 포함)을 value로 쓴다. previous가 있으면
// 성공했을 때 쓰기 전 값을 담는다(CU4 Undo용 — Map에 없던 키를 넣었으면 previous는 kind = 값 종류,
// present = false다: Undo는 PathEraseKey다, 리플렉션 B9f3): String·Soft는 previous의 allocator가
// 대상 값의 allocator와 같으면 옛 버퍼를 옮기고 (추가 할당 없음), 다르면 고정 아래 게시 전에
// previous의 allocator로 복사한다. 최상위 단일 값 필드의 previous는 write 직전에 등록된 read로
// 얻는다(read와 write 사이의 재진입 변경은 계약 밖 — 그 read 전에 쓰기 검증(ValidateFieldWrite)과
// 고정 Busy를 먼저 판정한다). previous가 쥐던 옛 버퍼는 고정을 푼 뒤 반환하고, 실패하면 previous는
// 바뀌지 않는다. 참조의 옛 handle은 약한 참조다: 간선을 지운 뒤에는 그 대상을 살리는 것이 없으므로,
// 호출자(CU4)가 RootRef로 붙잡기 전까지(이 연산의 해제 콜백 안 GC 포함) 옛 대상이 회수될 수 있다 —
// 되살리기 전에 handle의 생존을 확인한다.
//
// 사용 예:
//   FieldValue previous(allocator);
//   auto written = PathWrite(registry, access, mesh, path.View(), value, &previous);
Result<void> PathWrite(ObjectRegistry& registry, const ObjectAccessContext& access,
                       const PathTarget& target, PropertyPath path, const FieldValue& value,
                       FieldValue* previous = nullptr,
                       PropertyPathFailure* failure = nullptr) noexcept;

// path(마지막 단계는 index·key 없는 Array·Map·Set 필드)의 원소·항목 수를 돌려준다. 마지막 단계에
// index·key가 있으면 InvalidArgument(IndexNotAllowed), 컨테이너가 아니면 TypeMismatch(NotArray)다.
//
// 사용 예:
//   const PathStep words[] = {{WordsId}};
//   auto count = PathCount(registry, access, holder, PropertyPath{words});
Result<usize> PathCount(ObjectRegistry& registry, const ObjectAccessContext& access,
                        const PathTarget& target, PropertyPath path,
                        PropertyPathFailure* failure = nullptr) noexcept;

// path 마지막 단계(index가 있어야 한다 — 없으면 InvalidArgument(IndexRequired))의 배열에서 index
// 위치에 원소를 넣는다(index == 길이면 끝, 넘으면 InvalidArgument(IndexOutOfRange)). 단일 값 원소는
// value가 필수이고(없으면 InvalidArgument(InvalidValue)) struct 원소(Array<S>·StructRefArray)는 값
// 없이 기본 원소(S(Allocator&) 또는 S())를 넣는다(value를 주면 Unsupported(AggregateValue)).
// ObjectRefArray는 InsertReferenceAt, StructRefArray는 InsertElementAt을 쓴다. Array<E>는 원소와
// (용량이 모자라면) 새 버퍼를 먼저 만든 뒤 무할당으로 옮겨 게시하고 옛 버퍼는 게시 뒤에 반환한다.
//
// 사용 예:
//   const PathStep steps[] = {{LodsId, lods.Size(), PathSelector::Index}};   // 끝에 넣기
//   auto inserted = PathInsertElement(registry, access, mesh, PropertyPath{steps}, &value);
Result<void> PathInsertElement(ObjectRegistry& registry, const ObjectAccessContext& access,
                               const PathTarget& target, PropertyPath path, const FieldValue* value,
                               PropertyPathFailure* failure = nullptr) noexcept;

// path 마지막 단계(index가 있어야 한다)의 배열 원소를 지운다(뒤 원소를 당긴다). removed가 있으면
// 성공했을 때 지운 단일 값 원소의 값을 담는다(allocator 규칙과 참조 handle이 약한 참조라는 점은
// PathWrite의 previous와 같다). struct
// 원소에 removed를 주면 InvalidArgument(AggregateValue)다 — struct 원소의 옛 값은 CU4가 연산 전
// PathRead로 잎마다 만들고 "기본 원소 삽입 + 잎 쓰기"로 재생한다. 떼어 낸 원소는 게시 뒤 소멸한다.
Result<void> PathRemoveElement(ObjectRegistry& registry, const ObjectAccessContext& access,
                               const PathTarget& target, PropertyPath path,
                               FieldValue* removed = nullptr,
                               PropertyPathFailure* failure = nullptr) noexcept;

// path(마지막 단계는 index·key 없는 Array·Map·Set 필드)의 원소·항목을 모두 지운다. Array<E>·Map·
// Set은 용량까지 반환한다(원소와 저장소는 게시 뒤 반환). 오류는 PathCount와 같고 유효 플래그가
// ReadOnly면 Unsupported(ReadOnly)다. 옛 값은 CU4가 연산 전 PathRead로 만든다.
Result<void> PathClear(ObjectRegistry& registry, const ObjectAccessContext& access,
                       const PathTarget& target, PropertyPath path,
                       PropertyPathFailure* failure = nullptr) noexcept;

// path(마지막 단계는 Map·Set 필드의 key 단계 — 아니면 InvalidArgument(KeyRequired))의 키가 있는지
// 돌려준다(리플렉션 B9f3). 읽기라 할당하지 않고 encode 고정도 쥐지 않는다(유효 플래그 ReadOnly는
// 막지 않는다). Name 키가 pool에 없으면 pool을 늘리지 않고 거짓이다. 오류는 PathRead와 같다(key
// 종류·값 TypeMismatch·InvalidArgument(InvalidKey)).
//
// 사용 예:
//   auto present = PathContains(registry, access, holder, path.View());   // tags[Hero]
//   if (present && present.Value()) { ... }
Result<bool> PathContains(ObjectRegistry& registry, const ObjectAccessContext& access,
                          const PathTarget& target, PropertyPath path,
                          PropertyPathFailure* failure = nullptr) noexcept;

// path(마지막 단계는 key 단계)에 키를 넣는다(리플렉션 B9f3): Set은 원소를 끝에 넣고(이미 있으면
// 아무것도 하지 않는다), Map<K, 값 struct>는 기본 값(S(Allocator&) 또는 S() — Map의 allocator)을
// 끝에 넣거나 이미 있으면 자리를 지키며 기본 값으로 바꾼다(텍스트 `m[k]={}` — 옛 값은 게시 뒤
// 소멸). 반환: 새로 넣었으면 true, 이미 있었으면 false. 단일 값 Map은 값이 필요하므로
// InvalidArgument(InvalidValue)다(PathWrite로 넣는다). 넣은 뒤 원소 수가 MaxElements를 넘으면
// CapacityExceeded(TooManyElements), 유효 플래그가 ReadOnly면 Unsupported(ReadOnly)다. 실패하면
// 대상은 바뀌지 않는다. Undo: true면 PathEraseKey, false(Map 초기화)면 CU4가 연산 전 PathRead로
// 잎마다 만든 옛 값을 쓴다.
//
// 사용 예(텍스트 `points[hero]={}` 뒤 `points[hero].x=1`):
//   auto inserted = PathInsertKey(registry, access, holder, path.View());
//   if (!inserted) { return Result<void>::Failure(inserted.Error()); }
Result<bool> PathInsertKey(ObjectRegistry& registry, const ObjectAccessContext& access,
                           const PathTarget& target, PropertyPath path,
                           PropertyPathFailure* failure = nullptr) noexcept;

// path(마지막 단계는 key 단계)의 Map 항목·Set 원소를 지운다(리플렉션 B9f3 — 남은 항목의 순서는
// 그대로다). 없으면 NotFound(KeyNotFound)다. removed가 있으면 성공했을 때 지운 단일 값 Map 항목의
// 값(Set이면 지운 원소)을 담는다(Map 값의 allocator 규칙은 PathWrite의 previous와 같고, Set 원소는
// 키라 옮길 수 없어 늘 고정 아래 removed의 allocator로 복사한다). Map<K, 값 struct>
// 항목에 removed를 주면 InvalidArgument(AggregateValue)다(옛 값은 CU4가 연산 전 PathRead로 잎마다
// 만들고 "PathInsertKey + 잎 쓰기"로 재생한다). 유효 플래그가 ReadOnly면 Unsupported(ReadOnly)다.
// 지운 값은 게시 뒤 소멸한다.
//
// 사용 예(asset-set의 --erase=perPlatform[Android]):
//   FieldValue removed(allocator);
//   auto erased = PathEraseKey(registry, access, texture, path.View(), &removed);
Result<void> PathEraseKey(ObjectRegistry& registry, const ObjectAccessContext& access,
                          const PathTarget& target, PropertyPath path,
                          FieldValue* removed = nullptr,
                          PropertyPathFailure* failure = nullptr) noexcept;

// path(마지막 단계는 Optional<값 struct> 필드 — index 없음)에 기본 값(S(Allocator&) 또는 S() —
// ObjectRegistry::ObjectAllocator)을 만든다(리플렉션 B9e). 이미 set이면 기본 값으로 바꾼다(옛 값은
// 게시 뒤 소멸). 새 값은 고정 아래 만들고 owner를 다시 확인한 뒤 무할당 이동으로 게시한다. 끝이
// Optional<값 struct>가 아니면 TypeMismatch(NotOptional — 단일 값 Optional은 PathWrite), 유효
// 플래그가 ReadOnly면 Unsupported(ReadOnly)다. 실패하면 대상은 바뀌지 않는다. 옛 값은 CU4가 연산 전
// PathRead로 잎마다 만든다(Undo는 "PathResetOptional" 또는 "PathEmplaceOptional + 잎 쓰기").
//
// 사용 예:
//   const PathStep steps[] = {{PreviewId}};
//   auto made = PathEmplaceOptional(registry, access, mesh, PropertyPath{steps});
//   // 이어서 preview.width 같은 멤버를 PathWrite로 쓴다
Result<void> PathEmplaceOptional(ObjectRegistry& registry, const ObjectAccessContext& access,
                                 const PathTarget& target, PropertyPath path,
                                 PropertyPathFailure* failure = nullptr) noexcept;

// path(마지막 단계는 Optional<값 struct> 필드)를 unset으로 비운다(리플렉션 B9e — 이미 unset이면
// 아무것도 하지 않는다). 옛 값은 게시 뒤 소멸한다. 오류는 PathEmplaceOptional과 같다(할당 없음).
// 옛 값은 CU4가 연산 전 PathIsOptionalSet·PathRead로 잎마다 만든다(Undo는 "PathEmplaceOptional + 잎
// 쓰기").
//
// 사용 예(asset-set의 --unset=preview):
//   const PathStep steps[] = {{PreviewId}};
//   if (auto cleared = PathResetOptional(registry, access, mesh, PropertyPath{steps}); !cleared) {
//       return cleared;
//   }
Result<void> PathResetOptional(ObjectRegistry& registry, const ObjectAccessContext& access,
                               const PathTarget& target, PropertyPath path,
                               PropertyPathFailure* failure = nullptr) noexcept;

// path(마지막 단계는 Optional<값 struct> 필드)가 set인지 돌려준다(리플렉션 B9e — Docs/Design/
// ReflectionExtensions.ko.md 6절). Optional<struct>는 통째 PathRead가 AggregateValue이고 멤버가
// 없는 struct도 있으므로 set 여부는 이 조회로 얻는다: ReflectionText의 asset-info 출력(unset이면
// `--unset=opt`, set이면 `opt={}`와 멤버 줄)과 CU4 Undo 기록이 쓴다. 단일 값 Optional은 PathRead의
// FieldValue::present로 얻는다. 읽기라 owner를 바꾸지 않고 할당하지 않으며 encode 고정도 쥐지
// 않는다. 반환: set이면 true. 오류: 끝이 Optional<값 struct>가 아니면 TypeMismatch(NotOptional),
// 중간 단계의 unset Optional<struct>는 NotFound(OptionalUnset), 그 밖은 PathRead와 같다(유효 플래그
// ReadOnly는 막지 않는다).
//
// 사용 예:
//   auto set = PathIsOptionalSet(registry, access, mesh, path.View());
//   if (!set) {
//       return Result<void>::Failure(set.Error());
//   }
//
//   if (!set.Value()) { /* --unset=preview 줄을 낸다 */ }
Result<bool> PathIsOptionalSet(ObjectRegistry& registry, const ObjectAccessContext& access,
                               const PathTarget& target, PropertyPath path,
                               PropertyPathFailure* failure = nullptr) noexcept;

} // namespace rocks

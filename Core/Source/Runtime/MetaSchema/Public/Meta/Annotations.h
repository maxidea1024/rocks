#pragma once

#include <Core/Result.h>
#include <Meta/GeneratedType.h>

// Clang JSON AST가 추가 constant argument를 제공하므로 식별 marker를 인자로 반복한다.
// specifier는 제한 문법의 annotation payload이며 C++ 선언은 Clang AST만으로 해석한다.
// 지원하지 않는 compiler에서 annotation을 지우는 fallback은 제공하지 않는다.
// 이 마커는 바로 뒤 class/struct와 같은 소스·스코프에 위치해야 한다.
// 이름은 번역 단위에서만 유일하면 되며 정의된 enum 상수는 실행 코드나 저장 공간을 만들지 않는다.
#define ROCKS_DETAIL_JOIN_INNER(left, right) left##right
#define ROCKS_DETAIL_JOIN(left, right) ROCKS_DETAIL_JOIN_INNER(left, right)
#define ROCKS_CLASS(...)                                                                           \
    enum {                                                                                         \
        ROCKS_DETAIL_JOIN(rocksMetaClassMarker_, __COUNTER__)                                      \
        [[clang::annotate("rocks", "rocks.class.marker", #__VA_ARGS__)]] = 0                       \
    };
#define ROCKS_PROPERTY(...) [[clang::annotate("rocks", "rocks.field.arguments", #__VA_ARGS__)]]

// 리플렉션 함수다(UE UFUNCTION 대응 — Docs/Design/FunctionReflection.ko.md). ROCKS_CLASS 클래스의
// noexcept 멤버 함수(인스턴스·static) 선언 바로 앞에 둔다. 접근 수준은 제한하지 않는다(생성 thunk는
// ROCKS_GENERATED_BODY의 friend로 부른다). 이름에 C++ 오버로드가 없어야 하고, 매개변수는 값·const&
// 입력(강한 참조 ObjectRef와 그것을 담는 형식 제외), 반환은 void·값·Result<void>·Result<T>다.
// 지정자: Pure(관찰 가능한 상태를 바꾸지 않는다 — 반환값 필수), EditorOnly(#if ROCKS_WITH_EDITOR
// 안), Name="…"(도구 이름). 생성 코드가 TypeSchema::functions와 호출 thunk를 만들고 FunctionCall로
// 부른다.
//
// 사용 예:
//   ROCKS_CLASS()
//   class Door : public rocks::Component {
//       ROCKS_GENERATED_BODY()
//
//   public:
//       using rocks::Component::Component;
//       ROCKS_FUNCTION() rocks::Result<void> Open(rocks::ObjectHandle opener) noexcept;
//       ROCKS_FUNCTION(Pure) bool IsOpen() const noexcept;
//   };
#define ROCKS_FUNCTION(...) [[clang::annotate("rocks", "rocks.function", #__VA_ARGS__)]]

// 리플렉션 enum이다(Docs/Design/ReflectionFoundations.ko.md 3.4절). 바로 뒤에 underlying을 명시한
// enum class 정의가 와야 한다(UE와 같은 정수 8종 uint8·uint16·uint32·uint64·int8·int16·int32·int64,
// 열거자 하나 이상, 값 별칭 금지, uint64 열거자 값은 int64 최댓값 이하 — RMC2401, Docs/Design/
// ReflectionExtensions.ko.md 2절). 지정자: ToolPrefix="…"(도구 이름 조회가 앞에 붙어 있어도 받는
// 접두), Flags(비트 플래그 — 값은 선언 값의 OR). 열거자에는 ROCKS_ENUMERATOR(Name="…",
// DisplayName="…", Hidden)를 열거자 이름 바로 뒤에 붙일 수 있다(Name은 [A-Za-z0-9_]+ 도구 이름,
// Hidden은 도구 설정이 받지 않는 값). 접두 유무를 합친 도구 이름은 대소문자 무시로 유일해야
// 한다(RMC2203). 필드에 쓰면 저장 wire는 I64다.
//
// 사용 예:
//   ROCKS_ENUM(ToolPrefix = "TEXTUREGROUP_")
//   enum class TextureGroup : rocks::uint8 {
//       World = 0,
//       EightBitData ROCKS_ENUMERATOR(Name = "8BitData", DisplayName = "8비트 데이터") = 7,
//       Reserved13 ROCKS_ENUMERATOR(Hidden) = 13,
//   };
#define ROCKS_ENUM(...)                                                                            \
    enum {                                                                                         \
        ROCKS_DETAIL_JOIN(rocksMetaEnumMarker_, __COUNTER__)                                       \
        [[clang::annotate("rocks", "rocks.enum.marker", #__VA_ARGS__)]] = 0                        \
    };
#define ROCKS_ENUMERATOR(...) [[clang::annotate("rocks", "rocks.enumerator", #__VA_ARGS__)]]

// 리플렉션 struct다(Docs/Design/ReflectionFoundations.ko.md 4절). 바로 뒤에 base 없는 비템플릿
// class/struct 정의가 와야 한다(상속은 RMC2301, 템플릿은 RMC2000). 모든 비정적 데이터 멤버는
// ROCKS_PROPERTY 또는 ROCKS_NATIVE이고, `explicit S(Allocator&) noexcept` 또는 `S() noexcept`
// 생성자와 noexcept 소멸자가 있어야 한다. 멤버로 다른 리플렉션 struct를 담을 수 있다(중첩 깊이
// MaxStructDepth = 16 이하 — RMC2303, 순환은 RMC2302). ObjectRef 멤버를 (중첩까지) 담으면 참조
// struct이고 소유 클래스의 생성 Trace가 그 참조를 방문한다. 저장 wire는 중첩 Record(schema = struct
// ID, schema version = Version)다. 한 레코드에 ROCKS_CLASS와 함께 쓸 수 없다.
//
// 지정자: Version=N(Record schema version, 없으면 1). Atomic(값 없음 — TypeFlags::Atomic): 값이
// 통째 단위다. 차이 저장은 멤버 하나라도 기준과 다르면 모든 멤버를 쓰고 override 집합·되돌리기의
// 단위가 그 struct 필드 경로다(Docs/Design/ReflectionExtensions.ko.md 3.1절 — 차이 쓰기·override·
// 되돌리기 구현은 A5, 되돌리기는 RuntimeClassMetadata 'struct 통째 되돌리기' 후속과 함께).
// 로드·경로 접근은 멤버 단위 그대로이고 조회는 StructView::IsAtomic이다. Binds="…"(Core 값 타입
// 결합 — 미러 선언)는 CoreTypeBindings 모듈에서만 받는다(다른 모듈은 RMC2004). CLASS 지정자
// Abstract·WorldOnly는 받지 않는다.
//
// struct가 `static Result<Array<uint8>> Migrate(Allocator&, TypeId, uint32 from, uint32 to,
// Span<const uint8> record) noexcept`를 선언하면 옛 버전 Record를 올리는 타입 마이그레이션으로
// 등록된다. private이면 ROCKS_GENERATED_BODY가 필요하고, Migrate라는 이름의 멤버가 이 시그니처가
// 아니면 생성 코드가 컴파일되지 않는다(조용히 마이그레이션 없이 등록되지 않는다).
//
// 사용 예:
//   ROCKS_STRUCT(Version = 2)
//   struct BulkDataRef {
//       ROCKS_GENERATED_BODY()
//       ROCKS_PROPERTY() uint32 bulkId = 0;
//       ROCKS_PROPERTY() uint64 rawBytes = 0;
//       ROCKS_PROPERTY() Sha256Digest digest;
//   };
//
//   ROCKS_STRUCT(Atomic)      // 두 끝을 한 단위로 기준과 비교·저장·덮어쓴다
//   struct LodRange {
//       ROCKS_GENERATED_BODY()
//       ROCKS_PROPERTY() float minScreenSize = 0;
//       ROCKS_PROPERTY() float maxScreenSize = 1;
//   };
#define ROCKS_STRUCT(...)                                                                          \
    enum {                                                                                         \
        ROCKS_DETAIL_JOIN(rocksMetaStructMarker_, __COUNTER__)                                     \
        [[clang::annotate("rocks", "rocks.struct.marker", #__VA_ARGS__)]] = 0                      \
    };

// 리플렉션 클래스·struct 안의 비영속 런타임 멤버다(GPU 핸들·캐시 등). 저장·리플렉션 대상이 아니며
// 강한 객체 참조(ObjectRef·RootRef·Object 파생 타입의 raw 포인터 — 담는 타입·배열·템플릿
// 인자까지)를 담을 수 없다 (RMC2205). RocksRequiresWorldOnlyOwner 형질 타입은 WorldOnly 클래스에만
// 둔다(RMC2206 — struct에 두면 struct가 RequiresWorldOnlyOwner 형질을 갖고, 그 struct를 필드로 둔
// 클래스가 WorldOnly여야 한다). 지정자는 받지 않는다. 리플렉션 클래스·struct의 모든 비정적 필드는
// ROCKS_PROPERTY 또는 ROCKS_NATIVE 중 하나다(둘 다면 RMC2204).
//
// 사용 예:
//   ROCKS_CLASS()
//   class Mesh : public rocks::Object {
//       ROCKS_GENERATED_BODY()
//       ROCKS_PROPERTY() rocks::String sourcePath;
//       ROCKS_NATIVE() GpuBufferHandle gpuBuffer{};   // 저장하지 않는 런타임 핸들
//   };
#define ROCKS_NATIVE(...) [[clang::annotate("rocks", "rocks.field.native", #__VA_ARGS__)]]

namespace rocks::detail {

// 생성 setter(ROCKS_SETTER)의 매개변수 형식이다: 산술·enum 스칼라는 값, 그 밖은 const 참조다.
template <class T>
using SetterParam = ConditionalT<IsArithmeticV<T> || IsEnumV<T>, T, const T&>;

} // namespace rocks::detail

// 리플렉션 클래스 필드의 setter를 선언한다(UE `Setter` 지정자 대응 —
// Docs/Design/ChangeNotification.ko.md CN-D13). `Result<void> name(SetterParam<decltype(field)>
// value) noexcept`를 선언하고 메타 컴파일러가 정의를 모듈 생성 .cpp에 쓴다. 정의 순서: 값이
// 같으면(CN-D15 — Object/ValueSame.h) Success로 끝 → 대입(할당하는 값은 사본을 먼저 만들고 교환 —
// 실패면 그 오류, 필드는 그대로) → ROCKS_PROPERTY(Apply = "Method")가 있으면 Method() → Notify
// 필드면 변경 통지(통지 실패는 그 오류 — 값은 이미 새 값). 사본의 allocator는 필드 자신의
// 것(GetAllocator — String·Array·Map·Set·Soft 경로), 없으면 객체 Registry의 객체 allocator,
// Registry도 없으면(생성자 안) Unsupported다. 생성자 안·게시 전 객체는 통지하지 않는다(CN-D9).
//
// 자리: 그 필드 선언 뒤(decltype — 앞이면 컴파일 오류), 같은 클래스 본문. 매크로가 끝 `;`까지
// 펼치므로 세미콜론 없이 쓴다. 대상은 그 클래스가 선언한 ROCKS_PROPERTY 필드이고, struct 멤버·
// 이벤트·bitfield·Registry 참조를 담는
// 필드(ObjectRef·ObjectRefArray·ObjectRefMap·StructRefArray·참조 struct — Registry API로 제자리에
// 쓴다)는 RMC2903이다. EditorOnly 필드의 setter는 그 필드와 같은
// `#if ROCKS_WITH_EDITOR_DATA` 블록 안에 둔다. 이름은 자유다(SetX 강제 없음).
//
// 사용 예:
//   ROCKS_CLASS()
//   class Label : public rocks::Object {
//       ROCKS_GENERATED_BODY()
//
//   public:
//       explicit Label(rocks::Allocator& allocator) noexcept : text(allocator) {}
//
//   private:
//       void RebuildLayout() noexcept;   // text가 바뀐 뒤 setter가 부른다
//
//       ROCKS_PROPERTY(Notify, Apply = "RebuildLayout") rocks::String text;
//       ROCKS_PROPERTY(Notify) float size = 12.0f;
//
//   public:
//       ROCKS_SETTER(SetText, text)      // Result<void> SetText(const String& value) noexcept
//       ROCKS_GETTER(GetText, text)      // const String& GetText() const noexcept
//       ROCKS_SETTER(SetSize, size)      // Result<void> SetSize(float value) noexcept
//   };
#define ROCKS_SETTER(name, field)                                                                  \
    [[clang::annotate("rocks", "rocks.setter", #field)]] ::rocks::Result<void> name(               \
        ::rocks::detail::SetterParam<decltype(field)> value) noexcept;

// 리플렉션 클래스 필드의 getter를 선언·정의한다(CN-D13): `const decltype(field)& name() const
// noexcept`가 필드를 그대로 돌려준다(인라인 — 생성 정의 없음). 자리와 대상 규칙은 ROCKS_SETTER와
// 같다(Registry 참조를 담는 필드도 받는다 — 읽기만 한다). 리플렉션 읽기가 함수를 거치게 하는
// ROCKS_PROPERTY(Getter = "Method")와는 다른 기능이다.
#define ROCKS_GETTER(name, field)                                                                  \
    [[clang::annotate("rocks", "rocks.getter", #field)]] const decltype(field)& name()             \
        const noexcept {                                                                           \
        return field;                                                                              \
    }

// generated typed 접근기만 private/protected 필드 접근을 허용한다. 파생 타입에 펼친 기반 필드도
// 해당 GeneratedType specialization을 통해 접근한다. raw 주소 공개나 Object 수명 검사를 우회하지
// 않는다.
//
// 리플렉션 클래스·struct는 이 매크로를 반드시 둔다(빌드의 메타 단계가 RMC2211로 확인한다). 매크로가
// 타입에 선언만으로 판정되는 표지(rocksReflectedSelf)를 남겨, 생성 헤더를 include하지 않은 번역
// 단위에서 이 타입으로 리플렉션 템플릿을 쓰면 컴파일 오류가 난다(Meta/GeneratedType.h의
// ReflectedDeclarationV, Docs/Design/ReflectionGeneratedSources.ko.md 8.1절). 표지는 정의하지 않는
// 선언이라 타입의 크기·배치·aggregate 여부를 바꾸지 않는다.
#define ROCKS_GENERATED_BODY()                                                                     \
    template <class>                                                                               \
    friend struct ::rocks::GeneratedType;                                                          \
    friend struct ::rocks::detail::ReflectedDeclarationProbe;                                      \
    auto rocksReflectedSelf() noexcept -> decltype(this);                                          \
    [[clang::annotate("rocks", "rocks.generated")]] static void                                    \
    rocksGeneratedAccessMarker() noexcept;

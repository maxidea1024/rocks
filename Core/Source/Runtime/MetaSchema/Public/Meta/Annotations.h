#pragma once

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

// 리플렉션 enum이다(Docs/Design/ReflectionFoundations.ko.md 3.4절). 바로 뒤에 underlying을 명시한
// enum class 정의가 와야 한다(uint8·uint16·uint32·int32, 열거자 하나 이상, 값 별칭 금지 —
// RMC2401). 지정자: ToolPrefix="…"(도구 이름 조회가 앞에 붙어 있어도 받는 접두), Flags(비트 플래그
// — 값은 선언 값의 OR). 열거자에는 ROCKS_ENUMERATOR(Name="…", DisplayName="…", Hidden)를 열거자
// 이름 바로 뒤에 붙일 수 있다(Name은 [A-Za-z0-9_]+ 도구 이름, Hidden은 도구 설정이 받지 않는 값).
// 접두 유무를 합친 도구 이름은 대소문자 무시로 유일해야 한다(RMC2203). 필드에 쓰면 저장 wire는
// I64다.
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
// 지정자: Version=N(Record schema version, 없으면 1). Binds="…"(Core 값 타입 결합)는 B4c에서
// 받는다. struct가 `static Result<Array<uint8>> Migrate(Allocator&, TypeId, uint32 from, uint32 to,
// Span<const uint8> record) noexcept`를 선언하면 옛 버전 Record를 올리는 타입 마이그레이션으로
// 등록된다. private이면 ROCKS_GENERATED_BODY가 필요하고, Migrate라는 이름의 멤버가 이 시그니처가
// 아니면 생성 코드가 컴파일되지 않는다(조용히 마이그레이션 없이 등록되지 않는다).
//
// 사용 예:
//   ROCKS_STRUCT(Version = 2)
//   struct BulkDataRef {
//       ROCKS_PROPERTY() uint32 bulkId = 0;
//       ROCKS_PROPERTY() uint64 rawBytes = 0;
//       ROCKS_PROPERTY() Sha256Digest digest;
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

// generated typed 접근기만 private/protected 필드 접근을 허용한다. 파생 타입에 펼친 기반 필드도
// 해당 GeneratedType specialization을 통해 접근한다. raw 주소 공개나 Object 수명 검사를 우회하지
// 않는다.
#define ROCKS_GENERATED_BODY()                                                                     \
    template <class>                                                                               \
    friend struct ::rocks::GeneratedType;                                                          \
    [[clang::annotate("rocks", "rocks.generated")]] static void                                    \
    rocksGeneratedAccessMarker() noexcept;

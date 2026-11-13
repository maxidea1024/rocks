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

// 리플렉션 클래스 안의 비영속 런타임 멤버다(GPU 핸들·캐시 등). 저장·리플렉션 대상이 아니며 강한
// 객체 참조(ObjectRef·RootRef·Object 파생 타입의 raw 포인터 — 담는 타입·배열·템플릿 인자까지)를
// 담을 수 없다 (RMC2205). RocksRequiresWorldOnlyOwner 형질 타입은 WorldOnly 클래스에만
// 둔다(RMC2206). 지정자는 받지 않는다. 리플렉션 클래스의 모든 비정적 필드는 ROCKS_PROPERTY 또는
// ROCKS_NATIVE 중 하나다(둘 다면 RMC2204).
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

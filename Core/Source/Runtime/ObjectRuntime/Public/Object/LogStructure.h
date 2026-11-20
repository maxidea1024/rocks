#pragma once

#include <Base/TypeTraits.h>
#include <Core/Diagnostics/LogStructure.h>
#include <Core/Text/StringBuilder.h>
#include <Meta/GeneratedType.h>
#include <Object/GeneratedFieldAccess.h>
#include <Object/ObjectHandle.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

// 리플렉션 타입(ROCKS_STRUCT·ROCKS_CLASS)의 로그 구조체 펼치기 일반 특수화다
// (Docs/Design/LoggingSystem.ko.md 14.1.6절 LG16, 결정 L19).
//
// 이 헤더를 include하면 MetaCompiler가 GeneratedType<T>::VisitFields를 낸 모든 타입에
// LogStructure<T>가 생긴다. 그래서 "{@Name}" 자리에 리플렉션 값을 바로 쓸 수 있다.
//
// include 규칙(설계 14.1.6절 L30): 특수화의 선언은 모든 생성 헤더가 include하는
// Object/GeneratedFieldAccess.h에 있고, 정의는 이 헤더에 있다. 리플렉션 타입을 로그 인자로 쓰는
// 번역 단위는({Name}이든 "{@Name}"이든) 이 헤더를 include한다. 빠뜨리면 static_assert로 거부한다.
//
// 사용 예:
//   #include <Game/Player.generated.h>
//   #include <Object/LogStructure.h>
//   Log::Info(LogGame, "Spawned {@Player}", player); // Player { name: "Ann", hp: 30, ... }
//
// 필드 이름은 C++ 멤버 이름이고(스키마 필드 이름과 같다), 순서는 선언
// 순서다(가장 먼 base의 필드부터).
//
// 필드 읽기는 생성 필드 방문 함수(멤버 포인터 직접 읽기)로 한다. ObjectRegistry·TypeRegistry 조회와
// 할당이 없으므로 생산자 경로(설계 8.1절)에서 쓸 수 있다. 공개된 PathRead·FieldValue는 registry가
// 필요하고 문자열을 할당하므로 쓰지 않는다.
//
// 필드 종류별 캡처:
// - Bool·정수·실수: 원시 값, String: byte 사본
// - 리플렉션 enum: 생성 스키마의 열거자 이름(정적 문자열, 선언되지 않은 값이면 바탕 정수)
// - 리플렉션 struct: 중첩 객체(재귀), 리플렉션 클래스 객체도 같다
// - Binds 미러 대상 Core 값(Vec3·Quat·Transform 등): 필드 안에서는 중첩 객체다. 맨 위 "{@Name}"
//   인자로는 쓸 수 없다(일반 특수화에서 뺐다 — 설계 14.1.6절 L30, Formatter나 직접 특수화를 쓴다)
// - 배열(Array·ObjectRefArray·StructRefArray·정적 배열): 앞 MaxLogStructureElements개와 전체 개수
// - Optional: 값이 있으면 그 값, 없으면 null
// - ObjectRef·WeakObjectRef·ObjectRefArray 원소: 역참조하지 않고
//   handle의 "registry:slot:generation" 문자열(null이면 null)이다.
//   다른 스레드에서 대상을 읽으면 경쟁이고 GC가 대상을 옮기거나
//   지울 수 있으므로 id만 담는다.
// - Name·Text·AssetId·Guid·SoftObjectRef·ClassRef·Digest·Map·Set·
//   InstancedStruct: Formatter가 있으면 캡처 때 렌더한 문자열
//   (AssetId·Guid), 없으면 "<unsupported>"
//
// 사용자가 같은 타입에 LogStructure<T>를 직접 특수화하면 그것이 이긴다(완전 특수화가 이 부분
// 특수화보다 구체적이다).
//
// 수명: 필드 이름·타입 태그·열거자 이름은 생성 코드의 정적 문자열을 가리킨다. 생성 코드가 든 모듈을
// 내리기 전에 그 모듈 타입을 담은 로그 기록을 모두 렌더해야 한다(LogSystem::Flush).
namespace rocks {

namespace detail {

// 생성 타입의 로그 타입 태그다. 타입 도구 이름(기본은 한정하지 않은 C++ 이름,
// ROCKS_STRUCT(Name="…")로 바꾼 이름)이고, 도구 이름이 없으면 스키마 이름이다.
template <class T>
StringView GeneratedLogTypeTag() noexcept {
    const TypeSchema& schema = GeneratedType<T>::Schema();
    const Span<const char> name = schema.toolName.Size() != 0 ? schema.toolName : schema.name;
    return StringView(name.Data(), name.Size());
}

// 생성 필드 방문 함수가 부르는 visitor다. 필드마다 Field("이름", 값)이 불린다.
class LogStructureFieldVisitor {
public:
    explicit LogStructureFieldVisitor(LogStructureWriter& out) noexcept : out_(out) {
    }

    // 생성 코드가 문자열 리터럴로 부른다(정적 저장소).
    template <usize Count, class M>
    void Field(const char (&name)[Count], const M& member) noexcept {
        Write(LogStaticName::FromStatic(StringView(name, Count - 1)), member);
    }

    // 값 하나를 리플렉션 값 종류에 맞게 쓴다(배열 원소는 이름이 없다).
    template <class M>
    void Write(LogStaticName name, const M& member) noexcept {
        if constexpr (IsSameV<M, ObjectHandle>) {
            // ObjectRefArray의 원소다.
            WriteHandle(name, member);
        } else if constexpr (GeneratedBindsMirror<M> && GeneratedFieldVisitor<M>) {
            // Binds 미러 대상 struct(Vec3 등)는 일반 특수화가 없다(설계 14.1.6절 L30). 이 필드를
            // 가진 타입의 생성 헤더가 미러의 생성 헤더를 include하므로 방문 함수로 바로 펼친다.
            WriteGeneratedStruct(name, member);
        } else {
            constexpr ValueKind kind = FieldValueKind<M>();
            if constexpr (kind == ValueKind::ObjectReference ||
                          kind == ValueKind::WeakObjectReference) {
                WriteHandle(name, member.Handle());
            } else if constexpr (kind == ValueKind::StaticArray) {
                WriteRange(name, member, StaticArrayField<M>::Count);
            } else if constexpr (kind == ValueKind::Array) {
                WriteRange(name, member, member.Size());
            } else if constexpr (kind == ValueKind::Optional) {
                if (member.HasValue()) {
                    Write(name, member.Value());
                } else {
                    out_.WriteNull(name);
                }
            } else if constexpr (kind == ValueKind::Enum) {
                WriteEnum(name, member);
            } else if constexpr (kind == ValueKind::Struct || kind == ValueKind::Bool ||
                                 kind == ValueKind::UInt8 || kind == ValueKind::UInt16 ||
                                 kind == ValueKind::UInt32 || kind == ValueKind::UInt64 ||
                                 kind == ValueKind::Int8 || kind == ValueKind::Int16 ||
                                 kind == ValueKind::Int32 || kind == ValueKind::Int64 ||
                                 kind == ValueKind::Float32 || kind == ValueKind::Float64 ||
                                 kind == ValueKind::Utf8String) {
                // 원시 값·문자열·중첩 struct(LogStructure)는 Core의 일반 쓰기가 고른다.
                out_.Write(name, member);
            } else if constexpr (HasLogStructure<M> || HasFormatter<M>) {
                // Formatter가 있는 그 밖의 종류(AssetId·Guid 등)와 사용자 특수화다.
                out_.Write(name, member);
            } else {
                out_.WriteUnsupported(name);
            }
        }
    }

private:
    template <class M>
    void WriteGeneratedStruct(LogStaticName name, const M& member) noexcept {
        if (out_.BeginObject(name, LogStaticName::FromStatic(GeneratedLogTypeTag<M>()))) {
            GeneratedType<M>::VisitFields(member, *this);
        }

        out_.EndObject();
    }

    // handle을 "registry:slot:generation" 문자열로 담는다(역참조하지 않는다).
    // registry는 Registry 도메인 번호다. 프로세스 안에서 만든 순서대로 1부터
    // 매기는 수이고 주소가 아니라서, 같은 실행 순서면 출력이 같다.
    void WriteHandle(LogStaticName name, ObjectHandle handle) noexcept {
        if (handle.IsNull()) {
            out_.WriteNull(name);
            return;
        }

        // uint64 하나(20자리)·uint32 둘(10자리씩)·구분자 둘·끝 NUL이 들어가는 크기다.
        constexpr usize HandleTextBytes = 64;
        char storage[HandleTextBytes];
        FixedStringBuilder text{Span<char>(storage)};
        (void)text.TryAppendFormat("{}:{}:{}", handle.registry, handle.slot, handle.generation);
        out_.WriteString(name, text.View());
    }

    template <class E>
    void WriteEnum(LogStaticName name, E value) noexcept {
        const EnumSchema& schema = GeneratedType<E>::Schema();
        const auto raw = static_cast<int64>(value);
        for (usize index = 0; index < schema.values.Size(); ++index) {
            const EnumeratorSchema& enumerator = schema.values[index];
            if (enumerator.value == raw) {
                out_.WriteSymbol(name, LogStaticName::FromStatic(StringView(
                                           enumerator.name.Data(), enumerator.name.Size())));
                return;
            }
        }

        out_.Write(name, static_cast<UnderlyingTypeT<E>>(value));
    }

    template <class C>
    void WriteRange(LogStaticName name, const C& container, usize count) noexcept {
        if (out_.BeginArray(name, count)) {
            const usize kept = count < MaxLogStructureElements ? count : MaxLogStructureElements;
            for (usize index = 0; index < kept; ++index) {
                Write(LogStaticName{}, container[index]);
            }
        }

        out_.EndArray();
    }

    LogStructureWriter& out_;
};

} // namespace detail

// 리플렉션 타입의 일반 특수화다(Binds 미러 대상은 빼고, 그런 필드는 visitor가 직접 펼친다). 타입
// 태그는 GeneratedLogTypeTag다.
template <class T>
    requires detail::GeneratedLogFields<T>
struct LogStructure<T> {
    static StringView TypeTag() noexcept {
        return detail::GeneratedLogTypeTag<T>();
    }

    static void Write(LogStructureWriter& out, const T& value) noexcept {
        detail::LogStructureFieldVisitor visitor(out);
        GeneratedType<T>::VisitFields(value, visitor);
    }
};

} // namespace rocks

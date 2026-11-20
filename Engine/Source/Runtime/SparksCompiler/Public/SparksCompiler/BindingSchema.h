#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>

// 엔진 binding schema 파일(`<모듈>.sparkbind.json`)이다(Docs/Design/SparksLanguage.ko.md 10.2절
// "binding schema 파일 형식", 18.1절, 진단 S1120).
//
// 사용 예:
//   BindingSchema schema(allocator);
//   auto read = ReadBindingSchema(sources, schemaId, diagnostics, schema);
//   if (read && read.Value()) {
//       for (const SchemaType& type : schema.Types()) {
//           Use(schema.Text(type.name));
//       }
//   }
//
// 구조: 타입·필드·함수·매개변수·타입 매개변수·이벤트·enum case를 평평한 배열에 담고, 타입·함수·
// 이벤트가 자기 항목의 구간(first, count)을 가진다. 글자는 Text(SchemaText)로 읽는다. 타입 표기는
// 문법만 검사한 문자열이며 이름 해석은 뒤 단계(LC2b)가 한다.
//
// 수명: 값은 schema 객체가 가진다(원문과 무관). 한 객체는 한 스레드에서 쓴다.

namespace rocks::sparks {

// 이 reader가 아는 schema 형식 번호다(`format`).
inline constexpr uint32 BindingSchemaFormat = 1;

// TypeId·FieldId·FunctionId 표기의 16진 자리 수다(`.rocksids.json`과 같다).
inline constexpr usize StableIdHexDigits = 32;

enum class SchemaTypeKind : uint8 {
    Object,
    Asset,
    Struct,
    Enum,
    Interface
};

enum class SchemaPublisher : uint8 {
    All,
    Server,
    Owner
};

// schema 글자 하나(이름·ID·타입 표기)다. length가 0이면 없음이다.
struct SchemaText {
    uint32 offset = 0;
    uint32 length = 0;
};

struct SchemaType {
    SchemaText name;
    SchemaText id;
    SchemaText base; // 없으면 길이 0
    SchemaTypeKind kind = SchemaTypeKind::Object;
    bool scriptExtensible = false;
    bool constructible = true;
    uint32 firstField = 0;
    uint32 fieldCount = 0;
    uint32 firstFunction = 0;
    uint32 functionCount = 0;
    uint32 firstEvent = 0;
    uint32 eventCount = 0;
    uint32 firstCase = 0;
    uint32 caseCount = 0;
    SourceSpan span;   // `name` 값
    SourceSpan idSpan; // `id` 값
};

struct SchemaField {
    SchemaText name;
    SchemaText id; // computed면 길이 0
    SchemaText type;
    bool readWrite = false;
    bool computed = false;
    bool changed = false;
    SourceSpan span;
    SourceSpan idSpan; // computed면 빈 범위
};

struct SchemaParameter {
    SchemaText name;
    SchemaText type;
    bool perSecond = false;
    SourceSpan span;
};

struct SchemaTypeParameter {
    SchemaText name;
    SchemaText constraint; // 없으면 길이 0
    SourceSpan span;
};

struct SchemaFunction {
    SchemaText name;
    SchemaText id;
    SchemaText returns; // 없으면 void
    bool isStatic = false;
    bool isAsync = false;
    uint32 firstTypeParameter = 0;
    uint32 typeParameterCount = 0;
    uint32 firstParameter = 0;
    uint32 parameterCount = 0;
    SourceSpan span;
    SourceSpan idSpan;
};

struct SchemaEvent {
    SchemaText name;
    SchemaText id;
    SchemaPublisher publisher = SchemaPublisher::All;
    uint32 firstParameter = 0;
    uint32 parameterCount = 0;
    SourceSpan span;
    SourceSpan idSpan;
};

struct SchemaCase {
    SchemaText name;
    SourceSpan span;
};

namespace detail {
class SchemaReader;
} // namespace detail

class BindingSchema final {
public:
    explicit BindingSchema(Allocator& allocator) noexcept;

    BindingSchema(const BindingSchema&) = delete;
    BindingSchema& operator=(const BindingSchema&) = delete;
    BindingSchema(BindingSchema&&) noexcept = default;
    BindingSchema& operator=(BindingSchema&&) noexcept = default;
    ~BindingSchema() noexcept = default;

    SourceId Source() const noexcept {
        return source_;
    }

    StringView Package() const noexcept {
        return Text(package_);
    }

    StringView Module() const noexcept {
        return Text(module_);
    }

    // `module` 값의 원문 범위다(파일 사이 모듈 이름 중복의 위치).
    SourceSpan ModuleSpan() const noexcept {
        return moduleSpan_;
    }

    Span<const SchemaType> Types() const noexcept {
        return types_.View();
    }

    Span<const SchemaField> Fields() const noexcept {
        return fields_.View();
    }

    Span<const SchemaFunction> Functions() const noexcept {
        return functions_.View();
    }

    // 함수와 이벤트의 매개변수다(각자 first·count 구간).
    Span<const SchemaParameter> Parameters() const noexcept {
        return parameters_.View();
    }

    Span<const SchemaTypeParameter> TypeParameters() const noexcept {
        return typeParameters_.View();
    }

    Span<const SchemaEvent> Events() const noexcept {
        return events_.View();
    }

    Span<const SchemaCase> Cases() const noexcept {
        return cases_.View();
    }

    StringView Text(SchemaText text) const noexcept {
        return StringView(text_.Data() + text.offset, text.length);
    }

private:
    friend class detail::SchemaReader;

    Array<char> text_;
    Array<SchemaType> types_;
    Array<SchemaField> fields_;
    Array<SchemaFunction> functions_;
    Array<SchemaParameter> parameters_;
    Array<SchemaTypeParameter> typeParameters_;
    Array<SchemaEvent> events_;
    Array<SchemaCase> cases_;
    SchemaText package_;
    SchemaText module_;
    SourceSpan moduleSpan_;
    SourceId source_;
};

// id 파일(binding schema)을 읽어 out을 채운다. 형식 오류는 diagnostics에 S1120으로 붙이고(오류마다
// 하나, 계속 읽어 모두 알린다) false를 돌려준다 — 그때 out의 내용은 정해지지 않는다. 한 파일 안의
// 검사만 한다(여러 파일 사이의 이름·ID 중복은 Compilation이 본다). 파일은 IsUsable()이어야 한다
// (아니면 InvalidArgument). 실패: 할당 실패.
Result<bool> ReadBindingSchema(const SourceSet& sources, SourceId id, DiagnosticList& diagnostics,
                               BindingSchema& out) noexcept;

// TypeId 등 안정 ID 표기(32자리 소문자 16진, 모두 0은 아님 — `.rocksids.json`과 같다)인지다.
bool IsStableIdText(StringView text) noexcept;

} // namespace rocks::sparks

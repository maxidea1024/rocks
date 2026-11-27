#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Optional.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <MaterialSchema/NodePropertyList.h>
#include <Meta/TypeSchema.h>
#include <Serialization/Record.h>

// 노드 속성 Record의 필드 읽기다(Docs/Design/MaterialSystem 19.13.3절, NodeGraph 6.2절). 동적 핀
// 함수(nodegraph::DynamicPinsFn)는 문서 view와 NamePool만 받으므로 객체 Registry 없이 속성 바이트를
// 직접 읽는다. 필드는 리플렉션 schema의 필드 이름으로 찾고(id는 schema가 준다), 값은 리플렉션
// codec이 쓴 wire 그대로 읽는다. Record에 없는 필드는 기본값이다(누락 = 기준 — Docs/Design/
// ObjectFoundations.ko.md 4절). 읽은 문자열은 입력 바이트를 빌린다: NodeGraph 6.2절대로 동적 핀의
// 표시 이름은 노드 속성 바이트를 가리킬 수 있다.
//
// 수명: reader와 그것이 돌려준 StringView·하위 reader는 입력 바이트와 allocator보다 먼저 끝난다.
// 스레드: 한 reader를 여러 스레드가 함께 쓰지 않는다.
//
// 사용 예(Custom 노드의 입력 이름들):
//   auto reader = NodePropertyReader::Open(allocator, node.properties.bytes.View(),
//                                          GeneratedType<CustomProperties>::Schema());
//   auto inputs = reader.Value().OpenStructArray("inputs", GeneratedType<CustomInput>::Schema());
//   for (usize index = 0; index < inputs.Value().Count(); ++index) {
//       auto element = inputs.Value().Element(index);
//       auto name = element.Value().ReadText("inputName");
//   }
namespace rocks::material {

class ROCKS_MATERIALSCHEMA_API NodePropertyReader {
public:
    // bytes가 비어 있으면 모든 필드가 기본값인 reader다(속성을 쓰지 않은 노드 — NodeGraph GD23).
    // 오류: Record schema가 schema.id가 아니면 TypeMismatch, schema 버전이 1..schema.version 밖이면
    // Unsupported, header·TLV 손상은 InvalidArgument, 할당 실패.
    static Result<NodePropertyReader> Open(Allocator& allocator, Span<const uint8> bytes,
                                           const TypeSchema& schema) noexcept;

    NodePropertyReader(NodePropertyReader&&) noexcept = default;
    NodePropertyReader& operator=(NodePropertyReader&&) noexcept = default;
    NodePropertyReader(const NodePropertyReader&) = delete;
    NodePropertyReader& operator=(const NodePropertyReader&) = delete;

    // 기록된 필드의 값 view다. 기록되지 않았으면(기본값) 빈 Optional이다. 오류: schema에 그 이름의
    // 필드가 없으면 NotFound(호출 코드의 잘못 — 이름은 정적이다), Record 오류.
    Result<Optional<FieldView>> Find(StringView field) noexcept;

    // 정수 필드(정수 스칼라·enum)다. 기록되지 않았으면 fallback이다. 오류: wire가 정수가 아니면
    // TypeMismatch, 값이 int64 밖이면 Overflow, Record 오류.
    Result<int64> ReadInteger(StringView field, int64 fallback) noexcept;
    Result<bool> ReadBool(StringView field, bool fallback) noexcept;
    Result<float> ReadFloat(StringView field, float fallback) noexcept;
    Result<double> ReadDouble(StringView field, double fallback) noexcept;
    Result<Guid> ReadGuid(StringView field, Guid fallback) noexcept;
    // String·Name 필드의 원문이다(입력 바이트를 빌린다). 기록되지 않았으면 빈 문자열이다.
    Result<StringView> ReadText(StringView field) noexcept;

    // struct 배열 필드를 연다(원소 schema는 element). 기록되지 않았으면 원소 0개다.
    Result<NodePropertyList> OpenStructArray(StringView field, const TypeSchema& element) noexcept;

    // struct 필드를 연다(schema는 element). 기록되지 않았으면 모든 멤버가 기본값인 reader다.
    Result<NodePropertyReader> OpenStruct(StringView field, const TypeSchema& element) noexcept;

    // Guid 배열 필드의 원소를 out 끝에 더한다. 기록되지 않았으면 더하지 않는다. 오류: wire가 Guid
    // 원소 배열이 아니면 TypeMismatch, 원소 수가 필드 상한을 넘으면 CapacityExceeded, 할당 실패.
    Result<void> ReadGuidArray(StringView field, Array<Guid>& out) noexcept;

    // String·Name 배열 필드의 원소 원문을 out 끝에 더한다(입력 바이트를 빌린다). 오류는
    // ReadGuidArray와 같다(원소 wire는 String).
    Result<void> ReadTextArray(StringView field, Array<StringView>& out) noexcept;

private:
    friend class NodePropertyList;

    NodePropertyReader(Allocator& allocator, const TypeSchema& schema) noexcept
        : allocator_(&allocator), schema_(&schema) {
    }

    Allocator* allocator_;
    const TypeSchema* schema_;
    Optional<RecordReader> record_;
};

} // namespace rocks::material

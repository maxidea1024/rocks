#pragma once

#include <Base/Types.h>
#include <Core/Memory/Allocator.h>
#include <Core/Optional.h>
#include <Core/Result.h>
#include <Meta/TypeSchema.h>
#include <Serialization/RecordList.h>

// 노드 속성 Record의 struct 배열 필드 읽기다(Docs/Design/MaterialSystem 19.13.3절).
// NodePropertyReader가 열고, 원소마다 NodePropertyReader를 돌려준다. 수명·스레드 규칙은
// NodePropertyReader와 같다.
namespace rocks::material {

class NodePropertyReader;

// struct 배열 필드의 원소들이다(NodePropertyReader::OpenStructArray).
class ROCKS_MATERIALSCHEMA_API NodePropertyList {
public:
    NodePropertyList(NodePropertyList&&) noexcept = default;
    NodePropertyList& operator=(NodePropertyList&&) noexcept = default;
    NodePropertyList(const NodePropertyList&) = delete;
    NodePropertyList& operator=(const NodePropertyList&) = delete;

    usize Count() const noexcept;

    // 원소 index를 읽는 reader다. 오류: index가 범위 밖이면 InvalidArgument, 원소 Record 오류.
    Result<NodePropertyReader> Element(usize index) noexcept;

private:
    friend class NodePropertyReader;

    NodePropertyList(Allocator& allocator, const TypeSchema& element) noexcept
        : allocator_(&allocator), element_(&element) {
    }

    Allocator* allocator_;
    const TypeSchema* element_;
    Optional<RecordListReader> list_;
};

} // namespace rocks::material

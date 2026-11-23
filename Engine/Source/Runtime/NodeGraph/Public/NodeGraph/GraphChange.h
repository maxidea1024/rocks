#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <NodeGraph/GraphDocument.h>

// 문서 변경 통지다(Docs/Design/NodeGraph 8.3·8.6절). GraphMutator가 명령·undo·redo·bracket
// 취소 하나마다 묶음 하나를 구독자에게 보낸다.
//
// 사용 예:
//   class Canvas final : public GraphChangeListener {
//   public:
//       void OnGraphChanged(const GraphChangeBatch& batch) noexcept override {
//           for (const GraphChange& change : batch.changes) { ... 증분 갱신 ... }
//       }
//   };
//   mutator.AddListener(canvas);
namespace rocks::nodegraph {

// 통지 종류다(8.3절 표).
enum class GraphChangeKind : uint8 {
    NodeAdded,
    NodeRemoved,
    NodePropertiesChanged,
    NodePinsChanged,
    InputDefaultChanged,
    EdgeAdded,
    EdgeRemoved,
    EdgeOrderChanged,
    NodeLayoutChanged,
    CommentChanged,
    BookmarkChanged,
    NodeEnableStateChanged,
    GraphAdded,
    GraphRemoved,
    GraphRenamed,
    RetainedPinChanged
};

// 통지 하나다. 칸의 뜻은 종류마다 다르고, 해당하지 않는 칸은 0·None이다.
//   Node*·InputDefault·RetainedPin: node = NodeId, pin = PinKey(핀이 있는 종류)
//   Edge*: node = 입력 NodeId, pin = 입력 PinKey, other = 출력 NodeId, otherPin = 출력 PinKey
//          (EdgeOrderChanged는 출력 핀 하나: node = 출력 NodeId, pin = 출력 PinKey)
//   CommentChanged: node = 주석 id. BookmarkChanged: number = 번호. Graph*: graph만.
struct GraphChange {
    GraphChangeKind kind = GraphChangeKind::NodeAdded;
    Guid graph;
    Guid node;
    Guid other;
    Name pin;
    Name otherPin;
    uint32 number = 0;
};

// 통지 묶음의 원인이다.
enum class GraphChangeCause : uint8 {
    Edit,
    Undo,
    Redo,
    // bracket 취소로 되돌렸다.
    Cancel
};

// 명령 하나(또는 undo·redo·취소·bracket 확정)의 통지 묶음이다.
struct GraphChangeBatch {
    explicit GraphChangeBatch(Allocator& allocator) noexcept : changes(allocator) {
    }

    // 묶음 뒤 문서 revision이다.
    uint64 revision = 0;
    // 의미 데이터(GraphDocument)를 건드렸는가. 재경유만 바뀌어도 참이다.
    bool semanticTouched = false;
    GraphChangeCause cause = GraphChangeCause::Edit;
    // bracket 안 명령의 묶음이다(확정은 EndInteraction의 묶음).
    bool interactive = false;
    Array<GraphChange> changes;
};

// 통지 구독자다. mutator 소유 lane에서 부른다. 콜백 안에서 같은 mutator를 바꾸면 안 된다.
class GraphChangeListener {
public:
    virtual ~GraphChangeListener() noexcept = default;

    virtual void OnGraphChanged(const GraphChangeBatch& batch) noexcept = 0;
};

} // namespace rocks::nodegraph

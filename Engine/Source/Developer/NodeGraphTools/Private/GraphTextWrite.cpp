#include "GraphTextInternal.h"

#include <Core/Math/MathUtility.h>
#include <Core/Text/NamePool.h>
#include <NodeGraph/GraphNameOrder.h>

// 문서·조각 JSON 쓰기다(Docs/Design/NodeGraph 9.4절). 키 순서는 형식이 정한
// 순서이고(JsonWriter는 호출 순서대로 쓴다) 배열은 문서의 정규 순서다. 그래서 같은 입력은 같은
// 바이트다.
namespace rocks::nodegraph {
namespace {

using reflection_text::TextUse;
using namespace text_detail;

// 기본값(+0.0)인지 비트로 본다(-0.0은 기본값과 바이트가 달라 써야 한다).
bool IsPositiveZero(float value) noexcept {
    return platform::BitCast<uint32>(value) == 0;
}

// 첫 실패를 기억하며 이어 쓰는 도우미다(JsonWriter 오류는 sticky라 마지막 Finish만 봐도 되지만,
// Name· 레코드 값처럼 writer 밖에서 나는 오류를 함께 모은다).
class Writer {
public:
    Writer(GraphTextContext& context, JsonWriter& json, TextUse use, bool records) noexcept
        : context_(context), json_(json), use_(use), records_(records) {
    }

    Result<void> Status() const noexcept {
        return error_ == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error_);
    }

    void Note(const Result<void>& result) noexcept {
        if (!result && error_ == ErrorCode::None) {
            error_ = result.Error();
        }
    }

    void Begin() noexcept {
        Note(json_.BeginObject());
    }

    void End() noexcept {
        Note(json_.EndObject());
    }

    void BeginArray(StringView key) noexcept {
        Note(json_.Key(key));
        Note(json_.BeginArray());
    }

    void EndArray() noexcept {
        Note(json_.EndArray());
    }

    void Key(StringView key) noexcept {
        Note(json_.Key(key));
    }

    void String(StringView key, StringView value) noexcept {
        Note(json_.Key(key));
        Note(json_.String(value));
    }

    void UInt(StringView key, uint64 value) noexcept {
        Note(json_.Key(key));
        Note(json_.UInt64(value));
    }

    void Float(StringView key, float value) noexcept {
        Note(json_.Key(key));
        Note(json_.Float(value));
    }

    void Bool(StringView key, bool value) noexcept {
        Note(json_.Key(key));
        Note(json_.Bool(value));
    }

    void GuidValue(Guid value) noexcept {
        const GuidText text = FormatGuidText(value);
        Note(json_.String(text.View()));
    }

    void GuidKey(StringView key, Guid value) noexcept {
        Note(json_.Key(key));
        GuidValue(value);
    }

    void NameValue(Name name) noexcept {
        auto text = context_.names.Acquire(name);
        if (!text) {
            Note(Result<void>::Failure(text.Error()));
            return;
        }

        Note(json_.String(text.Value().View()));
    }

    void NameKey(StringView key, Name name) noexcept {
        Note(json_.Key(key));
        NameValue(name);
    }

    // 객체 키로 Name 표시 문자열을 쓴다(입력 기본값 객체).
    void NameAsKey(Name name) noexcept {
        auto text = context_.names.Acquire(name);
        if (!text) {
            Note(Result<void>::Failure(text.Error()));
            return;
        }

        Note(json_.Key(text.Value().View()));
    }

    void Record(StringView key, const PropertyRecord& record) noexcept {
        Note(json_.Key(key));
        Note(WriteRecordValue(context_, record, use_, records_, json_));
    }

    void RecordValue(const PropertyRecord& record) noexcept {
        Note(WriteRecordValue(context_, record, use_, records_, json_));
    }

    void Edge(const GraphEdge& edge) noexcept {
        Begin();
        Key("from");
        Note(json_.BeginArray());
        GuidValue(edge.fromNode);
        NameValue(edge.fromPin);
        EndArray();
        Key("to");
        Note(json_.BeginArray());
        GuidValue(edge.toNode);
        NameValue(edge.toPin);
        EndArray();
        if (edge.order != 0) {
            UInt("order", edge.order);
        }

        End();
    }

    void Layout(const NodeLayout& layout) noexcept {
        Key("layout");
        Begin();
        Float("x", layout.position.x);
        Float("y", layout.position.y);
        if (!IsPositiveZero(layout.size.x)) {
            Float("width", layout.size.x);
        }

        if (!IsPositiveZero(layout.size.y)) {
            Float("height", layout.size.y);
        }

        if (layout.collapsed) {
            Bool("collapsed", true);
        }

        if (layout.showAdvanced) {
            Bool("showAdvanced", true);
        }

        if (!layout.bubble.IsEmpty()) {
            String("bubble", layout.bubble.View());
        }

        if (layout.bubbleVisible) {
            Bool("bubbleVisible", true);
        }

        End();
    }

    void Node(const GraphNode& node, const NodeLayout* layout) noexcept {
        Begin();
        GuidKey("id", node.id);
        NameKey("type", node.type);
        UInt("typeVersion", node.typeSchemaVersion);
        if (!node.properties.bytes.IsEmpty()) {
            Record("properties", node.properties);
        }

        if (node.enableState == NodeEnableState::Disabled) {
            String("enableState", "Disabled");
        }

        if (!node.inputDefaults.IsEmpty()) {
            Key("inputDefaults");
            Begin();
            for (const PinValueEntry& entry : node.inputDefaults) {
                NameAsKey(entry.pin);
                RecordValue(entry.value);
            }

            End();
        }

        if (!node.retainedPins.IsEmpty()) {
            BeginArray("retainedPins");
            for (const RetainedPin& pin : node.retainedPins) {
                Begin();
                NameKey("key", pin.key);
                String("direction",
                       pin.direction == PinDirection::Input ? StringView("Input") : "Output");
                if (pin.lastType != 0) {
                    UInt("lastType", pin.lastType);
                }

                if (!pin.value.bytes.IsEmpty()) {
                    Record("value", pin.value);
                }

                End();
            }

            EndArray();
        }

        if (layout != nullptr) {
            Layout(*layout);
        }

        End();
    }

    void Comment(const CommentBox& comment) noexcept {
        Begin();
        GuidKey("id", comment.id);
        Float("x", comment.position.x);
        Float("y", comment.position.y);
        Float("width", comment.size.x);
        Float("height", comment.size.y);
        String("text", comment.text.View());
        Key("color");
        Note(json_.BeginArray());
        Note(json_.Float(comment.color.r));
        Note(json_.Float(comment.color.g));
        Note(json_.Float(comment.color.b));
        Note(json_.Float(comment.color.a));
        EndArray();
        UInt("fontSize", comment.fontSize);
        End();
    }

    void Bookmark(const rocks::nodegraph::Bookmark& bookmark) noexcept {
        Begin();
        UInt("number", bookmark.number);
        Float("x", bookmark.viewCenter.x);
        Float("y", bookmark.viewCenter.y);
        Float("zoom", bookmark.zoom);
        End();
    }

private:
    GraphTextContext& context_;
    JsonWriter& json_;
    TextUse use_;
    bool records_;
    ErrorCode error_ = ErrorCode::None;
};

StringView RoleName(GraphRole role) noexcept {
    switch (role) {
    case GraphRole::Main:
        return "Main";
    case GraphRole::Collapsed:
        return "Collapsed";
    case GraphRole::DomainDefined:
        return "DomainDefined";
    }

    return "Main";
}

const GraphLayout* FindGraphLayout(const GraphDocumentLayout* layout, Guid graph) noexcept {
    if (layout == nullptr) {
        return nullptr;
    }

    for (const GraphLayout& entry : layout->graphs) {
        if (entry.graph == graph) {
            return &entry;
        }
    }

    return nullptr;
}

// 정규 순서(노드 id 순)의 배치 배열에서 노드 배치를 찾는다. 정렬되어 있지 않으면 선형으로 찾는다.
const NodeLayout* FindNodeLayout(const GraphLayout* graph, Guid node) noexcept {
    if (graph == nullptr) {
        return nullptr;
    }

    usize low = 0;
    usize high = graph->nodes.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const int32 compared = CompareGuid(graph->nodes[middle].node, node);
        if (compared == 0) {
            return &graph->nodes[middle];
        }

        if (compared < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    for (const NodeLayout& entry : graph->nodes) {
        if (entry.node == node) {
            return &entry;
        }
    }

    return nullptr;
}

} // namespace

StringView GraphTextCodeName(GraphTextCode code) noexcept {
    switch (code) {
    case GraphTextCode::JsonSyntax:
        return "JsonSyntax";
    case GraphTextCode::UnsupportedFormat:
        return "UnsupportedFormat";
    case GraphTextCode::UnknownKey:
        return "UnknownKey";
    case GraphTextCode::MissingKey:
        return "MissingKey";
    case GraphTextCode::InvalidValue:
        return "InvalidValue";
    case GraphTextCode::UnknownRecordType:
        return "UnknownRecordType";
    case GraphTextCode::RecordText:
        return "RecordText";
    }

    return StringView();
}

Result<void> GraphTextDiagnostics::Add(GraphTextCode code, JsonPosition position,
                                       StringView message) noexcept {
    auto added = items.TryEmplaceBack(items.GetAllocator());
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    GraphTextDiagnostic& item = *added.Value();
    item.code = code;
    item.line = position.line;
    item.column = position.column;
    return item.message.TryAssign(message);
}

Result<void> WriteGraphDocumentJson(GraphTextContext& context, const GraphDocument& document,
                                    const GraphDocumentLayout* layout,
                                    const GraphTextWriteOptions& options,
                                    StringBuilderBase& out) noexcept {
    JsonWriter json(out);
    Writer writer(context, json, TextUse::Edit, options.records);
    const GraphDocumentLayout* layouts = options.semanticOnly ? nullptr : layout;
    writer.Begin();
    writer.String("format", DocumentFormat);
    writer.UInt("formatVersion", TextFormatVersion);
    writer.NameKey("domain", document.domain);
    writer.UInt("documentFormatVersion", document.documentFormatVersion);
    writer.GuidKey("mainGraph", document.mainGraph);
    writer.BeginArray("graphs");
    for (const Graph& graph : document.graphs) {
        const GraphLayout* graphLayout = FindGraphLayout(layouts, graph.id);
        writer.Begin();
        writer.GuidKey("id", graph.id);
        writer.String("role", RoleName(graph.role));
        if (graph.domainRole != 0) {
            writer.UInt("domainRole", graph.domainRole);
        }

        if (graphLayout != nullptr && !graphLayout->title.IsEmpty()) {
            writer.String("title", graphLayout->title.View());
        }

        writer.BeginArray("nodes");
        for (const GraphNode& node : graph.nodes) {
            writer.Node(node, FindNodeLayout(graphLayout, node.id));
        }

        writer.EndArray();
        writer.BeginArray("edges");
        for (const GraphEdge& edge : graph.edges) {
            writer.Edge(edge);
        }

        writer.EndArray();
        if (layouts != nullptr) {
            writer.BeginArray("comments");
            if (graphLayout != nullptr) {
                for (const CommentBox& comment : graphLayout->comments) {
                    writer.Comment(comment);
                }
            }

            writer.EndArray();
            writer.BeginArray("bookmarks");
            if (graphLayout != nullptr) {
                for (const Bookmark& bookmark : graphLayout->bookmarks) {
                    writer.Bookmark(bookmark);
                }
            }

            writer.EndArray();
        }

        writer.End();
    }

    writer.EndArray();
    writer.End();
    writer.Note(json.Finish());
    return writer.Status();
}

Result<void> WriteGraphFragmentJson(GraphTextContext& context, const GraphFragment& fragment,
                                    const GraphTextWriteOptions& options,
                                    StringBuilderBase& out) noexcept {
    for (const GraphFragmentNode& node : fragment.nodes) {
        if (!node.unresolvedType.IsEmpty()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    JsonWriter json(out);
    Writer writer(context, json, TextUse::Copy, options.records);
    writer.Begin();
    writer.String("format", FragmentFormat);
    writer.UInt("formatVersion", TextFormatVersion);
    writer.NameKey("domain", fragment.domain);
    if (!(fragment.sourceAsset == Guid{})) {
        writer.GuidKey("sourceAsset", fragment.sourceAsset);
    }

    writer.GuidKey("sourceGraph", fragment.sourceGraph);
    writer.BeginArray("nodes");
    for (const GraphFragmentNode& node : fragment.nodes) {
        writer.Node(node.node, &node.layout);
    }

    writer.EndArray();
    writer.BeginArray("edges");
    for (const GraphEdge& edge : fragment.edges) {
        writer.Edge(edge);
    }

    writer.EndArray();
    writer.BeginArray("externalEdges");
    for (const GraphEdge& edge : fragment.externalEdges) {
        writer.Edge(edge);
    }

    writer.EndArray();
    writer.BeginArray("comments");
    for (const CommentBox& comment : fragment.comments) {
        writer.Comment(comment);
    }

    writer.EndArray();
    writer.End();
    writer.Note(json.Finish());
    return writer.Status();
}

} // namespace rocks::nodegraph

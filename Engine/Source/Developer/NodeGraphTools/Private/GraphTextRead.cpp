#include "GraphTextInternal.h"

#include <Core/Guid.h>
#include <Core/Math/MathUtility.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/NumberText.h>
#include <NodeGraph/GraphValidation.h>

// 문서·조각 JSON 읽기다(Docs/Design/NodeGraph 9.4절). Json5 문법을 받고, 모든 객체에서 모르는
// 키를 거부하며, 첫 오류에서 멈추고 그 줄·열을 진단으로 남긴다(부분 읽기 없음).
namespace rocks::nodegraph {
namespace {

using reflection_text::TextUse;
using namespace text_detail;

// 주석 상자 색 성분 수다(r, g, b, a).
constexpr usize ColorChannels = 4;

class Reader {
public:
    Reader(GraphTextContext& context, GraphTextDiagnostics& diagnostics, TextUse use,
           bool fragment) noexcept
        : context_(context), diagnostics_(diagnostics), use_(use), fragment_(fragment) {
    }

    Result<void> Fail(GraphTextCode code, JsonValue at, StringView message) noexcept {
        if (auto added = diagnostics_.Add(code, at.Position(), message); !added) {
            return added;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 객체이고 키가 모두 allowed 안인지 본다.
    Result<void> Object(JsonValue value, Span<const StringView> allowed) noexcept {
        if (!value.IsObject()) {
            return Fail(GraphTextCode::InvalidValue, value, "expected an object");
        }

        for (usize index = 0; index < value.Size(); ++index) {
            const StringView key = value.KeyAt(index);
            bool known = false;
            for (const StringView& candidate : allowed) {
                known = known || candidate == key;
            }

            if (!known) {
                return Fail(GraphTextCode::UnknownKey, value.ValueAt(index), "unknown key");
            }
        }

        return Result<void>::Success();
    }

    Result<JsonValue> Require(JsonValue object, StringView key) noexcept {
        const JsonValue value = object.Find(key);
        if (!value.IsValid()) {
            auto failed = Fail(GraphTextCode::MissingKey, object, key);
            return Result<JsonValue>::Failure(failed.Error());
        }

        return Result<JsonValue>::Success(value);
    }

    Result<void> ReadString(JsonValue value, StringView& out) noexcept {
        if (!value.IsString()) {
            return Fail(GraphTextCode::InvalidValue, value, "expected a string");
        }

        out = value.GetString();
        return Result<void>::Success();
    }

    Result<void> ReadText(JsonValue value, String& out) noexcept {
        StringView text;
        if (auto read = ReadString(value, text); !read) {
            return read;
        }

        return out.TryAssign(text);
    }

    Result<void> ReadGuid(JsonValue value, Guid& out) noexcept {
        StringView text;
        if (auto read = ReadString(value, text); !read) {
            return read;
        }

        auto parsed = TryParseGuid(text);
        if (!parsed) {
            return Fail(GraphTextCode::InvalidValue, value, "expected a Guid");
        }

        out = parsed.Value();
        return Result<void>::Success();
    }

    Result<void> ReadName(JsonValue value, Name& out) noexcept {
        StringView text;
        if (auto read = ReadString(value, text); !read) {
            return read;
        }

        auto interned = context_.names.TryIntern(text);
        if (!interned) {
            if (interned.Error() == ErrorCode::InvalidArgument) {
                return Fail(GraphTextCode::InvalidValue, value, "expected a name");
            }

            return Result<void>::Failure(interned.Error());
        }

        out = interned.Value();
        return Result<void>::Success();
    }

    Result<void> ReadU32(JsonValue value, uint32& out) noexcept {
        auto number = value.IsNumber() ? value.TryGetUInt64()
                                       : Result<uint64>::Failure(ErrorCode::TypeMismatch);
        if (!number || number.Value() > NumericLimits<uint32>::Max()) {
            return Fail(GraphTextCode::InvalidValue, value, "expected an unsigned 32-bit integer");
        }

        out = static_cast<uint32>(number.Value());
        return Result<void>::Success();
    }

    // 실수는 원문 숫자를 단정밀도로 바로 읽는다(double을 거치면 이중 반올림 — Json.h Float 주석).
    Result<void> ReadFloat(JsonValue value, float& out) noexcept {
        auto number = value.IsNumber() ? TryParseFloat(value.RawNumber())
                                       : Result<float>::Failure(ErrorCode::TypeMismatch);
        if (!number || !math::IsFinite(number.Value())) {
            return Fail(GraphTextCode::InvalidValue, value, "expected a finite number");
        }

        out = number.Value();
        return Result<void>::Success();
    }

    Result<void> ReadBool(JsonValue value, bool& out) noexcept {
        if (!value.IsBool()) {
            return Fail(GraphTextCode::InvalidValue, value, "expected true or false");
        }

        out = value.GetBool();
        return Result<void>::Success();
    }

    // 선택 키다: 없으면 아무것도 하지 않는다.
    template <class Read>
    Result<void> Optional(JsonValue object, StringView key, Read&& read) noexcept {
        const JsonValue value = object.Find(key);
        if (!value.IsValid()) {
            return Result<void>::Success();
        }

        return read(value);
    }

    // 레코드 값 하나다. 미해석(조각)이면 unresolved에 type 문자열을 남긴다. 문서는 미해석이 오류다.
    Result<void> Record(JsonValue value, PropertyRecord& record, String& unresolved) noexcept {
        String type(context_.allocator);
        auto read = ReadRecordValue(context_, value, use_, record, type, diagnostics_);
        if (!read) {
            return Result<void>::Failure(read.Error());
        }

        if (read.Value() == RecordRead::Unresolved) {
            if (!fragment_) {
                return Fail(GraphTextCode::UnknownRecordType, value,
                            "record type is not registered");
            }

            if (unresolved.IsEmpty()) {
                return unresolved.TryAssign(type.View());
            }
        }

        return Result<void>::Success();
    }

    Result<void> Edge(JsonValue value, GraphEdge& edge) noexcept {
        static constexpr StringView Keys[] = {"from", "to", "order"};
        if (auto checked = Object(value, Keys); !checked) {
            return checked;
        }

        auto from = Require(value, "from");
        if (!from) {
            return Result<void>::Failure(from.Error());
        }

        auto to = Require(value, "to");
        if (!to) {
            return Result<void>::Failure(to.Error());
        }

        if (auto end = EdgeEnd(from.Value(), edge.fromNode, edge.fromPin); !end) {
            return end;
        }

        if (auto end = EdgeEnd(to.Value(), edge.toNode, edge.toPin); !end) {
            return end;
        }

        edge.order = 0;
        return Optional(value, "order",
                        [&](JsonValue order) noexcept { return ReadU32(order, edge.order); });
    }

    Result<void> Layout(JsonValue value, NodeLayout& layout) noexcept {
        static constexpr StringView Keys[] = {
            "x", "y", "width", "height", "collapsed", "showAdvanced", "bubble", "bubbleVisible"};
        if (auto checked = Object(value, Keys); !checked) {
            return checked;
        }

        auto x = Require(value, "x");
        if (!x) {
            return Result<void>::Failure(x.Error());
        }

        auto y = Require(value, "y");
        if (!y) {
            return Result<void>::Failure(y.Error());
        }

        if (auto read = ReadFloat(x.Value(), layout.position.x); !read) {
            return read;
        }

        if (auto read = ReadFloat(y.Value(), layout.position.y); !read) {
            return read;
        }

        auto width = Optional(value, "width",
                              [&](JsonValue v) noexcept { return ReadFloat(v, layout.size.x); });
        auto height =
            width ? Optional(value, "height",
                             [&](JsonValue v) noexcept { return ReadFloat(v, layout.size.y); })
                  : Move(width);
        if (!height) {
            return height;
        }

        if (auto read =
                Optional(value, "collapsed",
                         [&](JsonValue v) noexcept { return ReadBool(v, layout.collapsed); });
            !read) {
            return read;
        }

        if (auto read =
                Optional(value, "showAdvanced",
                         [&](JsonValue v) noexcept { return ReadBool(v, layout.showAdvanced); });
            !read) {
            return read;
        }

        if (auto read = Optional(value, "bubble",
                                 [&](JsonValue v) noexcept { return ReadText(v, layout.bubble); });
            !read) {
            return read;
        }

        return Optional(value, "bubbleVisible",
                        [&](JsonValue v) noexcept { return ReadBool(v, layout.bubbleVisible); });
    }

    Result<void> Node(JsonValue value, GraphNode& node, NodeLayout& layout,
                      String& unresolved) noexcept {
        static constexpr StringView Keys[] = {"id",           "type",        "typeVersion",
                                              "properties",   "enableState", "inputDefaults",
                                              "retainedPins", "layout"};
        if (auto checked = Object(value, Keys); !checked) {
            return checked;
        }

        auto id = Require(value, "id");
        auto type = id ? Require(value, "type") : Move(id);
        auto version = type ? Require(value, "typeVersion") : Move(type);
        if (!version) {
            return Result<void>::Failure(version.Error());
        }

        if (auto read = ReadGuid(value.Find("id"), node.id); !read) {
            return read;
        }

        if (auto read = ReadName(value.Find("type"), node.type); !read) {
            return read;
        }

        if (auto read = ReadU32(version.Value(), node.typeSchemaVersion); !read) {
            return read;
        }

        layout.node = node.id;
        if (auto read = Optional(
                value, "properties",
                [&](JsonValue v) noexcept { return Record(v, node.properties, unresolved); });
            !read) {
            return read;
        }

        if (auto read = Optional(value, "enableState",
                                 [&](JsonValue v) noexcept { return EnableState(v, node); });
            !read) {
            return read;
        }

        if (auto read =
                Optional(value, "inputDefaults",
                         [&](JsonValue v) noexcept { return InputDefaults(v, node, unresolved); });
            !read) {
            return read;
        }

        if (auto read =
                Optional(value, "retainedPins",
                         [&](JsonValue v) noexcept { return RetainedPins(v, node, unresolved); });
            !read) {
            return read;
        }

        return Optional(value, "layout", [&](JsonValue v) noexcept { return Layout(v, layout); });
    }

    Result<void> Comment(JsonValue value, CommentBox& comment) noexcept {
        static constexpr StringView Keys[] = {"id",     "x",    "y",     "width",
                                              "height", "text", "color", "fontSize"};
        if (auto checked = Object(value, Keys); !checked) {
            return checked;
        }

        // 주석 상자의 키는 모두 필수다.
        for (const StringView& key : Keys) {
            if (auto present = Require(value, key); !present) {
                return Result<void>::Failure(present.Error());
            }
        }

        if (auto read = ReadGuid(value.Find("id"), comment.id); !read) {
            return read;
        }

        float* floats[] = {&comment.position.x, &comment.position.y, &comment.size.x,
                           &comment.size.y};
        const StringView floatKeys[] = {"x", "y", "width", "height"};
        static_assert(sizeof(floats) / sizeof(floats[0]) ==
                          sizeof(floatKeys) / sizeof(floatKeys[0]),
                      "comment float keys must match their fields");
        for (usize index = 0; index < sizeof(floats) / sizeof(floats[0]); ++index) {
            if (auto read = ReadFloat(value.Find(floatKeys[index]), *floats[index]); !read) {
                return read;
            }
        }

        if (auto read = ReadText(value.Find("text"), comment.text); !read) {
            return read;
        }

        const JsonValue color = value.Find("color");
        if (!color.IsArray() || color.Size() != ColorChannels) {
            return Fail(GraphTextCode::InvalidValue, color, "color must be four numbers");
        }

        float* channels[ColorChannels] = {&comment.color.r, &comment.color.g, &comment.color.b,
                                          &comment.color.a};
        for (usize index = 0; index < ColorChannels; ++index) {
            if (auto read = ReadFloat(color.At(index), *channels[index]); !read) {
                return read;
            }
        }

        return ReadU32(value.Find("fontSize"), comment.fontSize);
    }

    Result<void> BookmarkValue(JsonValue value, Bookmark& bookmark) noexcept {
        static constexpr StringView Keys[] = {"number", "x", "y", "zoom"};
        if (auto checked = Object(value, Keys); !checked) {
            return checked;
        }

        for (const StringView& key : Keys) {
            if (auto present = Require(value, key); !present) {
                return Result<void>::Failure(present.Error());
            }
        }

        if (auto read = ReadU32(value.Find("number"), bookmark.number); !read) {
            return read;
        }

        if (auto read = ReadFloat(value.Find("x"), bookmark.viewCenter.x); !read) {
            return read;
        }

        if (auto read = ReadFloat(value.Find("y"), bookmark.viewCenter.y); !read) {
            return read;
        }

        return ReadFloat(value.Find("zoom"), bookmark.zoom);
    }

    // 배열 키다: 없으면 빈 배열, 있으면 배열이어야 한다.
    Result<JsonValue> Array(JsonValue object, StringView key) noexcept {
        const JsonValue value = object.Find(key);
        if (value.IsValid() && !value.IsArray()) {
            auto failed = Fail(GraphTextCode::InvalidValue, value, "expected an array");
            return Result<JsonValue>::Failure(failed.Error());
        }

        return Result<JsonValue>::Success(value);
    }

    Result<void> Header(JsonValue root, StringView format, Name& domain) noexcept {
        auto formatValue = Require(root, "format");
        if (!formatValue) {
            return Result<void>::Failure(formatValue.Error());
        }

        if (!formatValue.Value().IsString() || !(formatValue.Value().GetString() == format)) {
            return Fail(GraphTextCode::UnsupportedFormat, formatValue.Value(),
                        "unsupported graph text format");
        }

        auto version = Require(root, "formatVersion");
        if (!version) {
            return Result<void>::Failure(version.Error());
        }

        uint32 number = 0;
        if (auto read = ReadU32(version.Value(), number); !read) {
            return read;
        }

        if (number != TextFormatVersion) {
            return Fail(GraphTextCode::UnsupportedFormat, version.Value(),
                        "unsupported graph text format version");
        }

        auto domainValue = Require(root, "domain");
        if (!domainValue) {
            return Result<void>::Failure(domainValue.Error());
        }

        return ReadName(domainValue.Value(), domain);
    }

    GraphTextContext& Context() noexcept {
        return context_;
    }

private:
    Result<void> EdgeEnd(JsonValue value, Guid& node, Name& pin) noexcept {
        if (!value.IsArray() || value.Size() != 2) {
            return Fail(GraphTextCode::InvalidValue, value, "edge end must be [node, pin]");
        }

        if (auto read = ReadGuid(value.At(0), node); !read) {
            return read;
        }

        return ReadName(value.At(1), pin);
    }

    Result<void> EnableState(JsonValue value, GraphNode& node) noexcept {
        StringView text;
        if (auto read = ReadString(value, text); !read) {
            return read;
        }

        if (text == "Enabled") {
            node.enableState = NodeEnableState::Enabled;
        } else if (text == "Disabled") {
            node.enableState = NodeEnableState::Disabled;
        } else {
            return Fail(GraphTextCode::InvalidValue, value, "unknown enable state");
        }

        return Result<void>::Success();
    }

    Result<void> InputDefaults(JsonValue value, GraphNode& node, String& unresolved) noexcept {
        if (!value.IsObject()) {
            return Fail(GraphTextCode::InvalidValue, value, "inputDefaults must be an object");
        }

        for (usize index = 0; index < value.Size(); ++index) {
            PinValueEntry entry(node.inputDefaults.GetAllocator());
            auto pin = context_.names.TryIntern(value.KeyAt(index));
            if (!pin) {
                if (pin.Error() == ErrorCode::InvalidArgument) {
                    return Fail(GraphTextCode::InvalidValue, value.ValueAt(index),
                                "input default key must be a name");
                }

                return Result<void>::Failure(pin.Error());
            }

            // Name은 ASCII 대소문자를 구별하지 않으므로 JSON 키가 달라도 같은 핀일 수 있다.
            for (const PinValueEntry& existing : node.inputDefaults) {
                if (existing.pin == pin.Value()) {
                    return Fail(GraphTextCode::InvalidValue, value.ValueAt(index),
                                "input default pin is repeated");
                }
            }

            entry.pin = pin.Value();
            if (auto read = Record(value.ValueAt(index), entry.value, unresolved); !read) {
                return read;
            }

            if (auto added = node.inputDefaults.TryPushBack(Move(entry)); !added) {
                return added;
            }
        }

        return Result<void>::Success();
    }

    Result<void> RetainedPins(JsonValue value, GraphNode& node, String& unresolved) noexcept {
        if (!value.IsArray()) {
            return Fail(GraphTextCode::InvalidValue, value, "retainedPins must be an array");
        }

        static constexpr StringView Keys[] = {"key", "direction", "lastType", "value"};
        for (usize index = 0; index < value.Size(); ++index) {
            const JsonValue item = value.At(index);
            if (auto checked = Object(item, Keys); !checked) {
                return checked;
            }

            RetainedPin pin(node.retainedPins.GetAllocator());
            auto key = Require(item, "key");
            auto direction = key ? Require(item, "direction") : Move(key);
            if (!direction) {
                return Result<void>::Failure(direction.Error());
            }

            if (auto read = ReadName(item.Find("key"), pin.key); !read) {
                return read;
            }

            for (const RetainedPin& existing : node.retainedPins) {
                if (existing.key == pin.key) {
                    return Fail(GraphTextCode::InvalidValue, item.Find("key"),
                                "retained pin key is repeated");
                }
            }

            StringView text;
            if (auto read = ReadString(direction.Value(), text); !read) {
                return read;
            }

            if (text == "Input") {
                pin.direction = PinDirection::Input;
            } else if (text == "Output") {
                pin.direction = PinDirection::Output;
            } else {
                return Fail(GraphTextCode::InvalidValue, direction.Value(),
                            "unknown pin direction");
            }

            if (auto read =
                    Optional(item, "lastType",
                             [&](JsonValue v) noexcept { return ReadU32(v, pin.lastType); });
                !read) {
                return read;
            }

            if (auto read = Optional(
                    item, "value",
                    [&](JsonValue v) noexcept { return Record(v, pin.value, unresolved); });
                !read) {
                return read;
            }

            if (auto added = node.retainedPins.TryPushBack(Move(pin)); !added) {
                return added;
            }
        }

        return Result<void>::Success();
    }

    GraphTextContext& context_;
    GraphTextDiagnostics& diagnostics_;
    TextUse use_;
    bool fragment_;
};

// text를 Json5로 파싱한다. 문법 오류는 진단 하나와 InvalidArgument다.
Result<JsonValue> Parse(JsonDocument& document, StringView text,
                        GraphTextDiagnostics& diagnostics) noexcept {
    JsonReadOptions options;
    options.syntax = JsonSyntax::Json5;
    const JsonError error = document.Parse(text, options);
    if (!error.Succeeded()) {
        if (auto added = diagnostics.Add(GraphTextCode::JsonSyntax, error.position,
                                         JsonErrorMessage(error.code, JsonSyntax::Json5));
            !added) {
            return Result<JsonValue>::Failure(added.Error());
        }

        return Result<JsonValue>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<JsonValue>::Success(document.Root());
}

Result<void> ReadRole(Reader& reader, JsonValue value, GraphRole& role) noexcept {
    StringView text;
    if (auto read = reader.ReadString(value, text); !read) {
        return read;
    }

    if (text == "Main") {
        role = GraphRole::Main;
    } else if (text == "Collapsed") {
        role = GraphRole::Collapsed;
    } else if (text == "DomainDefined") {
        role = GraphRole::DomainDefined;
    } else {
        return reader.Fail(GraphTextCode::InvalidValue, value, "unknown graph role");
    }

    return Result<void>::Success();
}

// 하위 그래프 경계 목록 하나다(10.1절 NG3c). 항목마다 key·kind·type·maxEdges가 모두 있어야 한다.
Result<void> ReadBoundary(Reader& reader, JsonValue object, StringView key,
                          Array<GraphBoundaryPin>& out) noexcept {
    auto list = reader.Array(object, key);
    if (!list) {
        return Result<void>::Failure(list.Error());
    }

    static constexpr StringView Keys[] = {"key", "kind", "type", "maxEdges"};
    for (usize index = 0; list.Value().IsValid() && index < list.Value().Size(); ++index) {
        const JsonValue item = list.Value().At(index);
        if (auto checked = reader.Object(item, Keys); !checked) {
            return checked;
        }

        for (const StringView& required : Keys) {
            if (auto present = reader.Require(item, required); !present) {
                return Result<void>::Failure(present.Error());
            }
        }

        GraphBoundaryPin pin;
        if (auto read = reader.ReadName(item.Find("key"), pin.key); !read) {
            return read;
        }

        StringView kind;
        if (auto read = reader.ReadString(item.Find("kind"), kind); !read) {
            return read;
        }

        if (kind == "Data") {
            pin.kind = PinKind::Data;
        } else if (kind == "Flow") {
            pin.kind = PinKind::Flow;
        } else if (kind == "Structural") {
            pin.kind = PinKind::Structural;
        } else {
            return reader.Fail(GraphTextCode::InvalidValue, item.Find("kind"), "unknown pin kind");
        }

        if (auto read = reader.ReadName(item.Find("type"), pin.type); !read) {
            return read;
        }

        uint32 maxEdges = 0;
        if (auto read = reader.ReadU32(item.Find("maxEdges"), maxEdges); !read) {
            return read;
        }

        if (maxEdges > NumericLimits<uint16>::Max()) {
            return reader.Fail(GraphTextCode::InvalidValue, item.Find("maxEdges"),
                               "maxEdges is out of range");
        }

        pin.maxEdges = static_cast<uint16>(maxEdges);
        if (auto pushed = out.TryPushBack(pin); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

Result<void> ReadGraph(Reader& reader, JsonValue value, Graph& graph,
                       GraphLayout& layout) noexcept {
    static constexpr StringView Keys[] = {"id",     "role",     "domainRole", "ownerNode",
                                          "inputs", "outputs",  "title",      "nodes",
                                          "edges",  "comments", "bookmarks"};
    if (auto checked = reader.Object(value, Keys); !checked) {
        return checked;
    }

    auto id = reader.Require(value, "id");
    auto role = id ? reader.Require(value, "role") : Move(id);
    if (!role) {
        return Result<void>::Failure(role.Error());
    }

    if (auto read = reader.ReadGuid(value.Find("id"), graph.id); !read) {
        return read;
    }

    layout.graph = graph.id;
    if (auto read = ReadRole(reader, role.Value(), graph.role); !read) {
        return read;
    }

    if (auto read = reader.Optional(
            value, "domainRole",
            [&](JsonValue v) noexcept { return reader.ReadU32(v, graph.domainRole); });
        !read) {
        return read;
    }

    if (auto read = reader.Optional(
            value, "ownerNode",
            [&](JsonValue v) noexcept { return reader.ReadGuid(v, graph.ownerNode); });
        !read) {
        return read;
    }

    if (auto read = ReadBoundary(reader, value, "inputs", graph.inputs); !read) {
        return read;
    }

    if (auto read = ReadBoundary(reader, value, "outputs", graph.outputs); !read) {
        return read;
    }

    if (auto read = reader.Optional(
            value, "title", [&](JsonValue v) noexcept { return reader.ReadText(v, layout.title); });
        !read) {
        return read;
    }

    auto nodes = reader.Array(value, "nodes");
    if (!nodes) {
        return Result<void>::Failure(nodes.Error());
    }

    String unresolved(reader.Context().allocator);
    for (usize index = 0; nodes.Value().IsValid() && index < nodes.Value().Size(); ++index) {
        GraphNode node(graph.nodes.GetAllocator());
        NodeLayout position(layout.nodes.GetAllocator());
        if (auto read = reader.Node(nodes.Value().At(index), node, position, unresolved); !read) {
            return read;
        }

        if (auto added = graph.nodes.TryPushBack(Move(node)); !added) {
            return added;
        }

        if (auto added = layout.nodes.TryPushBack(Move(position)); !added) {
            return added;
        }
    }

    auto edges = reader.Array(value, "edges");
    if (!edges) {
        return Result<void>::Failure(edges.Error());
    }

    for (usize index = 0; edges.Value().IsValid() && index < edges.Value().Size(); ++index) {
        GraphEdge edge;
        if (auto read = reader.Edge(edges.Value().At(index), edge); !read) {
            return read;
        }

        if (auto added = graph.edges.TryPushBack(edge); !added) {
            return added;
        }
    }

    auto comments = reader.Array(value, "comments");
    if (!comments) {
        return Result<void>::Failure(comments.Error());
    }

    for (usize index = 0; comments.Value().IsValid() && index < comments.Value().Size(); ++index) {
        CommentBox comment(layout.comments.GetAllocator());
        if (auto read = reader.Comment(comments.Value().At(index), comment); !read) {
            return read;
        }

        if (auto added = layout.comments.TryPushBack(Move(comment)); !added) {
            return added;
        }
    }

    auto bookmarks = reader.Array(value, "bookmarks");
    if (!bookmarks) {
        return Result<void>::Failure(bookmarks.Error());
    }

    for (usize index = 0; bookmarks.Value().IsValid() && index < bookmarks.Value().Size();
         ++index) {
        Bookmark bookmark;
        if (auto read = reader.BookmarkValue(bookmarks.Value().At(index), bookmark); !read) {
            return read;
        }

        if (auto added = layout.bookmarks.TryPushBack(bookmark); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> ReadEdgeList(Reader& reader, JsonValue root, StringView key,
                          Array<GraphEdge>& out) noexcept {
    auto edges = reader.Array(root, key);
    if (!edges) {
        return Result<void>::Failure(edges.Error());
    }

    for (usize index = 0; edges.Value().IsValid() && index < edges.Value().Size(); ++index) {
        GraphEdge edge;
        if (auto read = reader.Edge(edges.Value().At(index), edge); !read) {
            return read;
        }

        if (auto added = out.TryPushBack(edge); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> ReadGraphDocumentJson(GraphTextContext& context, StringView text,
                                   GraphDocument& document, GraphDocumentLayout& layout,
                                   GraphTextDiagnostics& diagnostics) noexcept {
    JsonDocument json(context.allocator);
    auto root = Parse(json, text, diagnostics);
    if (!root) {
        return Result<void>::Failure(root.Error());
    }

    Reader reader(context, diagnostics, TextUse::Edit, false);
    static constexpr StringView Keys[] = {
        "format", "formatVersion", "domain", "documentFormatVersion", "mainGraph", "graphs"};
    if (auto checked = reader.Object(root.Value(), Keys); !checked) {
        return checked;
    }

    document.graphs.Clear();
    layout.graphs.Clear();
    if (auto header = reader.Header(root.Value(), DocumentFormat, document.domain); !header) {
        return header;
    }

    auto version = reader.Require(root.Value(), "documentFormatVersion");
    auto main = version ? reader.Require(root.Value(), "mainGraph") : Move(version);
    if (!main) {
        return Result<void>::Failure(main.Error());
    }

    if (auto read = reader.ReadU32(root.Value().Find("documentFormatVersion"),
                                   document.documentFormatVersion);
        !read) {
        return read;
    }

    if (auto read = reader.ReadGuid(main.Value(), document.mainGraph); !read) {
        return read;
    }

    auto graphs = reader.Array(root.Value(), "graphs");
    if (!graphs) {
        return Result<void>::Failure(graphs.Error());
    }

    for (usize index = 0; graphs.Value().IsValid() && index < graphs.Value().Size(); ++index) {
        Graph graph(document.graphs.GetAllocator());
        GraphLayout graphLayout(layout.graphs.GetAllocator());
        if (auto read = ReadGraph(reader, graphs.Value().At(index), graph, graphLayout); !read) {
            return read;
        }

        if (auto added = document.graphs.TryPushBack(Move(graph)); !added) {
            return added;
        }

        if (auto added = layout.graphs.TryPushBack(Move(graphLayout)); !added) {
            return added;
        }
    }

    if (auto canonical =
            CanonicalizeGraphDocument(document, &layout, context.names, context.allocator);
        !canonical) {
        if (canonical.Error() != ErrorCode::InvalidArgument) {
            return canonical;
        }

        return reader.Fail(GraphTextCode::InvalidValue, root.Value(),
                           "document cannot be put in canonical order");
    }

    return Result<void>::Success();
}

Result<void> ReadGraphFragmentJson(GraphTextContext& context, StringView text,
                                   GraphFragment& fragment,
                                   GraphTextDiagnostics& diagnostics) noexcept {
    JsonDocument json(context.allocator);
    auto root = Parse(json, text, diagnostics);
    if (!root) {
        return Result<void>::Failure(root.Error());
    }

    Reader reader(context, diagnostics, TextUse::Copy, true);
    static constexpr StringView Keys[] = {"format",      "formatVersion", "domain",
                                          "sourceAsset", "sourceGraph",   "nodes",
                                          "edges",       "externalEdges", "comments"};
    if (auto checked = reader.Object(root.Value(), Keys); !checked) {
        return checked;
    }

    fragment.nodes.Clear();
    fragment.edges.Clear();
    fragment.externalEdges.Clear();
    fragment.comments.Clear();
    fragment.sourceAsset = Guid{};
    if (auto header = reader.Header(root.Value(), FragmentFormat, fragment.domain); !header) {
        return header;
    }

    if (auto read = reader.Optional(
            root.Value(), "sourceAsset",
            [&](JsonValue v) noexcept { return reader.ReadGuid(v, fragment.sourceAsset); });
        !read) {
        return read;
    }

    auto graph = reader.Require(root.Value(), "sourceGraph");
    if (!graph) {
        return Result<void>::Failure(graph.Error());
    }

    if (auto read = reader.ReadGuid(graph.Value(), fragment.sourceGraph); !read) {
        return read;
    }

    auto nodes = reader.Array(root.Value(), "nodes");
    if (!nodes) {
        return Result<void>::Failure(nodes.Error());
    }

    for (usize index = 0; nodes.Value().IsValid() && index < nodes.Value().Size(); ++index) {
        GraphFragmentNode node(fragment.nodes.GetAllocator());
        if (auto read =
                reader.Node(nodes.Value().At(index), node.node, node.layout, node.unresolvedType);
            !read) {
            return read;
        }

        if (auto added = fragment.nodes.TryPushBack(Move(node)); !added) {
            return added;
        }
    }

    if (auto read = ReadEdgeList(reader, root.Value(), "edges", fragment.edges); !read) {
        return read;
    }

    if (auto read = ReadEdgeList(reader, root.Value(), "externalEdges", fragment.externalEdges);
        !read) {
        return read;
    }

    auto comments = reader.Array(root.Value(), "comments");
    if (!comments) {
        return Result<void>::Failure(comments.Error());
    }

    for (usize index = 0; comments.Value().IsValid() && index < comments.Value().Size(); ++index) {
        CommentBox comment(fragment.comments.GetAllocator());
        if (auto read = reader.Comment(comments.Value().At(index), comment); !read) {
            return read;
        }

        if (auto added = fragment.comments.TryPushBack(Move(comment)); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::nodegraph

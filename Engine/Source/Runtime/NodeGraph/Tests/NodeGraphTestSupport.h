#pragma once

#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <NodeGraph/GraphDocument.h>
#include <NodeGraph/GraphNameOrder.h>
#include <NodeGraph/GraphSemantic.h>
#include <NodeGraph/GraphValidation.h>
#include <NodeGraph/NodeTypeRegistry.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>

// NodeGraph 시험 공용 도우미다(Docs/Design/NodeGraph 13절): 시험 도메인 "Test"의 schema·노드
// 종류, 표본 문서(모든 핀 종류·재경유·순번 출력·동적 핀·고아 핀·알 수 없는 종류·하위 그래프·배치),
// 속성 Record 바이트 도우미.
//
// 시험 도메인 노드 종류(핀 타입 표: Float 0, Vector 1, Flow 2):
//   Test.Add       입력 A·B(Float), 출력 Result(Float). 속성 없음.
//   Test.Param     출력 Value(Float). 속성 struct TestParamStruct, 서술 버전 2.
//   Test.Sequence  입력 In(Flow), 출력 Then(Flow, ordered).
//   Test.Multi     입력 Items(Float, 간선 2개까지), 출력 Result.
//   Test.Dynamic   출력 Result + 동적 입력 Item_0..Item_<n-1>(n = 속성 Record의 schema version,
//   1–9).
//                  version으로 오류 경로를 고른다: TestDynamic*Version 상수(실패·키 중복·타입 범위
//                  밖· 핀 수 초과), 그 밖은 InvalidArgument.
//   Test.Toggle    입력 In·출력 Out(Float), 비활성 허용, 통과 In → Out.
//   Test.Convert   입력 In·출력 Out(wildcard). 연결 판정 Convert의 변환 노드.
namespace rocks::nodegraph_test {

using namespace rocks::nodegraph;

inline constexpr PinTypeId TestFloatType = 0;
inline constexpr PinTypeId TestVectorType = 1;
inline constexpr PinTypeId TestFlowType = 2;
inline constexpr uint32 TestPinTypeCount = 3;

inline constexpr PinTypeInfo TestPinTypeInfos[TestPinTypeCount] = {{"Float"}, {"Vector"}, {"Flow"}};

// 시험 속성 struct ID다(리플렉션 등록 없이 Record header schema로만 쓴다).
inline constexpr Guid TestParamStruct{0x5445535450415241ull, 1};
inline constexpr Guid TestDynamicStruct{0x54455354444e594eull, 2};
inline constexpr Guid TestValueStruct{0x5445535456414c55ull, 3};

// 시험 Record의 필드 ID다.
inline constexpr Guid TestRecordField{0x54455354u, 0x4649454cu};

// Test.Param의 서술 버전이다.
inline constexpr uint32 TestParamVersion = 2;

// Test.Dynamic 동적 핀 수의 상한이다(핀 이름 Item_0..Item_8).
inline constexpr uint32 TestDynamicMaxItems = 9;

// Test.Dynamic 오류 경로를 고르는 속성 Record schema version이다.
// 동적 핀 함수가 InvalidArgument가 아닌 오류(Unsupported)를 돌려준다.
inline constexpr uint32 TestDynamicFailVersion = 11;
// 정적 핀과 같은 키(Result)의 동적 핀을 더한다.
inline constexpr uint32 TestDynamicDuplicateVersion = 12;
// 타입 표 범위 밖 타입의 동적 핀을 더한다.
inline constexpr uint32 TestDynamicBadTypeVersion = 13;
// MaxPinsPerNode를 넘도록 동적 핀을 더한다.
inline constexpr uint32 TestDynamicOverflowVersion = 14;

// Test.Multi 입력 Items의 간선 상한이다.
inline constexpr uint16 TestMultiMaxEdges = 2;

// 시험 schema의 의미 정규화 방식이다.
enum class TestNormalize : uint8 {
    // 노드 전체(기본).
    Default,
    // 속성 바이트를 빼고 쓴다(도메인 정규화 흉내).
    StripProperties,
    // WriteNode를 부르지 않는다(계약 위반).
    Skip,
    // WriteNode를 두 번 부른다(계약 위반).
    Twice,
    // 너무 큰 속성 바이트(oversized)로 WriteNode를 불러 생긴 쓰기 오류를 무시하고 성공을
    // 돌려준다(계약 위반).
    IgnoreFailure
};

// 시험 schema의 연결 판정 방식이다.
enum class TestConnect : uint8 {
    // GraphSchema 기본 판정.
    Default,
    Deny,
    // 늘 Make(입력 상한 검사를 시험한다).
    AlwaysMake,
    ReplaceOutput,
    ReplaceBoth,
    // conversionType으로 MakeWithConversion.
    Convert
};

class TestSchema final : public GraphSchema {
public:
    Name Domain() const noexcept override {
        return domain;
    }

    const PinTypeTable& PinTypes() const noexcept override {
        return table;
    }

    Result<void> NormalizeSemanticNode(const GraphNode& node,
                                       SemanticNodeWriter& writer) const noexcept override {
        switch (mode) {
        case TestNormalize::StripProperties:
            return writer.WriteNode(node, Span<const uint8>());
        case TestNormalize::Skip:
            return Result<void>::Success();
        case TestNormalize::Twice: {
            if (auto first = writer.WriteNode(node); !first) {
                return first;
            }

            return writer.WriteNode(node);
        }
        case TestNormalize::IgnoreFailure: {
            static_cast<void>(writer.WriteNode(node, oversized));
            return Result<void>::Success();
        }
        case TestNormalize::Default:
            break;
        }

        return writer.WriteNode(node);
    }

    ConnectVerdict CanConnect(const GraphReadView& view,
                              const ConnectQuery& query) const noexcept override {
        ConnectVerdict verdict;
        switch (connect) {
        case TestConnect::Default:
            return GraphSchema::CanConnect(view, query);
        case TestConnect::Deny:
            return verdict;
        case TestConnect::AlwaysMake:
            verdict.action = ConnectAction::Make;
            return verdict;
        case TestConnect::ReplaceOutput:
            verdict.action = ConnectAction::MakeReplacingOutput;
            return verdict;
        case TestConnect::ReplaceBoth:
            verdict.action = ConnectAction::MakeReplacingBoth;
            return verdict;
        case TestConnect::Convert:
            verdict.action = ConnectAction::MakeWithConversion;
            verdict.conversionNodeType = conversionType;
            return verdict;
        }

        return verdict;
    }

    // 붙여넣기 판정(NG2c): deniedPasteType 종류의 노드는 거부한다.
    PasteDecision CanPasteNode(const GraphReadView& view,
                               const GraphNode& node) const noexcept override {
        static_cast<void>(view);
        return node.type == deniedPasteType ? PasteDecision::Deny : PasteDecision::Allow;
    }

    bool AllowExternalEdgeReconnect() const noexcept override {
        return reconnectExternal;
    }

    Name domain;
    // 붙여넣기 판정 시험용이다(기본은 거부 없음·바깥 간선 끊기).
    Name deniedPasteType;
    bool reconnectExternal = false;
    PinTypeTable table{Span<const PinTypeInfo>(TestPinTypeInfos, TestPinTypeCount)};
    TestNormalize mode = TestNormalize::Default;
    TestConnect connect = TestConnect::Default;
    Name conversionType;
    // IgnoreFailure가 넘기는 바이트다(시험이 MaxPropertyRecordBytes보다 크게 채운다).
    Span<const uint8> oversized;
};

// 시험 id다(0은 "없음"이므로 n은 1 이상).
constexpr Guid Id(uint64 n) noexcept {
    return Guid{0, n};
}

// schema·version의 Record 하나(U32 필드 하나 = payload)를 out에 채운다.
inline bool FillRecord(Array<uint8>& out, Guid schema, uint32 version, uint32 payload) noexcept {
    ArchiveWriter writer(out.GetAllocator());
    auto record = RecordWriter::Begin(writer, schema, version);
    if (!record || !record.Value().BeginField(TestRecordField) || !writer.WriteU32(payload) ||
        !record.Value().EndField(WireType::U32) || !record.Value().End()) {
        return false;
    }

    Array<uint8> bytes = writer.TakeBytes();
    out.Clear();
    return static_cast<bool>(out.TryAppend(bytes.View()));
}

// Test.Dynamic의 동적 핀 함수다.
// 동적 핀 Item_<index>(index는 10진 4자리까지)를 out에 더한다.
inline Result<void> AddTestItemPin(const DynamicPinsInput& input, PinList& out, uint32 index,
                                   PinTypeId type) noexcept {
    char text[] = {'I', 't', 'e', 'm', '_', '0', '0', '0', '0'};
    usize length = 6;
    if (index < 10) {
        text[5] = static_cast<char>('0' + index);
    } else {
        text[5] = static_cast<char>('0' + index / 1000 % 10);
        text[6] = static_cast<char>('0' + index / 100 % 10);
        text[7] = static_cast<char>('0' + index / 10 % 10);
        text[8] = static_cast<char>('0' + index % 10);
        length = sizeof(text);
    }

    auto key = input.names.TryIntern(StringView(text, length));
    if (!key) {
        return Result<void>::Failure(key.Error());
    }

    return out.Add(InputPin(key.Value(), type, "Item"));
}

// Test.Dynamic의 동적 핀 함수다.
inline Result<void> TestDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    auto header = ReadRecordHeader(input.node.properties.bytes.View());
    if (!header) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint32 version = header.Value().schemaVersion;
    switch (version) {
    case TestDynamicFailVersion:
        return Result<void>::Failure(ErrorCode::Unsupported);
    case TestDynamicDuplicateVersion: {
        auto key = input.names.TryIntern("Result");
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        return out.Add(InputPin(key.Value(), TestFloatType, "Result"));
    }
    case TestDynamicBadTypeVersion:
        return AddTestItemPin(input, out, 0, TestPinTypeCount);
    case TestDynamicOverflowVersion:
        for (uint32 index = 0; index < MaxPinsPerNode; ++index) {
            if (auto added = AddTestItemPin(input, out, index, TestFloatType); !added) {
                return added;
            }
        }

        return Result<void>::Success();
    default:
        break;
    }

    if (version == 0 || version > TestDynamicMaxItems) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 index = 0; index < version; ++index) {
        if (auto added = AddTestItemPin(input, out, index, TestFloatType); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

// 시험 도메인 환경이다: NamePool·등록표·schema·노드 종류와 자주 쓰는 Name.
struct TestEnv {
    TestEnv() noexcept : names(allocator), registry(allocator, names) {
        ready = names.Initialize() && registry.Initialize() && InternAll() && RegisterAll();
    }

    TestEnv(const TestEnv&) = delete;
    TestEnv& operator=(const TestEnv&) = delete;

    Name Intern(StringView text) noexcept {
        auto name = names.TryIntern(text);
        if (!name) {
            internFailed = true;
            ready = false;
            return Name();
        }

        return name.Value();
    }

    bool InternAll() noexcept {
        domain = Intern("Test");
        typeAdd = Intern("Test.Add");
        typeParam = Intern("Test.Param");
        typeSequence = Intern("Test.Sequence");
        typeMulti = Intern("Test.Multi");
        typeDynamic = Intern("Test.Dynamic");
        typeToggle = Intern("Test.Toggle");
        typeUnknown = Intern("Test.Missing");
        typeConvert = Intern("Test.Convert");
        pinA = Intern("A");
        pinB = Intern("B");
        pinResult = Intern("Result");
        pinValue = Intern("Value");
        pinIn = Intern("In");
        pinOut = Intern("Out");
        pinThen = Intern("Then");
        pinItems = Intern("Items");
        pinItem0 = Intern("Item_0");
        pinItem1 = Intern("Item_1");
        pinGone = Intern("Gone");
        pinOther = Intern("Other");
        reroute = registry.RerouteType();
        schema.domain = domain;
        return !internFailed;
    }

    bool Register(const NodeTypeDescriptor& descriptor) noexcept {
        return static_cast<bool>(registry.RegisterNodeType(domain, descriptor, ModuleLease()));
    }

    bool RegisterAll() noexcept {
        if (!registry.RegisterSchema(schema, ModuleLease())) {
            return false;
        }

        const PinDescriptor addPins[] = {InputPin(pinA, TestFloatType, "A"),
                                         InputPin(pinB, TestFloatType, "B"),
                                         OutputPin(pinResult, TestFloatType, "Result")};
        NodeTypeDescriptor add;
        add.type = typeAdd;
        add.staticPins = addPins;
        add.menu.displayName = "Add";

        const PinDescriptor paramPins[] = {OutputPin(pinValue, TestFloatType, "Value")};
        NodeTypeDescriptor param;
        param.type = typeParam;
        param.schemaVersion = TestParamVersion;
        param.propertiesStruct = TestParamStruct;
        param.staticPins = paramPins;

        PinDescriptor then = OutputPin(pinThen, TestFlowType, "Then", PinKind::Flow);
        then.ordered = true;
        const PinDescriptor sequencePins[] = {InputPin(pinIn, TestFlowType, "In", PinKind::Flow),
                                              then};
        NodeTypeDescriptor sequence;
        sequence.type = typeSequence;
        sequence.staticPins = sequencePins;

        PinDescriptor items = InputPin(pinItems, TestFloatType, "Items");
        items.maxEdges = TestMultiMaxEdges;
        const PinDescriptor multiPins[] = {items, OutputPin(pinResult, TestFloatType, "Result")};
        NodeTypeDescriptor multi;
        multi.type = typeMulti;
        multi.staticPins = multiPins;

        const PinDescriptor dynamicPins[] = {OutputPin(pinResult, TestFloatType, "Result")};
        NodeTypeDescriptor dynamic;
        dynamic.type = typeDynamic;
        dynamic.propertiesStruct = TestDynamicStruct;
        dynamic.staticPins = dynamicPins;
        dynamic.dynamicPins = &TestDynamicPins;

        const PinDescriptor togglePins[] = {InputPin(pinIn, TestFloatType, "In"),
                                            OutputPin(pinOut, TestFloatType, "Out")};
        const PassThroughPair togglePass[] = {{pinIn, pinOut}};
        NodeTypeDescriptor toggle;
        toggle.type = typeToggle;
        toggle.staticPins = togglePins;
        toggle.passThrough = togglePass;
        toggle.capabilities = DefaultNodeCapabilities | NodeCapabilities::AllowDisable;

        const PinDescriptor convertPins[] = {InputPin(pinIn, UnresolvedPinType, "In"),
                                             OutputPin(pinOut, UnresolvedPinType, "Out")};
        NodeTypeDescriptor convert;
        convert.type = typeConvert;
        convert.staticPins = convertPins;

        return Register(add) && Register(param) && Register(sequence) && Register(multi) &&
               Register(dynamic) && Register(toggle) && Register(convert);
    }

    SystemAllocator allocator;
    NamePool names;
    NodeTypeRegistry registry;
    TestSchema schema;
    bool ready = false;
    bool internFailed = false;

    Name domain;
    Name typeAdd;
    Name typeParam;
    Name typeSequence;
    Name typeMulti;
    Name typeDynamic;
    Name typeToggle;
    Name typeUnknown;
    Name typeConvert;
    Name reroute;
    Name pinA;
    Name pinB;
    Name pinResult;
    Name pinValue;
    Name pinIn;
    Name pinOut;
    Name pinThen;
    Name pinItems;
    Name pinItem0;
    Name pinItem1;
    Name pinGone;
    Name pinOther;
};

// 노드 하나를 만든다(속성 없음).
inline GraphNode MakeNode(TestEnv& env, Guid id, Name type, uint32 version = 1) noexcept {
    GraphNode node(env.allocator);
    node.id = id;
    node.type = type;
    node.typeSchemaVersion = version;
    return node;
}

// Test.Param 노드를 만든다(속성 Record payload).
inline GraphNode MakeParam(TestEnv& env, Guid id, uint32 payload) noexcept {
    GraphNode node = MakeNode(env, id, env.typeParam, TestParamVersion);
    if (!FillRecord(node.properties.bytes, TestParamStruct, TestParamVersion, payload)) {
        env.ready = false;
    }

    return node;
}

inline GraphEdge MakeEdge(Guid from, Name fromPin, Guid to, Name toPin, uint32 order = 0) noexcept {
    GraphEdge edge;
    edge.fromNode = from;
    edge.fromPin = fromPin;
    edge.toNode = to;
    edge.toPin = toPin;
    edge.order = order;
    return edge;
}

// 표본 문서의 id다.
inline constexpr Guid SampleMain = Id(1);
inline constexpr Guid SampleCollapsed = Id(2);
inline constexpr Guid SampleParamA = Id(10);
inline constexpr Guid SampleParamB = Id(11);
inline constexpr Guid SampleAdd = Id(12);
inline constexpr Guid SampleReroute = Id(13);
inline constexpr Guid SampleSequence = Id(14);
inline constexpr Guid SampleStepA = Id(15);
inline constexpr Guid SampleStepB = Id(16);
inline constexpr Guid SampleToggle = Id(17);
inline constexpr Guid SampleDefaults = Id(18);
inline constexpr Guid SampleDynamic = Id(19);
inline constexpr Guid SampleUnknown = Id(20);
inline constexpr Guid SampleInner = Id(30);
inline constexpr Guid SampleComment = Id(40);

// 표본 문서와 배치다(정규 순서로 맞춘 뒤 돌려준다). 주 그래프:
//   ParamA.Value → Add.A, ParamB.Value → Reroute.In, Reroute.Out → Add.B, Add.Result →
//   Toggle.In(비활성), Sequence.Then → StepA.In(순번 0)·StepB.In(순번 1), ParamA.Value →
//   Dynamic.Item_1, Unknown.Whatever → Defaults.B, Defaults는 입력 A 기본값, Add는 입력 고아 핀
//   Gone(값 있음).
// 하위 그래프(Collapsed): Add 하나. 배치: 두 그래프의 모든 노드, 주석 하나, 북마크 1·3.
struct Sample {
    explicit Sample(TestEnv& env) noexcept : document(env.allocator), layout(env.allocator) {
        Build(env);
    }

    void Build(TestEnv& env) noexcept {
        document.domain = env.domain;
        document.mainGraph = SampleMain;

        Graph main(env.allocator);
        main.id = SampleMain;
        main.role = GraphRole::Main;
        GraphNode add = MakeNode(env, SampleAdd, env.typeAdd);
        RetainedPin gone(env.allocator);
        gone.key = env.pinGone;
        gone.direction = PinDirection::Input;
        gone.lastType = TestVectorType;
        ok = ok && FillRecord(gone.value.bytes, TestValueStruct, 1, 3);
        ok = ok && add.retainedPins.TryPushBack(Move(gone));
        GraphNode defaults = MakeNode(env, SampleDefaults, env.typeAdd);
        PinValueEntry value(env.allocator);
        value.pin = env.pinA;
        ok = ok && FillRecord(value.value.bytes, TestValueStruct, 1, 42);
        ok = ok && defaults.inputDefaults.TryPushBack(Move(value));
        GraphNode toggle = MakeNode(env, SampleToggle, env.typeToggle);
        toggle.enableState = NodeEnableState::Disabled;
        GraphNode dynamic = MakeNode(env, SampleDynamic, env.typeDynamic);
        ok = ok && FillRecord(dynamic.properties.bytes, TestDynamicStruct, 2, 0);

        ok = ok && main.nodes.TryPushBack(MakeParam(env, SampleParamA, 5)) &&
             main.nodes.TryPushBack(MakeParam(env, SampleParamB, 7)) &&
             main.nodes.TryPushBack(Move(add)) &&
             main.nodes.TryPushBack(MakeNode(env, SampleReroute, env.reroute)) &&
             main.nodes.TryPushBack(MakeNode(env, SampleSequence, env.typeSequence)) &&
             main.nodes.TryPushBack(MakeNode(env, SampleStepA, env.typeSequence)) &&
             main.nodes.TryPushBack(MakeNode(env, SampleStepB, env.typeSequence)) &&
             main.nodes.TryPushBack(Move(toggle)) && main.nodes.TryPushBack(Move(defaults)) &&
             main.nodes.TryPushBack(Move(dynamic)) &&
             main.nodes.TryPushBack(MakeNode(env, SampleUnknown, env.typeUnknown));
        ok =
            ok &&
            main.edges.TryPushBack(MakeEdge(SampleParamA, env.pinValue, SampleAdd, env.pinA)) &&
            main.edges.TryPushBack(
                MakeEdge(SampleParamB, env.pinValue, SampleReroute, env.pinIn)) &&
            main.edges.TryPushBack(MakeEdge(SampleReroute, env.pinOut, SampleAdd, env.pinB)) &&
            main.edges.TryPushBack(MakeEdge(SampleAdd, env.pinResult, SampleToggle, env.pinIn)) &&
            main.edges.TryPushBack(
                MakeEdge(SampleSequence, env.pinThen, SampleStepA, env.pinIn, 0)) &&
            main.edges.TryPushBack(
                MakeEdge(SampleSequence, env.pinThen, SampleStepB, env.pinIn, 1)) &&
            main.edges.TryPushBack(
                MakeEdge(SampleParamA, env.pinValue, SampleDynamic, env.pinItem1)) &&
            main.edges.TryPushBack(MakeEdge(SampleUnknown, env.pinOther, SampleDefaults, env.pinB));

        Graph inner(env.allocator);
        inner.id = SampleCollapsed;
        inner.role = GraphRole::Collapsed;
        ok = ok && inner.nodes.TryPushBack(MakeNode(env, SampleInner, env.typeAdd));
        ok = ok && document.graphs.TryPushBack(Move(main)) &&
             document.graphs.TryPushBack(Move(inner));

        for (const Graph& graph : document.graphs) {
            GraphLayout entry(env.allocator);
            entry.graph = graph.id;
            for (const GraphNode& node : graph.nodes) {
                NodeLayout position(env.allocator);
                position.node = node.id;
                position.position = Vec2(static_cast<float>(node.id.low) * 10.0f, -20.0f);
                ok = ok && entry.nodes.TryPushBack(Move(position));
            }

            if (graph.id == SampleMain) {
                CommentBox comment(env.allocator);
                comment.id = SampleComment;
                comment.size = Vec2(400.0f, 200.0f);
                comment.color = LinearColor(1.0f, 0.5f, 0.25f, 1.0f);
                comment.fontSize = 18;
                Bookmark first;
                first.number = 1;
                Bookmark third;
                third.number = 3;
                third.zoom = 0.5f;
                ok = ok && entry.comments.TryPushBack(Move(comment)) &&
                     entry.bookmarks.TryPushBack(first) && entry.bookmarks.TryPushBack(third);
            }

            ok = ok && layout.graphs.TryPushBack(Move(entry));
        }

        ok = ok && env.ready &&
             CanonicalizeGraphDocument(document, &layout, env.names, env.allocator);
    }

    Graph& Main() noexcept {
        return document.graphs[0];
    }

    // 주 그래프에서 id인 노드다. 없으면 Fatal 대신 첫 노드를 돌려주고 ok를 거짓으로 둔다.
    GraphNode& Node(Guid id) noexcept {
        for (GraphNode& node : Main().nodes) {
            if (node.id == id) {
                return node;
            }
        }

        ok = false;
        return Main().nodes[0];
    }

    GraphDocument document;
    GraphDocumentLayout layout;
    bool ok = true;
};

// document(와 layout)를 검사해 위반 목록을 돌려준다.
inline Result<void> Validate(TestEnv& env, const GraphDocument& document,
                             const GraphDocumentLayout* layout, GraphViolationList& out) noexcept {
    return ValidateGraphDocument(document, layout, env.registry, env.allocator, out);
}

// 두 바이트 배열이 같은가.
inline bool SameBytes(const Array<uint8>& left, const Array<uint8>& right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// 위반이 정확히 하나이고 rule·subject인가.
inline bool OnlyViolation(const GraphViolationList& violations, GraphInvariant rule,
                          GraphSubject subject) noexcept {
    return violations.items.Size() == 1 && violations.dropped == 0 &&
           violations.items[0].rule == rule && violations.items[0].subject == subject;
}

} // namespace rocks::nodegraph_test

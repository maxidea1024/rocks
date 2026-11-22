#include "NodeGraphTestSupport.h"

// Name 정규 순서, 핀 목록, schema·노드 종류 등록표
// 시험이다(Docs/Design/NodeGraph.ko.md 6.1·6.2·10절): 원문 순서와 Name 같음 규칙의 일치, 등록 검사
// 항목마다 거부, 공통 재경유 종류, 해제와 lease, 핀 목록 계산(정적·동적).
using namespace rocks;
namespace node_graph_registry_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;

// lease 보존 횟수를 센다.
struct LeaseCounter {
    int32 count = 0;

    static void Retain(void* context) noexcept {
        ++static_cast<LeaseCounter*>(context)->count;
    }

    static void Release(void* context) noexcept {
        --static_cast<LeaseCounter*>(context)->count;
    }

    ModuleLease Make() noexcept {
        auto lease = ModuleLease::Acquire(this, &Retain, &Release);
        CHECK(lease);
        return lease ? Move(lease.Value()) : ModuleLease();
    }
};

// 시험 등록용 서술이다(pins는 호출자가 둔다).
NodeTypeDescriptor MakeDescriptor(Name type, Span<const PinDescriptor> pins) noexcept {
    NodeTypeDescriptor descriptor;
    descriptor.type = type;
    descriptor.staticPins = pins;
    return descriptor;
}

TEST(NodeGraph_NameTextOrder) {
    CHECK(CompareNameText("Alpha", "ALPHA") == 0);
    CHECK(CompareNameText("alpha", "Beta") < 0);
    CHECK(CompareNameText("Beta", "alpha") > 0);
    CHECK(CompareNameText("Pin", "Pin_0") < 0);
    CHECK(CompareNameText("Pin_2", "Pin_10") < 0);
    CHECK(CompareNameText("Pin_07", "Pin_7") != 0);
    CHECK(CompareNameText("Ab", "Abc") < 0);
    // None: 빈 문자열·"None"(대소문자 무시)·빈 base의 번호 이름은 모두 None이고 맨 앞이다.
    CHECK(CompareNameText("", "none") == 0);
    CHECK(CompareNameText("NONE", "_5") == 0);
    CHECK(CompareNameText("", "A") < 0);
    CHECK(CompareNameText("A", "None") > 0);
    // "None_3"은 번호 있는 보통 이름이다.
    CHECK(CompareNameText("None_3", "none_3") == 0);
    CHECK(CompareNameText("None", "None_3") < 0);
    CHECK(CompareNameText("None_3", "None_4") < 0);
    // ASCII만 대소문자를 무시하고 비ASCII는 바이트로 비교한다.
    CHECK(CompareNameText("\xC3\x9Cnit", "\xC3\x9CNIT") == 0);
    CHECK(CompareNameText("\xC3\x9Cnit", "\xC3\xBCnit") != 0);
    // 같은 Name은 정확히 0, 서로 다른 Name은 0이 아니다(NameTextEquals와 일치).
    const char* samples[] = {"", "A", "a_1", "A_2", "b", "None_0", "Item_10", "Item_9", "Z"};
    for (const char* left : samples) {
        for (const char* right : samples) {
            CHECK((CompareNameText(left, right) == 0) == NameTextEquals(left, right));
            CHECK(CompareNameText(left, right) == -CompareNameText(right, left));
        }
    }
}

TEST(NodeGraph_NameTokenOrder) {
    TestEnv env;
    REQUIRE(env.ready);
    const Name lower = env.Intern("result");
    CHECK(lower == env.pinResult);
    auto same = CompareNames(env.names, lower, env.pinResult);
    REQUIRE(same);
    CHECK(same.Value() == 0);
    auto before = CompareNames(env.names, env.pinA, env.pinB);
    REQUIRE(before);
    CHECK(before.Value() < 0);
    auto none = CompareNames(env.names, Name(), env.pinA);
    REQUIRE(none);
    CHECK(none.Value() < 0);
    Name broken;
    broken.kind = NameKind::Normal;
    auto invalid = CompareNames(env.names, broken, env.pinA);
    REQUIRE(!invalid);
    CHECK(invalid.Error() == ErrorCode::InvalidArgument);
}

TEST(NodeGraph_PinListRules) {
    TestEnv env;
    REQUIRE(env.ready);
    PinList pins(env.allocator);
    CHECK(pins.Add(InputPin(env.pinA, TestFloatType, "A")));
    CHECK(pins.Find(env.pinA) != nullptr);
    CHECK(pins.Find(env.Intern("a")) != nullptr);
    auto duplicate = pins.Add(OutputPin(env.Intern("A"), TestFloatType, "A"));
    REQUIRE(!duplicate);
    CHECK(duplicate.Error() == ErrorCode::InvalidArgument);
    auto none = pins.Add(InputPin(Name(), TestFloatType, "None"));
    REQUIRE(!none);
    CHECK(none.Error() == ErrorCode::InvalidArgument);
    CHECK(pins.Size() == 1);

    PinList full(env.allocator);
    char text[] = {'P', '_', '0', '0', '0', '0'};
    for (uint32 index = 0; index < MaxPinsPerNode; ++index) {
        text[2] = static_cast<char>('0' + index / 1000 % 10);
        text[3] = static_cast<char>('0' + index / 100 % 10);
        text[4] = static_cast<char>('0' + index / 10 % 10);
        text[5] = static_cast<char>('0' + index % 10);
        REQUIRE(full.Add(InputPin(env.Intern(StringView(text, sizeof(text))), TestFloatType, "P")));
    }

    auto over = full.Add(InputPin(env.pinA, TestFloatType, "A"));
    REQUIRE(!over);
    CHECK(over.Error() == ErrorCode::CapacityExceeded);
    CHECK(full.Size() == MaxPinsPerNode);
}

TEST(NodeGraph_RegistryInitializeAndReroute) {
    SystemAllocator allocator;
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    NodeTypeRegistry registry(allocator, names);
    TestSchema schema;
    schema.domain = names.TryIntern("Test").Value();
    auto early = registry.RegisterSchema(schema, ModuleLease());
    REQUIRE(!early);
    CHECK(early.Error() == ErrorCode::Unsupported);
    CHECK(!registry.IsReroute(registry.RerouteType()));

    REQUIRE(registry.Initialize());
    auto again = registry.Initialize();
    REQUIRE(!again);
    CHECK(again.Error() == ErrorCode::AlreadyExists);

    // 재경유는 공통 종류다: 도메인과 무관하게(None 포함) 찾고, 핀은 In(입력 1개)·Out(출력
    // 무제한)이다.
    const Name reroute = registry.RerouteType();
    CHECK(registry.IsReroute(names.TryIntern("graph.reroute").Value()));
    const RegisteredNodeType* common = registry.FindNodeType(Name(), reroute);
    REQUIRE(common != nullptr);
    CHECK(common->domain.IsNone());
    CHECK(registry.FindNodeType(schema.domain, reroute) == common);
    REQUIRE(common->descriptor.staticPins.Size() == 2);
    const PinDescriptor& in = common->descriptor.staticPins[0];
    const PinDescriptor& out = common->descriptor.staticPins[1];
    CHECK(in.key == names.TryIntern(ReroutePinIn).Value() && in.direction == PinDirection::Input &&
          in.maxEdges == 1 && in.type == UnresolvedPinType);
    CHECK(out.key == names.TryIntern(ReroutePinOut).Value() &&
          out.direction == PinDirection::Output && out.maxEdges == UnlimitedEdges);
    CHECK(common->descriptor.schemaVersion == RerouteSchemaVersion);
    CHECK(common->descriptor.propertiesStruct == Guid{});
}

TEST(NodeGraph_RegistrySchemaRules) {
    TestEnv env;
    REQUIRE(env.ready);
    CHECK(env.registry.FindSchema(env.domain) == &env.schema);
    CHECK(env.registry.FindSchema(env.Intern("TEST")) == &env.schema);
    CHECK(env.registry.FindSchema(env.Intern("Other")) == nullptr);

    TestSchema duplicate;
    duplicate.domain = env.Intern("test");
    auto twice = env.registry.RegisterSchema(duplicate, ModuleLease());
    REQUIRE(!twice);
    CHECK(twice.Error() == ErrorCode::AlreadyExists);

    TestSchema none;
    auto noDomain = env.registry.RegisterSchema(none, ModuleLease());
    REQUIRE(!noDomain);
    CHECK(noDomain.Error() == ErrorCode::InvalidArgument);
}

TEST(NodeGraph_RegistryNodeTypeRules) {
    TestEnv env;
    REQUIRE(env.ready);
    const Name fresh = env.Intern("Test.Fresh");
    const PinDescriptor good[] = {InputPin(env.pinA, TestFloatType, "A"),
                                  OutputPin(env.pinResult, UnresolvedPinType, "Result")};

    // 도메인 schema가 없으면 NotFound, 도메인 None이면 InvalidArgument.
    auto noSchema = env.registry.RegisterNodeType(env.Intern("Other"), MakeDescriptor(fresh, good),
                                                  ModuleLease());
    REQUIRE(!noSchema);
    CHECK(noSchema.Error() == ErrorCode::NotFound);
    auto noDomain =
        env.registry.RegisterNodeType(Name(), MakeDescriptor(fresh, good), ModuleLease());
    REQUIRE(!noDomain);
    CHECK(noDomain.Error() == ErrorCode::InvalidArgument);

    auto rejects = [&env](const NodeTypeDescriptor& descriptor) noexcept {
        auto registered = env.registry.RegisterNodeType(env.domain, descriptor, ModuleLease());
        return !registered && registered.Error() == ErrorCode::InvalidArgument &&
               env.registry.FindNodeType(env.domain, descriptor.type) == nullptr;
    };

    // 종류 이름: None, 공통 접두(대소문자 무시).
    CHECK(rejects(MakeDescriptor(Name(), good)));
    CHECK(rejects(MakeDescriptor(env.Intern("Graph.Custom"), good)));
    CHECK(rejects(MakeDescriptor(env.Intern("graph.custom2"), good)));

    // 버전 0.
    NodeTypeDescriptor versionZero = MakeDescriptor(fresh, good);
    versionZero.schemaVersion = 0;
    CHECK(rejects(versionZero));

    // 핀: 키 None, 키 중복(대소문자만 다른 것 포함), 타입 색인 범위 밖, maxEdges 0, 입력 핀의
    // ordered.
    const PinDescriptor noneKey[] = {InputPin(Name(), TestFloatType, "None")};
    CHECK(rejects(MakeDescriptor(fresh, noneKey)));
    const PinDescriptor duplicateKey[] = {InputPin(env.pinA, TestFloatType, "A"),
                                          OutputPin(env.Intern("a"), TestFloatType, "a")};
    CHECK(rejects(MakeDescriptor(fresh, duplicateKey)));
    const PinDescriptor badType[] = {InputPin(env.pinA, TestPinTypeCount, "A")};
    CHECK(rejects(MakeDescriptor(fresh, badType)));
    PinDescriptor zeroEdges = InputPin(env.pinA, TestFloatType, "A");
    zeroEdges.maxEdges = 0;
    CHECK(rejects(MakeDescriptor(fresh, Span<const PinDescriptor>(&zeroEdges, 1))));
    PinDescriptor orderedInput = InputPin(env.pinA, TestFloatType, "A");
    orderedInput.ordered = true;
    CHECK(rejects(MakeDescriptor(fresh, Span<const PinDescriptor>(&orderedInput, 1))));

    // 통과 쌍: 입력이 아님, 출력이 아님, 같은 출력 두 번.
    const PinDescriptor toggle[] = {InputPin(env.pinIn, TestFloatType, "In"),
                                    InputPin(env.pinA, TestFloatType, "A"),
                                    OutputPin(env.pinOut, TestFloatType, "Out")};
    NodeTypeDescriptor passThrough = MakeDescriptor(fresh, toggle);
    const PassThroughPair wrongInput[] = {{env.pinOut, env.pinOut}};
    passThrough.passThrough = wrongInput;
    CHECK(rejects(passThrough));
    const PassThroughPair wrongOutput[] = {{env.pinIn, env.pinA}};
    passThrough.passThrough = wrongOutput;
    CHECK(rejects(passThrough));
    const PassThroughPair twice[] = {{env.pinIn, env.pinOut}, {env.pinA, env.pinOut}};
    passThrough.passThrough = twice;
    CHECK(rejects(passThrough));

    // 정의되지 않은 capability 비트.
    NodeTypeDescriptor badCapability = MakeDescriptor(fresh, good);
    badCapability.capabilities = static_cast<NodeCapabilities>(1u << 7);
    CHECK(rejects(badCapability));

    // 정적 핀이 MaxPinsPerNode를 넘는다.
    Array<PinDescriptor> many(env.allocator);
    REQUIRE(many.TryResize(MaxPinsPerNode + 1));
    CHECK(rejects(MakeDescriptor(fresh, many.View())));

    // 통과: wildcard 핀 타입, 올바른 통과 쌍. 같은 종류 다시 등록은 AlreadyExists(대소문자 무시).
    REQUIRE(env.registry.RegisterNodeType(env.domain, MakeDescriptor(fresh, good), ModuleLease()));
    auto duplicate = env.registry.RegisterNodeType(
        env.domain, MakeDescriptor(env.Intern("TEST.FRESH"), good), ModuleLease());
    REQUIRE(!duplicate);
    CHECK(duplicate.Error() == ErrorCode::AlreadyExists);
    const PassThroughPair valid[] = {{env.pinIn, env.pinOut}};
    passThrough.type = env.Intern("Test.Pass");
    passThrough.passThrough = valid;
    REQUIRE(env.registry.RegisterNodeType(env.domain, passThrough, ModuleLease()));
    const RegisteredNodeType* found = env.registry.FindNodeType(env.domain, passThrough.type);
    REQUIRE(found != nullptr);
    CHECK(found->domain == env.domain);
    REQUIRE(found->descriptor.passThrough.Size() == 1);
    CHECK(found->descriptor.passThrough[0].input == env.pinIn);
}

// 등록표는 핀·통과 쌍 배열을 소유 복사한다: 호출자 배열을 바꾸거나 없애도 등록된 서술은 그대로다.
TEST(NodeGraph_RegistryCopiesDescriptorArrays) {
    TestEnv env;
    REQUIRE(env.ready);
    const Name type = env.Intern("Test.Copied");
    {
        PinDescriptor pins[] = {InputPin(env.pinA, TestFloatType, "A"),
                                OutputPin(env.pinResult, TestFloatType, "Result")};
        REQUIRE(
            env.registry.RegisterNodeType(env.domain, MakeDescriptor(type, pins), ModuleLease()));
        pins[0].key = env.pinB;
        pins[0].maxEdges = 7;
    }

    // 다른 등록으로 내부 배열이 자라도 서술 Span이 유효하다.
    for (uint32 index = 0; index < 32; ++index) {
        char text[] = {'T',
                       'e',
                       's',
                       't',
                       '.',
                       'N',
                       static_cast<char>('A' + index / 26),
                       static_cast<char>('a' + index % 26)};
        const PinDescriptor pins[] = {InputPin(env.pinA, TestFloatType, "A")};
        REQUIRE(env.registry.RegisterNodeType(
            env.domain, MakeDescriptor(env.Intern(StringView(text, sizeof(text))), pins),
            ModuleLease()));
    }

    const RegisteredNodeType* found = env.registry.FindNodeType(env.domain, type);
    REQUIRE(found != nullptr);
    REQUIRE(found->descriptor.staticPins.Size() == 2);
    CHECK(found->descriptor.staticPins[0].key == env.pinA);
    CHECK(found->descriptor.staticPins[0].maxEdges == 1);
}

TEST(NodeGraph_RegistryUnregisterAndLeases) {
    TestEnv env;
    REQUIRE(env.ready);
    LeaseCounter counter;
    TestSchema other;
    other.domain = env.Intern("Other");
    auto schema = env.registry.RegisterSchema(other, counter.Make());
    REQUIRE(schema);
    CHECK(counter.count == 1);

    const PinDescriptor pins[] = {InputPin(env.pinA, TestFloatType, "A")};
    auto first = env.registry.RegisterNodeType(
        other.domain, MakeDescriptor(env.Intern("Other.One"), pins), counter.Make());
    auto second = env.registry.RegisterNodeType(
        other.domain, MakeDescriptor(env.Intern("Other.Two"), pins), counter.Make());
    REQUIRE(first);
    REQUIRE(second);
    CHECK(counter.count == 3);

    // 그 도메인 종류가 남아 있으면 schema 해제는 Busy다.
    auto busy = env.registry.UnregisterSchema(schema.Value());
    REQUIRE(!busy);
    CHECK(busy.Error() == ErrorCode::Busy);
    CHECK(env.registry.FindSchema(other.domain) == &other);

    REQUIRE(env.registry.UnregisterNodeType(first.Value()));
    CHECK(counter.count == 2);
    CHECK(env.registry.FindNodeType(other.domain, env.Intern("Other.One")) == nullptr);
    CHECK(env.registry.FindNodeType(other.domain, env.Intern("Other.Two")) != nullptr);
    auto missing = env.registry.UnregisterNodeType(first.Value());
    REQUIRE(!missing);
    CHECK(missing.Error() == ErrorCode::NotFound);
    auto zero = env.registry.UnregisterNodeType(NodeTypeRegistrationId{});
    REQUIRE(!zero);
    CHECK(zero.Error() == ErrorCode::NotFound);

    REQUIRE(env.registry.UnregisterNodeType(second.Value()));
    REQUIRE(env.registry.UnregisterSchema(schema.Value()));
    CHECK(counter.count == 0);
    CHECK(env.registry.FindSchema(other.domain) == nullptr);
    auto schemaMissing = env.registry.UnregisterSchema(schema.Value());
    REQUIRE(!schemaMissing);
    CHECK(schemaMissing.Error() == ErrorCode::NotFound);

    // 등록표가 소멸하면 남은 lease를 놓는다.
    LeaseCounter kept;
    {
        TestEnv scoped;
        REQUIRE(scoped.ready);
        TestSchema keptSchema;
        keptSchema.domain = scoped.Intern("Kept");
        REQUIRE(scoped.registry.RegisterSchema(keptSchema, kept.Make()));
        REQUIRE(scoped.registry.RegisterNodeType(
            keptSchema.domain, MakeDescriptor(scoped.Intern("Kept.One"), pins), kept.Make()));
        CHECK(kept.count == 2);
    }

    CHECK(kept.count == 0);
}

TEST(NodeGraph_RegistryCollectPins) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    REQUIRE(sample.ok);
    const Graph& main = sample.Main();
    PinList pins(env.allocator);

    REQUIRE(env.registry.CollectPins(sample.document, main, sample.Node(SampleAdd), pins));
    REQUIRE(pins.Size() == 3);
    CHECK(pins.View()[0].key == env.pinA && pins.View()[1].key == env.pinB &&
          pins.View()[2].key == env.pinResult);

    // 동적 핀: 속성 Record의 schema version 2 → Result + Item_0·Item_1.
    REQUIRE(env.registry.CollectPins(sample.document, main, sample.Node(SampleDynamic), pins));
    REQUIRE(pins.Size() == 3);
    CHECK(pins.View()[1].key == env.pinItem0 && pins.View()[2].key == env.pinItem1);
    CHECK(pins.View()[2].direction == PinDirection::Input);

    // 알 수 없는 종류는 NotFound이고 out은 비어 있다.
    auto unknown =
        env.registry.CollectPins(sample.document, main, sample.Node(SampleUnknown), pins);
    REQUIRE(!unknown);
    CHECK(unknown.Error() == ErrorCode::NotFound);
    CHECK(pins.Size() == 0);

    // 동적 핀 함수의 실패는 그대로 돌려주고 out을 비운다.
    GraphNode broken = MakeNode(env, Id(99), env.typeDynamic);
    auto failed = env.registry.CollectPins(sample.document, main, broken, pins);
    REQUIRE(!failed);
    CHECK(failed.Error() == ErrorCode::InvalidArgument);
    CHECK(pins.Size() == 0);

    // 재경유 핀.
    REQUIRE(env.registry.CollectPins(sample.document, main, sample.Node(SampleReroute), pins));
    CHECK(pins.Size() == 2);
}

} // namespace node_graph_registry_tests_file

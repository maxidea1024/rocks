#include "GraphToolsTestSupport.h"

#include "GraphDumpGolden.h"
#include <NodeGraphTools/GraphCommandlets.h>

// graph-dump·graph-validate·graph-diff 시험이다(Docs/Design/NodeGraph.ko.md 9.4절 "commandlet",
// 13절 GT10): 표본 문서 dump golden, --out·--semantic-only, 사용법·파일·형식 오류의 종료 코드, 검증
// 결과, diff 결과. 명령은 시험 도메인의 Name pool·등록표를 문맥으로 받는다.
using namespace rocks;
namespace graph_commandlet_tests_file {

using namespace rocks::nodegraph_tools_test;

Result<void> RegisterFixtureTypes(void*, TypeRegistry& types) noexcept {
    if (auto point = GeneratedType<InstancedPoint>::Register(types); !point) {
        return Result<void>::Failure(point.Error());
    }

    return Result<void>::Success();
}

// 명령 환경이다: 시험 도메인, 시험 디렉터리, 문맥.
struct CommandEnv {
    explicit CommandEnv(const char* tag) noexcept : directory(tag), captured(tools.env.allocator) {
        context.allocator = &tools.env.allocator;
        context.names = &tools.env.names;
        context.registry = &tools.env.registry;
        context.setupTypes = &RegisterFixtureTypes;
        ready = tools.ready && directory.made;
    }

    // directory 아래 파일 경로다.
    String Path(StringView name) noexcept {
        String path(tools.env.allocator);
        static_cast<void>(path.TryAppend(directory.Root()));
        static_cast<void>(path.TryAppend("/"));
        static_cast<void>(path.TryAppend(name));
        return path;
    }

    bool Write(StringView name, StringView text) noexcept {
        String path = Path(name);
        char buffer[256] = {};
        if (path.ByteCount() >= sizeof(buffer)) {
            return false;
        }

        for (usize index = 0; index < path.ByteCount(); ++index) {
            buffer[index] = path.Data()[index];
        }

        platform::File file;
        return file.Open(buffer, platform::FileOpenMode::CreateAlways) &&
               file.WriteAll(text.Data(), text.ByteCount()).status && file.Close();
    }

    bool WriteDocument(StringView name, const Sample& sample) noexcept {
        StringBuilder<> text(tools.env.allocator);
        return WriteGraphDocumentJson(tools.context, sample.document, &sample.layout, {}, text) &&
               Write(name, text.View());
    }

    int32 Run(Result<int32> (*command)(void*, Span<const StringView>, CommandletOutput&),
              Span<const StringView> args) noexcept {
        captured.Clear();
        CommandletOutput output = captured.Output();
        auto result = command(&context, args, output);
        return result ? result.Value() : -1;
    }

    ToolsEnv tools;
    TempDirectory directory;
    GraphCommandletContext context;
    Captured captured;
    bool ready = false;
};

// 표본 문서 dump는 golden과 같고, 다시 dump해도 같다(GT10).
TEST(GraphCommandlet_DumpMatchesGolden) {
    CommandEnv env("dump");
    REQUIRE(env.ready);
    Sample sample(env.tools.env);
    REQUIRE(sample.ok);
    REQUIRE(env.WriteDocument("sample.graph.json", sample));
    String input = env.Path("sample.graph.json");
    const StringView args[] = {input.View()};
    CHECK(env.Run(&RunGraphDumpCommand, args) == GraphCommandSuccess);
    const bool golden = env.captured.out.View() == SampleDocumentGolden;
    if (!golden) {
        fprintf(stderr, "graph-dump golden mismatch, actual:\n%.*s\n",
                static_cast<int>(env.captured.out.ByteCount()), env.captured.out.Data());
    }

    CHECK(golden);
    CHECK(env.captured.error.IsEmpty());

    // --out은 같은 내용을 파일로 쓴다. 그 파일을 다시 dump해도 같다.
    String outPath = env.Path("out.graph.json");
    String outOption(env.tools.env.allocator);
    REQUIRE(outOption.TryAppend("--out=") && outOption.TryAppend(outPath.View()));
    const StringView toFile[] = {input.View(), outOption.View()};
    CHECK(env.Run(&RunGraphDumpCommand, toFile) == GraphCommandSuccess);
    CHECK(env.captured.out.IsEmpty());
    const StringView again[] = {outPath.View()};
    CHECK(env.Run(&RunGraphDumpCommand, again) == GraphCommandSuccess);
    CHECK(env.captured.out.View() == SampleDocumentGolden);

    // --semantic-only는 배치를 뺀다.
    const StringView semantic[] = {input.View(), "--semantic-only"};
    CHECK(env.Run(&RunGraphDumpCommand, semantic) == GraphCommandSuccess);
    CHECK(!Contains(env.captured.out.View(), "\"layout\""));
}

TEST(GraphCommandlet_UsageAndInputErrors) {
    CommandEnv env("errors");
    REQUIRE(env.ready);
    const StringView none[] = {"--records"};
    CHECK(env.Run(&RunGraphDumpCommand, Span<const StringView>(none, 1)) == GraphCommandUsage);
    const StringView unknown[] = {"a.json", "--nope"};
    CHECK(env.Run(&RunGraphDumpCommand, unknown) == GraphCommandUsage);
    CHECK(env.Run(&RunGraphValidateCommand, Span<const StringView>()) == GraphCommandUsage);
    const StringView one[] = {"a.json"};
    CHECK(env.Run(&RunGraphDiffCommand, one) == GraphCommandUsage);

    String missing = env.Path("missing.graph.json");
    const StringView missingArgs[] = {missing.View()};
    CHECK(env.Run(&RunGraphDumpCommand, missingArgs) == GraphCommandInputError);
    CHECK(Contains(env.captured.error.View(), "cannot open"));

    REQUIRE(env.Write("broken.graph.json", "{\"format\": \"rocks.graph-document\",\n  oops}"));
    String broken = env.Path("broken.graph.json");
    const StringView brokenArgs[] = {broken.View()};
    CHECK(env.Run(&RunGraphValidateCommand, brokenArgs) == GraphCommandInputError);
    CHECK(Contains(env.captured.error.View(), ":2:"));
    CHECK(Contains(env.captured.error.View(), "JsonSyntax"));

    // --out을 두 번 주면 사용법 오류, 디렉터리에 쓰면 입력 오류다. diff의 둘째 파일 오류도 입력
    // 오류다.
    Sample sample(env.tools.env);
    REQUIRE(sample.ok);
    REQUIRE(env.WriteDocument("good.graph.json", sample));
    String good = env.Path("good.graph.json");
    const StringView twice[] = {good.View(), "--out=a.json", "--out=b.json"};
    CHECK(env.Run(&RunGraphDumpCommand, twice) == GraphCommandUsage);
    String directory(env.tools.env.allocator);
    REQUIRE(directory.TryAppend("--out=") && directory.TryAppend(env.directory.Root()));
    const StringView toDirectory[] = {good.View(), directory.View()};
    CHECK(env.Run(&RunGraphDumpCommand, toDirectory) == GraphCommandInputError);
    const StringView secondMissing[] = {good.View(), missing.View()};
    CHECK(env.Run(&RunGraphDiffCommand, secondMissing) == GraphCommandInputError);

    // --records는 모든 값을 바이트로 쓴다(표본은 원래 바이트라 golden과 같다).
    const StringView records[] = {good.View(), "--records"};
    CHECK(env.Run(&RunGraphDumpCommand, records) == GraphCommandSuccess);
    CHECK(env.captured.out.View() == SampleDocumentGolden);

    // Name pool만 준 문맥은 내부 오류(종료 코드 4)다.
    GraphCommandletContext half;
    half.allocator = &env.tools.env.allocator;
    half.names = &env.tools.env.names;
    Captured captured(env.tools.env.allocator);
    CommandletOutput output = captured.Output();
    const StringView single[] = {good.View()};
    auto result = RunGraphDumpCommand(&half, single, output);
    REQUIRE(result);
    CHECK(result.Value() == GraphCommandInternalError);
    CHECK(Contains(captured.error.View(), "graph-dump: internal error"));
    CommandletRegistry registry;
    CHECK(!RegisterGraphCommandlets(registry, half));
}

TEST(GraphCommandlet_Validate) {
    CommandEnv env("validate");
    REQUIRE(env.ready);
    Sample sample(env.tools.env);
    REQUIRE(sample.ok);
    REQUIRE(env.WriteDocument("good.graph.json", sample));
    String good = env.Path("good.graph.json");
    const StringView goodArgs[] = {good.View()};
    CHECK(env.Run(&RunGraphValidateCommand, goodArgs) == GraphCommandSuccess);
    CHECK(env.captured.out.IsEmpty());

    // 없는 노드로 가는 간선(GI1)은 위반이다.
    REQUIRE(sample.Main().edges.TryPushBack(
        MakeEdge(SampleParamA, env.tools.env.pinValue, Id(99), env.tools.env.pinA)));
    REQUIRE(env.WriteDocument("bad.graph.json", sample));
    String bad = env.Path("bad.graph.json");
    const StringView badArgs[] = {bad.View()};
    CHECK(env.Run(&RunGraphValidateCommand, badArgs) == GraphCommandDifferent);
    CHECK(Contains(env.captured.out.View(), "error "));

    // 등록표를 주지 않은 문맥(도메인 없음)은 도메인 미등록을 보고한다.
    GraphCommandletContext bare;
    bare.allocator = &env.tools.env.allocator;
    Captured captured(env.tools.env.allocator);
    CommandletOutput output = captured.Output();
    auto result = RunGraphValidateCommand(&bare, goodArgs, output);
    REQUIRE(result);
    CHECK(result.Value() == GraphCommandDifferent);
    CHECK(Contains(captured.out.View(), "document domain is not registered"));
}

TEST(GraphCommandlet_Diff) {
    CommandEnv env("diff");
    REQUIRE(env.ready);
    Sample sample(env.tools.env);
    REQUIRE(sample.ok);
    REQUIRE(env.WriteDocument("a.graph.json", sample));
    String a = env.Path("a.graph.json");
    const StringView same[] = {a.View(), a.View()};
    CHECK(env.Run(&RunGraphDiffCommand, same) == GraphCommandSuccess);
    CHECK(env.captured.out.IsEmpty());

    // 배치만 바꾸면 --semantic-only로는 같다.
    for (NodeLayout& node : sample.layout.graphs[0].nodes) {
        node.position.x += 10.0f;
    }

    REQUIRE(env.WriteDocument("b.graph.json", sample));
    String b = env.Path("b.graph.json");
    const StringView moved[] = {a.View(), b.View()};
    CHECK(env.Run(&RunGraphDiffCommand, moved) == GraphCommandDifferent);
    CHECK(Contains(env.captured.out.View(), "node-layout "));
    const StringView semantic[] = {a.View(), b.View(), "--semantic-only"};
    CHECK(env.Run(&RunGraphDiffCommand, semantic) == GraphCommandSuccess);
}

TEST(GraphCommandlet_Register) {
    CommandEnv env("register");
    REQUIRE(env.ready);
    CommandletRegistry registry;
    REQUIRE(RegisterGraphCommandlets(registry, env.context));
    auto count = registry.Count();
    REQUIRE(count);
    CHECK(count.Value() == 3);
    GraphCommandletContext empty;
    CommandletRegistry other;
    auto rejected = RegisterGraphCommandlets(other, empty);
    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
}

} // namespace graph_commandlet_tests_file

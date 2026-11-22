#include "NodeGraphTestSupport.h"

#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <NodeGraph/GraphDocument.generated.h>
#include <NodeGraph/NodeGraphModule.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <ReflectionText/TextJson.h>

// 문서·배치 리플렉션 타입의 시험이다(Docs/Design/NodeGraph.ko.md 6.1절): 모듈 등록, 표본
// 문서·배치의 JSON 왕복(내보내기 → 새 값에 들여오기 → 다시 내보낸 텍스트가 같다), 들여온 값의 의미
// 바이트가 원본과 같음.
using namespace rocks;
namespace node_graph_reflection_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;
using namespace rocks::reflection_text;

// 표본 문서의 Name이 같은 pool이어야 하므로 시험 도메인 환경의 pool을 객체 Registry에 붙인다.
struct ReflectionEnv {
    explicit ReflectionEnv(TestEnv& env) noexcept
        : types(env.allocator), modules(types, env.allocator),
          objects(env.allocator, types, ContextId{0x4e4f4445ull, 1}, Options(env)),
          access(objects.OwnerAccess()), diagnostics(env.allocator) {
        registered = static_cast<bool>(RegisterNodeGraphTypes(modules));
    }

    ~ReflectionEnv() {
        CHECK(objects.Shutdown(access));
        CHECK(modules.Unregister());
        CHECK(types.Shutdown());
    }

    static RegistryOptions Options(TestEnv& env) noexcept {
        RegistryOptions options;
        options.names = &env.names;
        return options;
    }

    template <class T>
    Result<void> Export(T& value, Allocator& allocator, StringBuilder<>& out) noexcept {
        auto lease = types.AcquireStructByNative(NativeTypeKey<T>());
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        out.Clear();
        JsonWriter writer(out);
        if (auto written = ExportJson(
                AssignmentTarget::Value(objects, access, lease.Value(), &value, allocator), writer,
                options);
            !written) {
            return written;
        }

        return writer.Finish();
    }

    template <class T>
    Result<void> Import(T& value, Allocator& allocator, StringView json) noexcept {
        auto lease = types.AcquireStructByNative(NativeTypeKey<T>());
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        diagnostics.Clear();
        JsonDocument document(allocator);
        if (!document.Parse(json).Succeeded()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto imported =
            ImportJson(document.Root(),
                       AssignmentTarget::Value(objects, access, lease.Value(), &value, allocator),
                       options, diagnostics);
        if (!imported) {
            return Result<void>::Failure(imported.Error());
        }

        auto published = imported.Value().Publish();
        return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
    }

    TypeRegistry types;
    ModuleRegistration modules;
    ObjectRegistry objects;
    ObjectAccessContext access;
    TextDiagnostics diagnostics;
    TextCodecOptions options;
    bool registered = false;
};

TEST(NodeGraph_ReflectionTypesRegister) {
    TestEnv env;
    REQUIRE(env.ready);
    ReflectionEnv reflection(env);
    REQUIRE(reflection.registered);
    CHECK(reflection.types.AcquireStructByNative(NativeTypeKey<GraphDocument>()));
    CHECK(reflection.types.AcquireStructByNative(NativeTypeKey<GraphDocumentLayout>()));
    CHECK(reflection.types.AcquireStructByNative(NativeTypeKey<PropertyRecord>()));
}

// 문서와 배치를 JSON으로 내보내고 새 값에 들여온 뒤 다시 내보내면 같은 텍스트이고, 들여온 문서는
// 검사를 통과하며 의미 바이트가 원본과 같다.
TEST(NodeGraph_ReflectionJsonRoundTrip) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    REQUIRE(sample.ok);
    ReflectionEnv reflection(env);
    REQUIRE(reflection.registered);

    StringBuilder<> first(env.allocator);
    StringBuilder<> second(env.allocator);
    REQUIRE(reflection.Export(sample.document, env.allocator, first));
    GraphDocument copy(env.allocator);
    REQUIRE(reflection.Import(copy, env.allocator, first.View()));
    REQUIRE(reflection.Export(copy, env.allocator, second));
    CHECK(first.View() == second.View());

    REQUIRE(reflection.Export(sample.layout, env.allocator, first));
    GraphDocumentLayout layoutCopy(env.allocator);
    REQUIRE(reflection.Import(layoutCopy, env.allocator, first.View()));
    REQUIRE(reflection.Export(layoutCopy, env.allocator, second));
    CHECK(first.View() == second.View());

    GraphViolationList violations(env.allocator);
    CHECK(Validate(env, copy, &layoutCopy, violations));
    Array<uint8> original(env.allocator);
    Array<uint8> imported(env.allocator);
    REQUIRE(WriteSemanticBytes(sample.document, env.registry, env.allocator, original));
    REQUIRE(WriteSemanticBytes(copy, env.registry, env.allocator, imported));
    CHECK(SameBytes(original, imported));
}

} // namespace node_graph_reflection_tests_file

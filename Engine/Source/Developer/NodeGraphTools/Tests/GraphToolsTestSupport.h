#pragma once

#include <ReflectionTestFixtures/InstancedStructFixture.generated.h>
#include <NodeGraph/Tests/NodeGraphMutatorTestSupport.h>
#include <NodeGraphTools/GraphText.h>
#include <Object/ObjectRegistry.h>
#include <ObjectSerialization/StructRecord.h>
#include <Core/Commandlet/Commandlet.h>
#include <Platform/File.h>
#include <Platform/Process.h>

#include <stdio.h>

// NodeGraphTools 시험 공용 도우미다(Docs/Design/NodeGraph.ko.md 9.4절, 13절 GT5·GT10): NodeGraph
// 시험 도메인(TestEnv) 위에 리플렉션 Registry(같은 NamePool)와 텍스트 문맥을 둔다. 속성 struct는
// ReflectionTestFixtures의 InstancedPoint·InstancedScale을 쓴다.
namespace rocks::nodegraph_tools_test {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;
using reflection_fixture::InstancedLocked;
using reflection_fixture::InstancedPoint;
using reflection_fixture::InstancedScale;

inline RegistryOptions WithNames(NamePool& names) noexcept {
    RegistryOptions options;
    options.names = &names;
    return options;
}

// 텍스트 시험 환경이다.
struct ToolsEnv {
    ToolsEnv() noexcept
        : types(env.allocator), objects(env.allocator, types, {0x4e4754, 1}, WithNames(env.names)),
          context(env.allocator, env.names, objects, objects.OwnerAccess()) {
        ready = env.ready && GeneratedType<InstancedPoint>::Register(types).IsOk() &&
                GeneratedType<InstancedScale>::Register(types).IsOk() &&
                GeneratedType<InstancedLocked>::Register(types).IsOk();
    }

    ~ToolsEnv() {
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        CHECK(types.Shutdown());
    }

    template <class S>
    Array<uint8> Record(const S& value) noexcept {
        auto lease = types.AcquireValueType(TypeIdOf<S>(), TypeKind::Struct);
        if (!lease) {
            platform::Fatal("test struct lease failed");
        }

        auto saved =
            SaveStructRecord(env.allocator, objects, objects.OwnerAccess(), lease.Value(), &value);
        if (!saved) {
            platform::Fatal("test struct record failed");
        }

        return Move(saved.Value());
    }

    // 표본 점 Record다(x, label).
    Array<uint8> PointRecord(int32 x, StringView label) noexcept {
        InstancedPoint point(objects.ObjectAllocator());
        point.x = x;
        if (!point.label.TryAssign(label)) {
            platform::Fatal("test label failed");
        }

        return Record(point);
    }

    Array<uint8> WriteDocument(const GraphDocument& document, const GraphDocumentLayout* layout,
                               GraphTextWriteOptions options = {}) noexcept {
        StringBuilder<> text(env.allocator);
        Array<uint8> bytes(env.allocator);
        if (WriteGraphDocumentJson(context, document, layout, options, text)) {
            static_cast<void>(bytes.TryAppend(Span<const uint8>(
                reinterpret_cast<const uint8*>(text.View().Data()), text.View().ByteCount())));
        }

        return bytes;
    }

    TestEnv env;
    TypeRegistry types;
    ObjectRegistry objects;
    GraphTextContext context;
    bool ready = false;
};

// 시험 디렉터리다(현재 디렉터리 아래 `rocks-node-graph-tools-<tag>-<pid>` — 끝나면 지운다).
struct TempDirectory {
    explicit TempDirectory(const char* tag) noexcept {
        const int count = snprintf(root, sizeof(root), "rocks-node-graph-tools-%s-%u", tag,
                                   static_cast<unsigned>(platform::CurrentProcessId()));
        rootBytes = count > 0 ? static_cast<usize>(count) : 0;
        made = rootBytes != 0 && rootBytes < sizeof(root) &&
               platform::MakeDirectoryTree(platform::PhysicalFileSystem(), root, rootBytes);
    }

    ~TempDirectory() noexcept {
        if (made) {
            CHECK(platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root, rootBytes));
        }
    }

    StringView Root() const noexcept {
        return StringView(root, rootBytes);
    }

    char root[96] = {};
    usize rootBytes = 0;
    bool made = false;
};

// commandlet 표준 출력·오류를 모은다.
struct Captured {
    explicit Captured(Allocator& allocator) noexcept : out(allocator), error(allocator) {
    }

    static Result<void> Write(void* context, CommandletStream stream, StringView text) noexcept {
        Captured& captured = *static_cast<Captured*>(context);
        return (stream == CommandletStream::Output ? captured.out : captured.error).TryAppend(text);
    }

    CommandletOutput Output() noexcept {
        return CommandletOutput{&Captured::Write, this};
    }

    void Clear() noexcept {
        out.Clear();
        error.Clear();
    }

    String out;
    String error;
};

inline StringView TextOf(const Array<uint8>& bytes) noexcept {
    return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
}

inline bool Contains(StringView text, StringView part) noexcept {
    if (part.ByteCount() > text.ByteCount()) {
        return false;
    }

    for (usize start = 0; start + part.ByteCount() <= text.ByteCount(); ++start) {
        if (StringView(text.Data() + start, part.ByteCount()) == part) {
            return true;
        }
    }

    return false;
}

} // namespace rocks::nodegraph_tools_test

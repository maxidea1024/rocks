#include "Tests/Support/Test.h"
#include "Tests/Support/NFailAllocator.h"

#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>

namespace {

using namespace rocks;

uint32 graphDestructions = 0;

struct GraphNode final : Object {
    ObjectRef<GraphNode> first;
    ObjectRef<GraphNode> second;

    ~GraphNode() noexcept {
        ++graphDestructions;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(first);
        visitor.Visit(second);
    }
};

constexpr Guid GraphType{0x474354455354ULL, 1};
constexpr uint32 GraphSize = 128;

void MarkModel(const uint32 (&edges)[GraphSize][2], uint32 root, bool (&visited)[GraphSize]) {
    uint32 stack[GraphSize]{};
    uint32 count = 0;

    if (!visited[root]) {
        visited[root] = true;
        stack[count++] = root;
    }

    while (count != 0) {
        const uint32 node = stack[--count];

        for (uint32 edge = 0; edge < 2; ++edge) {
            const uint32 target = edges[node][edge];

            if (!visited[target]) {
                visited[target] = true;
                stack[count++] = target;
            }
        }
    }
}

} // namespace

TEST(ObjectGcMatchesIndependentGraphOracle) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        const TypeSchema schema{GraphType, 1, {}, {}};
        auto registered = types.Register(schema, MakeRuntimeTypeOps<GraphNode>());
        REQUIRE(registered.IsOk());

        ObjectRegistry registry(allocator, types, Guid{71, 1});
        const auto access = registry.OwnerAccess();
        ObjectHandle handles[GraphSize]{};
        uint32 edges[GraphSize][2]{};
        graphDestructions = 0;

        ConstructionContext construction(registry, access);

        for (uint32 index = 0; index < GraphSize; ++index) {
            auto created = construction.Create<GraphNode>(GraphType);
            REQUIRE(created.IsOk());
            handles[index] = created.Value();
        }

        for (uint32 index = 0; index < GraphSize; ++index) {
            const uint32 component = (index / 32) * 32;
            const uint32 local = index % 32;
            edges[index][0] = component + (local + 1) % 16;

            if (local == 0) {
                edges[index][1] = component + 16;
            } else if (local >= 16) {
                edges[index][1] = component + (local + 1) % 32;
            } else {
                edges[index][1] = component + 3;
            }

            auto node = construction.Get<GraphNode>(handles[index]);
            REQUIRE(node.IsOk());
            REQUIRE(construction
                        .SetReference(handles[index], node.Value()->first, handles[edges[index][0]])
                        .IsOk());
            REQUIRE(
                construction
                    .SetReference(handles[index], node.Value()->second, handles[edges[index][1]])
                    .IsOk());
        }

        auto firstRoot = construction.Commit();
        REQUIRE(firstRoot.IsOk());
        auto secondRoot = registry.TryPin<GraphNode>(access, handles[64]);
        REQUIRE(secondRoot.IsOk());

        bool expected[GraphSize]{};
        MarkModel(edges, 0, expected);
        MarkModel(edges, 64, expected);
        uint32 expectedCount = 0;

        for (bool reachable : expected) {
            expectedCount += reachable ? 1U : 0U;
        }

        auto collected = registry.Collect(access);
        REQUIRE(collected.IsOk());
        CHECK(registry.Stats().liveObjects == expectedCount);
        CHECK(graphDestructions == GraphSize - expectedCount);

        for (uint32 index = 0; index < GraphSize; ++index) {
            auto resolved = registry.Resolve<GraphNode>(access, handles[index]);
            CHECK(resolved.IsOk() == expected[index]);
        }

        secondRoot.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(registry.Stats().liveObjects == 32);
        CHECK(graphDestructions == GraphSize - 32);

        firstRoot.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(registry.Stats().liveObjects == 0);
        CHECK(graphDestructions == GraphSize);
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectGcHandlesDeepChainWithoutRecursiveVisitorTraversal) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        const TypeSchema schema{GraphType, 1, {}, {}};
        auto registered = types.Register(schema, MakeRuntimeTypeOps<GraphNode>());
        REQUIRE(registered.IsOk());

        constexpr uint32 count = 2048;
        ObjectHandle handles[count]{};
        ObjectRegistry registry(allocator, types, Guid{72, 1});
        const auto access = registry.OwnerAccess();
        graphDestructions = 0;
        ConstructionContext construction(registry, access);

        for (uint32 index = 0; index < count; ++index) {
            auto created = construction.Create<GraphNode>(GraphType);
            REQUIRE(created.IsOk());
            handles[index] = created.Value();

            if (index != 0) {
                auto parent = construction.Get<GraphNode>(handles[index - 1]);
                REQUIRE(parent.IsOk());
                REQUIRE(construction
                            .SetReference(handles[index - 1], parent.Value()->first, handles[index])
                            .IsOk());
            }
        }

        auto root = construction.Commit();
        REQUIRE(root.IsOk());
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(registry.Stats().liveObjects == count);
        CHECK(graphDestructions == 0);

        root.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(graphDestructions == count);
        CHECK(registry.Stats().liveObjects == 0);
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectGcAllocationFailureCannotSweepFromIncompleteMark) {
    SystemAllocator allocator;
    uint64 allocationCount = 0;

    {
        TypeRegistry types(allocator);
        auto registered =
            types.Register(TypeSchema{GraphType, 1, {}, {}}, MakeRuntimeTypeOps<GraphNode>());
        REQUIRE(registered.IsOk());
        rocks::test::NFailAllocator fault(allocator);
        ObjectRegistry registry(fault, types, Guid{73, 1});
        auto access = registry.OwnerAccess();
        auto live = registry.NewObject<GraphNode>(access, GraphType);
        auto dead = registry.NewObject<GraphNode>(access, GraphType);
        REQUIRE(live.IsOk());
        REQUIRE(dead.IsOk());
        dead.Value().Reset();
        fault.DisableFailure();
        REQUIRE(registry.Collect(access).IsOk());
        allocationCount = fault.Attempts();
        live.Value().Reset();
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);

    for (uint64 attempt = 1; attempt <= allocationCount; ++attempt) {
        {
            TypeRegistry types(allocator);
            auto registered =
                types.Register(TypeSchema{GraphType, 1, {}, {}}, MakeRuntimeTypeOps<GraphNode>());
            REQUIRE(registered.IsOk());
            rocks::test::NFailAllocator fault(allocator);
            ObjectRegistry registry(fault, types, Guid{74, attempt});
            auto access = registry.OwnerAccess();
            graphDestructions = 0;
            auto live = registry.NewObject<GraphNode>(access, GraphType);
            auto dead = registry.NewObject<GraphNode>(access, GraphType);
            REQUIRE(live.IsOk());
            REQUIRE(dead.IsOk());
            const ObjectHandle liveHandle = live.Value().Handle();
            const ObjectHandle deadHandle = dead.Value().Handle();
            dead.Value().Reset();
            fault.FailOnAttempt(attempt);

            CHECK(!registry.Collect(access).IsOk());
            CHECK(graphDestructions == 0);
            CHECK(registry.Stats().liveObjects == 2);
            CHECK(registry.Stats().retiredObjects == 0);
            CHECK(registry.Resolve<GraphNode>(access, liveHandle).IsOk());
            CHECK(registry.Resolve<GraphNode>(access, deadHandle).IsOk());

            fault.DisableFailure();
            REQUIRE(registry.Collect(access).IsOk());
            CHECK(graphDestructions == 1);
            live.Value().Reset();
            REQUIRE(registry.Shutdown(access).IsOk());
            CHECK(graphDestructions == 2);
            REQUIRE(types.Unregister(registered.Value()).IsOk());
            REQUIRE(types.Shutdown().IsOk());
        }

        CHECK(allocator.Stats().liveAllocations == 0);
    }
}

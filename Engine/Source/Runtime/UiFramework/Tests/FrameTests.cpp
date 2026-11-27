#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <TestSupport/Test.h>
#include <UiCore/DrawBatch.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Docking/DockManager.h>
#include <UiFramework/Layout/Frame.h>

using namespace rocks;
using namespace rocks::ui;

TEST(UiFramework_FramePaintsInTreeOrderAndSkipsHiddenChildren) {
    SystemAllocator allocator;
    auto rootResult = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto childResult = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(rootResult && childResult);
    auto root = rootResult.Value();
    auto child = childResult.Value();
    root->SetColor({1, 0, 0, 1});
    child->SetColor({0, 1, 0, 1});
    REQUIRE(root->TryAddChild(child));

    REQUIRE(root->Arrange(Geometry::Root({100, 50}, {})));
    PaintList list(allocator);
    REQUIRE(root->Paint(list, 7));
    REQUIRE(list.Boxes().Size() == 2);
    CHECK(list.Boxes()[0].color.r == 1 && list.Boxes()[1].color.g == 1);
    CHECK(list.Boxes()[0].layer == 7 && list.Boxes()[1].layer == 7);

    child->SetVisibility(Visibility::Hidden);
    list.Clear();
    REQUIRE(root->Paint(list, 0));
    CHECK(list.Boxes().Size() == 1);
}

TEST(UiFramework_CachedPaintSkipsIdleAndUpdatesOnlyChangedVertices) {
    SystemAllocator allocator;
    auto root = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto child = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(root && child);
    REQUIRE(root.Value()->TryAddChild(child.Value()));
    Application application(allocator);
    application.SetRoot(root.Value());
    const Geometry viewport = Geometry::Root({80, 40}, {});
    auto frame = application.UpdatePaintCache(viewport, 1);
    REQUIRE(frame);
    const CacheRoot& cache = *frame.Value();
    DrawBatch batch(allocator);
    REQUIRE(batch.Update(cache));
    REQUIRE(batch.Vertices().Size() == 8);
    const uint64 initial = cache.Revision();
    const uint64 topology = batch.TopologyRevision();

    REQUIRE(application.UpdatePaintCache(viewport, 1));
    CHECK(cache.Revision() == initial);
    CHECK(cache.LastUpdate().paintCalls == 0);
    child.Value()->SetColor({0, 1, 0, 1});
    REQUIRE(application.UpdatePaintCache(viewport, 1));
    CHECK(cache.LastUpdate().paintCalls == 1);
    REQUIRE(batch.Update(cache));
    CHECK(batch.TopologyRevision() == topology);
    REQUIRE(batch.ChangedVertices().Size() == 1);
    CHECK(batch.ChangedVertices()[0].first == 4);
    CHECK(batch.ChangedVertices()[0].count == 4);
    CHECK(batch.Vertices()[4].color.r == 0);

    child.Value()->SetVolatile(true);
    REQUIRE(application.UpdatePaintCache(viewport, 1));
    const uint64 unchanged = cache.Revision();
    REQUIRE(application.UpdatePaintCache(viewport, 1));
    CHECK(cache.LastUpdate().paintCalls == 1);
    CHECK(cache.Revision() == unchanged);
    child.Value()->SetVisibility(Visibility::Collapsed);
    REQUIRE(application.UpdatePaintCache(viewport, 1));
    REQUIRE(batch.Update(cache));
    CHECK(batch.Vertices().Size() == 4);
    REQUIRE(application.UpdatePaintCache(viewport, 1));
    CHECK(cache.LastUpdate().paintCalls == 0);
}

TEST(UiFramework_DockTransferAndRestorePreserveOpenTabs) {
    SystemAllocator allocator;
    DockManager main(allocator);
    DockManager floating(allocator);
    constexpr StringView ids[] = {"Scene", "Inspector", "Assets"};
    for (StringView id : ids) {
        auto header = SharedPtr<Frame>::TryMake(allocator, allocator);
        auto panel = SharedPtr<Frame>::TryMake(allocator, allocator);
        REQUIRE(header && panel);
        REQUIRE(main.RegisterTab({id, header.Value(), panel.Value()}));
    }

    REQUIRE(main.OpenTab("Scene"));
    REQUIRE(main.Dock("Inspector", "Scene", DockSide::Right));
    REQUIRE(main.Dock("Assets", "Scene", DockSide::Center));
    Array<uint8> saved(allocator);
    REQUIRE(main.SaveLayout(1, saved));
    JsonDocument document(allocator);
    REQUIRE(
        document.Parse({reinterpret_cast<const char*>(saved.Data()), saved.Size()}).Succeeded());
    const auto split = document.Root().Find("root");
    REQUIRE(split.Find("type").IsString());
    CHECK(split.Find("type").GetString() == "horizontal");
    CHECK(split.Find("first").Find("activeTab").GetString() == "Assets");
    CHECK(split.Find("second").Find("activeTab").GetString() == "Inspector");
    REQUIRE(main.TransferTabTo("Inspector", floating));
    CHECK(floating.IsActive("Inspector"));
    CHECK(!main.IsOpen("Inspector"));
    REQUIRE(floating.TransferTabTo("Inspector", main, "Assets"));
    CHECK(main.IsActive("Inspector"));

    CHECK(!main.RestoreLayout(2, saved.View()));
    CHECK(main.IsActive("Inspector"));
    REQUIRE(main.RestoreLayout(1, saved.View()));
    CHECK(main.IsActive("Inspector"));
    CHECK(main.IsActive("Assets"));
    CHECK(!main.IsActive("Scene"));
    Array<uint8> restored(allocator);
    REQUIRE(main.SaveLayout(1, restored));
    REQUIRE(restored.Size() == saved.Size());
    for (usize i = 0; i < saved.Size(); ++i) {
        CHECK(saved[i] == restored[i]);
    }
}

TEST(UiFramework_DockJsonValidatesBeforeReplacingAndImportsLegacy) {
    SystemAllocator allocator;
    DockManager manager(allocator);
    auto header = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto panel = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(header && panel);
    REQUIRE(manager.RegisterTab({"A", header.Value(), panel.Value()}));
    constexpr uint8 legacy[] = {'D', 'O', 'C', 'K', 1, 0, 0, 0, 3, 0, 0, 0, 1,  0,
                                1,   0,   0,   0,   0, 0, 0, 0, 1, 0, 0, 0, 'A'};
    REQUIRE(manager.RestoreLegacyLayout(3, legacy));
    CHECK(manager.IsOpen("A"));

    const StringView invalid[] = {
        R"({"formatVersion":1,"layoutVersion":3})",
        R"({"formatVersion":1,"layoutVersion":3,"root":{"type":"horizontal","fraction":1}})",
        R"({"formatVersion":1,"layoutVersion":3,"root":{"type":"stack","tabs":["A","A"],"activeTab":"A"}})",
        R"({"formatVersion":1,"layoutVersion":3,"root":{"type":"stack","tabs":["A"],"activeTab":"B"}})",
        R"({"formatVersion":1,"layoutVersion":3,"root":{"type":"stack","tabs":["X"],"activeTab":"X"}})",
    };
    for (const auto input : invalid) {
        CHECK(!manager.RestoreLayout(
            3, {reinterpret_cast<const uint8*>(input.Data()), input.ByteCount()}));
        CHECK(manager.IsActive("A"));
    }

    constexpr StringView empty = R"({"formatVersion":1,"layoutVersion":3,"root":null})";
    REQUIRE(manager.RestoreLayout(
        3, {reinterpret_cast<const uint8*>(empty.Data()), empty.ByteCount()}));
    CHECK(!manager.IsOpen("A"));
}

#include <AssetRuntime/MemoryBulkSource.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/PackageWriter.h>
#include <AssetRuntime/Tests/AssetTestSupport.h>
#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <CurveAssetTools/CurveTableCook.h>
#include <CurveAssets/CurveAssetsModule.h>
#include <CurveAssets/CurveTableAsset.generated.h>
#include <CurveAssets/FloatCurveAsset.generated.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <string.h>

// 커브 표 쿠킹 시험이다(Docs/Design/CurveAssets 5.3절 — CA4): 쿠킹 패키지를 쓰고 다시
// 로드해(리플렉션 왕복 — 모드·Name 키 맵·커브 값) 중복 키 제거(Plain·Tangent, 키 3개 미만은
// 그대로·정확히 3개 경계)·Tangent 접선 재계산·쓰지 않는 맵 비우기와 경고를 보고, 에디터 객체가
// 그대로인지(성공·쓰기 실패·경고 할당 실패), 결정성, 타입 거부, 등록, 처리기 정체성을 본다.
using namespace rocks;

namespace curve_table_cook_tests_file {

namespace {

constexpr uint64 TablePackageId = 0x7200;
constexpr uint64 OtherPackageId = 0x7201;

struct CookOut {
    explicit CookOut(Allocator& allocator) noexcept : sink(allocator), warnings(allocator) {
    }

    MemoryPackageSink sink;
    AssetDiagnostic failure;
    Array<CookDiagnostic> warnings;
};

// 쓰기가 늘 실패하는 sink다(쓰기 실패 경로).
class FailingSink final : public PackageSink {
public:
    Result<void> Append(Span<const uint8>) noexcept override {
        return Result<void>::Failure(ErrorCode::IoError);
    }

    Result<void> WriteAt(uint64, Span<const uint8>) noexcept override {
        return Result<void>::Failure(ErrorCode::IoError);
    }
};

struct CookScene {
    CookScene() noexcept : bulk(env.allocator), profile(env.allocator), entry(env.allocator) {
        ready = env.Ready() && RegisterCurveAssetsTypes(env.modules).IsOk() &&
                static_cast<bool>(profile.id.TryAssign("Windows"));
        table = ready ? MakeAsset(TablePackageId, TypeIdOf<CurveTableAsset>(), tablePackage)
                      : ObjectHandle{};
        ready = ready && !table.IsNull();
    }

    ObjectHandle MakeAsset(uint64 id, TypeId type, ObjectHandle& package) noexcept {
        ObjectRegistry& registry = *env.source;
        auto made = NewPackage(registry, registry.OwnerAccess(), asset_test::FixedId(id),
                               PackageKind::Editor);
        if (!made) {
            return ObjectHandle{};
        }

        package = made.Value().Handle();
        roots[rootCount++] = Move(made.Value());
        return asset_test::Make<Asset>(registry, type, package, "");
    }

    Name Of(StringView text) noexcept {
        auto name = env.names.TryIntern(text);
        return name ? name.Value() : Name{};
    }

    CurveTableAsset* Table() noexcept {
        return asset_test::Pointer<CurveTableAsset>(*env.source, table);
    }

    CookContext Context(ObjectHandle package, uint64 id, CookOut& out) noexcept {
        entry.id = asset_test::FixedId(id);
        CookContext context;
        context.allocator = &env.allocator;
        context.registry = env.source.Get();
        context.access = env.source->OwnerAccess();
        context.package = package;
        context.entry = &entry;
        context.profile = &profile;
        context.cookerVersion = 1;
        context.bulkSource = &bulk;
        context.sink = &out.sink;
        context.failure = &out.failure;
        context.warnings = &out.warnings;
        return context;
    }

    // 표를 쿠킹하고 target에 로드한 표를 돌려준다(실패면 null).
    const CurveTableAsset* CookAndLoad(CookOut& out) noexcept {
        CurveTableCookProcessor processor;
        CookContext context = Context(tablePackage, TablePackageId, out);
        if (!processor.Cook(context)) {
            return nullptr;
        }

        Array<uint8> files[1] = {asset_test::Copy(env.allocator, out.sink.Bytes())};
        RootRef<Object> loaded[1];
        ObjectHandle handles[1];
        if (!asset_test::LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, loaded,
                                 handles)) {
            return nullptr;
        }

        loadedRoot = Move(loaded[0]);
        auto found = env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
        return found ? asset_test::Pointer<CurveTableAsset>(*env.target, found.Value()) : nullptr;
    }

    asset_test::AssetEnv env;
    MemoryBulkSource bulk;
    TargetProfile profile;
    AssetIndexEntry entry;
    RootRef<Package> roots[2];
    usize rootCount = 0;
    RootRef<Object> loadedRoot;
    ObjectHandle table{};
    ObjectHandle tablePackage{};
    bool ready = false;
};

} // namespace

// Plain 표: 키 3개 이상 행의 중복 키를 지우고(가운데 직선 위 키), 3개 미만은 그대로, 에디터 객체는
// 그대로다. 쓰지 않는 맵(tangentRows)은 쿠킹 데이터에서 비고 경고 setting = 버린 행 수다.
TEST(CurveTableCook_PlainRemovesRedundantKeysOnCopy) {
    CookScene scene;
    REQUIRE(scene.ready);
    CurveTableAsset* table = scene.Table();
    REQUIRE(table != nullptr);
    auto line = table->TryAddPlainRow(scene.Of("Line"));
    REQUIRE(line);
    for (const float time : {0.0f, 1.0f, 2.0f, 3.0f}) {
        REQUIRE(line.Value()->TryAddKey(time, time * 2.0f));
    }

    auto pair = table->TryAddPlainRow(scene.Of("Pair"));
    REQUIRE(pair);
    REQUIRE(pair.Value()->TryAddKey(0.0f, 1.0f));
    REQUIRE(pair.Value()->TryAddKey(1.0f, 1.0f));
    // 경로 쓰기가 남긴 쓰지 않는 맵의 행 둘.
    REQUIRE(
        table->tangentRows.InsertOrAssign(scene.Of("StrayA"), TangentCurve(scene.env.allocator)));
    REQUIRE(
        table->tangentRows.InsertOrAssign(scene.Of("StrayB"), TangentCurve(scene.env.allocator)));

    CookOut out(scene.env.allocator);
    const CurveTableAsset* cooked = scene.CookAndLoad(out);
    REQUIRE(cooked != nullptr);
    CHECK(cooked->mode == CurveTableMode::Plain && cooked->tangentRows.IsEmpty());
    const PlainCurve* cookedLine = cooked->FindPlainRow(scene.Of("Line"));
    const PlainCurve* cookedPair = cooked->FindPlainRow(scene.Of("Pair"));
    REQUIRE(cookedLine != nullptr && cookedPair != nullptr);
    CHECK(cookedLine->KeyCount() == 2);
    CHECK(cookedLine->Evaluate(1.5f) == 3.0f);
    // 키 2개는 같은 값이어도 그대로다(UE 조기 반환).
    CHECK(cookedPair->KeyCount() == 2);
    REQUIRE(out.warnings.Size() == 1);
    CHECK(out.warnings[0].code == AssetDiagnosticCode::CurveTableUnusedRows &&
          out.warnings[0].warning && out.warnings[0].setting == 2);

    // 에디터 객체는 그대로다.
    table = scene.Table();
    REQUIRE(table != nullptr);
    CHECK(table->FindPlainRow(scene.Of("Line"))->KeyCount() == 4);
    CHECK(table->tangentRows.Count() == 2);
}

// Tangent 표: 지운 뒤 자동 접선을 다시 계산한다(UE RemoveRedundantKeysInternal 끝). 키 1(직선 위
// Linear)이 지워지면 바로 뒤 Cubic Auto 키 2의 이웃이 바뀌어 접선이 달라진다. 쓰지 않는
// 맵(plainRows)은 비고 경고한다. 키 2개 Cubic 행(3개 미만)은 지우지도 접선을 다시 계산하지도
// 않는다. 정확히 3개는 한다.
TEST(CurveTableCook_TangentRecomputesTangents) {
    CookScene scene;
    REQUIRE(scene.ready);
    CurveTableAsset* table = scene.Table();
    REQUIRE(table != nullptr);
    auto addRow = [&](StringView name, std::initializer_list<float> times,
                      std::initializer_list<float> values,
                      std::initializer_list<CurveInterp> interps) noexcept -> TangentCurve* {
        auto row = table->TryAddTangentRow(scene.Of(name));
        if (!row) {
            return nullptr;
        }

        const float* time = times.begin();
        const float* value = values.begin();
        for (const CurveInterp interp : interps) {
            auto key = row.Value()->TryAddKey(*time++, *value++);
            if (!key || !row.Value()->TrySetKeyInterp(key.Value(), interp)) {
                return nullptr;
            }
        }

        return row.Value();
    };

    TangentCurve* hill =
        addRow("Hill", {0.0f, 1.0f, 2.0f, 3.0f}, {0.0f, 1.0f, 2.0f, 0.0f},
               {CurveInterp::Linear, CurveInterp::Linear, CurveInterp::Cubic, CurveInterp::Cubic});
    REQUIRE(hill != nullptr);
    hill->AutoSetTangents();
    const float editorTangent = hill->Keys()[2].leaveTangent;
    // 기준: 같은 커브를 지우고 접선을 다시 계산한 것.
    TangentCurve expected(scene.env.allocator);
    REQUIRE(expected.TryCopyFrom(*hill));
    REQUIRE(expected.TryRemoveRedundantKeys(0.0f));
    expected.AutoSetTangents();
    REQUIRE(expected.KeyCount() == 3);

    // 키 2개(지우지도 다시 계산하지도 않는다 — 손으로 정한 접선이 남는다).
    TangentCurve* pair =
        addRow("Pair", {0.0f, 1.0f}, {0.0f, 0.0f}, {CurveInterp::Cubic, CurveInterp::Cubic});
    REQUIRE(pair != nullptr);
    REQUIRE(pair->TrySetKeyTangents(0, 7.0f, 7.0f));
    // 정확히 키 3개: 직선 위 가운데 키를 지운다.
    TangentCurve* three = addRow("Three", {0.0f, 1.0f, 2.0f}, {0.0f, 1.0f, 2.0f},
                                 {CurveInterp::Linear, CurveInterp::Linear, CurveInterp::Linear});
    REQUIRE(three != nullptr);
    REQUIRE(table->plainRows.InsertOrAssign(scene.Of("Stray"), PlainCurve(scene.env.allocator)));

    CookOut out(scene.env.allocator);
    const CurveTableAsset* cooked = scene.CookAndLoad(out);
    REQUIRE(cooked != nullptr);
    CHECK(cooked->plainRows.IsEmpty());
    REQUIRE(out.warnings.Size() == 1);
    CHECK(out.warnings[0].code == AssetDiagnosticCode::CurveTableUnusedRows &&
          out.warnings[0].setting == 1);
    const TangentCurve* cookedHill = cooked->FindTangentRow(scene.Of("Hill"));
    REQUIRE(cookedHill != nullptr && cookedHill->KeyCount() == expected.KeyCount());
    for (usize index = 0; index < cookedHill->KeyCount(); ++index) {
        CHECK(cookedHill->Keys()[index].time == expected.Keys()[index].time);
        CHECK(cookedHill->Keys()[index].arriveTangent == expected.Keys()[index].arriveTangent);
        CHECK(cookedHill->Keys()[index].leaveTangent == expected.Keys()[index].leaveTangent);
    }

    // 접선이 실제로 다시 계산되었다(지운 키 옆 Cubic Auto 키 — 이제 index 1).
    CHECK(cookedHill->Keys()[1].leaveTangent != editorTangent);
    const TangentCurve* cookedPair = cooked->FindTangentRow(scene.Of("Pair"));
    REQUIRE(cookedPair != nullptr);
    CHECK(cookedPair->KeyCount() == 2 && cookedPair->Keys()[0].leaveTangent == 7.0f);
    CHECK(cooked->FindTangentRow(scene.Of("Three"))->KeyCount() == 2);
    CHECK(scene.Table()->FindTangentRow(scene.Of("Hill"))->KeyCount() == 4);
}

// 실패해도 에디터 객체는 원래 맵을 되찾는다: 쓰기 실패(IoError), 경고 push 할당 실패.
TEST(CurveTableCook_FailuresRestoreEditorRows) {
    CookScene scene;
    REQUIRE(scene.ready);
    CurveTableAsset* table = scene.Table();
    REQUIRE(table != nullptr);
    auto line = table->TryAddPlainRow(scene.Of("Line"));
    REQUIRE(line);
    for (const float time : {0.0f, 1.0f, 2.0f}) {
        REQUIRE(line.Value()->TryAddKey(time, time));
    }

    REQUIRE(
        table->tangentRows.InsertOrAssign(scene.Of("Stray"), TangentCurve(scene.env.allocator)));
    auto untouched = [&]() noexcept {
        const CurveTableAsset* value = scene.Table();
        return value != nullptr && value->FindPlainRow(scene.Of("Line")) != nullptr &&
               value->FindPlainRow(scene.Of("Line"))->KeyCount() == 3 &&
               value->tangentRows.Count() == 1;
    };

    CurveTableCookProcessor processor;
    {
        CookOut out(scene.env.allocator);
        FailingSink failing;
        CookContext context = scene.Context(scene.tablePackage, TablePackageId, out);
        context.sink = &failing;
        CHECK(processor.Cook(context).Error() == ErrorCode::IoError);
        CHECK(untouched());
    }

    {
        CookOut out(scene.env.allocator);
        test::NFailAllocator failingAllocator(scene.env.allocator);
        Array<CookDiagnostic> warnings(failingAllocator);
        CookContext context = scene.Context(scene.tablePackage, TablePackageId, out);
        context.warnings = &warnings;
        failingAllocator.FailOnAttempt(1);
        CHECK(processor.Cook(context).Error() == ErrorCode::OutOfMemory);
        failingAllocator.DisableFailure();
        CHECK(untouched() && out.sink.Bytes().Size() == 0);
    }
}

// 같은 입력을 두 번 쿠킹하면 같은 byte다.
TEST(CurveTableCook_Deterministic) {
    CookScene scene;
    REQUIRE(scene.ready);
    CurveTableAsset* table = scene.Table();
    REQUIRE(table != nullptr);
    auto row = table->TryAddPlainRow(scene.Of("A"));
    REQUIRE(row);
    for (const float time : {0.0f, 1.0f, 2.0f, 3.0f}) {
        REQUIRE(row.Value()->TryAddKey(time, time * time));
    }

    CurveTableCookProcessor processor;
    CookOut first(scene.env.allocator);
    CookOut second(scene.env.allocator);
    CookContext firstContext = scene.Context(scene.tablePackage, TablePackageId, first);
    REQUIRE(processor.Cook(firstContext));
    CookContext secondContext = scene.Context(scene.tablePackage, TablePackageId, second);
    REQUIRE(processor.Cook(secondContext));
    REQUIRE(first.sink.Bytes().Size() == second.sink.Bytes().Size());
    CHECK(memcmp(first.sink.Bytes().Data(), second.sink.Bytes().Data(),
                 first.sink.Bytes().Size()) == 0);
}

// Empty 표는 두 맵을 모두 비우고 경고한다. 다른 타입의 패키지는 TypeMismatch이고 에디터 객체는
// 그대로다. 등록·정체성.
TEST(CurveTableCook_EmptyTypeAndRegistration) {
    CookScene scene;
    REQUIRE(scene.ready);
    CurveTableAsset* table = scene.Table();
    REQUIRE(table != nullptr);
    REQUIRE(table->plainRows.InsertOrAssign(scene.Of("Hidden"), PlainCurve(scene.env.allocator)));
    {
        CookOut out(scene.env.allocator);
        const CurveTableAsset* cooked = scene.CookAndLoad(out);
        REQUIRE(cooked != nullptr);
        CHECK(cooked->mode == CurveTableMode::Empty && cooked->plainRows.IsEmpty());
        REQUIRE(out.warnings.Size() == 1);
        CHECK(out.warnings[0].setting == 1);
        CHECK(scene.Table()->plainRows.Count() == 1);
    }

    ObjectHandle otherPackage{};
    const ObjectHandle other =
        scene.MakeAsset(OtherPackageId, TypeIdOf<FloatCurveAsset>(), otherPackage);
    REQUIRE(!other.IsNull());
    CurveTableCookProcessor processor;
    CookOut out(scene.env.allocator);
    CookContext context = scene.Context(otherPackage, OtherPackageId, out);
    CHECK(processor.Cook(context).Error() == ErrorCode::TypeMismatch);
    CHECK(out.failure.code == AssetDiagnosticCode::TypeMismatch);

    DefaultCookProcessor fallback;
    CookProcessorTable processors(scene.env.allocator, scene.env.types, fallback);
    REQUIRE(RegisterCurveTableCookProcessor(&processor, processors));
    auto found = processors.Find(TypeIdOf<CurveTableAsset>());
    REQUIRE(found);
    CHECK(found.Value() == &processor);
    CHECK(RegisterCurveTableCookProcessor(nullptr, processors).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(processor.Identity().processor == CurveTableCookProcessorId.processor);
    CHECK(!(processor.Identity().processor == DefaultCookProcessorId.processor));
}

} // namespace curve_table_cook_tests_file

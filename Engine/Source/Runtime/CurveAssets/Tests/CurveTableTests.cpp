#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <CurveAssets/CurveAssetsModule.h>
#include <CurveAssets/CurveTableAsset.generated.h>
#include <CurveAssets/CurveTableAsset.h>
#include <CurveAssetsTestFixtures/CurveAssetsTestFixtures.module.generated.h>
#include <CurveAssetsTestFixtures/CurveSourceHolder.generated.h>
#include <CurveAssetsTestFixtures/CurveSourceHolder.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// 커브 표 시험이다(Docs/Design/CurveAssets 3.2·3.3절 — CA2): 모드 정하기·다른 모드 거부·같은 이름
// 대입 자리 유지·지우기·이름 바꾸기 판정과 순서·비우기, 쓰지 않는 맵 무시, 할당 실패 때 표 그대로,
// 행 참조.
using namespace rocks;
using rocks::curve_fixture::CurveSourceHolder;

namespace curve_table_tests_file {

namespace {

constexpr uint64 TestIdHigh = 0x43413241ull;

struct Names {
    Names() noexcept : pool(allocator) {
        ready = pool.Initialize().IsOk();
    }

    Name Of(StringView text) noexcept {
        auto name = pool.TryIntern(text);
        return name ? name.Value() : Name{};
    }

    SystemAllocator allocator;
    NamePool pool;
    bool ready = false;
};

// 표의 행 이름을 순서대로 모은다.
Array<Name> RowNames(const CurveTableAsset& table, Allocator& allocator) noexcept {
    Array<Name> names(allocator);
    table.ForEachRowName([&names](Name name) noexcept { (void)names.TryPushBack(name); });
    return names;
}

} // namespace

// 3.2.1·3.2.2·3.2.3절: 모드 정하기, 다른 모드 TypeMismatch, None 거부, 같은 이름 대입의 자리 유지,
// 조회의 모드 검사, 평가 NotFound.
TEST(CurveTable_ModesAndAdd) {
    Names names;
    REQUIRE(names.ready);
    const Name health = names.Of("Health");
    const Name armor = names.Of("Armor");
    CurveTableAsset table(names.allocator);
    CHECK(table.mode == CurveTableMode::Empty && table.RowCount() == 0);
    CHECK(table.TryEvaluateRow(health, 0.0f).Error() == ErrorCode::NotFound);

    auto added = table.TryAddPlainRow(health);
    REQUIRE(added);
    REQUIRE(added.Value()->TryAddKey(1.0f, 100.0f));
    REQUIRE(added.Value()->TryAddKey(2.0f, 200.0f));
    CHECK(table.mode == CurveTableMode::Plain);
    REQUIRE(table.TryAddPlainRow(armor));

    // 다른 모드·None.
    CHECK(table.TryAddTangentRow(names.Of("Speed")).Error() == ErrorCode::TypeMismatch);
    CHECK(table.TryAddPlainRow(Name{}).Error() == ErrorCode::InvalidArgument);
    CHECK(table.RowCount() == 2);

    // 조회는 모드를 본다.
    CHECK(table.FindPlainRow(health) != nullptr && table.FindTangentRow(health) == nullptr);
    CHECK(table.FindRow(health) == table.FindPlainRow(health));
    auto value = table.TryEvaluateRow(health, 1.5f);
    REQUIRE(value);
    CHECK(value.Value() == 150.0f);
    CHECK(table.TryEvaluateRow(Name{}, 0.0f).Error() == ErrorCode::NotFound);

    // 같은 이름 대입은 빈 커브로 바꾸고 자리를 지킨다.
    REQUIRE(table.TryAddPlainRow(health));
    CHECK(table.FindPlainRow(health)->KeyCount() == 0);
    Array<Name> order = RowNames(table, names.allocator);
    REQUIRE(order.Size() == 2);
    CHECK(order[0] == health && order[1] == armor);
}

// 3.2.3절: 지우기, 이름 바꾸기 판정 다섯·끝으로 옮김, 비우기 뒤 다른 모드.
TEST(CurveTable_RemoveRenameEmpty) {
    Names names;
    REQUIRE(names.ready);
    const Name a = names.Of("A");
    const Name b = names.Of("B");
    const Name c = names.Of("C");
    CurveTableAsset table(names.allocator);
    for (const Name row : {a, b, c}) {
        auto added = table.TryAddTangentRow(row);
        REQUIRE(added);
        REQUIRE(added.Value()->TryAddKey(0.0f, row == a ? 1.0f : (row == b ? 2.0f : 3.0f)));
    }

    // 같은 이름: 아무 일 없음.
    CHECK(table.TryRenameRow(a, a).IsOk());
    // 없는 원본: NotFound.
    CHECK(table.TryRenameRow(names.Of("Missing"), names.Of("Other")).Error() ==
          ErrorCode::NotFound);
    // None 대상: InvalidArgument.
    CHECK(table.TryRenameRow(a, Name{}).Error() == ErrorCode::InvalidArgument);
    // 있는 대상: 아무 일 없음(UE와 같다).
    CHECK(table.TryRenameRow(a, b).IsOk());
    CHECK(table.FindTangentRow(a) != nullptr && table.TryEvaluateRow(b, 0.0f).Value() == 2.0f);
    // 그 밖: 지우고 끝에 넣는다.
    const Name d = names.Of("D");
    REQUIRE(table.TryRenameRow(a, d));
    CHECK(table.FindTangentRow(a) == nullptr && table.TryEvaluateRow(d, 0.0f).Value() == 1.0f);
    Array<Name> order = RowNames(table, names.allocator);
    REQUIRE(order.Size() == 3);
    CHECK(order[0] == b && order[1] == c && order[2] == d);

    // 판정 순서: 원본이 없으면 대상이 None이든 있든 NotFound다(2단계가 3·4단계보다 먼저).
    CHECK(table.TryRenameRow(names.Of("Missing"), Name{}).Error() == ErrorCode::NotFound);
    CHECK(table.TryRenameRow(names.Of("Missing"), b).Error() == ErrorCode::NotFound);

    // 지운 뒤 다시 넣으면 끝이다.
    CHECK(table.RemoveRow(b));
    REQUIRE(table.TryAddTangentRow(b));
    order = RowNames(table, names.allocator);
    REQUIRE(order.Size() == 3);
    CHECK(order[0] == c && order[1] == d && order[2] == b);

    // 반대 방향: Tangent 표에 Plain 행·None 행.
    CHECK(table.TryAddPlainRow(names.Of("P")).Error() == ErrorCode::TypeMismatch);
    CHECK(table.TryAddTangentRow(Name{}).Error() == ErrorCode::InvalidArgument);

    CHECK(table.RemoveRow(c) && !table.RemoveRow(c));
    CHECK(table.RowCount() == 2 && table.mode == CurveTableMode::Tangent);
    table.EmptyTable();
    CHECK(table.mode == CurveTableMode::Empty && table.RowCount() == 0);
    CHECK(table.plainRows.IsEmpty() && table.tangentRows.IsEmpty());
    REQUIRE(table.TryAddPlainRow(a));
    CHECK(table.mode == CurveTableMode::Plain);
    // Plain 모드 이름 바꾸기.
    REQUIRE(table.TryRenameRow(a, b));
    CHECK(table.FindPlainRow(b) != nullptr && table.FindPlainRow(a) == nullptr);
}

// 3.2.1절: 모드가 쓰지 않는 맵의 행(경로 쓰기가 쓴 것)은 없는 것으로 치고, 편집이 정리한다.
TEST(CurveTable_UnusedMapIgnored) {
    Names names;
    REQUIRE(names.ready);
    const Name stray = names.Of("Stray");
    const Name row = names.Of("Row");
    CurveTableAsset table(names.allocator);
    // Empty 표인데 맵에 행이 있다.
    REQUIRE(table.plainRows.InsertOrAssign(stray, PlainCurve(names.allocator)));
    CHECK(table.FindRow(stray) == nullptr && table.RowCount() == 0);
    CHECK(table.TryEvaluateRow(stray, 0.0f).Error() == ErrorCode::NotFound);
    // 행을 더하면 보이지 않던 행은 비워진다.
    REQUIRE(table.TryAddPlainRow(row));
    CHECK(table.RowCount() == 1 && table.FindPlainRow(stray) == nullptr);

    // Plain 표인데 tangentRows에 행이 있다.
    REQUIRE(table.tangentRows.InsertOrAssign(stray, TangentCurve(names.allocator)));
    CHECK(table.FindTangentRow(stray) == nullptr && table.FindRow(stray) == nullptr);
    CHECK(table.RowCount() == 1);
    Array<Name> order = RowNames(table, names.allocator);
    CHECK(order.Size() == 1 && order[0] == row);
    // 같은 모드 행을 더하면 반대쪽 맵을 비운다.
    REQUIRE(table.TryAddPlainRow(names.Of("Other")));
    CHECK(table.tangentRows.IsEmpty());
    // 지우기·이름 바꾸기도 반대쪽을 비운다. 숨은 행은 바꿀 수 없다(NotFound).
    REQUIRE(table.tangentRows.InsertOrAssign(stray, TangentCurve(names.allocator)));
    CHECK(table.TryRenameRow(stray, names.Of("Visible")).Error() == ErrorCode::NotFound);
    CHECK(table.tangentRows.IsEmpty());
    REQUIRE(table.tangentRows.InsertOrAssign(stray, TangentCurve(names.allocator)));
    CHECK(!table.RemoveRow(stray) && table.tangentRows.IsEmpty());

    // Empty 표의 숨은 행도 바꿀 수 없다.
    table.EmptyTable();
    REQUIRE(table.plainRows.InsertOrAssign(stray, PlainCurve(names.allocator)));
    CHECK(table.TryRenameRow(stray, row).Error() == ErrorCode::NotFound);
    CHECK(table.plainRows.IsEmpty() && table.mode == CurveTableMode::Empty);
}

// 할당 실패: 맵이 커져야 하는 행 추가·이름 바꾸기가 실패하면 표는 그대로다. 행을 하나씩 더하며 첫
// 할당을 실패시켜 성장 지점을 찾는다(성장하지 않는 삽입은 할당하지 않는다).
TEST(CurveTable_AllocationFailureKeepsTable) {
    Names names;
    REQUIRE(names.ready);
    test::NFailAllocator failing(names.allocator);
    CurveTableAsset table(failing);
    const Name first = names.Of("Row0");
    auto seeded = table.TryAddPlainRow(first);
    REQUIRE(seeded);
    REQUIRE(seeded.Value()->TryAddKey(0.0f, 7.0f));
    usize rows = 1;
    bool addFailed = false;
    // 성장 지점을 찾을 때 더해 볼 행 수 상한과 행 이름 버퍼 크기다.
    constexpr usize MaxProbeRows = 256;
    constexpr usize RowNameBytes = 16;
    char text[RowNameBytes] = {};
    while (rows < MaxProbeRows && !addFailed) {
        (void)snprintf(text, sizeof(text), "Row%zu", rows);
        const Name next = names.Of(text);
        failing.FailOnAttempt(1);
        auto added = table.TryAddPlainRow(next);
        failing.DisableFailure();
        if (!added) {
            addFailed = true;
            CHECK(added.Error() == ErrorCode::OutOfMemory);
            CHECK(table.RowCount() == rows && table.FindPlainRow(next) == nullptr);
            CHECK(table.TryEvaluateRow(first, 0.0f).Value() == 7.0f);
            // 지금 표는 가득 차 있다: 새 이름으로 바꾸기는 성장이 필요해 실패하고 표는 그대로다.
            failing.FailOnAttempt(1);
            auto renamed = table.TryRenameRow(first, next);
            failing.DisableFailure();
            CHECK(renamed.Error() == ErrorCode::OutOfMemory);
            CHECK(table.FindPlainRow(first) != nullptr && table.FindPlainRow(next) == nullptr);
            CHECK(table.TryEvaluateRow(first, 0.0f).Value() == 7.0f && table.RowCount() == rows);
            continue;
        }

        ++rows;
    }

    CHECK(addFailed);
}

// 3.3절: 행 참조 — null 표·없는 행 NotFound, 성공, 파괴 요청된 표 Destroyed, IsNull.
TEST(CurveTableRowRef_Evaluate) {
    Names names;
    REQUIRE(names.ready);
    TypeRegistry types(names.allocator);
    ModuleRegistration modules(types, names.allocator);
    REQUIRE(RegisterCurveAssetsTypes(modules));
    REQUIRE(generated::CurveAssetsTestFixtures::RegisterModuleTypes(modules));
    ObjectRegistry objects(names.allocator, types, {TestIdHigh, 1});
    const auto access = objects.OwnerAccess();
    auto holderMade = objects.NewObject<CurveSourceHolder>(access, TypeIdOf<CurveSourceHolder>());
    auto tableMade = objects.NewObject<CurveTableAsset>(access, TypeIdOf<CurveTableAsset>());
    REQUIRE(holderMade && tableMade);
    const ObjectHandle holder = holderMade.Value().Handle();
    const ObjectHandle tableHandle = tableMade.Value().Handle();
    const Name damage = names.Of("Damage");
    {
        auto writable = objects.ResolveForWrite<CurveTableAsset>(access, tableHandle);
        REQUIRE(writable);
        auto pointer = writable.Value().Get();
        REQUIRE(pointer);
        auto row = pointer.Value()->TryAddTangentRow(damage);
        REQUIRE(row);
        REQUIRE(row.Value()->TryAddKey(1.0f, 10.0f));
        REQUIRE(row.Value()->TryAddKey(3.0f, 30.0f));
    }

    auto evaluate = [&](float time) noexcept {
        auto borrowed = objects.Resolve<CurveSourceHolder>(access, holder);
        auto pointer = borrowed.Value().Get();
        return pointer.Value()->tableRow.TryEvaluate(objects, access, time);
    };

    auto isNull = [&]() noexcept {
        auto borrowed = objects.Resolve<CurveSourceHolder>(access, holder);
        auto pointer = borrowed.Value().Get();
        return pointer.Value()->tableRow.IsNull();
    };

    CHECK(isNull());
    CHECK(evaluate(2.0f).Error() == ErrorCode::NotFound);
    // null 표 + None이 아닌 행: IsNull 거짓, 평가 NotFound.
    {
        auto writable = objects.ResolveForWrite<CurveSourceHolder>(access, holder);
        REQUIRE(writable);
        auto pointer = writable.Value().Get();
        REQUIRE(pointer);
        pointer.Value()->tableRow.row = damage;
    }

    CHECK(!isNull());
    CHECK(evaluate(2.0f).Error() == ErrorCode::NotFound);
    {
        auto writable = objects.ResolveForWrite<CurveSourceHolder>(access, holder);
        REQUIRE(writable);
        auto pointer = writable.Value().Get();
        REQUIRE(pointer);
        REQUIRE(objects.SetReference(access, holder, pointer.Value()->tableRow.table, tableHandle));
        // 없는 행(None이 아닌 이름): 평가는 NotFound.
        pointer.Value()->tableRow.row = names.Of("Missing");
        CHECK(!pointer.Value()->tableRow.IsNull());
    }

    CHECK(evaluate(2.0f).Error() == ErrorCode::NotFound);
    {
        auto writable = objects.ResolveForWrite<CurveSourceHolder>(access, holder);
        REQUIRE(writable);
        auto pointer = writable.Value().Get();
        REQUIRE(pointer);
        pointer.Value()->tableRow.row = damage;
    }

    auto value = evaluate(2.0f);
    REQUIRE(value);
    CHECK(value.Value() > 10.0f && value.Value() < 30.0f);
    REQUIRE(objects.RequestDestroy(access, tableHandle));
    CHECK(evaluate(2.0f).Error() == ErrorCode::Destroyed);
}

} // namespace curve_table_tests_file

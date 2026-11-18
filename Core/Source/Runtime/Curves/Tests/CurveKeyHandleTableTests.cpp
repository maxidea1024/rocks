#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <Curves/CurveKeyHandleTable.h>
#include <Curves/Private/CurveKeyHandleTableTestAccess.h>
#include <Platform/Fatal.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 커브 키 핸들 표 시험이다(Docs/Design/CurveData.ko.md 8절, D3 — CV7): 다섯 커브 형식에서 무작위
// 편집 순서(더하기·UpdateOrAdd·지우기·시간 옮기기·값 바꾸기·선택 키 이동·필드 편집)마다 모든 핸들이
// 같은 키를 가리키는지(키 값에 고유 번호를 두어 본다), 표를 거치지 않은 수정의 감지(StaleHandle)와
// TryRebuild, 발급 카운터 Overflow, 할당 실패 원자성(주입 단언), 복사·이동.
using namespace rocks;

namespace {

template <class T>
ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 커브 형식마다 "고유 번호 → 값"과 "키 → 번호"다.
struct TangentTraits {
    using Curve = TangentCurve;
    static float Value(int32 id) noexcept {
        return static_cast<float>(id);
    }
    static int32 Id(const TangentCurveKey& key) noexcept {
        return static_cast<int32>(key.value);
    }
};

struct PlainTraits {
    using Curve = PlainCurve;
    static float Value(int32 id) noexcept {
        return static_cast<float>(id);
    }
    static int32 Id(const PlainCurveKey& key) noexcept {
        return static_cast<int32>(key.value);
    }
};

struct IntegerTraits {
    using Curve = IntegerStepCurve;
    static int32 Value(int32 id) noexcept {
        return id;
    }
    static int32 Id(const IntegerStepKey& key) noexcept {
        return key.value;
    }
};

// String 값은 "k<번호>"다(번호 0–999).
struct StringTraits {
    using Curve = StringStepCurve;
    struct Text {
        char bytes[8] = {};
        operator StringView() const noexcept {
            return StringView(bytes);
        }
    };
    static Text Value(int32 id) noexcept {
        Text text;
        text.bytes[0] = 'k';
        text.bytes[1] = static_cast<char>('0' + (id / 100) % 10);
        text.bytes[2] = static_cast<char>('0' + (id / 10) % 10);
        text.bytes[3] = static_cast<char>('0' + id % 10);
        return text;
    }
    static int32 Id(const StringStepKey& key) noexcept {
        const StringView view = key.value.View();
        return (view.Data()[1] - '0') * 100 + (view.Data()[2] - '0') * 10 + (view.Data()[3] - '0');
    }
};

// Name 값은 번호 이름 "Key_<번호>"다(번호가 Name 번호 칸).
NamePool* CurrentPool = nullptr;

struct NameTraits {
    using Curve = NameStepCurve;
    static Name Value(int32 id) noexcept {
        char text[16] = "Key_";
        usize length = 4;
        char digits[8];
        usize count = 0;
        int32 rest = id;
        do {
            digits[count++] = static_cast<char>('0' + rest % 10);
            rest /= 10;
        } while (rest != 0);
        while (count > 0) {
            text[length++] = digits[--count];
        }

        auto interned = CurrentPool->TryIntern(StringView(text, length));
        if (!interned) {
            platform::Fatal("test name intern failed");
        }

        return interned.Value();
    }
    static int32 Id(const NameStepKey& key) noexcept {
        return static_cast<int32>(key.value.GetNumber());
    }
};

// 모델: 살아 있는 핸들과 그 키의 번호다.
struct Entry {
    CurveKeyHandle handle;
    int32 id = 0;
};

// 표가 모델과 맞는지다: 핸들마다 그 키의 번호가 같고, 핸들 수가 키 수와 같다.
template <class Traits>
bool Consistent(CurveKeyHandleTable<typename Traits::Curve>& table,
                const typename Traits::Curve& curve, const Array<Entry>& model) noexcept {
    if (table.Count() != curve.KeyCount() || model.Size() != curve.KeyCount()) {
        return false;
    }

    for (const Entry& entry : model.View()) {
        auto index = table.TryFindIndex(curve, entry.handle);
        if (!index || Traits::Id(curve.Keys()[index.Value()]) != entry.id) {
            return false;
        }

        auto back = table.TryGetHandle(curve, index.Value());
        if (!back || !(back.Value() == entry.handle)) {
            return false;
        }
    }

    return true;
}

usize FindEntry(const Array<Entry>& model, CurveKeyHandle handle) noexcept {
    for (usize index = 0; index < model.Size(); ++index) {
        if (model[index].handle == handle) {
            return index;
        }
    }

    return IndexNone;
}

// 무작위 편집 순서마다 핸들 대응이 모델과 맞는지 본다(curves개 커브 × steps단계).
template <class Traits, class EditFields>
int32 RunSequence(Allocator& allocator, uint64 seed, int32 curves, int32 steps,
                  EditFields&& editFields) noexcept {
    using Curve = typename Traits::Curve;
    test::MathRandom random(seed);
    int32 mismatches = 0;
    int32 nextId = 1;
    for (int32 curveIndex = 0; curveIndex < curves; ++curveIndex) {
        uint32 lastHandle = 0; // 표마다 1부터
        Curve curve(allocator);
        CurveKeyHandleTable<Curve> table(allocator);
        Array<Entry> model(allocator);
        for (int32 step = 0; step < steps; ++step) {
            const usize count = curve.KeyCount();
            const uint64 operation = count == 0 ? 0 : random.Next() % 7;
            const int32 id = nextId++ % 1000;
            if (operation == 0 || operation == 1) {
                const float time = count > 0 && random.Next() % 3 == 0
                                       ? curve.Keys()[random.Next() % count].time
                                       : static_cast<float>(random.Range(-10.0, 10.0));
                if (operation == 0) {
                    auto handle = table.TryAddKey(curve, time, Traits::Value(id));
                    if (!handle || !(handle.Value().value > lastHandle)) {
                        ++mismatches;
                        continue;
                    }

                    lastHandle = handle.Value().value;
                    static_cast<void>(model.TryPushBack(Entry{handle.Value(), id}));
                } else {
                    auto handle = table.TryUpdateOrAddKey(curve, time, Traits::Value(id));
                    if (!handle) {
                        ++mismatches;
                        continue;
                    }

                    const usize existing = FindEntry(model, handle.Value());
                    if (existing != IndexNone) {
                        model[existing].id = id;
                    } else {
                        lastHandle = handle.Value().value;
                        static_cast<void>(model.TryPushBack(Entry{handle.Value(), id}));
                    }
                }
            } else if (operation == 2) {
                const usize pick = random.Next() % model.Size();
                mismatches += table.TryRemoveKey(curve, model[pick].handle) ? 0 : 1;
                static_cast<void>(model.TryRemoveAt(pick));
            } else if (operation == 3) {
                const usize pick = random.Next() % model.Size();
                const float time = static_cast<float>(random.Range(-10.0, 10.0));
                mismatches += table.TrySetKeyTime(curve, model[pick].handle, time) ? 0 : 1;
            } else if (operation == 4) {
                const usize pick = random.Next() % model.Size();
                mismatches +=
                    table.TrySetKeyValue(curve, model[pick].handle, Traits::Value(id)) ? 0 : 1;
                model[pick].id = id;
            } else if (operation == 5) {
                CurveKeyHandle picked[4];
                usize pickedCount = 0;
                for (usize index = 0; index < model.Size() && pickedCount < 4; ++index) {
                    if (random.Next() % 3 == 0) {
                        picked[pickedCount++] = model[index].handle;
                    }
                }

                const float delta = static_cast<float>(random.Range(-5.0, 5.0));
                mismatches += table.TryShiftKeyTimes(
                                  curve, Span<const CurveKeyHandle>(picked, pickedCount), delta)
                                  ? 0
                                  : 1;
            } else {
                mismatches += editFields(table, curve, random) ? 0 : 1;
            }

            mismatches += Consistent<Traits>(table, curve, model) && !table.IsStale() ? 0 : 1;
        }
    }

    return mismatches;
}

// 필드 편집이 없는 커브는 아무것도 하지 않는다.
template <class Table, class Curve>
bool NoFieldEdit(Table&, Curve&, test::MathRandom&) noexcept {
    return true;
}

} // namespace

// 다섯 커브 형식에서 무작위 편집 순서(각 60커브 × 40단계)마다 모든 핸들이 같은 키를 가리키고, 새
// 핸들은 단조 증가한다. TangentCurve는 표를 거친 필드 편집(보간·접선 방식, AutoSetTangents)도
// 섞는다.
TEST(CurveKeyHandleTable_HandlesFollowKeysThroughEdits) {
    SystemAllocator allocator;
    CHECK(RunSequence<TangentTraits>(
              allocator, 0x4A1, 60, 40,
              [](CurveKeyHandleTable<TangentCurve>& table, TangentCurve& curve,
                 test::MathRandom& random) noexcept {
                  const usize index = random.Next() % curve.KeyCount();
                  const auto interp = static_cast<CurveInterp>(random.Next() % 3);
                  return table
                      .TryEditKeys(curve,
                                   [&](TangentCurve& edited) noexcept {
                                       if (auto set = edited.TrySetKeyInterp(index, interp); !set) {
                                           return set;
                                       }

                                       edited.AutoSetTangents();
                                       return Result<void>::Success();
                                   })
                      .IsOk();
              }) == 0);
    CHECK(RunSequence<PlainTraits>(allocator, 0x4A2, 60, 40,
                                   NoFieldEdit<CurveKeyHandleTable<PlainCurve>, PlainCurve>) == 0);
    CHECK(RunSequence<IntegerTraits>(
              allocator, 0x4A3, 60, 40,
              NoFieldEdit<CurveKeyHandleTable<IntegerStepCurve>, IntegerStepCurve>) == 0);
    CHECK(RunSequence<StringTraits>(
              allocator, 0x4A4, 60, 40,
              NoFieldEdit<CurveKeyHandleTable<StringStepCurve>, StringStepCurve>) == 0);
    NamePool pool(allocator);
    REQUIRE(pool.Initialize());
    CurrentPool = &pool;
    CHECK(RunSequence<NameTraits>(allocator, 0x4A5, 60, 40,
                                  NoFieldEdit<CurveKeyHandleTable<NameStepCurve>, NameStepCurve>) ==
          0);
    CurrentPool = nullptr;
    CHECK(pool.Shutdown());
}

// 기본 규칙: 핸들은 1부터, 0은 무효(NotFound), 지운 키의 핸들은 NotFound, 범위 밖 index는
// InvalidArgument. 필드 편집이 키 시간을 바꾸면 표는 무효(StaleHandle)다.
TEST(CurveKeyHandleTable_BasicRules) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    CurveKeyHandleTable<TangentCurve> table(allocator);
    auto first = table.TryAddKey(curve, 1.0f, 10.0f);
    REQUIRE(first);
    CHECK(first.Value().value == 1);
    auto second = table.TryAddKey(curve, 1.0f, 20.0f, false); // 같은 시간 키 앞
    REQUIRE(second);
    CHECK(second.Value().value == 2);
    auto index = table.TryFindIndex(curve, second.Value());
    REQUIRE(index);
    CHECK(index.Value() == 0);
    CHECK(ErrorOf(table.TryFindIndex(curve, CurveKeyHandle::Invalid())) == ErrorCode::NotFound);
    CHECK(ErrorOf(table.TryGetHandle(curve, 2)) == ErrorCode::InvalidArgument);
    REQUIRE(table.TryRemoveKey(curve, first.Value()));
    CHECK(ErrorOf(table.TryFindIndex(curve, first.Value())) == ErrorCode::NotFound);
    CHECK(ErrorOf(table.TryRemoveKey(curve, first.Value())) == ErrorCode::NotFound);
    auto updated = table.TryUpdateOrAddKey(curve, 1.00005f, 30.0f);
    REQUIRE(updated);
    CHECK(updated.Value() == second.Value() && curve.Keys()[0].value == 30.0f);

    CHECK(ErrorOf(table.TryEditKeys(curve, [](TangentCurve& edited) noexcept {
              auto moved = edited.TrySetKeyTime(0, 5.0f);
              return moved ? Result<void>::Success() : Result<void>::Failure(moved.Error());
          })) == ErrorCode::StaleHandle);
    CHECK(table.IsStale());
}

// 표를 거치지 않은 수정(값·시간·키 수·접선 하나라도)은 다음 호출에서 StaleHandle이고 그 뒤 모든
// 호출이 StaleHandle이다. TryRebuild는 새 핸들(옛 값보다 크다)을 발급해 다시 쓸 수 있게 하고 옛
// 핸들은 NotFound다. 새로 만든 표는 키가 있는 커브에 맞지 않아(빈 사본) TryRebuild 전까지
// StaleHandle이다.
TEST(CurveKeyHandleTable_DetectsOutsideEditsAndRebuilds) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    REQUIRE(curve.TryAddKey(0.0f, 1.0f));
    REQUIRE(curve.TryAddKey(1.0f, 2.0f));
    CurveKeyHandleTable<TangentCurve> table(allocator);
    CHECK(ErrorOf(table.TryGetHandle(curve, 0)) == ErrorCode::StaleHandle);
    REQUIRE(table.TryRebuild(curve));
    CHECK(!table.IsStale() && table.Count() == 2);
    auto old = table.TryGetHandle(curve, 1);
    REQUIRE(old);

    using Outside = Result<void> (*)(TangentCurve&);
    const Outside outside[] = {
        [](TangentCurve& edited) noexcept { return edited.TrySetKeyValue(0, 9.0f); },
        [](TangentCurve& edited) noexcept {
            auto moved = edited.TrySetKeyTime(0, 0.5f);
            return moved ? Result<void>::Success() : Result<void>::Failure(moved.Error());
        },
        [](TangentCurve& edited) noexcept {
            auto added = edited.TryAddKey(3.0f, 0.0f);
            return added ? Result<void>::Success() : Result<void>::Failure(added.Error());
        },
        [](TangentCurve& edited) noexcept { return edited.TrySetKeyTangents(1, 0.0f, -0.0f); },
        [](TangentCurve& edited) noexcept { return edited.TrySetKeyInterp(1, CurveInterp::Cubic); },
    };
    for (const Outside edit : outside) {
        REQUIRE(table.TryRebuild(curve));
        auto handle = table.TryGetHandle(curve, 0);
        REQUIRE(handle);
        REQUIRE(edit(curve));
        CHECK(ErrorOf(table.TryFindIndex(curve, handle.Value())) == ErrorCode::StaleHandle);
        CHECK(table.IsStale());
        CHECK(ErrorOf(table.TryAddKey(curve, 7.0f, 1.0f)) == ErrorCode::StaleHandle);
        CHECK(ErrorOf(table.TryRemoveKey(curve, handle.Value())) == ErrorCode::StaleHandle);
        REQUIRE(table.TryRebuild(curve));
        auto fresh = table.TryGetHandle(curve, 0);
        REQUIRE(fresh);
        CHECK(fresh.Value().value > handle.Value().value);
        CHECK(ErrorOf(table.TryFindIndex(curve, handle.Value())) == ErrorCode::NotFound);
    }

    CHECK(ErrorOf(table.TryFindIndex(curve, old.Value())) == ErrorCode::NotFound);
}

// 발급 카운터가 uint32 끝이면 새 핸들 발급(TryAddKey·키를 더하는 UpdateOrAdd·TryRebuild)이
// Overflow이고 커브와 표가 그대로다. 기존 키 값만 바꾸는 UpdateOrAdd는 된다.
TEST(CurveKeyHandleTable_HandleOverflow) {
    SystemAllocator allocator;
    IntegerStepCurve curve(allocator);
    CurveKeyHandleTable<IntegerStepCurve> table(allocator);
    curves_detail::CurveKeyHandleTableTestAccess::SetLastIssued(table,
                                                                NumericLimits<uint32>::Max() - 1);
    auto last = table.TryAddKey(curve, 1.0f, 5);
    REQUIRE(last);
    CHECK(last.Value().value == NumericLimits<uint32>::Max());
    CHECK(ErrorOf(table.TryAddKey(curve, 2.0f, 6)) == ErrorCode::Overflow);
    CHECK(ErrorOf(table.TryUpdateOrAddKey(curve, 2.0f, 6)) == ErrorCode::Overflow);
    CHECK(curve.KeyCount() == 1 && table.Count() == 1);
    auto updated = table.TryUpdateOrAddKey(curve, 1.0f, 7);
    REQUIRE(updated);
    CHECK(updated.Value() == last.Value() && curve.Keys()[0].value == 7);
    CHECK(ErrorOf(table.TryRebuild(curve)) == ErrorCode::Overflow);
    auto index = table.TryFindIndex(curve, last.Value());
    REQUIRE(index);
    CHECK(index.Value() == 0);
}

// 할당하는 표 연산은 시도마다 실패를 주입하면 OutOfMemory이고 커브와 표가 그대로다(옛 핸들이 같은
// 키를 가리키고 표가 무효가 아니다). 실패가 실제로 주입됐는지도 단언한다.
TEST(CurveKeyHandleTable_AllocationFailureIsAtomic) {
    SystemAllocator system;
    using Table = CurveKeyHandleTable<StringStepCurve>;
    using Operation = Result<void> (*)(Table&, StringStepCurve&, CurveKeyHandle);
    const Operation operations[] = {
        [](Table& table, StringStepCurve& curve, CurveKeyHandle) noexcept {
            auto added = table.TryAddKey(curve, 0.5f, "a value long enough to allocate a buffer");
            return added ? Result<void>::Success() : Result<void>::Failure(added.Error());
        },
        [](Table& table, StringStepCurve& curve, CurveKeyHandle) noexcept {
            auto updated =
                table.TryUpdateOrAddKey(curve, 1.0f, "a replacement long enough to allocate");
            return updated ? Result<void>::Success() : Result<void>::Failure(updated.Error());
        },
        [](Table& table, StringStepCurve& curve, CurveKeyHandle handle) noexcept {
            return table.TrySetKeyValue(curve, handle,
                                        "another replacement long enough to allocate");
        },
        [](Table& table, StringStepCurve& curve, CurveKeyHandle handle) noexcept {
            return table.TryShiftKeyTimes(curve, Span<const CurveKeyHandle>(&handle, 1), 5.0f);
        },
        [](Table& table, StringStepCurve& curve, CurveKeyHandle) noexcept {
            return table.TryRebuild(curve);
        },
    };
    for (const Operation operation : operations) {
        bool injected = false;
        for (uint64 attempt = 1; attempt <= 8; ++attempt) {
            test::NFailAllocator failing(system);
            StringStepCurve curve(failing);
            Table table(failing);
            CurveKeyHandle handles[2];
            for (usize index = 0; index < 2; ++index) {
                auto added = table.TryAddKey(curve, static_cast<float>(index),
                                             index == 0 ? "first key value with some bytes"
                                                        : "second key value with some bytes");
                REQUIRE(added);
                handles[index] = added.Value();
            }

            failing.FailOnAttempt(attempt); // 시도 수는 여기서 0부터 센다
            const Result<void> result = operation(table, curve, handles[0]);
            failing.DisableFailure();
            if (!result) {
                injected = true;
                CHECK(result.Error() == ErrorCode::OutOfMemory);
                REQUIRE(curve.KeyCount() == 2);
                CHECK(curve.Keys()[0].value.View() ==
                      StringView("first key value with some bytes"));
                CHECK(curve.Keys()[1].value.View() ==
                      StringView("second key value with some bytes"));
                CHECK(!table.IsStale());
                for (usize index = 0; index < 2; ++index) {
                    auto found = table.TryFindIndex(curve, handles[index]);
                    CHECK(found && found.Value() == index);
                }
            }
        }

        CHECK(injected);
    }
}

// 복사(TryCopyFrom)는 핸들·사본·발급 카운터를 그대로 옮긴다: 복사한 커브와 복사한 표로 같은 핸들이
// 같은 키를 가리키고, 그 뒤 새 핸들 번호도 같다. 이동도 같다. 무효 표의 복사는 무효다.
TEST(CurveKeyHandleTable_CopyAndMove) {
    SystemAllocator allocator;
    StringStepCurve curve(allocator);
    CurveKeyHandleTable<StringStepCurve> table(allocator);
    auto first = table.TryAddKey(curve, 0.0f, "zero");
    auto second = table.TryAddKey(curve, 1.0f, "one");
    REQUIRE(first && second);
    StringStepCurve curveCopy(allocator);
    REQUIRE(curveCopy.TryCopyFrom(curve));
    CurveKeyHandleTable<StringStepCurve> copy(allocator);
    REQUIRE(copy.TryCopyFrom(table));
    auto index = copy.TryFindIndex(curveCopy, second.Value());
    REQUIRE(index);
    CHECK(index.Value() == 1);
    auto nextOriginal = table.TryAddKey(curve, 2.0f, "two");
    auto nextCopy = copy.TryAddKey(curveCopy, 2.0f, "two");
    REQUIRE(nextOriginal && nextCopy);
    CHECK(nextOriginal.Value() == nextCopy.Value());

    CurveKeyHandleTable<StringStepCurve> moved(Move(copy));
    auto movedIndex = moved.TryFindIndex(curveCopy, first.Value());
    REQUIRE(movedIndex);
    CHECK(movedIndex.Value() == 0);

    // 복사가 할당에 실패하면 대상 표가 그대로다(주입 단언).
    bool injected = false;
    for (uint64 attempt = 1; attempt <= 6; ++attempt) {
        test::NFailAllocator failing(allocator);
        CurveKeyHandleTable<StringStepCurve> target(failing);
        failing.FailOnAttempt(attempt); // 시도 수는 여기서 0부터 센다
        const Result<void> copied = target.TryCopyFrom(table);
        failing.DisableFailure();
        if (!copied) {
            injected = true;
            CHECK(copied.Error() == ErrorCode::OutOfMemory && target.Count() == 0 &&
                  !target.IsStale());
        }
    }

    CHECK(injected);

    REQUIRE(curve.TrySetKeyValue(0, "outside"));
    CHECK(ErrorOf(table.TryGetHandle(curve, 0)) == ErrorCode::StaleHandle);
    CurveKeyHandleTable<StringStepCurve> staleCopy(allocator);
    REQUIRE(staleCopy.TryCopyFrom(table));
    CHECK(staleCopy.IsStale());
}

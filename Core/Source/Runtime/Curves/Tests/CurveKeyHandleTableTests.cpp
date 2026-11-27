#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <Curves/CurveKeyHandleTable.h>
#include <Curves/Private/CurveKeyHandleTableTestAccess.h>
#include <Platform/Fatal.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 커브 키 핸들 표 시험이다(Docs/Design/CurveData/README.ko.md 8절, D3 — CV7): 다섯 커브 형식에서
// 무작위 편집 순서(더하기·UpdateOrAdd·지우기·시간 옮기기·값 바꾸기·선택 키 이동·필드 편집)마다 모든
// 핸들이 같은 키를 가리키는지(키 값에 고유 번호를 두어 본다), 표를 거치지 않은 수정의
// 감지(StaleHandle)와 TryRebuild, 발급 카운터 Overflow, 할당 실패 원자성(주입 단언), 복사·이동.
using namespace rocks;
namespace curve_key_handle_table_tests_file {

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
    // 허용오차가 커서 Auto 키가 지워진다(범위판·표본판도 섞는다).
    static Result<void> RemoveRedundant(CurveKeyHandleTable<Curve>& table, Curve& curve,
                                        test::MathRandom& random) noexcept {
        const uint64 pick = random.Next() % 3;
        if (pick == 0) {
            return table.TryRemoveRedundantKeys(curve, 1.0e9f);
        }

        if (pick == 1) {
            return table.TryRemoveRedundantKeys(curve, 1.0e9f, -3.0f, 3.0f);
        }

        return table.TryRemoveRedundantKeys(
            curve, 1.0e9f, Optional<FrameRate>(FrameRate::TryCreate(30, 1).Value()));
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
    static Result<void> RemoveRedundant(CurveKeyHandleTable<Curve>& table, Curve& curve,
                                        test::MathRandom& random) noexcept {
        return random.Next() % 2 == 0 ? table.TryRemoveRedundantKeys(curve, 1.0e9f)
                                      : table.TryRemoveRedundantKeys(curve, 1.0e9f, -3.0f, 3.0f);
    }
};

// 계단 커브는 키 값(번호)이 모두 달라 중복 제거가 아무것도 지우지 않는다(지우는 경우는
// CurveKeyHandleTable_RemoveRedundantKeepsStepHandles가 본다).
template <class Curve>
Result<void> RemoveStepRedundant(CurveKeyHandleTable<Curve>& table, Curve& curve,
                                 test::MathRandom&) noexcept {
    return table.TryRemoveRedundantKeys(curve);
}

struct IntegerTraits {
    using Curve = IntegerStepCurve;
    static int32 Value(int32 id) noexcept {
        return id;
    }
    static int32 Id(const IntegerStepKey& key) noexcept {
        return key.value;
    }
    static constexpr auto RemoveRedundant = RemoveStepRedundant<Curve>;
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
    static constexpr auto RemoveRedundant = RemoveStepRedundant<Curve>;
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
    static constexpr auto RemoveRedundant = RemoveStepRedundant<Curve>;
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
            const uint64 operation = count == 0 ? 0 : random.Next() % 10;
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
            } else if (operation == 6) {
                mismatches += editFields(table, curve, random) ? 0 : 1;
            } else if (operation == 7) {
                const float delta = static_cast<float>(random.Range(-5.0, 5.0));
                mismatches += table.TryShiftTime(curve, delta) ? 0 : 1;
            } else if (operation == 8) {
                const float origin = static_cast<float>(random.Range(-5.0, 5.0));
                const float factor = static_cast<float>(random.Range(0.5, 2.0));
                mismatches += table.TryScaleTime(curve, origin, factor) ? 0 : 1;
            } else {
                // 남은 키의 핸들은 그대로, 지운 키의 핸들은 NotFound다.
                mismatches += Traits::RemoveRedundant(table, curve, random) ? 0 : 1;
                for (usize index = model.Size(); index > 0; --index) {
                    auto found = table.TryFindIndex(curve, model[index - 1].handle);
                    if (!found) {
                        mismatches += found.Error() == ErrorCode::NotFound ? 0 : 1;
                        static_cast<void>(model.TryRemoveAt(index - 1));
                    }
                }
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
                  auto handle = table.TryGetHandle(curve, random.Next() % curve.KeyCount());
                  if (!handle) {
                      return false;
                  }

                  const auto interp = static_cast<CurveInterp>(random.Next() % 3);
                  const auto weight = static_cast<CurveTangentWeight>(random.Next() % 4);
                  auto edited = table.TryEditKeys(curve, [&](auto& keys) noexcept {
                      if (auto set = keys.TrySetInterp(handle.Value(), interp); !set) {
                          return set;
                      }

                      if (auto set = keys.TrySetTangentWeight(handle.Value(), weight); !set) {
                          return set;
                      }

                      return keys.TrySetTangents(handle.Value(), 0.5f, -0.5f);
                  });
                  return edited && table.TryAutoSetTangents(curve, 0.25f);
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

    // 편집자의 실패(모르는 핸들)는 그 결과를 돌려주고, 앞선 편집은 남으며 표는 무효가 아니다.
    const Result<void> partial = table.TryEditKeys(curve, [&](auto& keys) noexcept {
        if (auto set = keys.TrySetInterp(second.Value(), CurveInterp::Cubic); !set) {
            return set;
        }

        return keys.TrySetInterp(first.Value(), CurveInterp::Constant); // 지운 키
    });
    CHECK(ErrorOf(partial) == ErrorCode::NotFound);
    CHECK(!table.IsStale() && curve.Keys()[0].interp == CurveInterp::Cubic);
    CHECK(table.TryFindIndex(curve, second.Value()));

    // 편집 함수가 편집자 밖에서 키 시간을 바꾸면 표는 무효(StaleHandle)다.
    CHECK(ErrorOf(table.TryEditKeys(curve, [&](auto&) noexcept {
              auto moved = curve.TrySetKeyTime(0, 5.0f);
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

    // 이동 대입: 대상은 옮겨 온 핸들로 같은 키를 가리킨다. 이동된 쪽은 빈 사본의 표(빈 커브에만
    // 맞는다)이고 발급 카운터는 남아 옛 핸들 값을 다시 발급하지 않는다.
    CurveKeyHandleTable<StringStepCurve> assigned(allocator);
    assigned = Move(moved);
    auto assignedIndex = assigned.TryFindIndex(curveCopy, nextCopy.Value());
    REQUIRE(assignedIndex);
    CHECK(assignedIndex.Value() == 2);
    CHECK(moved.Count() == 0 && !moved.IsStale());
    CHECK(ErrorOf(moved.TryGetHandle(curveCopy, 0)) == ErrorCode::StaleHandle);
    REQUIRE(moved.TryRebuild(curveCopy));
    auto rebuilt = moved.TryGetHandle(curveCopy, 0);
    REQUIRE(rebuilt);
    CHECK(rebuilt.Value().value > nextCopy.Value().value);

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

// 리뷰 1 재현: 같은 시간 키 A·B의 순서를 편집이 바꾸면(시간 비트는 그대로) 핸들이 다른 키를
// 가리키게 두지 않는다.
TEST(CurveKeyHandleTable_EditKeysCannotReorderSameTimeKeys) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    CurveKeyHandleTable<TangentCurve> table(allocator);
    auto a = table.TryAddKey(curve, 1.0f, 10.0f);
    auto b = table.TryAddKey(curve, 1.0f, 20.0f); // 같은 시간 키 앞 — 순서 B, A
    REQUIRE(a && b);
    // 편집자에는 시간·값·순서를 바꾸는 길이 없다. 편집 함수가 커브를 직접 잡아 순서를 바꿔도(시간
    // 비트는 그대로) 값 비교 안전장치가 표를 무효로 한다.
    const Result<void> edited = table.TryEditKeys(curve, [&](auto&) noexcept {
        auto moved = curve.TrySetKeyTime(1, 1.0f); // A를 같은 시간 키 앞으로 — 순서 A, B
        return moved ? Result<void>::Success() : Result<void>::Failure(moved.Error());
    });
    CHECK(ErrorOf(edited) == ErrorCode::StaleHandle);
    CHECK(table.IsStale());
    auto index = table.TryFindIndex(curve, a.Value());
    CHECK(ErrorOf(index) == ErrorCode::StaleHandle);
}

// 리뷰 3 재현: 허용오차 인자가 float가 아니어도(double·int) 카운터 끝의 Overflow 판단이 맞다 —
// 더하는 경우는 Overflow이고 그대로, 값만 바꾸는 경우는 그 키의 핸들이다.
TEST(CurveKeyHandleTable_UpdateOrAddOverflowWithNonFloatTolerance) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    CurveKeyHandleTable<TangentCurve> table(allocator);
    auto first = table.TryAddKey(curve, 0.0f, 1.0f);
    REQUIRE(first);
    curves_detail::CurveKeyHandleTableTestAccess::SetLastIssued(table,
                                                                NumericLimits<uint32>::Max());
    auto added = table.TryUpdateOrAddKey(curve, 5.0e-5f, 2.0f, false, 0.0); // double 0: 더한다
    CHECK(ErrorOf(added) == ErrorCode::Overflow);
    CHECK(curve.KeyCount() == 1 && table.Count() == 1 && !table.IsStale());
    auto updated = table.TryUpdateOrAddKey(curve, 0.5f, 3.0f, false, 1); // int 1: 값만 바꾼다
    REQUIRE(updated);
    CHECK(updated.Value() == first.Value() && curve.KeyCount() == 1 &&
          curve.Keys()[0].value == 3.0f);

    IntegerStepCurve steps(allocator);
    CurveKeyHandleTable<IntegerStepCurve> stepTable(allocator);
    auto key = stepTable.TryAddKey(steps, 0.0f, 1);
    REQUIRE(key);
    curves_detail::CurveKeyHandleTableTestAccess::SetLastIssued(stepTable,
                                                                NumericLimits<uint32>::Max());
    CHECK(ErrorOf(stepTable.TryUpdateOrAddKey(steps, 5.0e-5f, 2, 0.0)) == ErrorCode::Overflow);
    CHECK(steps.KeyCount() == 1);
    auto stepUpdated = stepTable.TryUpdateOrAddKey(steps, 0.25f, 4, 1);
    REQUIRE(stepUpdated);
    CHECK(stepUpdated.Value() == key.Value() && steps.Keys()[0].value == 4);
}

// 표 판 중복 제거(계단 커브 셋): 지운 키의 핸들만 NotFound이고 남은 키의 핸들은 같은 키를 가리킨다.
// Name은 표시 철자가 다른 키를 남긴다. TangentCurve 범위판도 범위 밖 키의 핸들을 지킨다. 실패
// (허용오차 거부)는 커브와 표가 그대로다.
TEST(CurveKeyHandleTable_RemoveRedundantKeepsStepHandles) {
    SystemAllocator allocator;
    {
        IntegerStepCurve curve(allocator);
        CurveKeyHandleTable<IntegerStepCurve> table(allocator);
        const int32 values[] = {1, 1, 1, 2, 2, 3};
        CurveKeyHandle handles[6];
        for (usize index = 0; index < 6; ++index) {
            auto handle = table.TryAddKey(curve, static_cast<float>(index), values[index]);
            REQUIRE(handle);
            handles[index] = handle.Value();
        }

        REQUIRE(table.TryRemoveRedundantKeys(curve));
        REQUIRE(curve.KeyCount() == 5 && table.Count() == 5 && !table.IsStale());
        CHECK(ErrorOf(table.TryFindIndex(curve, handles[1])) == ErrorCode::NotFound);
        const usize expected[] = {0, IndexNone, 1, 2, 3, 4};
        for (usize index = 0; index < 6; ++index) {
            if (expected[index] != IndexNone) {
                auto found = table.TryFindIndex(curve, handles[index]);
                REQUIRE(found);
                CHECK(found.Value() == expected[index] &&
                      curve.Keys()[found.Value()].time == static_cast<float>(index));
            }
        }
    }

    {
        StringStepCurve curve(allocator);
        CurveKeyHandleTable<StringStepCurve> table(allocator);
        const char* const values[] = {"same value long enough to allocate",
                                      "same value long enough to allocate",
                                      "same value long enough to allocate", "other"};
        CurveKeyHandle handles[4];
        for (usize index = 0; index < 4; ++index) {
            auto handle = table.TryAddKey(curve, static_cast<float>(index), values[index]);
            REQUIRE(handle);
            handles[index] = handle.Value();
        }

        REQUIRE(table.TryRemoveRedundantKeys(curve));
        REQUIRE(curve.KeyCount() == 3 && table.Count() == 3);
        CHECK(ErrorOf(table.TryFindIndex(curve, handles[1])) == ErrorCode::NotFound);
        auto last = table.TryFindIndex(curve, handles[3]);
        REQUIRE(last);
        CHECK(last.Value() == 2 && curve.Keys()[2].value.View() == StringView("other"));
        REQUIRE(table.TrySetKeyValue(curve, handles[2], "changed")); // 사본이 커브와 맞다
    }

    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        {
            NameStepCurve curve(allocator);
            CurveKeyHandleTable<NameStepCurve> table(allocator);
            const char* const values[] = {"Walk", "walk", "Walk", "Walk", "Walk"};
            CurveKeyHandle handles[5];
            for (usize index = 0; index < 5; ++index) {
                auto name = pool.TryIntern(StringView(values[index]));
                REQUIRE(name);
                auto handle = table.TryAddKey(curve, static_cast<float>(index), name.Value());
                REQUIRE(handle);
                handles[index] = handle.Value();
            }

            REQUIRE(table.TryRemoveRedundantKeys(curve));
            REQUIRE(curve.KeyCount() == 4 && table.Count() == 4);
            CHECK(ErrorOf(table.TryFindIndex(curve, handles[3])) == ErrorCode::NotFound);
            auto spelled = table.TryFindIndex(curve, handles[1]);
            REQUIRE(spelled);
            CHECK(spelled.Value() == 1);
        }

        CHECK(pool.Shutdown());
    }

    {
        TangentCurve curve(allocator);
        CurveKeyHandleTable<TangentCurve> table(allocator);
        CurveKeyHandle handles[6];
        for (usize index = 0; index < 6; ++index) {
            auto handle = table.TryAddKey(curve, static_cast<float>(index), 2.0f);
            REQUIRE(handle);
            handles[index] = handle.Value();
        }

        CHECK(ErrorOf(table.TryRemoveRedundantKeys(curve, -1.0f)) == ErrorCode::InvalidArgument);
        CHECK(curve.KeyCount() == 6 && !table.IsStale());
        // 범위 [1, 3]: 시작 키 1과 끝 키 3 사이의 키 2만 지운다.
        REQUIRE(table.TryRemoveRedundantKeys(curve, 1.0e-3f, 1.0f, 3.0f));
        REQUIRE(curve.KeyCount() == 5 && table.Count() == 5);
        CHECK(ErrorOf(table.TryFindIndex(curve, handles[2])) == ErrorCode::NotFound);
        auto tail = table.TryFindIndex(curve, handles[5]);
        REQUIRE(tail);
        CHECK(tail.Value() == 4 && curve.Keys()[4].time == 5.0f);
    }
}

// 표 판 Shift·Scale은 순서를 그대로 두어 핸들이 같은 키를 가리키고(같은 시간 키 포함), 거부되는
// 인자는 커브와 표가 그대로다. 커브를 직접 Shift하면 표는 무효다(UE와 달리 표 판을 써야 한다).
TEST(CurveKeyHandleTable_ShiftAndScaleKeepHandles) {
    SystemAllocator allocator;
    PlainCurve curve(allocator);
    CurveKeyHandleTable<PlainCurve> table(allocator);
    auto a = table.TryAddKey(curve, 1.0f, 10.0f);
    auto b = table.TryAddKey(curve, 1.0f, 20.0f); // 같은 시간 키 앞
    auto c = table.TryAddKey(curve, 3.0f, 30.0f);
    REQUIRE(a && b && c);
    REQUIRE(table.TryShiftTime(curve, 2.5f));
    REQUIRE(table.TryScaleTime(curve, 1.0f, 2.0f));
    const CurveKeyHandle handles[] = {b.Value(), a.Value(), c.Value()};
    const float values[] = {20.0f, 10.0f, 30.0f};
    for (usize index = 0; index < 3; ++index) {
        auto found = table.TryFindIndex(curve, handles[index]);
        REQUIRE(found);
        CHECK(found.Value() == index && curve.Keys()[index].value == values[index]);
    }

    CHECK(curve.Keys()[0].time == 6.0f && curve.Keys()[2].time == 10.0f);
    CHECK(ErrorOf(table.TryScaleTime(curve, 0.0f, 0.0f)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(table.TryShiftTime(curve, platform::PlatformMath::Infinity<float>())) ==
          ErrorCode::InvalidArgument);
    CHECK(!table.IsStale() && curve.Keys()[0].time == 6.0f);
    REQUIRE(curve.TryShiftTime(1.0f));
    CHECK(ErrorOf(table.TryShiftTime(curve, 1.0f)) == ErrorCode::StaleHandle);
}

// 표 판 중복 제거의 할당 실패(커브의 남은 index 목록·키 배열)는 OutOfMemory이고 커브와 표가
// 그대로다 (주입 단언).
TEST(CurveKeyHandleTable_RemoveRedundantAllocationFailureIsAtomic) {
    SystemAllocator allocator;
    bool injected = false;
    for (uint64 attempt = 1; attempt <= 8; ++attempt) {
        test::NFailAllocator failing(allocator);
        StringStepCurve curve(failing);
        CurveKeyHandleTable<StringStepCurve> table(failing);
        CurveKeyHandle handles[4];
        for (usize index = 0; index < 4; ++index) {
            auto handle = table.TryAddKey(curve, static_cast<float>(index),
                                          index < 3 ? "repeated value long enough to allocate"
                                                    : "last value long enough to allocate");
            REQUIRE(handle);
            handles[index] = handle.Value();
        }

        failing.FailOnAttempt(attempt); // 시도 수는 여기서 0부터 센다
        const Result<void> removed = table.TryRemoveRedundantKeys(curve);
        failing.DisableFailure();
        if (!removed) {
            injected = true;
            CHECK(removed.Error() == ErrorCode::OutOfMemory);
            REQUIRE(curve.KeyCount() == 4 && table.Count() == 4 && !table.IsStale());
            for (usize index = 0; index < 4; ++index) {
                auto found = table.TryFindIndex(curve, handles[index]);
                CHECK(found && found.Value() == index);
            }
        } else {
            CHECK(curve.KeyCount() == 3 && table.Count() == 3);
        }
    }

    CHECK(injected);
}
} // namespace curve_key_handle_table_tests_file

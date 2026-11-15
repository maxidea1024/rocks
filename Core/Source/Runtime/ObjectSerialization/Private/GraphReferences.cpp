#include "GraphInternal.h"

// 그래프 복원 1단계의 참조 형식 검사다(Docs/Design/ObjectFoundations.ko.md 5.4절 — ValidateGraph가
// 모든 노드를 해석한 뒤 부른다): 노드 참조 token은 대상 노드 타입으로 기대 형식을 판정하고, 외부
// 참조 token은 선언 타입(importTypes)과 기대 타입의 양립을 본 뒤 (import, 기대 형식) 쌍을 기록한다
// (2b가 실제 대상으로 판정 — GraphExternal.cpp). ObjectRegistry에 닿지 않는다.
namespace rocks::graph_detail {
namespace {

// 강한 참조 대상 타입 검사 캐시의 키다: (기대 TypeId, 대상 TypeId).
struct TypePair {
    TypeId expected{};
    TypeId actual{};

    bool operator==(const TypePair&) const noexcept = default;
};

struct TypePairHash {
    uint64 operator()(const TypePair& pair) const noexcept {
        return MixHash(Hash<Guid>{}(pair.expected) ^ (Hash<Guid>{}(pair.actual) >> 1));
    }
};

// 기록한 (외부 참조 i, 기대 형식) 쌍의 중복 제거 키다.
struct ImportKey {
    uint32 externalIndex = 0;
    TypeId schemaType{};
    const void* staticNative = nullptr;

    bool operator==(const ImportKey&) const noexcept = default;
};

struct ImportKeyHash {
    uint64 operator()(const ImportKey& key) const noexcept {
        const uint64 native = static_cast<uint64>(reinterpret_cast<uptr>(key.staticNative));
        return MixHash(Hash<Guid>{}(key.schemaType) ^
                       MixHash((uint64{key.externalIndex} << 32) ^ native));
    }
};

// 기대 TypeId의 등록 타입 lease다(leases에 쥐어 로드 끝까지 둔다). 없을 때만 획득·삽입을 청구한다
// (미등록 NotFound).
Result<const TypeLease*> ExpectedLease(TypeRegistry& types, HashMap<TypeId, TypeLease>& leases,
                                       TypeId type, Budget& budget) noexcept {
    if (const TypeLease* found = leases.Find(type)) {
        return Result<const TypeLease*>::Success(found);
    }

    if (auto spent = budget.Spend(1); !spent) {
        return Result<const TypeLease*>::Failure(spent.Error());
    }

    auto acquired = types.Acquire(type);
    if (!acquired) {
        return Result<const TypeLease*>::Failure(acquired.Error());
    }

    if (auto added = leases.Insert(type, Move(acquired.Value())); !added) {
        return Result<const TypeLease*>::Failure(added.Error());
    }

    return Result<const TypeLease*>::Success(leases.Find(type));
}

// 외부 참조 token 하나를 검사하고 (import, 기대 형식) 쌍을 기록한다(Docs/Design/
// ObjectFoundations.ko.md 5.4절). 선언 타입(importTypes)과 스키마 기대 타입은 양립해야 한다 —
// 한쪽이 다른 쪽의 native 파생이면 그 선언 타입의 객체가 기대를 만족할 수 있다(아니면
// TypeMismatch). 실제 연결의 판정(대상이 기대를 만족하는지 — 정적 thunk 대상 타입 포함)은 2b가
// 기록한 쌍으로 한다. compatible은 (기대, 선언) 양립 캐시, recorded는 쌍 중복 제거 표이며
// 조회·삽입마다 작업량 1이다.
Result<void> CheckExternalToken(TypeRegistry& types, StageData& data, uint32 externalIndex,
                                const ReferenceExpectation& expectation,
                                HashMap<TypePair, bool, TypePairHash>& compatible,
                                HashMap<ImportKey, bool, ImportKeyHash>& recorded) noexcept {
    Budget& budget = data.budget;
    const TypeLease& declared = data.importLeases[externalIndex - 1];
    if (expectation.schemaType != Guid{}) {
        if (auto spent = budget.Spend(1); !spent) {
            return spent;
        }

        const TypePair key{expectation.schemaType, declared.Id()};
        bool accepted = false;
        if (const bool* cached = compatible.Find(key)) {
            accepted = *cached;
        } else {
            if (auto spent = budget.Spend(1); !spent) {
                return spent;
            }

            auto expected =
                ExpectedLease(types, data.expectedLeases, expectation.schemaType, budget);
            if (!expected) {
                return Result<void>::Failure(expected.Error());
            }

            accepted = declared.IsNativeA(expected.Value()->Ops().nativeType) ||
                       expected.Value()->IsNativeA(declared.Ops().nativeType);
            if (auto spent = budget.Spend(1); !spent) {
                return spent;
            }

            if (auto added = compatible.Insert(key, accepted); !added) {
                return Result<void>::Failure(added.Error());
            }
        }

        if (!accepted) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }
    }

    if (auto spent = budget.Spend(1); !spent) {
        return spent;
    }

    const ImportKey key{externalIndex, expectation.schemaType, expectation.staticNative};
    if (recorded.Contains(key)) {
        return Result<void>::Success();
    }

    if (auto spent = budget.Spend(1); !spent) {
        return spent;
    }

    if (auto added = recorded.Insert(key, true); !added) {
        return Result<void>::Failure(added.Error());
    }

    if (!data.importChecks.TryPushBack(ImportExpectation{externalIndex, expectation})) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<void>::Success();
}

} // namespace

// 모든 노드의 강한 참조 token이 대상의 타입 기대를 만족하는지 검사한다(도달 여부와 무관 — setter와
// 같은 native 상속 계약을 생성 전에). 먼저 외부 참조의 선언 타입(importTypes) lease를 연다
// (미등록 NotFound·클래스 아님 TypeMismatch). 노드 token은 대상 노드 타입으로 판정하고, 외부 참조
// token은 선언 타입과의 양립만 보고 쌍을 기록한다(CheckExternalToken). data.expectedLeases는 검사에
// 쓴 기대 타입 lease(기대 TypeId → lease)를 받아 Stage가 끝까지 쥔다. (기대, 실제) 결과 캐시와 기대
// lease 목록은 해시 표이고 조회·삽입마다 작업량 1을 청구한다(원소 참조가 많아도 대상 타입 수에
// 비례하지 않는다).
Result<void> ValidateReferenceTypes(Allocator& allocator, TypeRegistry& types, StageData& data,
                                    Span<const TypeId> importTypes) noexcept {
    const Array<LoadNode>& nodes = data.envelope.nodes;
    Budget& budget = data.budget;
    HashMap<TypeId, TypeLease>& leases = data.expectedLeases;
    if (auto reserved = data.importLeases.TryReserve(importTypes.Size()); !reserved) {
        return reserved;
    }

    for (TypeId type : importTypes) {
        if (auto spent = budget.Spend(1); !spent) {
            return spent;
        }

        auto lease = types.Acquire(type);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        if (!data.importLeases.TryPushBack(Move(lease.Value()))) {
            platform::Fatal("Object graph import lease reservation failed");
        }
    }

    HashMap<TypePair, bool, TypePairHash> checks(allocator);
    HashMap<TypePair, bool, TypePairHash> compatible(allocator);
    HashMap<ImportKey, bool, ImportKeyHash> recorded(allocator);
    for (const LoadNode& node : nodes) {
        for (usize tokenIndex = 0; tokenIndex < node.tokens.Size(); ++tokenIndex) {
            auto spent = budget.Spend(1);
            if (!spent) {
                return spent;
            }

            const ReferenceToken& token = node.tokens[tokenIndex];
            const ReferenceExpectation& expectation = node.expectations[token.expectation];
            if (const uint32 external = ExternalIndexOf(token); external != 0) {
                if (auto checked = CheckExternalToken(types, data, external, expectation,
                                                      compatible, recorded);
                    !checked) {
                    return checked;
                }

                continue;
            }

            const LoadNode& target = nodes[token.target - 1];
            // 선언 타입의 파생 객체와 같은 native의 다른 TypeId는 허용한다. 정적 thunk의 대상
            // 타입도 함께 만족해야 한다.
            if (expectation.staticNative && !target.lease.IsNativeA(expectation.staticNative)) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }

            if (expectation.schemaType == Guid{}) {
                continue;
            }

            spent = budget.Spend(1);
            if (!spent) {
                return spent;
            }

            const TypePair key{expectation.schemaType, target.type};
            bool accepted = false;
            if (const bool* cached = checks.Find(key)) {
                accepted = *cached;
            } else {
                spent = budget.Spend(1);
                if (!spent) {
                    return spent;
                }

                // 조회와 마찬가지로 lease 획득·삽입도 청구한다.
                auto expected = ExpectedLease(types, leases, expectation.schemaType, budget);
                if (!expected) {
                    return Result<void>::Failure(expected.Error());
                }

                accepted = target.lease.IsNativeA(expected.Value()->Ops().nativeType);
                spent = budget.Spend(1);
                if (!spent) {
                    return spent;
                }

                if (auto added = checks.Insert(key, accepted); !added) {
                    return Result<void>::Failure(added.Error());
                }
            }

            if (!accepted) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::graph_detail

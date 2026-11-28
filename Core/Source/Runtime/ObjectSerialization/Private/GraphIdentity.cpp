#include "GraphInternal.h"
#include <Core/Hash/TypeHash.h>
#include <Core/Text/NameComponent.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Memory.h>

// 노드 표의 정체성 규칙이다(Docs/Design/ObjectFoundations.ko.md 5.4절): 1단계의 노드별
// CheckNewObjectIdentity·그래프 안 형제 유일성과 2a 사전 확인(실제 기준 Outer 요약으로 재판정·이름
// 점유). Registry 생성 규칙(detail::CheckNewObjectIdentity)을 그대로 써서 규칙이 갈라지지 않게
// 한다.
namespace rocks::graph_detail {

NodeIdentityCheck::NodeIdentityCheck(Allocator& allocator, const RegistryPolicy& policy,
                                     bool hasBase, const OuterSummary& base) noexcept
    : allocator_(allocator), policy_(policy), hasBase_(hasBase), base_(base), shapes_(allocator),
      keys_(allocator), heads_(allocator) {
}

Result<void> NodeIdentityCheck::Reserve(usize count) noexcept {
    if (auto reserved = shapes_.TryReserve(count); !reserved) {
        return reserved;
    }

    return heads_.Reserve(count);
}

const OuterSummary* NodeIdentityCheck::OuterOf(uint32 outer, OuterSummary& summary) const noexcept {
    if (outer == 0) {
        if (!hasBase_) {
            return nullptr;
        }

        // 기준 Outer는 입력 요약이고, 앞서 나온 기준 Outer 노드(형제)를 Inner로 더한다. 타입은
        // 비운다 — 1단계는 어느 스레드에서나 돌고 Registry에 닿지 않으므로 입력 요약이 가리키는
        // slot lease를 읽지 않는다(Within 판정은 2a와 실제 생성이 한다 — 리플렉션 B9h2).
        summary = base_;
        summary.type = nullptr;
        return &summary;
    }

    const NodeShape& shape = shapes_[outer - 1];
    summary = OuterSummary{};
    summary.isOutermost = shape.isOutermost;
    summary.outermostIsPackage = shape.outermostIsPackage;
    summary.hasRepresentative = shape.hasRepresentative;
    summary.hasNamedInners = shape.hasNamedInners;
    summary.pathBytes = shape.pathBytes;
    summary.type = shape.type;
    return &summary;
}

Result<void> NodeIdentityCheck::Check(const LoadNode& node, Budget& budget) noexcept {
    const auto local = static_cast<uint32>(shapes_.Size() + 1);
    const StringView name = node.name;
    OuterSummary summary;
    const OuterSummary* outer = OuterOf(node.outer, summary);
    // 읽은 노드의 이름·대표 자식 규칙 위반은 손상이다(AssetSystem 19장 — 같은 함수가 실행 중
    // 생성에서는 인자 오용 InvalidArgument를 낸다).
    if (auto identity =
            detail::CheckNewObjectIdentity(policy_, node.lease, outer, name, allocator_);
        !identity) {
        return Result<void>::Failure(identity.Error() == ErrorCode::InvalidArgument
                                         ? ErrorCode::CorruptData
                                         : identity.Error());
    }

    // 이름 형식·비교 키·해시 몫이다(이름 byte에 비례 — 규칙을 지난 이름은 64 byte 이하다).
    if (auto spent = budget.Spend(uint64{name.ByteCount()} + 1); !spent) {
        return spent;
    }

    NodeShape shape;
    shape.outer = node.outer;
    shape.type = &node.lease;
    shape.isOutermost = outer == nullptr;
    shape.outermostIsPackage = outer != nullptr
                                   ? outer->outermostIsPackage
                                   : policy_.packageNativeType != nullptr &&
                                         node.lease.IsNativeA(policy_.packageNativeType);
    // 정체성 규칙이 경로 길이를 MaxObjectPathBytes 이하로 확인했다.
    shape.pathBytes = static_cast<uint32>(detail::ChildPathBytes(outer, name.ByteCount()));
    // 이름 색인에 들어가는 노드(Outer가 있거나 이름이 있는 노드 — Registry와 같은 기준)만 형제
    // 유일성을 본다. 같은 (outer, 비교 키)는 Create가 AlreadyExists로 거부할 파일 손상이다.
    const bool indexed = outer != nullptr || !name.IsEmpty();
    uint64 hash = 0;
    if (indexed) {
        StringBuilder<128> key(allocator_);
        if (auto made = TryMakeNameComponentKey(name, key); !made) {
            return Result<void>::Failure(
                made.Error() == ErrorCode::InvalidArgument ? ErrorCode::CorruptData : made.Error());
        }

        hash = HashCombine(MixHash(uint64{node.outer}), HashStringBytes(key.View()));
        if (const uint32* head = heads_.Find(hash)) {
            for (uint32 other = *head; other != 0; other = shapes_[other - 1].nextSameHash) {
                const NodeShape& sibling = shapes_[other - 1];
                if (auto spent = budget.Spend(uint64{key.ByteCount()} + 1); !spent) {
                    return spent;
                }

                if (sibling.outer == node.outer && sibling.keyBytes == key.ByteCount() &&
                    platform::MemoryCompare(keys_.Data() + sibling.keyOffset, key.View().Data(),
                                            key.ByteCount()) == 0) {
                    return Result<void>::Failure(ErrorCode::CorruptData);
                }
            }

            shape.nextSameHash = *head;
        }

        // 키 위치는 uint32로 기록한다(한도를 크게 연 옵션에서도 잘리지 않게 넘으면 거부한다).
        if (key.ByteCount() > NumericLimits<uint32>::Max() - keys_.Size()) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        shape.keyOffset = static_cast<uint32>(keys_.Size());
        shape.keyBytes = static_cast<uint32>(key.ByteCount());
        if (auto grown = keys_.TryResize(keys_.Size() + key.ByteCount()); !grown) {
            return grown;
        }

        platform::MemoryCopy(keys_.Data() + shape.keyOffset, key.View().Data(), key.ByteCount());
    }

    if (!shapes_.TryPushBack(shape)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    if (indexed) {
        if (auto inserted = heads_.InsertOrAssign(hash, local); !inserted) {
            return Result<void>::Failure(inserted.Error());
        }
    }

    // 이 노드를 Outer의 Inner로 기록한다(대표 자식 규칙이 뒤 형제에 걸리게).
    if (node.outer == 0) {
        if (hasBase_) {
            (name.IsEmpty() ? base_.hasRepresentative : base_.hasNamedInners) = true;
        }
    } else {
        NodeShape& parent = shapes_[node.outer - 1];
        (name.IsEmpty() ? parent.hasRepresentative : parent.hasNamedInners) = true;
    }

    return Result<void>::Success();
}

Result<void> CheckCreatePreconditions(Allocator& allocator, ConstructionContext& transaction,
                                      const RegistryPolicy& policy, const Array<LoadNode>& nodes,
                                      bool hasBase, const OuterSummary& base,
                                      ObjectHandle baseOuter) noexcept {
    if (hasBase == baseOuter.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ObjectRegistry& registry = transaction.Registry();
    if (!hasBase) {
        // Outer 없는 이름 있는 노드는 최상위 이름 공간을 점유한다(이름 없는 outermost는 색인에
        // 없다).
        for (const LoadNode& node : nodes) {
            if (node.outer != 0 || node.name.IsEmpty()) {
                continue;
            }

            auto taken = registry.IsNameTaken(transaction.Access(), ObjectHandle{}, node.name);
            if (!taken) {
                return Result<void>::Failure(taken.Error());
            }

            if (taken.Value()) {
                return Result<void>::Failure(ErrorCode::AlreadyExists);
            }
        }

        return Result<void>::Success();
    }

    // 기준 Outer는 생존 객체이거나 이 트랜잭션의 객체다. 1단계가 기댄 요약 값(outermost 여부·
    // 패키지 여부·경로 길이)이 같아야 한다 — handle과 Inner 유무는 아래에서 실제 상태로 다시 본다.
    auto actual = transaction.DescribeOuter(baseOuter);
    if (!actual) {
        return Result<void>::Failure(actual.Error());
    }

    if (actual.Value().isOutermost != base.isOutermost ||
        actual.Value().outermostIsPackage != base.outermostIsPackage ||
        actual.Value().pathBytes != base.pathBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 기준 Outer 노드는 실제 Inner 상태(대표 자식·이름 있는 Inner)로 대표 자식 규칙을 다시
    // 판정한다. 앞선 기준 Outer 노드는 그 Inner로 더한다(1단계와 같은 누적).
    OuterSummary running = actual.Value();
    for (const LoadNode& node : nodes) {
        if (node.outer != 0) {
            continue;
        }

        if (auto identity =
                detail::CheckNewObjectIdentity(policy, node.lease, &running, node.name, allocator);
            !identity) {
            return identity;
        }

        (node.name.IsEmpty() ? running.hasRepresentative : running.hasNamedInners) = true;
    }

    // 이름 점유: 대표 노드는 빈 비교 키로 본다(기준 Outer에 이미 대표 자식이 있으면 점유).
    for (const LoadNode& node : nodes) {
        if (node.outer != 0) {
            continue;
        }

        auto taken = registry.IsNameTaken(transaction.Access(), baseOuter, node.name);
        if (!taken) {
            return Result<void>::Failure(taken.Error());
        }

        if (taken.Value()) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::graph_detail

#include "GraphInternal.h"

// 단계형 그래프 복원(ObjectGraphStage)의 상태 기계와 그 위의 편의 복원(LoadObjectGraph)이다
// (Docs/Design/ObjectFoundations.ko.md 5.4·5.5절). 단계 본문은 1단계 GraphLoad.cpp, 정체성·사전
// 확인 GraphIdentity.cpp, 생성·적용 GraphApply.cpp, Required 검사 GraphRequired.cpp, 도달 검사
// GraphReachability.cpp다.
namespace rocks {

using namespace graph_detail;

ObjectGraphStage::ObjectGraphStage(Allocator& allocator, ObjectRegistry& registry,
                                   TypeRegistry& types, const ObjectAccessContext& access,
                                   ObjectGraphLoadOptions options) noexcept
    : allocator_(allocator), registry_(registry), types_(types), access_(access), options_(options),
      policy_(registry.Policy()), nodes_(allocator) {
    // 실패를 돌려줄 수 없으므로 기억해 두고 모든 단계가 먼저 돌려준다.
    if (!registry.UsesTypeRegistry(types)) {
        contextError_ = ErrorCode::WrongContext;
    } else if (auto owned = registry.VerifyAccess(access); !owned) {
        contextError_ = owned.Error();
    }
}

ObjectGraphStage::~ObjectGraphStage() = default;

// 호출 검사(상태를 바꾸지 않는다): 생성자가 기억한 오류 → 진행 중(같은 Stage 재진입) Busy →
// 순서(expected 단계가 아님) InvalidArgument.
Result<void> ObjectGraphStage::CheckCall(Phase expected) const noexcept {
    if (contextError_ != ErrorCode::None) {
        return Result<void>::Failure(contextError_);
    }

    if (phase_ == Phase::Running) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (phase_ != expected) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> ObjectGraphStage::Validate(const ObjectGraphInput& input) noexcept {
    if (auto callable = CheckCall(Phase::Constructed); !callable) {
        return callable;
    }

    // ObjectRegistry에 닿지 않는다 — 정책은 생성자가 둔 사본이다. 진행 중에는 같은 Stage의 다른
    // 호출이 Busy다.
    phase_ = Phase::Running;
    auto validated = ValidateGraph(allocator_, types_, policy_, options_, input);
    if (!validated) {
        phase_ = Phase::Failed;
        return Result<void>::Failure(validated.Error());
    }

    data_ = Move(validated.Value());
    hasBase_ = input.hasBase;
    base_ = input.base;
    phase_ = Phase::Validated;
    return Result<void>::Success();
}

Result<void>
ObjectGraphStage::CheckOwnerStage(const ConstructionContext& transaction) const noexcept {
    if (auto owned = registry_.VerifyAccess(access_); !owned) {
        return owned;
    }

    // 트랜잭션은 이 Stage의 Registry·접근 문맥 것이어야 한다(정책이 같은 다른 Registry도 받지
    // 않는다 — 노드 lease와 TypeRegistry가 이 Registry 것이다. Registry 정책은 생성 뒤 불변이라
    // 같은 Registry면 정책 사본과도 같다).
    if (&transaction.Registry() != &registry_ ||
        !(transaction.Access().context == access_.context)) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    return Result<void>::Success();
}

Result<void>
ObjectGraphStage::CheckSameTransaction(const ConstructionContext& transaction) const noexcept {
    if (auto owned = CheckOwnerStage(transaction); !owned) {
        return owned;
    }

    // CreateObjects의 트랜잭션이어야 한다(끝난 트랜잭션의 ID는 0이라 여기서 걸린다).
    if (transaction.TransactionId() == 0 || transaction.TransactionId() != transaction_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 다른 단계의 실패로 고정된 트랜잭션에는 일하지 않는다(그 오류 — 공개될 수 없다).
    return transaction.CheckOpen();
}

Result<void> ObjectGraphStage::CreateObjects(ConstructionContext& transaction,
                                             ObjectHandle baseOuter) noexcept {
    if (auto callable = CheckCall(Phase::Validated); !callable) {
        return callable;
    }

    // 호출 검사: 소유 스레드·Registry·접근 문맥, 그리고 트랜잭션 상태 규칙(끝났으면
    // InvalidArgument, 고정된 오류면 그 오류 — 이름 점유 확인보다 먼저).
    if (auto owned = CheckOwnerStage(transaction); !owned) {
        return owned;
    }

    if (auto open = transaction.CheckOpen(); !open) {
        return open;
    }

    phase_ = Phase::Running;
    Array<LoadNode>& nodes = data_->envelope.nodes;
    // 사전 확인 실패는 객체를 만들지 않았으므로 트랜잭션을 고정하지 않는다. 생성 도중의 실패는
    // Create가 이미 그 오류로 트랜잭션을 고정했다. handle은 지역 배열에 모았다가 성공한 뒤 옮긴다.
    Array<ObjectHandle> handles(allocator_);
    auto created = CheckCreatePreconditions(allocator_, transaction, policy_, nodes, hasBase_,
                                            base_, baseOuter);
    if (created) {
        created = CreateNodes(transaction, nodes, baseOuter, handles);
    }

    if (!created) {
        phase_ = Phase::Failed;
        return created;
    }

    nodes_ = Move(handles);
    transaction_ = transaction.TransactionId();
    phase_ = Phase::Created;
    return Result<void>::Success();
}

Result<uint32> ObjectGraphStage::StoredVersion(usize node) const noexcept {
    if (!data_ || node >= data_->envelope.nodes.Size()) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<uint32>::Success(data_->envelope.nodes[node].version);
}

Result<void> ObjectGraphStage::ApplyFields(ConstructionContext& transaction) noexcept {
    if (auto callable = CheckCall(Phase::Created); !callable) {
        return callable;
    }

    if (auto same = CheckSameTransaction(transaction); !same) {
        return same;
    }

    // 객체를 이미 만들었으므로 실패하면 트랜잭션을 그 오류로 고정한다(Commit이 실패하고 소멸자가
    // 되돌린다 — 부분 적용 객체를 공개하지 않는다).
    phase_ = Phase::Running;
    auto applied = ApplyNodeFields(allocator_, transaction, data_->envelope.nodes, options_);
    if (!applied) {
        (void)transaction.Fail(applied.Error());
        phase_ = Phase::Failed;
        return applied;
    }

    phase_ = Phase::Applied;
    return Result<void>::Success();
}

Result<void> ObjectGraphStage::CheckRequired(ConstructionContext& transaction) noexcept {
    if (auto callable = CheckCall(Phase::Applied); !callable) {
        return callable;
    }

    if (auto same = CheckSameTransaction(transaction); !same) {
        return same;
    }

    // ApplyFields와 같이 실패하면 트랜잭션을 고정한다(검사 중 표시는 CheckRequiredFields가 끝날 때
    // 내려간다).
    phase_ = Phase::Running;
    auto checked = CheckRequiredFields(allocator_, transaction, data_->envelope.nodes, options_);
    if (!checked) {
        (void)transaction.Fail(checked.Error());
        phase_ = Phase::Failed;
        return checked;
    }

    phase_ = Phase::Checked;
    return Result<void>::Success();
}

// 편의 복원이다: 단계형 복원(ObjectGraphStage) 위에서 기준 Outer 요약을 만들고, 1단계 뒤 도달
// 검사를 더하고, 노드 1을 root로 Commit한다(Docs/Design/ObjectFoundations.ko.md 5.5절).
Result<RootRef<Object>> LoadObjectGraph(Allocator& allocator, ObjectRegistry& registry,
                                        TypeRegistry& types, const ObjectAccessContext& access,
                                        Span<const uint8> source,
                                        ObjectGraphLoadOptions options) noexcept {
    using Output = Result<RootRef<Object>>;
    if (!registry.UsesTypeRegistry(types)) {
        return Output::Failure(ErrorCode::WrongContext);
    }

    if (auto checked = CheckLoadOptions(options); !checked) {
        return Output::Failure(checked.Error());
    }

    // 기준 Outer는 생존 객체여야 하고 그 요약이 단계형 입력의 base다(접근·종료·handle 오류는
    // DescribeOuter의 것). 노드 표는 그래프의 NodeTable이다.
    ObjectGraphInput input;
    input.bytes = source;
    input.hasBase = options.baseOuter != ObjectHandle{};
    if (input.hasBase) {
        auto described = registry.DescribeOuter(access, options.baseOuter);
        if (!described) {
            return Output::Failure(described.Error());
        }

        input.base = described.Value();
    }

    ObjectGraphStage stage(allocator, registry, types, access, options);
    if (auto valid = stage.Validate(input); !valid) {
        return Output::Failure(valid.Error());
    }

    // 도달 검사는 편의 복원만 한다(단계형 복원은 고아를 허용한다 — 생성 전에 판정한다).
    StageData& data = *StageAccess::Data(stage);
    const bool packagePolicy = registry.Policy().packageNativeType != nullptr;
    if (auto reachable = CheckReachable(allocator, data.envelope.nodes, packagePolicy,
                                        input.hasBase, data.budget);
        !reachable) {
        return Output::Failure(reachable.Error());
    }

    ConstructionContext transaction(registry, access);
    if (auto created = stage.CreateObjects(transaction, options.baseOuter); !created) {
        return Output::Failure(created.Error());
    }

    if (auto applied = stage.ApplyFields(transaction); !applied) {
        return Output::Failure(applied.Error());
    }

    if (auto checked = stage.CheckRequired(transaction); !checked) {
        return Output::Failure(checked.Error());
    }

    if (options.prepare) {
        if (auto prepared = options.prepare(transaction, stage.Nodes(), options.prepareUser);
            !prepared) {
            return Output::Failure(prepared.Error());
        }
    }

    // 노드 1이 트랜잭션의 첫 객체라 Commit()의 root다. 실패하면 소멸자가 rollback한다.
    return transaction.Commit();
}

} // namespace rocks

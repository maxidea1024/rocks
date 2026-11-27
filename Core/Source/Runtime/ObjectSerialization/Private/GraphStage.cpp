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
    // Name intern(Docs/Design/ReflectionExtensions.ko.md 4.1절)도 생성 전의 사전 확인 몫이다 — pool
    // 한도·pool 없음 실패는 객체를 만들지 않았으므로 트랜잭션을 고정하지 않는다(intern한 entry는
    // pool에 남는다). TryIntern은 pool allocator callback(사용자 코드)을 부르므로, callback이
    // Registry에 재진입해 노드 이름을 점유하면 사전 확인이 무효가 되어 뒤의 Create가
    // AlreadyExists로 실패하고 트랜잭션이 고정된다(계약 위반 — 손상·부분 공개는 없다).
    Array<ObjectHandle> handles(allocator_);
    auto created = CheckCreatePreconditions(allocator_, transaction, policy_, nodes, hasBase_,
                                            base_, baseOuter);
    if (created) {
        created = InternNodeNames(allocator_, registry_.Names(), nodes);
    }

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

Span<const GraphExternalUse> ObjectGraphStage::ExternalUses() const noexcept {
    return data_ ? data_->externalUses.View() : Span<const GraphExternalUse>{};
}

Result<void> ObjectGraphStage::ApplyFields(ConstructionContext& transaction,
                                           Span<const ObjectHandle> imports) noexcept {
    if (auto callable = CheckCall(Phase::Created); !callable) {
        return callable;
    }

    if (auto same = CheckSameTransaction(transaction); !same) {
        return same;
    }

    // imports 확인(쓰기 전 전부)과 쓰기의 실패는 모두 객체를 만든 뒤의 실패라 트랜잭션을 그 오류로
    // 고정한다(Commit이 실패하고 소멸자가 되돌린다 — 부분 적용 객체를 공개하지 않는다). imports는
    // 호출 검사가 아니라 작업의 입력이므로 개수 불일치도 같다.
    phase_ = Phase::Running;
    auto applied = CheckImports(allocator_, transaction, *data_, imports);
    if (applied) {
        applied = ApplyNodeFields(allocator_, transaction, data_->envelope.nodes, options_, imports,
                                  data_->pendingEvents);
    }

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

    // 외부 참조 대상은 생존 객체이고 선언 타입은 그 실제 타입이다(접근·종료 오류는 AcquireType의
    // 것). 파괴 요청·cutoff 대상은 Registry AcquireType이 Destroyed로 알리지만 단계형 2b
    // (ConstructionContext::AcquireType — staged 참조 대상 규칙)와 같은 코드가 되게 StaleHandle로
    // 바꾼다. 크기 대조(= externalCount)는 Validate의 노드 출처 단계가 한다. 실제 타입 lease는 생성
    // 전 판정에 다시 쓴다.
    Array<TypeId> importTypes(allocator);
    Array<TypeLease> importLeases(allocator);
    if (auto reserved = importTypes.TryReserve(options.imports.Size()); !reserved) {
        return Output::Failure(reserved.Error());
    }

    if (auto reserved = importLeases.TryReserve(options.imports.Size()); !reserved) {
        return Output::Failure(reserved.Error());
    }

    for (ObjectHandle target : options.imports) {
        auto type = registry.AcquireType(access, target);
        if (!type) {
            return Output::Failure(type.Error() == ErrorCode::Destroyed ? ErrorCode::StaleHandle
                                                                        : type.Error());
        }

        if (!importTypes.TryPushBack(type.Value().Id()) ||
            !importLeases.TryPushBack(Move(type.Value()))) {
            platform::Fatal("Object graph import type reservation failed");
        }
    }

    input.importTypes = importTypes.View();
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

    // imports의 실제 타입을 이미 알므로 1단계가 기록한 (import, 기대 형식) 쌍을 생성자 전에
    // 판정한다(2b와 같은 판정 — 기대 형식 불만족 TypeMismatch가 객체를 만들기 전에 난다).
    if (auto targets = CheckImportTargets(data, importLeases.View()); !targets) {
        return Output::Failure(targets.Error());
    }

    ConstructionContext transaction(registry, access);
    if (auto created = stage.CreateObjects(transaction, options.baseOuter); !created) {
        return Output::Failure(created.Error());
    }

    if (auto applied = stage.ApplyFields(transaction, options.imports); !applied) {
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
    auto committed = transaction.Commit();
    if (!committed) {
        return committed;
    }

    // 게시한 객체에 이벤트 바인딩을 다시 만든다(Docs/Design/EventReflection.ko.md ED21 — 되돌릴 수
    // 없으므로 실패는 진단만 낸다).
    (void)stage.BindEvents();
    return committed;
}

Result<usize> ObjectGraphStage::BindEvents() noexcept {
    if (auto callable = CheckCall(Phase::Checked); !callable) {
        return Result<usize>::Failure(callable.Error());
    }

    if (eventsBound_) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // 객체가 게시되기 전이면(Commit 전) 바인딩을 만들 수 없다 — 순서 오류다.
    if (!nodes_.IsEmpty()) {
        auto lifecycle = registry_.InspectLifecycle(access_, nodes_[0]);
        if (!lifecycle) {
            return Result<usize>::Failure(lifecycle.Error() == ErrorCode::StaleHandle
                                              ? ErrorCode::InvalidArgument
                                              : lifecycle.Error());
        }
    }

    eventsBound_ = true;
    const usize bound = graph_detail::BindPendingEvents(
        registry_, access_, nodes_.View(), data_->pendingEvents.View(), options_.hooks);
    data_->pendingEvents.Clear();
    return Result<usize>::Success(bound);
}

} // namespace rocks

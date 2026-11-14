#include <Object/ConstructionContext.h>

namespace rocks {

ConstructionContext::ConstructionContext(ObjectRegistry& registry,
                                         const ObjectAccessContext& access) noexcept
    : registry_(registry), access_(access), staged_(registry.allocator_) {
    auto transaction = registry_.BeginConstruction(access_);
    if (transaction) {
        transaction_ = transaction.Value();
    } else {
        error_ = transaction.Error();
        finished_ = true;
    }
}

ConstructionContext::~ConstructionContext() {
    Rollback();
}

Result<void> ConstructionContext::CheckOpen() const noexcept {
    // Create·Commit·DescribeOuter 공통 규칙: 고정된 오류(시작 실패 또는 실패한 Create의 오류)가
    // 먼저이고, 그 밖의 끝난 트랜잭션은 InvalidArgument다.
    if (error_ != ErrorCode::None) {
        return Result<void>::Failure(error_);
    }

    if (finished_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> ConstructionContext::Commit(Span<const ObjectHandle> roots,
                                         Span<RootRef<Object>> out) noexcept {
    if (auto state = CheckOpen(); !state) {
        return state;
    }

    if (out.Size() != roots.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const RootRef<Object>& held : out) {
        if (held.IsValid()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    auto result =
        registry_.Commit(access_, transaction_, {staged_.Data(), staged_.Size()}, roots, out);
    if (result) {
        finished_ = true;
        staged_.Clear();
    }

    return result;
}

Result<RootRef<Object>> ConstructionContext::Commit() noexcept {
    if (auto state = CheckOpen(); !state) {
        return Result<RootRef<Object>>::Failure(state.Error());
    }

    if (staged_.Size() == 0) {
        return Result<RootRef<Object>>::Failure(ErrorCode::InvalidArgument);
    }

    const ObjectHandle first[] = {staged_[0]};
    RootRef<Object> held[1];
    auto committed = Commit(first, held);
    if (!committed) {
        return Result<RootRef<Object>>::Failure(committed.Error());
    }

    return Result<RootRef<Object>>::Success(Move(held[0]));
}

void ConstructionContext::Rollback() noexcept {
    if (!finished_) {
        registry_.Rollback(transaction_, {staged_.Data(), staged_.Size()});
        finished_ = true;
        staged_.Clear();
    }
}

Result<OuterSummary> ConstructionContext::DescribeOuter(ObjectHandle outer) noexcept {
    if (auto state = CheckOpen(); !state) {
        return Result<OuterSummary>::Failure(state.Error());
    }

    return registry_.DescribeOuterChecked(access_, outer, transaction_);
}

Result<ObjectHandle> ConstructionContext::CreateImpl(TypeId type, const void* nativeType,
                                                     ObjectHandle outer, StringView name) noexcept {
    if (auto state = CheckOpen(); !state) {
        return Result<ObjectHandle>::Failure(state.Error());
    }

    // 객체 생성에 앞서 롤백 저널 공간을 확보한다. 생성 성공 후 저널 OOM으로 객체를 잃지 않는다.
    // 가득 찼을 때만 두 배로 늘려(기하 성장) 생성마다 저널 전체를 다시 할당·복사하지 않는다.
    if (staged_.Size() == staged_.Capacity()) {
        const usize current = staged_.Capacity();
        const usize grown = current > static_cast<usize>(-1) / 2 ? staged_.Size() + 1
                            : current * 2 > staged_.Size() + 1   ? current * 2
                                                                 : staged_.Size() + 1;
        auto reserve = staged_.TryReserve(grown);
        if (!reserve) {
            error_ = reserve.Error();
            return Result<ObjectHandle>::Failure(error_);
        }
    }

    auto object = registry_.CreatePending(access_, type, transaction_, nativeType, outer, name);
    if (!object) {
        error_ = object.Error();
        return Result<ObjectHandle>::Failure(error_);
    }

    auto stored = staged_.TryPushBack(object.Value());
    if (!stored) {
        platform::Fatal("Reserved construction journal unexpectedly failed");
    }

    return object;
}

Result<RootRef<Object>> ObjectRegistry::NewRoot(const ObjectAccessContext& access, TypeId type,
                                                const void* nativeType, ObjectHandle outer,
                                                StringView name) noexcept {
    ConstructionContext transaction(*this, access);
    auto pending = transaction.CreateImpl(type, nativeType, outer, name);
    if (!pending) {
        return Result<RootRef<Object>>::Failure(pending.Error());
    }
    return transaction.Commit();
}

} // namespace rocks

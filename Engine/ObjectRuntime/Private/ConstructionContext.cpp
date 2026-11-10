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

Result<RootRef<Object>> ConstructionContext::Commit() noexcept {
    if (finished_ || error_ != ErrorCode::None) {
        return Result<RootRef<Object>>::Failure(
            error_ != ErrorCode::None ? error_ : ErrorCode::InvalidArgument);
    }
    auto result = registry_.Commit(access_, transaction_, {staged_.Data(), staged_.Size()});
    if (result) {
        finished_ = true;
        staged_.Clear();
    }
    return result;
}

void ConstructionContext::Rollback() noexcept {
    if (!finished_) {
        registry_.Rollback(transaction_, {staged_.Data(), staged_.Size()});
        finished_ = true;
        staged_.Clear();
    }
}

Result<ObjectHandle> ConstructionContext::CreateImpl(TypeId type, const void* nativeType) noexcept {
    if (error_ != ErrorCode::None || finished_) {
        return Result<ObjectHandle>::Failure(finished_ ? ErrorCode::InvalidArgument : error_);
    }
    // 객체 생성에 앞서 롤백 저널 공간을 확보한다. 생성 성공 후 저널 OOM으로 객체를 잃지 않는다.
    auto reserve = staged_.TryReserve(staged_.Size() + 1);
    if (!reserve) {
        error_ = reserve.Error();
        return Result<ObjectHandle>::Failure(error_);
    }
    auto object = registry_.CreatePending(access_, type, transaction_, nativeType);
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
                                                const void* nativeType) noexcept {
    ConstructionContext transaction(*this, access);
    auto pending = transaction.CreateImpl(type, nativeType);
    if (!pending) {
        return Result<RootRef<Object>>::Failure(pending.Error());
    }
    return transaction.Commit();
}

} // namespace rocks

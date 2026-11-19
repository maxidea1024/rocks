#include <Object/ModuleTypeRegistrations.h>

#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks {

ModuleTypeRegistrations::ModuleTypeRegistrations(TypeRegistry& registry,
                                                 Allocator& allocator) noexcept
    : registry_(&registry), allocator_(&allocator), entries_(allocator) {
}

usize ModuleTypeRegistrations::IndexOf(StringView module) const noexcept {
    for (usize index = 0; index < entries_.Size(); ++index) {
        if (entries_[index].module == module) {
            return index;
        }
    }

    return IndexNone;
}

bool ModuleTypeRegistrations::Contains(StringView module) const noexcept {
    return IndexOf(module) != IndexNone;
}

Result<void> ModuleTypeRegistrations::Register(StringView module,
                                               ModuleEntry::RegisterTypesFunction registerTypes,
                                               const ModuleLease& lease) noexcept {
    if (registerTypes == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (IndexOf(module) != IndexNone) {
        return Result<void>::Failure(ErrorCode::AlreadyRegistered);
    }

    // 등록한 뒤 표를 못 늘려 기록을 잃지 않게 자리를 먼저 잡는다.
    if (auto reserved = entries_.TryEnsureCapacity(entries_.Size() + 1); !reserved) {
        return reserved;
    }

    auto created = UniquePtr<ModuleRegistration>::TryMake(*allocator_, *registry_, *allocator_);
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    UniquePtr<ModuleRegistration> registration = Move(created.Value());
    if (auto registered = registerTypes(*registration, lease); !registered) {
        // 생성 함수는 자기 호출의 등록을 되돌렸다. 여러 함수(public·private)가 나눠 등록했으면 앞
        // 함수의 기록이 남을 수 있으므로 기록 전체를 되돌린다.
        (void)registration->Unregister();
        return registered;
    }

    if (!entries_.TryPushBack(Entry{module, Move(registration)})) {
        platform::Fatal("ModuleTypeRegistrations reserved entry push failed");
    }

    return Result<void>::Success();
}

void ModuleTypeRegistrations::Unregister(StringView module) noexcept {
    const usize index = IndexOf(module);
    if (index == IndexNone) {
        return;
    }

    // Busy(남은 객체·native 파생)면 남은 레코드를 레지스트리 종료에 맡긴다(클래스 주석).
    (void)entries_[index].registration->Unregister();
    if (!entries_.TryRemoveAt(index)) {
        platform::Fatal("ModuleTypeRegistrations entry removal failed");
    }
}

} // namespace rocks

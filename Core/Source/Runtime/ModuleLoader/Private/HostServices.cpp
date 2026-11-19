#include <ModuleLoader/HostServices.h>
#include <Platform/Fatal.h>

namespace rocks {

HostServices::HostServices(Allocator& allocator) noexcept : entries_(allocator) {
}

Result<void> HostServices::RegisterKey(const HostServiceKey& key, void* service) noexcept {
    if (FindKey(key) != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyRegistered);
    }

    return entries_.TryPushBack(Entry{&key, service});
}

bool HostServices::UnregisterKey(const HostServiceKey& key, void* service) noexcept {
    for (usize index = 0; index < entries_.Size(); ++index) {
        if (entries_[index].key == &key && entries_[index].service == service) {
            // 등록 순서는 의미가 없으므로 마지막 항목으로 메운다. 범위 안 색인이라 실패하지 않는다.
            if (!entries_.TryRemoveAtSwap(index)) {
                platform::Fatal("HostServices entry removal failed");
            }

            return true;
        }
    }

    return false;
}

void* HostServices::FindKey(const HostServiceKey& key) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.key == &key) {
            return entry.service;
        }
    }

    return nullptr;
}

} // namespace rocks

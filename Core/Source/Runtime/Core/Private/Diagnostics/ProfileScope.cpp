#include <Core/Diagnostics/ProfileScope.h>
#include <Platform/Clock.h>

namespace rocks {

ProfileScope::ProfileScope(LogBuffer& buffer, const char* name, uint32 category) noexcept
    : buffer_(buffer), start_(platform::MonotonicNanoseconds()), category_(category) {
    if (name != nullptr) {
        usize i = 0;
        while (i + 1 < sizeof(name_) && name[i] != '\0') {
            name_[i] = name[i];
            ++i;
        }
        nameTruncated_ = name[i] != '\0';
    }
    (void)buffer_.Write(LogLevel::Trace, category_, name_, DiagnosticKind::ScopeBegin, 0,
                        nameTruncated_);
}

// 계측용 소멸자는 기록 실패를 전파하지 않는다. 실제 측정 구간은 생성부터 소멸까지다.
ProfileScope::~ProfileScope() noexcept {
    (void)buffer_.Write(LogLevel::Trace, category_, name_, DiagnosticKind::ScopeEnd,
                        platform::MonotonicNanoseconds() - start_, nameTruncated_);
}
} // namespace rocks

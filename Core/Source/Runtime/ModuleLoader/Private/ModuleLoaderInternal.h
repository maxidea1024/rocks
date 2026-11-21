#pragma once

#include <ModuleLoader/ModuleReport.h>

#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>

// 모듈 로더의 cpp들이 함께 쓰는 도구다(ModuleLoader.cpp·ModuleManifest.cpp). 공개 API가 아니다.
namespace rocks::detail {

// 진입점 기호 이름을 만들 수 있는 C 식별자인지다(영문자·숫자·'_', 숫자로 시작하지 않음).
inline bool IsModuleIdentifier(StringView name) noexcept {
    if (name.IsEmpty()) {
        return false;
    }

    for (usize index = 0; index < name.ByteCount(); ++index) {
        const char value = name.Data()[index];
        const bool letter = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
        const bool digit = value >= '0' && value <= '9';
        if (!letter && value != '_' && !(digit && index != 0)) {
            return false;
        }
    }

    return true;
}

// report 문자열 칸 하나에 쓴다. 넘치면 잘리고 report.truncated를 켠다.
template <usize Capacity, typename... Args>
void WriteReportText(ModuleLoadReport& report, char (&field)[Capacity], StringView format,
                     const Args&... args) noexcept {
    FixedStringBuilder text{Span<char>(field)};
    text.Clear();
    (void)text.TryAppendFormat(format, args...);
    if (text.Truncated()) {
        report.truncated = true;
    }
}

inline void WriteReportName(ModuleLoadReport& report,
                            char (&field)[ModuleLoadReport::ModuleNameBytes],
                            StringView name) noexcept {
    FixedStringBuilder text{Span<char>(field)};
    text.Clear();
    (void)text.TryAppend(name);
    if (text.Truncated()) {
        report.truncated = true;
    }
}

} // namespace rocks::detail

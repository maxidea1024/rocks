#pragma once

#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Localization/Text.h>

// 받는 쪽이 문구를 줄로 낼 때 쓰는 도우미다(Docs/Design/ProgressReporting 9.2절). 원문은 invalid
// CatalogSnapshot으로 푼 fallback 원문이고, 지역화 id는 Invariant·Localized 형태에서만 있다.
namespace rocks::detail {

// text의 원문을 out에 담는다(유효하지 않은 Text면 비운다). 실패하면 out을 비우고 false다.
bool ProgressSourceInto(Allocator& allocator, const Text& text, String& out) noexcept;

// text의 지역화 id(Guid 문자열)와 원문 판을 담는다. Localized가 아니면 id는 비고 판은 0이다(Format·
// Integer 이력은 원문만 — 한계 15.6).
void ProgressPersistentId(const Text& text, StringBuilderBase& id, uint64& revision) noexcept;

// value를 1,234,567처럼 세 자리마다 쉼표로 붙인다(명령줄 진행 줄).
void ProgressAppendGrouped(StringBuilderBase& out, uint64 value) noexcept;

} // namespace rocks::detail

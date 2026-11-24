#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <TextLayout/TextShaping.h>

// ICU 경계·BiDi다(Docs/Design/Font 6.4·6.8절). FreeTypeBackend.cpp가 TextBackend 계약으로 내놓는다.
namespace rocks::text::detail {

Result<void> FindBoundaries(TextBoundaryKind kind, StringView utf8, StringView culture,
                            Array<usize>& out) noexcept;

Result<void> ResolveBidi(StringView utf8, TextBaseDirection base, Array<BidiRun>& out) noexcept;

} // namespace rocks::text::detail

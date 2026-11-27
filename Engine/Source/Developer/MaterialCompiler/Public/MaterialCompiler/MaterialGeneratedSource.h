#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <MaterialCompiler/MaterialSourceLocation.h>
#include <NodeGraph/GraphDiagnostic.h>

// 생성 원문 /Engine/Generated/Material.rsh의 내용이다(Docs/Design/MaterialSystem 7.3·7.7절).
// succeeded가 거짓이면(생성할 수 없는 값 — diagnostics) text·locations는 비어 있다.
//
// 수명: allocator는 결과보다 오래 살아야 한다.
namespace rocks::material {

struct MaterialGeneratedSource {
    explicit MaterialGeneratedSource(Allocator& allocator) noexcept
        : text(allocator), locations(allocator), diagnostics(allocator) {
    }

    MaterialGeneratedSource(const MaterialGeneratedSource&) = delete;
    MaterialGeneratedSource& operator=(const MaterialGeneratedSource&) = delete;

    // UTF-8, 줄 끝 LF다.
    String text;
    // 값을 낸 줄마다 하나, 줄 번호 순이다.
    Array<MaterialSourceLocation> locations;
    nodegraph::GraphDiagnosticList diagnostics;
    bool succeeded = false;
};

} // namespace rocks::material

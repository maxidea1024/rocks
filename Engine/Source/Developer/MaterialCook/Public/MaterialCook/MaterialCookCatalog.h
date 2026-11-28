#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/NamePool.h>
#include <MaterialSchema/MaterialGraphSchema.h>
#include <MaterialSchema/MaterialNodeCatalog.h>
#include <NodeGraph/NodeTypeRegistry.h>

// 쿠킹의 머티리얼 노드 카탈로그다(Docs/Design/MaterialSystem 12.8절 MS3c): 번역(노드 정의)과 그래프
// semantic hash(노드 종류 등록부 — 5.5절)가 쓰는 엔진 노드 등록을 쿠킹 Registry의 NamePool 위에 한
// 번 만든다(머티리얼·함수 두 도메인). 처리기가 명령 동안 쥔다.
//
// 사용 예:
//   MaterialCookCatalog catalog(allocator, *registry.Names());
//   if (auto ready = catalog.Initialize(); !ready) { ... }
//   input.catalog = catalog.Catalog();
//
// 스레드: 만든 뒤 읽기만 하면 여러 스레드가 함께 쓸 수 있다(등록부 규칙).
namespace rocks {

class ROCKS_MATERIALCOOK_API MaterialCookCatalog {
public:
    MaterialCookCatalog(Allocator& allocator, NamePool& names) noexcept;

    MaterialCookCatalog(const MaterialCookCatalog&) = delete;
    MaterialCookCatalog& operator=(const MaterialCookCatalog&) = delete;

    // 등록부를 열고 두 도메인 schema와 엔진 노드 묶음을 등록한다. 오류: 등록 오류, 할당 실패.
    Result<void> Initialize() noexcept;

    const material::MaterialNodeCatalog& Catalog() const noexcept {
        return catalog_;
    }

    const nodegraph::NodeTypeRegistry& Registry() const noexcept {
        return registry_;
    }

    NamePool& Names() const noexcept {
        return *names_;
    }

private:
    NamePool* names_;
    nodegraph::NodeTypeRegistry registry_;
    material::MaterialNodeCatalog catalog_;
    material::MaterialGraphSchema materialSchema_;
    material::MaterialGraphSchema functionSchema_;
};

} // namespace rocks

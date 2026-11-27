#pragma once

#include <Core/Result.h>
#include <MaterialCompiler/MaterialTranslation.h>
#include <MaterialCompiler/MaterialTranslationInput.h>

// 머티리얼 그래프 → IR 번역이다(Docs/Design/MaterialSystem 6.3·6.8절, 단계 MS2a2). 주 그래프의 출력
// 노드 핀에서 연결을 거슬러 노드마다 lowering 함수(UE IR Build와 같은 구조, 결과 값은 UE 기본
// 번역기와 같다)를 부르고, 영역·블렌드 모드가 쓰는 핀만 진입점으로 만든다. 그래프 오류는 진단이다:
// V1·V2·V4·V5·V7·V8·V11·V12와 번역 코드(MaterialDiagnosticCode).
//
// 결정성: 같은 문서는 같은 IR(값 번호·IR hash)이다. 노드 저장 순서와 무관하다(출력 핀에서 거슬러
// 가고, 모듈이 값 번호를 다시 매긴다 — 6.6절).
// 스레드: 입력을 읽기만 하므로 서로 다른 결과로 여러 번역을 함께 돌려도 된다.
//
// 사용 예:
//   MaterialTranslation translation(allocator);
//   const MaterialTranslationInput input{document, catalog, names, traits, asset};
//   if (auto translated = TranslateMaterial(allocator, input, translation); !translated) {
//       // 할당 실패 같은 시스템 오류
//   }
//   if (!translation.succeeded) {
//       // translation.diagnostics에 오류가 있다
//   }
namespace rocks::material {

// input을 번역해 out을 채운다(out의 이전 내용은 지운다). 반환 실패는 할당·NamePool 같은 시스템
// 오류뿐이고, 그래프 오류는 out.diagnostics와 out.succeeded로 알린다. 문서 도메인이 Material이
// 아니거나 주 그래프·출력 노드가 없으면 InvalidArgument다.
ROCKS_MATERIALCOMPILER_API Result<void> TranslateMaterial(Allocator& allocator,
                                                          const MaterialTranslationInput& input,
                                                          MaterialTranslation& out) noexcept;

} // namespace rocks::material

#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <TextLayout/TextBackend.h>

// FreeType·HarfBuzz·ICU·msdfgen 글꼴 backend다(Docs/Design/Font 6.8절, ClientOnly). 전역 등록 표는
// 없다 — 호스트가 InitializeTextLibraries로 프로세스 전역 라이브러리 상태를 한 번 준비하고,
// CreateTextBackend로 만든 backend를 TextLayout 사용자(UI·도구·시험)에게 넘긴다.
//
// 사용 예:
//   if (auto ready = text::InitializeTextLibraries(icuData); !ready) { return ready.Error(); }
//   auto backend = text::CreateTextBackend({.allocator = &allocator});
//   auto face = backend.Value()->OpenFace({.data = faceData});
namespace rocks::text {

// 프로세스 전역 라이브러리 상태를 한 번 준비한다: ICU 할당 hook(u_setMemoryFunctions)과 데이터
// 패키지(udata_setCommonData). 첫 ICU·HarfBuzz 사용보다 먼저 불러야 한다. icuData는 IcuData
// 부분집합 (icudt79l.dat) byte이며 프로세스 끝까지 살아야 한다(ICU가 계속 읽는다). 같은 데이터로
// 다시 부르면 성공, 다른 데이터면 InvalidArgument, 빈 데이터·ICU 거부는 InvalidArgument다. 스레드
// 안전하다.
ROCKS_TEXTLAYOUTBACKEND_API Result<void>
InitializeTextLibraries(Span<const uint8> icuData) noexcept;

struct TextBackendOptions {
    // backend·face·FreeType 할당에 쓴다. backend보다 오래 살아야 한다.
    Allocator* allocator = nullptr;
};

// backend를 만든다. allocator가 없으면 InvalidArgument, InitializeTextLibraries 전이면 NotReady,
// FreeType library 생성 실패는 OutOfMemory다. backend와 그 face는 한 스레드에서 쓴다(6.7절).
ROCKS_TEXTLAYOUTBACKEND_API Result<UniquePtr<TextBackend>>
CreateTextBackend(const TextBackendOptions& options) noexcept;

} // namespace rocks::text

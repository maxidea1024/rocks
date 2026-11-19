#pragma once

// Core의 공유 PCH 헤더다(Docs/Design/UnityBuildPch.ko.md 8.4절). 빌드가 이 헤더를 미리 컴파일해,
// 모듈 PCH가 없고 Core에 닿는 모듈의 C++ 번역 단위에 강제 include한다(Core.Build.cs의
// SharedPchHeader). 더 위의 제공 모듈(ObjectRuntime)에 닿는 모듈은 그 모듈의 공유 PCH를 쓴다.
//
// 사용 규칙:
// - 소스와 헤더는 이 파일을 include하지 않는다(정책 검사가 막는다 — 위 제공 모듈의 공유 PCH 헤더만
//   예외다). 필요한 헤더는 지금처럼 각자 include한다. PCH 없이도(--pch off, 공개 헤더 단독 컴파일
//   검사) 컴파일되어야 하기 때문이다.
// - 여기에는 Base·PlatformBase·Core의 public 헤더 가운데 이 PCH를 쓰는 번역 단위의 30% 이상이
//   include하고 잘 바뀌지 않는 것만 둔다(고른 방법과 수치는 스펙 8.4절). 이 목록의 헤더가 바뀌면
//   이 PCH를 쓰는 번역 단위가 모두 다시 컴파일되므로, 자주 바뀌는 헤더는 넣지 않는다.
// - 소비 모듈마다 다른 정의(ROCKS_MODULE_NAME, ROCKS_MODULE_TYPES, 모듈 private define)를 전처리
//   조건에 쓰는 헤더는 넣지 못한다(그 정의는 PCH 뒤에 오므로 빌드가 오류로 막는다 — 예:
//   Core/Module/ModuleEntry.h).
// - 헤더를 더하거나 뺄 때는 스펙 8.4절의 목록도 함께 고친다.

#include <Base/Checked.h>
#include <Base/Compiler.h>
#include <Base/Endian.h>
#include <Base/Guid.h>
#include <Base/NumericLimits.h>
#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Algorithms.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/ContainerDetail.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Error.h>
#include <Core/Hash.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Core/Traits.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Sync.h>

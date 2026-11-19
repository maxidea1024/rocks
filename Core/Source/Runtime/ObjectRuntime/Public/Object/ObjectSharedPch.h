#pragma once

// ObjectRuntime의 공유 PCH 헤더다(Docs/Design/UnityBuildPch.ko.md 8.4절). 빌드가 이 헤더를 미리
// 컴파일해, 모듈 PCH가 없고 ObjectRuntime에 닿는 모듈(객체 계층의 런타임 모듈과 시험 실행 파일)의
// C++ 번역 단위에 강제 include한다(ObjectRuntime.Build.cs의 SharedPchHeader).
//
// 사용 규칙은 Core/CoreSharedPch.h와 같다: 소스와 헤더는 이 파일을 include하지 않고, 목록에는 이
// PCH를 쓰는 번역 단위의 30% 이상이 include하고 잘 바뀌지 않는 헤더만 둔다.
//
// Object/Object.h와 그것이 끌어오는 헤더(Meta/TypeSchema.h, Object/ObjectRegistry.h,
// Object/RuntimeTypeOps.h 등)는 많이 쓰이지만 넣지 않았다. 지금은 자주 바뀌어서, 넣으면 그 헤더를
// 쓰지 않는 번역 단위까지 바뀔 때마다 다시 컴파일된다(스펙 8.4절). 안정되면 다시 정한다.

#include <Core/CoreSharedPch.h>

#include <Core/Assert.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Guid.h>
#include <Core/Lifetime/ModuleLease.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Optional.h>
#include <Core/Text/FormatParse.h>
#include <Core/Text/StringBuilder.h>
#include <Localization/Catalog.h>
#include <Localization/Text.h>
#include <Object/AssetId.h>
#include <Object/ObjectHandle.h>
#include <Object/ReferenceObserver.h>

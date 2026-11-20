#pragma once

#include <Base/Types.h>

// Unreal 교환 묶음 형식의 공통 상수다(Docs/Design/UnrealExchange.ko.md 3절). 묶음은 UE 쪽
// RocksExport 플러그인이 쓰고 이 플러그인의 리더가 읽는다.

namespace rocks {

// 이 리더가 받는 bundle.json의 formatVersion이다(3.4절). 다른 값은 UnsupportedBundleVersion
// 오류다(하위호환 읽기 없음).
inline constexpr uint32 UnrealExchangeBundleFormatVersion = 1;

} // namespace rocks

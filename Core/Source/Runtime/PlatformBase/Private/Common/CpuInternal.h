#pragma once

#include <Platform/Cpu.h>

namespace rocks::platform::detail {

// ARM64 Crypto Extensions(armAes·armPmull·armSha2)를 OS에 물어 채운다. ARM64 빌드에서만 부른다.
// 구현: Apple은 sysctl(Apple/AppleCpu.cpp, sysctl 이름이 없는 구버전 OS는 대상 기준 기능 매크로로
// 판정), Android는 getauxval(Android/AndroidCpu.cpp), Windows는
// IsProcessorFeaturePresent(Win64/Cpu.cpp, 미검증). OS가 알려 주지 않는 기능은 false로 둔다(기능이
// 없는 것으로 다룬다).
void DetectArm64CryptoFeatures(CpuFeatures& features) noexcept;

} // namespace rocks::platform::detail

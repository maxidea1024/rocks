#pragma once

#include <Platform/Cpu.h>

namespace rocks::platform::detail {

// ARM64 Crypto Extensions(armAes·armPmull·armSha2)를 OS에 물어 채운다. ARM64 빌드에서만 부른다.
// 구현: Apple은 sysctl(Apple/AppleCpu.cpp), Android는 getauxval(Android/AndroidCpu.cpp).
// 감지할 수 없으면 false로 둔다(기능이 없는 것으로 다룬다).
void DetectArm64CryptoFeatures(CpuFeatures& features) noexcept;

} // namespace rocks::platform::detail

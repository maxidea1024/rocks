#pragma once

#include <Base/Types.h>
#include <MaterialCompiler/MaterialBindlessMode.h>
#include <MaterialCompiler/MaterialQualityLevel.h>

// 번역 대상이다(Docs/Design/MaterialSystem 6.9절 MS2b3, 19.3.4절). 번역 때 고르는 스위치(품질·형식
// 사실·샘플러 예산·bindless·프로필 기능·컴파일러 종류)가 읽는 값이다. 기본값은 UE 기본(품질 없음,
// PC 형식, 샘플러 16, bindless 꺼짐, 기능 꺼짐, 보통 컴파일러)이다. 번역은 읽은 항목을
// MaterialTranslation.consultedTarget에 기록한다.
//
// 사용 예:
//   MaterialTranslationTarget target;
//   target.quality = MaterialQualityLevel::High;
//   target.isPc = true;
namespace rocks::material {

// UE 형식 샘플러 상한 기본값이다(DataDrivenShaderPlatformInfo.cpp:163).
inline constexpr uint32 DefaultMaterialMaxSamplers = 16;

struct MaterialTranslationTarget {
    MaterialQualityLevel quality = MaterialQualityLevel::None;
    // 형식 사실이다(UE DDSPI IsMobile·IsPC·IsConsole).
    bool isMobile = false;
    bool isPc = true;
    bool isConsole = false;
    // 형식의 샘플러 상한이다(Sampler Budget Switch).
    uint32 maxSamplers = DefaultMaterialMaxSamplers;
    MaterialBindlessMode bindless = MaterialBindlessMode::Disabled;
    // 프로필 기능이다.
    bool distanceFields = false;
    bool virtualTexturing = false;
    bool rayTracing = false;
    bool virtualizedGeometry = false;
    // 컴파일러 종류다(UE 머티리얼 프록시 굽기·Lightmass 컴파일러).
    bool proxyBake = false;
    bool lightBake = false;
    // Low 품질을 모바일 입력으로 흉내 낸다(UE r.Material.SimulateMobileForLowQuality).
    bool simulateMobileForLowQuality = false;
};

} // namespace rocks::material

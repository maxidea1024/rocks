#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 후처리 설정의 방식 enum이다(Docs/Design/EngineRuntime 27.5절 — UE Scene.h).
namespace rocks {

// 흰색 균형 방식이다(UE ETemperatureMethod).
ROCKS_ENUM()
enum class TemperatureMethod : uint8 {
    WhiteBalance,
    ColorTemperature
};

// 톤 매핑 방식이다(UE ETonemappingMethod).
ROCKS_ENUM()
enum class TonemappingMethod : uint8 {
    Filmic,
    StandardAces
};

// bloom 방식이다(UE EBloomMethod — BM_SOG "Standard"·BM_FFT "Convolution").
ROCKS_ENUM()
enum class BloomMethod : uint8 {
    Standard,
    Convolution
};

// 자동 노출 방식이다(UE EAutoExposureMethod).
ROCKS_ENUM()
enum class AutoExposureMethod : uint8 {
    Histogram,
    Basic,
    Manual
};

} // namespace rocks

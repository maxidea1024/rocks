#include <Graphics/DeviceTypes.h>

namespace rocks::graphics {

namespace {

constexpr uint64 NanosecondsPerSecond = 1000000000;

} // namespace

uint64 TicksToNanoseconds(uint64 ticks, uint64 frequency) noexcept {
    if (frequency == 0) {
        return 0;
    }

    // 몫·나머지로 나눠 중간 곱의 overflow를 피한다.
    const uint64 seconds = ticks / frequency;
    const uint64 remainder = ticks % frequency;
    return seconds * NanosecondsPerSecond +
           static_cast<uint64>(static_cast<double>(remainder) *
                               static_cast<double>(NanosecondsPerSecond) /
                               static_cast<double>(frequency));
}

uint64 GpuTicksToCpuNanoseconds(const ClockCalibration& calibration, uint64 ticks) noexcept {
    if (calibration.gpuFrequency == 0) {
        return calibration.cpuNanoseconds;
    }

    if (ticks >= calibration.gpuTicks) {
        return calibration.cpuNanoseconds +
               TicksToNanoseconds(ticks - calibration.gpuTicks, calibration.gpuFrequency);
    }

    const uint64 before =
        TicksToNanoseconds(calibration.gpuTicks - ticks, calibration.gpuFrequency);
    return before < calibration.cpuNanoseconds ? calibration.cpuNanoseconds - before : 0;
}

StringView ClipDebugMarkerName(StringView name) noexcept {
    usize bytes = name.ByteCount();
    if (bytes <= MaxDebugMarkerNameBytes) {
        return name;
    }

    bytes = MaxDebugMarkerNameBytes;
    // 이어지는 byte(10xxxxxx) 앞에서 자르지 않도록 글자 시작까지 물린다.
    const char* data = name.Data();
    while (bytes != 0 && (static_cast<uint8>(data[bytes]) & 0xC0u) == 0x80u) {
        --bytes;
    }

    return StringView(data, bytes);
}

} // namespace rocks::graphics

#include "D3D12DeviceImpl.h"

#include <Platform/Win64/WideString.h>

namespace rocks::graphics::d3d12 {

usize D3D12Device::CollectTimestamps(Span<TimestampSample> out) noexcept {
    if (!ownership_.IsOwner()) {
        return 0;
    }
    platform::LockGuard lock(gate_);
    if (timestampHeap_.Get() == nullptr) {
        return 0;
    }
    HarvestTimestamps();
    return timestamps_.Collect(out);
}

Result<ClockCalibration> D3D12Device::CalibrateClocks() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<ClockCalibration>::Failure(owner.Error());
    }
    if (timestampHeap_.Get() == nullptr) {
        return Result<ClockCalibration>::Failure(ErrorCode::Unsupported);
    }
    if (timeline_.IsLost()) {
        return Result<ClockCalibration>::Failure(ErrorCode::Destroyed);
    }
    // CPU 쪽 값은 QueryPerformanceCounter tick이다. platform::MonotonicNanoseconds와 같은
    // 시계이므로 같은 주파수로 ns로 옮긴다.
    UINT64 gpu = 0;
    UINT64 cpu = 0;
    LARGE_INTEGER frequency{};
    const HRESULT calibrated = queue_->GetClockCalibration(&gpu, &cpu);
    if (FAILED(calibrated) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        DetectLost(calibrated);
        return FailWith<ClockCalibration>("GetClockCalibration failed");
    }
    ClockCalibration calibration;
    calibration.gpuTicks = gpu;
    calibration.cpuNanoseconds = TicksToNanoseconds(cpu, static_cast<uint64>(frequency.QuadPart));
    calibration.gpuFrequency = timestampFrequency_;
    // QPC는 platform::MonotonicTicks와 같은 원천이다(PlatformBase Win64).
    calibration.cpuTicks = cpu;
    // GPU·CPU 값을 한 호출이 함께 읽으므로 오차는 CPU tick 하나의 길이로 둔다.
    calibration.uncertaintyNanoseconds =
        TicksToNanoseconds(1, static_cast<uint64>(frequency.QuadPart));
    return Result<ClockCalibration>::Success(calibration);
}

// timestamp query heap과 resolve 대상 readback 버퍼를 만든다(GX9). queue가 timestamp 주파수를
// 주지 않거나 만들 수 없으면 timestamp를 "없음"으로 공개한다(장치 생성은 실패하지 않는다).
void D3D12Device::InitializeTimestamps() noexcept {
    UINT64 frequency = 0;
    if (FAILED(queue_->GetTimestampFrequency(&frequency)) || frequency == 0) {
        return;
    }
    constexpr uint32 Count = TimestampFrameSlots * MaxTimestampsPerFrame;
    D3D12_QUERY_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = Count;
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC buffer = BufferResource(uint64{Count} * sizeof(uint64));
    ComRef<ID3D12QueryHeap> queries;
    ComRef<ID3D12Resource> readback;
    if (FAILED(device_->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(queries.Out()))) ||
        FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(readback.Out()))) ||
        !timestamps_.Reset(TimestampFrameSlots, MaxTimestampsPerFrame)) {
        return;
    }
    timestampHeap_ = Move(queries);
    timestampReadback_ = Move(readback);
    timestampSource_.buffer = timestampReadback_.Get();
    timestampFrequency_ = frequency;
}

// GPU가 끝낸 프레임의 timestamp를 회수한다(잠금 안). 장치가 제거되었으면 값이 없으므로 읽지
// 않는다.
void D3D12Device::HarvestTimestamps() noexcept {
    const uint64 completed = fence_->GetCompletedValue();
    if (completed == D3D12FenceSource::RemovedFenceValue) {
        return;
    }
    timestamps_.Harvest(completed, timestampSource_);
}

// 프레임 끝에 열린 marker 구간을 닫는다(잠금 안).
void D3D12Device::CloseMarkers() noexcept {
    for (; markerDepth_ != 0; --markerDepth_) {
        list_->EndEvent();
    }
}

Result<uint64> D3D12Device::WriteTimestamp() noexcept {
    platform::LockGuard lock(gate_);
    if (timestampHeap_.Get() == nullptr) {
        return Result<uint64>::Failure(ErrorCode::Unsupported);
    }
    if (!recording_) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }
    HarvestTimestamps();
    auto write = timestamps_.Allocate();
    if (!write) {
        return Result<uint64>::Failure(write.Error());
    }
    list_->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, write.Value().index);
    return Result<uint64>::Success(write.Value().query);
}

// PIX가 읽는 이벤트 형식(metadata 0 = UTF-16 문자열)으로 command list에 구간을 기록한다.
void D3D12Device::PushMarker(StringView name) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return;
    }
    // UTF-8 n byte는 UTF-16 n 단위를 넘지 않으므로 잘리지 않는다. 잘못된 UTF-8이면 빈 이름이다.
    const StringView clipped = ClipDebugMarkerName(name);
    wchar_t text[MaxDebugMarkerNameBytes + 1];
    const platform::windows::TextConversion converted = platform::windows::ConvertUtf8ToWide(
        clipped.Data(), clipped.ByteCount(), text, MaxDebugMarkerNameBytes + 1,
        platform::windows::TextFit::Truncate);
    constexpr UINT UnicodeEventMetadata = 0;
    list_->BeginEvent(UnicodeEventMetadata, text,
                      static_cast<UINT>((converted.length + 1) * sizeof(wchar_t)));
    ++markerDepth_;
}

void D3D12Device::PopMarker() noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_ || markerDepth_ == 0) {
        ++ignoredDraws_;
        return;
    }
    list_->EndEvent();
    --markerDepth_;
}

} // namespace rocks::graphics::d3d12

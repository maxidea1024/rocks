#include "D3D11DeviceImpl.h"

#include <Platform/Clock.h>

namespace rocks::graphics::d3d11 {

TimestampReadState QueryTimestampSource::ReadTimestamps(uint32 first, uint32 count,
                                                        uint64* ticks) noexcept {
    // slot 하나가 제출 하나이므로 구간의 slot은 첫 칸으로 정해진다.
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT interval = {};
    HRESULT result = context->GetData(disjoint[first / MaxTimestampsPerFrame].Get(), &interval,
                                      sizeof(interval), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (result == S_FALSE) {
        return TimestampReadState::NotReady;
    }

    // 끊김, 공개한 주파수와 다른 구간, 읽기 실패는 모두 값을 쓸 수 없다(추정하지 않는다).
    if (FAILED(result) || interval.Disjoint || interval.Frequency != frequency) {
        return TimestampReadState::Disjoint;
    }

    for (uint32 index = 0; index < count; ++index) {
        result = context->GetData(stamps[first + index].Get(), &ticks[index], sizeof(uint64),
                                  D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_FALSE) {
            return TimestampReadState::NotReady;
        }

        if (FAILED(result)) {
            return TimestampReadState::Disjoint;
        }
    }

    return TimestampReadState::Ready;
}

// query 객체를 만들고 GPU timestamp 주파수를 한 번 잰다(GX9). 만들 수 없거나 주파수가 0이면
// timestamp를 "없음"으로 공개한다(장치 생성은 실패하지 않는다).
void D3D11Device::InitializeTimestamps() noexcept {
    D3D11_QUERY_DESC disjointDesc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC timestampDesc = {D3D11_QUERY_TIMESTAMP, 0};
    D3D11_QUERY_DESC eventDesc = {D3D11_QUERY_EVENT, 0};
    for (ComRef<ID3D11Query>& query : disjointQueries_) {
        if (FAILED(device_->CreateQuery(&disjointDesc, query.Out()))) {
            return;
        }
    }

    for (ComRef<ID3D11Query>& query : timestampQueries_) {
        if (FAILED(device_->CreateQuery(&timestampDesc, query.Out()))) {
            return;
        }
    }

    if (FAILED(device_->CreateQuery(&disjointDesc, calibrationDisjoint_.Out())) ||
        FAILED(device_->CreateQuery(&timestampDesc, calibrationTimestamp_.Out())) ||
        FAILED(device_->CreateQuery(&eventDesc, calibrationEvent_.Out()))) {
        return;
    }

    // 빈 disjoint 구간 하나로 주파수를 읽는다. 끝난 뒤에 읽도록 fence로 기다린다.
    immediate_->Begin(calibrationDisjoint_.Get());
    immediate_->End(calibrationDisjoint_.Get());
    auto signaled = SignalQueue();
    if (!signaled || !WaitFenceOwner(signaled.Value())) {
        return;
    }

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT interval = {};
    if (PollQuery(calibrationDisjoint_.Get(), &interval, sizeof(interval)) != S_OK ||
        interval.Frequency == 0 || !timestamps_.Reset(TimestampFrameSlots, MaxTimestampsPerFrame)) {
        return;
    }

    timestampFrequency_ = interval.Frequency;
    timestampSource_.context = immediate_.Get();
    timestampSource_.disjoint = disjointQueries_;
    timestampSource_.stamps = timestampQueries_;
    timestampSource_.frequency = timestampFrequency_;
}

HRESULT D3D11Device::PollQuery(ID3D11Query* query, void* data, UINT size) noexcept {
    for (;;) {
        const HRESULT result = immediate_->GetData(query, data, size, 0);
        if (result != S_FALSE) {
            if (FAILED(result)) {
                DetectLost(result);
            }

            return result;
        }
    }
}

// GPU가 끝낸 프레임의 timestamp를 회수한다(잠금 안). 장치가 제거되었으면 값이 없으므로 읽지 않는다.
void D3D11Device::HarvestTimestamps() noexcept {
    const uint64 completed = CompletedFence();
    if (completed == RemovedFenceValue) {
        return;
    }

    timestamps_.Harvest(completed, timestampSource_);
}

usize D3D11Device::CollectTimestamps(Span<TimestampSample> out) noexcept {
    if (!ownership_.IsOwner()) {
        return 0;
    }

    platform::LockGuard lock(gate_);
    if (timestampFrequency_ == 0) {
        return 0;
    }

    HarvestTimestamps();
    return timestamps_.Collect(out);
}

// D3D11에는 queue clock calibration이 없다. 제출한 GPU 작업이 끝나기를 기다린 뒤 CPU 시각 t0을
// 읽고, timestamp(disjoint로 감쌈)와 event query를 넣어 내보낸 다음 event가 끝난 것을 본 CPU 시각
// t1을 읽는다. timestamp는 t0과 t1 사이에 찍혔으므로 가운데를 같은 순간으로, 반폭을 오차로 둔다.
Result<ClockCalibration> D3D11Device::CalibrateClocks() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<ClockCalibration>::Failure(owner.Error());
    }

    if (timestampFrequency_ == 0) {
        return Result<ClockCalibration>::Failure(ErrorCode::Unsupported);
    }

    if (timeline_.IsLost()) {
        return Result<ClockCalibration>::Failure(ErrorCode::Destroyed);
    }

    platform::LockGuard lock(gate_);
    // 앞 작업이 끝나야 timestamp가 내보낸 직후에 찍힌다(기록 중인 프레임은 아직 제출 전이다).
    auto idle = WaitFenceOwner(fenceValue_);
    if (!idle) {
        return Result<ClockCalibration>::Failure(idle.Error());
    }

    const uint64 cpuFrequency = platform::MonotonicTickFrequency();
    for (uint32 attempt = 0; attempt < MaxCalibrationAttempts; ++attempt) {
        const uint64 before = platform::MonotonicTicks();
        immediate_->Begin(calibrationDisjoint_.Get());
        immediate_->End(calibrationTimestamp_.Get());
        immediate_->End(calibrationDisjoint_.Get());
        immediate_->End(calibrationEvent_.Get());
        immediate_->Flush();
        uint64 after = before;
        for (;;) {
            BOOL done = FALSE;
            const HRESULT result =
                immediate_->GetData(calibrationEvent_.Get(), &done, sizeof(done), 0);
            after = platform::MonotonicTicks();
            if (FAILED(result)) {
                return FailWith<ClockCalibration>("GetData (calibration event)", result);
            }

            if (result == S_OK && done) {
                break;
            }
        }

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT interval = {};
        HRESULT result = PollQuery(calibrationDisjoint_.Get(), &interval, sizeof(interval));
        if (FAILED(result)) {
            return FailWith<ClockCalibration>("GetData (calibration disjoint)", result);
        }

        if (interval.Disjoint || interval.Frequency != timestampFrequency_) {
            continue;
        }

        UINT64 gpu = 0;
        result = PollQuery(calibrationTimestamp_.Get(), &gpu, sizeof(gpu));
        if (FAILED(result)) {
            return FailWith<ClockCalibration>("GetData (calibration timestamp)", result);
        }

        ClockCalibration calibration;
        calibration.gpuTicks = gpu;
        calibration.gpuFrequency = timestampFrequency_;
        calibration.cpuTicks = before + (after - before) / 2;
        calibration.cpuNanoseconds = TicksToNanoseconds(calibration.cpuTicks, cpuFrequency);
        calibration.uncertaintyNanoseconds =
            TicksToNanoseconds(after - before, cpuFrequency) / 2 + 1;
        return Result<ClockCalibration>::Success(calibration);
    }

    return FailWith<ClockCalibration>("timestamp calibration stayed disjoint");
}

Result<uint64> D3D11Device::WriteTimestamp() noexcept {
    platform::LockGuard lock(gate_);
    if (timestampFrequency_ == 0) {
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

    deferred_->End(timestampQueries_[write.Value().index].Get());
    return Result<uint64>::Success(write.Value().query);
}

// 캡처 도구(PIX·RenderDoc)가 읽는 이벤트로 기록 context에 구간을 남긴다.
void D3D11Device::PushMarker(StringView name) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_ || !annotation_) {
        return;
    }

    const StringView clipped = ClipDebugMarkerName(name);
    wchar_t text[MaxDebugMarkerNameBytes + 1];
    int length = 0;
    if (clipped.ByteCount() != 0) {
        length =
            MultiByteToWideChar(CP_UTF8, 0, clipped.Data(), static_cast<int>(clipped.ByteCount()),
                                text, static_cast<int>(MaxDebugMarkerNameBytes));
    }

    text[length > 0 ? length : 0] = L'\0';
    (void)annotation_->BeginEvent(text);
    ++markerDepth_;
}

void D3D11Device::PopMarker() noexcept {
    platform::LockGuard lock(gate_);
    if (!annotation_) {
        return; // debugMarkers가 거짓이면 아무것도 하지 않는다.
    }

    if (!recording_ || markerDepth_ == 0) {
        ++ignoredDraws_;
        return;
    }

    (void)annotation_->EndEvent();
    --markerDepth_;
}

// 프레임 끝에 열린 marker 구간을 닫는다(잠금 안).
void D3D11Device::CloseMarkers() noexcept {
    for (; markerDepth_ != 0; --markerDepth_) {
        (void)annotation_->EndEvent();
    }
}

} // namespace rocks::graphics::d3d11

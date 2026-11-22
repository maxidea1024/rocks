#pragma once

#include <Base/Types.h>
#include <Compression/Compression.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>

// 작업 공간을 만들 때 한 번 확보하고, 그 뒤 압축 호출에서 할당하지 않는 압축 문맥이다. 기록
// 경로처럼 "시작 뒤 무할당"이 필요한 곳(로그 파일 묶음 압축 등)에 쓴다.
//
// - 형식은 Zstd만이다(그 밖은 Unsupported). 매개변수(window·hash·chain·전략)를 수준과
//   maxSourceBytes로 정해 고정하고, 그 크기의 작업 공간을 ZSTD_estimateCStreamSize_usingCParams로
//   구해 한 번 확보한 뒤 ZSTD_initStaticCCtx로 만든다. 호출마다 원본 크기(frame 머리)는 기록하지만
//   매개변수는 바뀌지 않으므로 작업 공간을 다시 잡지 않는다.
// - 호출마다 독립 frame 하나를 만든다(이어 붙인 frame은 그대로 올바른 zstd 스트림이다).
// - 이동 전용이다. 한 객체는 동시에 한 스레드만 쓴다. 할당기는 객체보다 오래 살아야 한다.
//
// 사용 예:
//   auto settings = CompressionSettings::TryMake(CompressionFormat::Zstd, 3);
//   auto compressor = FixedWorkspaceCompressor::TryCreate(settings.Value(), 64 * 1024, allocator);
//   // 이후 할당 없음
//   auto written = compressor.Value().TryCompress(batch, Span<uint8>(out, outBytes));
namespace rocks {

class ROCKS_COMPRESSION_API FixedWorkspaceCompressor {
public:
    FixedWorkspaceCompressor() noexcept = default;
    ~FixedWorkspaceCompressor() noexcept;
    FixedWorkspaceCompressor(FixedWorkspaceCompressor&& other) noexcept;
    FixedWorkspaceCompressor& operator=(FixedWorkspaceCompressor&& other) noexcept;
    FixedWorkspaceCompressor(const FixedWorkspaceCompressor&) = delete;
    FixedWorkspaceCompressor& operator=(const FixedWorkspaceCompressor&) = delete;

    // 문맥을 만든다. maxSourceBytes는 한 번에 압축할 원본의 상한이다(1 이상).
    // 실패: Zstd가 아닌 형식 Unsupported, maxSourceBytes 0 InvalidArgument, 작업 공간 할당 실패
    // OutOfMemory, 외부 코덱 초기화 실패 DependencyFailed.
    static Result<FixedWorkspaceCompressor> TryCreate(const CompressionSettings& settings,
                                                      usize maxSourceBytes,
                                                      Allocator& allocator) noexcept;

    // source를 독립 frame 하나로 압축해 destination 앞부분에 쓰고 쓴 byte 수를 돌려준다. 할당하지
    // 않는다. destination은 Bound(source 크기) 이상이면 모자라지 않는다.
    // 실패: source가 MaxSourceBytes보다 크면 CapacityExceeded, destination이 모자라면
    // CapacityExceeded, 빈 문맥(이동된 뒤) StaleHandle, 코덱 오류 DependencyFailed.
    Result<usize> TryCompress(Span<const uint8> source, Span<uint8> destination) noexcept;

    // sourceBytes(MaxSourceBytes 이하)를 압축한 결과의 최악 크기다.
    static usize Bound(usize sourceBytes) noexcept;

    usize MaxSourceBytes() const noexcept {
        return maxSourceBytes_;
    }

    // 확보한 작업 공간 byte 수다(진단·예산용).
    usize WorkspaceBytes() const noexcept {
        return workspaceBytes_;
    }

private:
    void Release() noexcept;

    Allocator* allocator_ = nullptr;
    void* workspace_ = nullptr;
    void* context_ = nullptr;
    usize workspaceBytes_ = 0;
    usize maxSourceBytes_ = 0;
};

} // namespace rocks

#include <Compression/FixedWorkspaceCompressor.h>

// ZSTD_STATIC_LINKING_ONLY(ZSTD_initStaticCCtx·estimate)는 모듈 규칙이 정의한다(ZstdCodec.cpp
// 설명).
#include <zstd.h>
#include <zstd_errors.h>

namespace rocks {

namespace {

// 작업 공간 정렬이다(ZSTD_initStaticCCtx는 8 byte 정렬을 요구한다).
constexpr usize WorkspaceAlignment = 8;

bool SetParameter(ZSTD_CCtx* context, ZSTD_cParameter parameter, int value) noexcept {
    return !ZSTD_isError(ZSTD_CCtx_setParameter(context, parameter, value));
}

} // namespace

FixedWorkspaceCompressor::~FixedWorkspaceCompressor() noexcept {
    Release();
}

FixedWorkspaceCompressor::FixedWorkspaceCompressor(FixedWorkspaceCompressor&& other) noexcept
    : allocator_(other.allocator_), workspace_(other.workspace_), context_(other.context_),
      workspaceBytes_(other.workspaceBytes_), maxSourceBytes_(other.maxSourceBytes_) {
    other.allocator_ = nullptr;
    other.workspace_ = nullptr;
    other.context_ = nullptr;
    other.workspaceBytes_ = 0;
    other.maxSourceBytes_ = 0;
}

FixedWorkspaceCompressor&
FixedWorkspaceCompressor::operator=(FixedWorkspaceCompressor&& other) noexcept {
    if (this != &other) {
        Release();
        allocator_ = other.allocator_;
        workspace_ = other.workspace_;
        context_ = other.context_;
        workspaceBytes_ = other.workspaceBytes_;
        maxSourceBytes_ = other.maxSourceBytes_;
        other.allocator_ = nullptr;
        other.workspace_ = nullptr;
        other.context_ = nullptr;
        other.workspaceBytes_ = 0;
        other.maxSourceBytes_ = 0;
    }

    return *this;
}

void FixedWorkspaceCompressor::Release() noexcept {
    // 정적 문맥은 작업 공간 안에 있으므로 따로 해제하지 않는다.
    if (workspace_ != nullptr) {
        (void)allocator_->Deallocate(workspace_);
    }

    workspace_ = nullptr;
    context_ = nullptr;
}

Result<FixedWorkspaceCompressor>
FixedWorkspaceCompressor::TryCreate(const CompressionSettings& settings, usize maxSourceBytes,
                                    Allocator& allocator) noexcept {
    if (settings.Format() != CompressionFormat::Zstd) {
        return Result<FixedWorkspaceCompressor>::Failure(ErrorCode::Unsupported);
    }

    if (maxSourceBytes == 0) {
        return Result<FixedWorkspaceCompressor>::Failure(ErrorCode::InvalidArgument);
    }

    // 수준과 최대 원본 크기로 매개변수를 정해 고정한다. 작업 공간은 스트림 버퍼까지 포함한 크기다
    // (ZSTD_compress2가 쓰는 경로가 무엇이든 모자라지 않게).
    const ZSTD_compressionParameters parameters =
        ZSTD_getCParams(settings.Level(), static_cast<unsigned long long>(maxSourceBytes), 0);
    const usize bytes = ZSTD_estimateCStreamSize_usingCParams(parameters);
    auto workspace = allocator.Allocate(bytes, WorkspaceAlignment);
    if (!workspace) {
        return Result<FixedWorkspaceCompressor>::Failure(ErrorCode::OutOfMemory);
    }

    ZSTD_CCtx* context = ZSTD_initStaticCCtx(workspace.Value(), bytes);
    const bool configured =
        context != nullptr &&
        SetParameter(context, ZSTD_c_windowLog, static_cast<int>(parameters.windowLog)) &&
        SetParameter(context, ZSTD_c_chainLog, static_cast<int>(parameters.chainLog)) &&
        SetParameter(context, ZSTD_c_hashLog, static_cast<int>(parameters.hashLog)) &&
        SetParameter(context, ZSTD_c_searchLog, static_cast<int>(parameters.searchLog)) &&
        SetParameter(context, ZSTD_c_minMatch, static_cast<int>(parameters.minMatch)) &&
        SetParameter(context, ZSTD_c_targetLength, static_cast<int>(parameters.targetLength)) &&
        SetParameter(context, ZSTD_c_strategy, static_cast<int>(parameters.strategy)) &&
        SetParameter(context, ZSTD_c_contentSizeFlag, 1) &&
        SetParameter(context, ZSTD_c_checksumFlag, 0);
    if (!configured) {
        (void)allocator.Deallocate(workspace.Value());
        return Result<FixedWorkspaceCompressor>::Failure(ErrorCode::DependencyFailed);
    }

    FixedWorkspaceCompressor compressor;
    compressor.allocator_ = &allocator;
    compressor.workspace_ = workspace.Value();
    compressor.context_ = context;
    compressor.workspaceBytes_ = bytes;
    compressor.maxSourceBytes_ = maxSourceBytes;
    return Result<FixedWorkspaceCompressor>::Success(Move(compressor));
}

usize FixedWorkspaceCompressor::Bound(usize sourceBytes) noexcept {
    return ZSTD_compressBound(sourceBytes);
}

Result<usize> FixedWorkspaceCompressor::TryCompress(Span<const uint8> source,
                                                    Span<uint8> destination) noexcept {
    if (context_ == nullptr) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    if (source.Size() > maxSourceBytes_) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    auto* context = static_cast<ZSTD_CCtx*>(context_);
    // 앞 호출의 실패 상태를 지운다. 원본 크기는 ZSTD_compress2가 frame 머리에 적는다. 매개변수는
    // 고정이라 작업 공간을 다시 잡지 않는다. 정적 문맥(ZSTD_initStaticCCtx)은 더 큰 작업 공간이
    // 필요해도 할당하지 않고 오류를 돌려준다(무할당의 근거 — zstd 내부 malloc을 쓰지 않는다).
    (void)ZSTD_CCtx_reset(context, ZSTD_reset_session_only);
    const size_t written = ZSTD_compress2(context, destination.Data(), destination.Size(),
                                          source.Data(), source.Size());
    if (ZSTD_isError(written)) {
        return Result<usize>::Failure(ZSTD_getErrorCode(written) == ZSTD_error_dstSize_tooSmall
                                          ? ErrorCode::CapacityExceeded
                                          : ErrorCode::DependencyFailed);
    }

    return Result<usize>::Success(written);
}

} // namespace rocks

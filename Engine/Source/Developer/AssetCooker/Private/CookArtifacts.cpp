#include <AssetCooker/CookArtifacts.h>

// 비에셋 산출물 단계 표다(Docs/Design/AssetSystem 10.3.1절 A4f3 2번).
namespace rocks {

Result<void> CookArtifactStageTable::Register(CookArtifactStage& stage) noexcept {
    const usize slot = static_cast<usize>(stage.Kind());
    if (slot == 0 || slot >= KindSlots) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (InUse()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (stages_[slot] != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    stages_[slot] = &stage;
    return Result<void>::Success();
}

Result<void> CookArtifactStageTable::Unregister(CookArtifactStage& stage) noexcept {
    const usize slot = static_cast<usize>(stage.Kind());
    if (slot == 0 || slot >= KindSlots || stages_[slot] != &stage) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (InUse()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    stages_[slot] = nullptr;
    return Result<void>::Success();
}

CookArtifactStage* CookArtifactStageTable::Find(CookArtifactKind kind) const noexcept {
    const usize slot = static_cast<usize>(kind);
    return slot == 0 || slot >= KindSlots ? nullptr : stages_[slot];
}

CookArtifactStageTable& CookArtifactStages() noexcept {
    // 모듈 시작 hook보다 먼저 쓰일 수 있어 함수 안 정적 객체로 둔다(생성자는 상수 초기화다).
    static CookArtifactStageTable table;
    return table;
}

StringView CookArtifactCallName(CookArtifactCall call) noexcept {
    switch (call) {
    case CookArtifactCall::None:
        return "None";
    case CookArtifactCall::Run:
        return "Run";
    case CookArtifactCall::NewStagingPath:
        return "NewStagingPath";
    case CookArtifactCall::Produce:
        return "Produce";
    case CookArtifactCall::Reuse:
        return "Reuse";
    case CookArtifactCall::Report:
        return "Report";
    case CookArtifactCall::ReportStat:
        return "ReportStat";
    }

    return "None";
}

} // namespace rocks

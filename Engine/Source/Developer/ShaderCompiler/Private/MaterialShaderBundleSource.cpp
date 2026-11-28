#include <ShaderCompiler/MaterialShaderBundleSource.h>

// 머티리얼 셰이더 묶음 원천 표다(Docs/Design/ShaderSystem 16.5절 SSD87).
namespace rocks::shader {

Result<void>
MaterialShaderBundleSourceTable::Register(MaterialShaderBundleSource& source) noexcept {
    if (InUse()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (source_ != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    source_ = &source;
    return Result<void>::Success();
}

Result<void>
MaterialShaderBundleSourceTable::Unregister(MaterialShaderBundleSource& source) noexcept {
    if (source_ != &source) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (InUse()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    source_ = nullptr;
    return Result<void>::Success();
}

MaterialShaderBundleSourceTable& MaterialShaderBundleSources() noexcept {
    // 모듈 시작 hook보다 먼저 쓰일 수 있어 함수 안 정적 객체로 둔다(생성자는 상수 초기화다).
    static MaterialShaderBundleSourceTable table;
    return table;
}

} // namespace rocks::shader

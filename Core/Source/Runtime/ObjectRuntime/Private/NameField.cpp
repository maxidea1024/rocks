#include <Object/NameField.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/Unicode.h>

// Name 필드 값의 내용 규칙과 토큰 검사다(Object/NameField.h, Docs/Design/ReflectionExtensions.ko.md
// 4절).
namespace rocks {

namespace {

// 유효 UTF-8 text의 UTF-16 코드 단위 수다(보충 문자 — 4 byte 시작 byte — 는 2단위). 시작 byte만
// 센다(이어지는 byte 10xxxxxx는 세지 않는다).
usize Utf16Units(StringView text) noexcept {
    usize units = 0;
    const char* bytes = text.Data();
    for (usize index = 0; index < text.ByteCount(); ++index) {
        const uint8 byte = static_cast<uint8>(bytes[index]);
        if ((byte & 0xC0u) != 0x80u) {
            units += byte >= 0xF0u ? 2 : 1;
        }
    }

    return units;
}

} // namespace

Result<void> ValidateNameFieldText(StringView text, Allocator& scratch) noexcept {
    // byte 상한을 먼저 본다(긴 입력에 UTF-8·NFC 판정을 하지 않는다 — 복원 경로는 Archive reader가
    // String을 읽을 때 UTF-8을 이미 봤다).
    if (text.ByteCount() > MaxNameFieldBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto valid = text.ValidateUtf8(); !valid) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const char* bytes = text.Data();
    for (usize index = 0; index < text.ByteCount(); ++index) {
        if (bytes[index] == '\0') {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    // UE 5.8과 같이 번호 접미를 뗀 base의 길이를 UTF-16 단위로 센다.
    if (Utf16Units(SplitNameText(text).base) > MaxNameBaseUtf16Units) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto normalized = IsNormalized(NormalizationForm::NFC, text, scratch);
    if (!normalized) {
        return Result<void>::Failure(normalized.Error());
    }

    if (!normalized.Value()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<NameLease> AcquireNameFieldText(NamePool* pool, Name token, Allocator& scratch) noexcept {
    if (!token.IsValid()) {
        return Result<NameLease>::Failure(ErrorCode::InvalidArgument);
    }

    // 기본 None은 어느 pool에도 속하지 않는다(pool 없는 Registry도 None은 쓰고 저장한다).
    if (token.kind == NameKind::None) {
        return Result<NameLease>::Success(NameLease{});
    }

    if (pool == nullptr) {
        return Result<NameLease>::Failure(ErrorCode::Unsupported);
    }

    // Acquire가 실행 상태(ShuttingDown)를 먼저 본다. 찾지 못한 토큰(StaleHandle)만 domain으로 다른
    // pool(또는 다시 초기화하기 전 세션)의 토큰을 문맥 오류로 구별한다 — 성공 경로에서 Stats를
    // 복사하지 않는다.
    auto lease = pool->Acquire(token);
    if (!lease) {
        if (lease.Error() == ErrorCode::StaleHandle && token.domain != pool->Stats().domain) {
            return Result<NameLease>::Failure(ErrorCode::WrongContext);
        }

        return lease;
    }

    // 번호 없는 None 표시 변형("none")은 None이다 — 빈 문자열로 저장한다.
    if (token.IsNone()) {
        return Result<NameLease>::Success(NameLease{});
    }

    if (auto valid = ValidateNameFieldText(lease.Value().View(), scratch); !valid) {
        return Result<NameLease>::Failure(valid.Error());
    }

    return lease;
}

Result<void> ValidateNameFieldToken(NamePool* pool, Name token, Allocator& scratch) noexcept {
    auto display = AcquireNameFieldText(pool, token, scratch);
    if (!display) {
        return Result<void>::Failure(display.Error());
    }

    return Result<void>::Success();
}

bool IsNoneNameText(StringView text) noexcept {
    return NameTextEquals(text, StringView());
}

} // namespace rocks

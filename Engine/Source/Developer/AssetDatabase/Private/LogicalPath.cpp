#include "AssetDatabaseInternal.h"

#include <AssetDatabase/LogicalPath.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/Unicode.h>

// 논리 경로 파싱·정규화·비교 키다(Docs/Design/AssetSystem.ko.md 4.2절, 7.1절 10 — A2a).
namespace rocks {

namespace {

using asset_database_detail::AppendAll;

constexpr char Separator = '/';

// 정규화 임시 저장소의 인라인 byte 수다(경로 상한 + 여유 — 넘으면 allocator로 자란다).
constexpr usize PathScratchBytes = MaxLogicalPathBytes + 64;
// 구성 요소 하나의 비교 키 임시 저장소 인라인 byte 수다(64 byte 이름의 접기 결과는 대개 이 안이다).
constexpr usize ComponentKeyBytes = MaxNameComponentBytes * 2;

// 구성 요소 하나를 꺼낸다. begin부터 다음 '/' 앞까지다.
StringView ComponentAt(StringView text, usize begin) noexcept {
    usize end = begin;
    while (end < text.ByteCount() && text.Data()[end] != Separator) {
        ++end;
    }

    return StringView(text.Data() + begin, end - begin);
}

Result<LogicalPath> Fail(LogicalPathFailure* failure, LogicalPathError error, uint32 component = 0,
                         NameComponentError componentError = NameComponentError::None,
                         usize offset = 0) noexcept {
    if (failure != nullptr) {
        failure->error = error;
        failure->component = component;
        failure->componentError = componentError;
        failure->offset = offset;
    }

    return Result<LogicalPath>::Failure(ErrorCode::InvalidArgument);
}

Result<LogicalPath> FailResource(LogicalPathFailure* failure, ErrorCode error) noexcept {
    if (failure != nullptr) {
        *failure = LogicalPathFailure{};
    }

    return Result<LogicalPath>::Failure(error);
}

} // namespace

const char* LogicalPathErrorName(LogicalPathError error) noexcept {
    switch (error) {
    case LogicalPathError::None:
        return "None";
    case LogicalPathError::Empty:
        return "Empty";
    case LogicalPathError::MissingLeadingSlash:
        return "MissingLeadingSlash";
    case LogicalPathError::Backslash:
        return "Backslash";
    case LogicalPathError::EmptyComponent:
        return "EmptyComponent";
    case LogicalPathError::MissingName:
        return "MissingName";
    case LogicalPathError::UnknownMount:
        return "UnknownMount";
    case LogicalPathError::InvalidComponent:
        return "InvalidComponent";
    case LogicalPathError::TooLong:
        return "TooLong";
    case LogicalPathError::NotNfcOnDisk:
        return "NotNfcOnDisk";
    }

    return "Unknown";
}

LogicalPath::LogicalPath(Allocator& allocator) noexcept : text_(allocator), key_(allocator) {
}

Result<LogicalPath> LogicalPath::TryClone() const noexcept {
    Allocator& allocator = text_.GetAllocator();
    LogicalPath copy(allocator);
    if (auto text = copy.text_.TryAssign(text_.View()); !text) {
        return Result<LogicalPath>::Failure(text.Error());
    }

    if (auto key = copy.key_.TryAssign(key_.View()); !key) {
        return Result<LogicalPath>::Failure(key.Error());
    }

    copy.mountBytes_ = mountBytes_;
    return Result<LogicalPath>::Success(Move(copy));
}

StringView LogicalPath::Mount() const noexcept {
    return StringView(text_.View().Data(), mountBytes_);
}

StringView LogicalPath::Relative() const noexcept {
    const StringView text = text_.View();
    if (text.IsEmpty()) {
        return StringView();
    }

    return StringView(text.Data() + mountBytes_ + 1, text.ByteCount() - mountBytes_ - 1);
}

StringView LogicalPath::Name() const noexcept {
    const StringView text = text_.View();
    if (text.IsEmpty()) {
        return StringView();
    }

    usize begin = text.ByteCount();
    // 비지 않은 정규 경로는 '/'로 시작하고 구성 요소가 둘 이상이라 시작 전에 '/'를 만난다.
    while (text.Data()[begin - 1] != Separator) {
        --begin;
    }

    return StringView(text.Data() + begin, text.ByteCount() - begin);
}

StringView LogicalPath::Directory() const noexcept {
    const StringView text = text_.View();
    if (text.IsEmpty()) {
        return StringView();
    }

    return StringView(text.Data(), text.ByteCount() - Name().ByteCount() - 1);
}

Result<LogicalPath> ParseLogicalPath(Allocator& allocator, StringView text,
                                     LogicalPathFailure* failure) noexcept {
    // 1. 비지 않고 '/'로 시작하며 '\'가 없다.
    if (text.IsEmpty()) {
        return Fail(failure, LogicalPathError::Empty);
    }

    if (text.Data()[0] != Separator) {
        return Fail(failure, LogicalPathError::MissingLeadingSlash);
    }

    for (char c : text) {
        if (c == '\\') {
            return Fail(failure, LogicalPathError::Backslash);
        }
    }

    // 2. 빈 구성 요소·끝 '/'가 없고 구성 요소가 둘 이상이다.
    uint32 components = 0;
    for (usize begin = 1;; ++components) {
        const StringView component = ComponentAt(text, begin);
        if (component.IsEmpty()) {
            return Fail(failure, LogicalPathError::EmptyComponent, components);
        }

        begin += component.ByteCount() + 1;
        if (begin > text.ByteCount()) {
            ++components;
            break;
        }
    }

    if (components < 2) {
        return Fail(failure, LogicalPathError::MissingName);
    }

    // 3. 마운트(대소문자 무시, 표기는 마운트 철자).
    const StringView mount = ComponentAt(text, 1);
    StringBuilder<PathScratchBytes> mountKey(allocator);
    StringBuilder<PathScratchBytes> gameKey(allocator);
    const StringView gameName(GameMount.Data() + 1, GameMount.ByteCount() - 1);
    if (auto made = TryMakeCaselessKey(mount, mountKey); !made) {
        // 잘못된 UTF-8은 모르는 마운트다(등록된 마운트는 ASCII다).
        if (made.Error() != ErrorCode::InvalidArgument) {
            return FailResource(failure, made.Error());
        }

        return Fail(failure, LogicalPathError::UnknownMount);
    }

    if (auto made = TryMakeCaselessKey(gameName, gameKey); !made) {
        return FailResource(failure, made.Error());
    }

    if (mountKey.View() != gameKey.View()) {
        return Fail(failure, LogicalPathError::UnknownMount);
    }

    // 4. 구성 요소마다 NFC로 정규화하고 규칙을 본다. 정규화 결과를 이어 붙여 정규 경로를 만든다.
    LogicalPath path(allocator);
    StringBuilder<PathScratchBytes> normalized(allocator);
    StringBuilder<PathScratchBytes> key(allocator);
    usize begin = 1 + mount.ByteCount() + 1;
    for (uint32 index = 1; index < components; ++index) {
        const StringView component = ComponentAt(text, begin);
        begin += component.ByteCount() + 1;
        StringBuilder<PathScratchBytes> nfc(allocator);
        if (auto made = TryNormalize(NormalizationForm::NFC, component, nfc); !made) {
            if (made.Error() != ErrorCode::InvalidArgument) {
                return FailResource(failure, made.Error());
            }

            // 잘못된 UTF-8 — 규칙 검사가 같은 판정과 위치를 준다.
            NameComponentError reason{};
            usize offset = 0;
            auto valid = ValidateNameComponent(component, allocator, &reason, &offset);
            if (reason == NameComponentError::None) {
                // 규칙 검사가 잘못된 UTF-8을 받아들이면(정규화와 판정이 어긋남) 사유 없는 거부가
                // 되지 않게 그 결과를 그대로 돌려준다.
                return FailResource(failure, valid ? ErrorCode::InvalidArgument : valid.Error());
            }

            return Fail(failure, LogicalPathError::InvalidComponent, index, reason, offset);
        }

        NameComponentError reason{};
        usize offset = 0;
        if (auto valid = ValidateNameComponent(nfc.View(), allocator, &reason, &offset); !valid) {
            if (reason == NameComponentError::None) {
                return FailResource(failure, valid.Error());
            }

            return Fail(failure, LogicalPathError::InvalidComponent, index, reason, offset);
        }

        StringBuilder<ComponentKeyBytes> componentKey(allocator);
        if (auto made = TryMakeNameComponentKey(nfc.View(), componentKey); !made) {
            return FailResource(failure, made.Error());
        }

        // 첫 구성 요소 앞에는 마운트(표기는 마운트 철자, 키는 그 caseless 키)를 둔다.
        const StringView pathPieces[] = {index == 1 ? GameMount : StringView(), "/", nfc.View()};
        const StringView keyPieces[] = {index == 1 ? gameKey.View() : StringView(), "/",
                                        componentKey.View()};
        if (auto appended = AppendAll(normalized, pathPieces); !appended) {
            return FailResource(failure, appended.Error());
        }

        if (auto appended = AppendAll(key, keyPieces); !appended) {
            return FailResource(failure, appended.Error());
        }
    }

    // 5. 정규화 뒤 전체 길이.
    if (normalized.View().ByteCount() > MaxLogicalPathBytes) {
        return Fail(failure, LogicalPathError::TooLong);
    }

    if (auto assigned = path.text_.TryAssign(normalized.View()); !assigned) {
        return FailResource(failure, assigned.Error());
    }

    if (auto assigned = path.key_.TryAssign(key.View()); !assigned) {
        return FailResource(failure, assigned.Error());
    }

    path.mountBytes_ = GameMount.ByteCount();
    if (failure != nullptr) {
        *failure = LogicalPathFailure{};
    }

    return Result<LogicalPath>::Success(Move(path));
}

} // namespace rocks

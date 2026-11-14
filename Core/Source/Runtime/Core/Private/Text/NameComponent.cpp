#include <Core/Text/NameComponent.h>
#include <Core/Text/Unicode.h>

namespace rocks {

namespace {

Result<void> Reject(NameComponentError error, usize position, NameComponentError* reason,
                    usize* offset) noexcept {
    if (reason) {
        *reason = error;
    }

    if (offset) {
        *offset = position;
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

void Accept(NameComponentError* reason, usize* offset) noexcept {
    if (reason) {
        *reason = NameComponentError::None;
    }

    if (offset) {
        *offset = 0;
    }
}

bool IsLetterOrNumber(GeneralCategory category) noexcept {
    return category <= GeneralCategory::Lo ||
           (category >= GeneralCategory::Nd && category <= GeneralCategory::No);
}

bool IsMark(GeneralCategory category) noexcept {
    return category >= GeneralCategory::Mn && category <= GeneralCategory::Me;
}

char AsciiUpper(char character) noexcept {
    return character >= 'a' && character <= 'z' ? static_cast<char>(character - ('a' - 'A'))
                                                : character;
}

// 대소문자 무시로 component의 앞 3 byte가 prefix(대문자 ASCII 3자)와 같은지다.
bool HasAsciiPrefix(StringView component, const char* prefix) noexcept {
    const char* data = component.Data();
    return AsciiUpper(data[0]) == prefix[0] && AsciiUpper(data[1]) == prefix[1] &&
           AsciiUpper(data[2]) == prefix[2];
}

// Windows 예약 장치 이름인지다(이름 전체가 같을 때만 — 이름에는 '.'이 없으므로 "CON.txt" 형태는
// 생기지 않는다). Win32는 위첨자 1·2·3도 COM/LPT 포트 번호로 취급한다.
bool IsReservedDeviceName(StringView component) noexcept {
    const usize bytes = component.ByteCount();
    if (bytes < 3 || bytes > 5) {
        return false;
    }

    if (bytes == 3) {
        return HasAsciiPrefix(component, "CON") || HasAsciiPrefix(component, "PRN") ||
               HasAsciiPrefix(component, "AUX") || HasAsciiPrefix(component, "NUL");
    }

    if (!HasAsciiPrefix(component, "COM") && !HasAsciiPrefix(component, "LPT")) {
        return false;
    }

    const auto* tail = reinterpret_cast<const uint8*>(component.Data()) + 3;
    if (bytes == 4) {
        return tail[0] >= '0' && tail[0] <= '9';
    }

    // U+00B9·U+00B2·U+00B3의 UTF-8은 C2 B9·C2 B2·C2 B3이다.
    return tail[0] == 0xC2 && (tail[1] == 0xB9 || tail[1] == 0xB2 || tail[1] == 0xB3);
}

bool IsAscii(StringView component) noexcept {
    const char* data = component.Data();
    for (usize index = 0; index < component.ByteCount(); ++index) {
        if (static_cast<uint8>(data[index]) >= 0x80) {
            return false;
        }
    }

    return true;
}

// 잘못된 UTF-8 입력에서 유효한 앞부분의 길이(= 첫 잘못된 sequence의 시작)다. 입력은 64 byte 이하다.
usize ValidUtf8Prefix(StringView component) noexcept {
    for (usize bytes = component.ByteCount(); bytes != 0; --bytes) {
        if (StringView(component.Data(), bytes).ValidateUtf8()) {
            return bytes;
        }
    }

    return 0;
}

} // namespace

Result<void> ValidateNameComponent(StringView component, Allocator& scratch,
                                   NameComponentError* reason, usize* offset) noexcept {
    const usize bytes = component.ByteCount();
    if (bytes == 0) {
        return Reject(NameComponentError::Empty, 0, reason, offset);
    }

    if (bytes > MaxNameComponentBytes) {
        return Reject(NameComponentError::TooLong, MaxNameComponentBytes, reason, offset);
    }

    auto points = component.TryCodePoints();
    if (!points) {
        return Reject(NameComponentError::InvalidCharacter, ValidUtf8Prefix(component), reason,
                      offset);
    }

    bool first = true;
    for (auto at = points.Value().begin(); at != points.Value().end(); ++at) {
        const char32_t point = *at;
        const auto position = static_cast<usize>(at.Position() - component.Data());
        // 보이지 않는 문자는 L*·M*이어도 거부한다(예: U+FE0F는 Mn이다).
        if (IsDefaultIgnorable(point)) {
            return Reject(NameComponentError::DefaultIgnorable, position, reason, offset);
        }

        const GeneralCategory category = GetGeneralCategory(point);
        if (first) {
            if (!IsLetterOrNumber(category)) {
                return Reject(NameComponentError::InvalidFirst, position, reason, offset);
            }

            first = false;
        } else if (!IsLetterOrNumber(category) && !IsMark(category) && point != U'_' &&
                   point != U'-') {
            return Reject(NameComponentError::InvalidCharacter, position, reason, offset);
        }
    }

    if (IsReservedDeviceName(component)) {
        return Reject(NameComponentError::ReservedDeviceName, 0, reason, offset);
    }

    // ASCII는 언제나 NFC다.
    if (!IsAscii(component)) {
        auto normalized = IsNormalized(NormalizationForm::NFC, component, scratch);
        if (!normalized) {
            Accept(reason, offset);
            return Result<void>::Failure(normalized.Error());
        }

        if (!normalized.Value()) {
            // 위치를 묻지 않으면 위치 계산용 정규화(할당)를 하지 않는다.
            if (offset == nullptr) {
                return Reject(NameComponentError::NotNfc, 0, reason, offset);
            }

            // 위치는 NFC 결과와 처음 달라지는 byte가 속한 code point의 시작이다.
            StringBuilder<192> nfc(scratch);
            if (auto made = TryNormalize(NormalizationForm::NFC, component, nfc); !made) {
                Accept(reason, offset);
                return made;
            }

            const char* data = component.Data();
            const char* expected = nfc.View().Data();
            const usize common = nfc.View().ByteCount() < bytes ? nfc.View().ByteCount() : bytes;
            usize position = 0;
            while (position < common && data[position] == expected[position]) {
                ++position;
            }

            // 입력 전체가 NFC 결과의 앞부분이면 마지막 code point를 가리킨다(입력 밖을 읽지
            // 않는다).
            if (position == bytes) {
                --position;
            }

            while (position > 0 && (static_cast<uint8>(data[position]) & 0xC0u) == 0x80u) {
                --position;
            }

            return Reject(NameComponentError::NotNfc, position, reason, offset);
        }
    }

    Accept(reason, offset);
    return Result<void>::Success();
}

Result<void> TryMakeNameComponentKey(StringView component, StringBuilderBase& out) noexcept {
    if (!IsAscii(component)) {
        return TryMakeCaselessKey(component, out);
    }

    // ASCII의 단순 case folding은 A–Z → a–z뿐이고 NFD·NFC는 그대로다(TryMakeCaselessKey와 같은 키).
    const usize start = out.ByteCount();
    if (component.ByteCount() > static_cast<usize>(-1) - start) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (auto reserved = out.TryReserve(start + component.ByteCount()); !reserved) {
        return reserved;
    }

    const char* data = component.Data();
    for (usize index = 0; index < component.ByteCount(); ++index) {
        const char character = data[index];
        const char lower = character >= 'A' && character <= 'Z'
                               ? static_cast<char>(character + ('a' - 'A'))
                               : character;
        if (auto appended = out.TryAppendAscii(lower); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks

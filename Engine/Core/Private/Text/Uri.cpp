#include <Core/Text/Uri.h>

#include <Base/Checked.h>

namespace rocks {
namespace {

// RFC 3986 문자 클래스. 비ASCII byte(>= 0x80)와 제어 문자는 어느 클래스에도 속하지 않는다.
bool IsAlpha(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

int32 HexValue(char c) noexcept {
    if (IsDigit(c)) {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool IsUnreserved(char c) noexcept {
    return IsAlpha(c) || IsDigit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

bool IsSubDelimiter(char c) noexcept {
    switch (c) {
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=':
        return true;
    default:
        return false;
    }
}

// pchar = unreserved / pct-encoded / sub-delims / ":" / "@" (pct-encoded는 별도 처리)
bool IsPathCharacter(char c) noexcept {
    return IsUnreserved(c) || IsSubDelimiter(c) || c == ':' || c == '@';
}

enum class Component : uint8 {
    UserInfo,
    RegisteredName,
    Path,
    QueryOrFragment
};

bool IsAllowed(Component component, char c) noexcept {
    switch (component) {
    case Component::UserInfo:
        return IsUnreserved(c) || IsSubDelimiter(c) || c == ':';
    case Component::RegisteredName:
        return IsUnreserved(c) || IsSubDelimiter(c);
    case Component::Path:
        return IsPathCharacter(c) || c == '/';
    case Component::QueryOrFragment:
        return IsPathCharacter(c) || c == '/' || c == '?';
    }
    return false;
}

// 구간의 모든 문자가 허용 클래스이거나 올바른 %HH인지 검사한다.
bool ValidateComponent(const char* data, usize begin, usize end, Component component) noexcept {
    usize index = begin;
    while (index < end) {
        const char c = data[index];
        if (c == '%') {
            if (end - index < 3) {
                return false; // "%"·"%A"처럼 두 자리 16진수가 없다.
            }
            if (HexValue(data[index + 1]) < 0 || HexValue(data[index + 2]) < 0) {
                return false;
            }
            index += 3;
        } else if (IsAllowed(component, c)) {
            ++index;
        } else {
            return false;
        }
    }
    return true;
}

// dec-octet: 0–255, 앞자리 0 금지(RFC 3986).
bool ValidateIpv4(const char* data, usize begin, usize end) noexcept {
    usize index = begin;
    for (int part = 0; part < 4; ++part) {
        if (part != 0) {
            if (index >= end || data[index] != '.') {
                return false;
            }
            ++index;
        }
        const usize start = index;
        uint32 value = 0;
        while (index < end && IsDigit(data[index]) && index - start < 3) {
            value = value * 10 + static_cast<uint32>(data[index] - '0');
            ++index;
        }
        const usize digits = index - start;
        if (digits == 0 || value > 255 || (digits > 1 && data[start] == '0')) {
            return false;
        }
    }
    return index == end;
}

// IPv6address 또는 IPvFuture(대괄호 안 내용).
bool ValidateIpLiteral(const char* data, usize begin, usize end) noexcept {
    if (begin < end && (data[begin] == 'v' || data[begin] == 'V')) {
        usize index = begin + 1;
        const usize hexStart = index;
        while (index < end && HexValue(data[index]) >= 0) {
            ++index;
        }
        if (index == hexStart || index >= end || data[index] != '.') {
            return false;
        }
        ++index;
        if (index >= end) {
            return false;
        }
        for (; index < end; ++index) {
            const char c = data[index];
            if (!IsUnreserved(c) && !IsSubDelimiter(c) && c != ':') {
                return false;
            }
        }
        return true;
    }
    usize index = begin;
    bool compressed = false;
    uint32 pieces = 0;
    if (end - begin >= 2 && data[begin] == ':' && data[begin + 1] == ':') {
        compressed = true;
        index = begin + 2;
    } else if (begin < end && data[begin] == ':') {
        return false;
    }
    while (index < end) {
        usize tokenEnd = index;
        bool dotted = false;
        while (tokenEnd < end && data[tokenEnd] != ':') {
            dotted = dotted || data[tokenEnd] == '.';
            ++tokenEnd;
        }
        if (dotted) {
            // 끝의 IPv4 표기는 16-bit 조각 두 개에 해당한다.
            if (tokenEnd != end || !ValidateIpv4(data, index, tokenEnd)) {
                return false;
            }
            pieces += 2;
            index = end;
            break;
        }
        const usize digits = tokenEnd - index;
        if (digits == 0 || digits > 4) {
            return false;
        }
        for (usize position = index; position < tokenEnd; ++position) {
            if (HexValue(data[position]) < 0) {
                return false;
            }
        }
        ++pieces;
        if (tokenEnd == end) {
            index = end;
            break;
        }
        if (tokenEnd + 1 < end && data[tokenEnd + 1] == ':') {
            if (compressed) {
                return false; // "::"는 한 번만 허용한다.
            }
            compressed = true;
            index = tokenEnd + 2;
        } else {
            index = tokenEnd + 1;
            if (index == end) {
                return false; // 끝의 단일 ':'
            }
        }
    }
    return compressed ? pieces <= 7 : pieces == 8;
}

bool ParseAuthority(const char* data, usize begin, usize end, UriView& uri) noexcept {
    usize hostBegin = begin;
    for (usize index = begin; index < end; ++index) {
        if (data[index] == '@') {
            if (!ValidateComponent(data, begin, index, Component::UserInfo)) {
                return false;
            }
            uri.hasUserInfo = true;
            uri.userInfo = StringView(data + begin, index - begin);
            hostBegin = index + 1;
            break;
        }
    }
    usize hostEnd = hostBegin;
    usize portSeparator = end;
    if (hostBegin < end && data[hostBegin] == '[') {
        usize close = hostBegin + 1;
        while (close < end && data[close] != ']') {
            ++close;
        }
        if (close >= end || !ValidateIpLiteral(data, hostBegin + 1, close)) {
            return false;
        }
        uri.isIpLiteral = true;
        uri.host = StringView(data + hostBegin + 1, close - hostBegin - 1);
        hostEnd = close + 1;
        if (hostEnd != end && data[hostEnd] != ':') {
            return false; // "]" 뒤에는 포트 구분자만 올 수 있다.
        }
        portSeparator = hostEnd;
    } else {
        while (hostEnd < end && data[hostEnd] != ':') {
            ++hostEnd;
        }
        if (!ValidateComponent(data, hostBegin, hostEnd, Component::RegisteredName)) {
            return false;
        }
        uri.host = StringView(data + hostBegin, hostEnd - hostBegin);
        portSeparator = hostEnd;
    }
    if (portSeparator < end) {
        uri.hasPort = true;
        uint32 value = 0;
        for (usize index = portSeparator + 1; index < end; ++index) {
            if (!IsDigit(data[index])) {
                return false;
            }
            value = value * 10 + static_cast<uint32>(data[index] - '0');
            if (value > 65535) {
                return false;
            }
        }
        uri.port = static_cast<uint16>(value);
    }
    return true;
}

bool IsSchemeValid(const char* data, usize length) noexcept {
    if (length == 0 || !IsAlpha(data[0])) {
        return false;
    }
    for (usize index = 1; index < length; ++index) {
        const char c = data[index];
        if (!IsAlpha(c) && !IsDigit(c) && c != '+' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

} // namespace

Result<UriView> TryParseUri(StringView text) noexcept {
    const char* data = text.Data();
    const usize length = text.ByteCount();
    UriView uri;
    usize index = 0;

    // scheme: '/', '?', '#'보다 먼저 나오는 ':' 앞이 올바른 scheme일 때만 scheme이다.
    for (usize scan = 0; scan < length; ++scan) {
        const char c = data[scan];
        if (c == '/' || c == '?' || c == '#') {
            break;
        }
        if (c == ':') {
            if (IsSchemeValid(data, scan)) {
                uri.scheme = StringView(data, scan);
                index = scan + 1;
            }
            break;
        }
    }

    if (length - index >= 2 && data[index] == '/' && data[index + 1] == '/') {
        uri.hasAuthority = true;
        const usize authorityBegin = index + 2;
        usize authorityEnd = authorityBegin;
        while (authorityEnd < length && data[authorityEnd] != '/' && data[authorityEnd] != '?' &&
               data[authorityEnd] != '#') {
            ++authorityEnd;
        }
        if (!ParseAuthority(data, authorityBegin, authorityEnd, uri)) {
            return Result<UriView>::Failure(ErrorCode::InvalidArgument);
        }
        index = authorityEnd;
    }

    usize pathEnd = index;
    while (pathEnd < length && data[pathEnd] != '?' && data[pathEnd] != '#') {
        ++pathEnd;
    }
    if (!ValidateComponent(data, index, pathEnd, Component::Path)) {
        return Result<UriView>::Failure(ErrorCode::InvalidArgument);
    }
    uri.path = StringView(data + index, pathEnd - index);
    if (uri.hasAuthority && pathEnd != index && data[index] != '/') {
        return Result<UriView>::Failure(ErrorCode::InvalidArgument);
    }
    if (uri.scheme.IsEmpty() && !uri.hasAuthority) {
        // path-noscheme: 첫 segment에 ':'가 있으면 scheme과 구분할 수 없다.
        for (usize position = index; position < pathEnd && data[position] != '/'; ++position) {
            if (data[position] == ':') {
                return Result<UriView>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }
    index = pathEnd;

    if (index < length && data[index] == '?') {
        usize queryEnd = index + 1;
        while (queryEnd < length && data[queryEnd] != '#') {
            ++queryEnd;
        }
        if (!ValidateComponent(data, index + 1, queryEnd, Component::QueryOrFragment)) {
            return Result<UriView>::Failure(ErrorCode::InvalidArgument);
        }
        uri.hasQuery = true;
        uri.query = StringView(data + index + 1, queryEnd - index - 1);
        index = queryEnd;
    }

    if (index < length && data[index] == '#') {
        if (!ValidateComponent(data, index + 1, length, Component::QueryOrFragment)) {
            return Result<UriView>::Failure(ErrorCode::InvalidArgument);
        }
        uri.hasFragment = true;
        uri.fragment = StringView(data + index + 1, length - index - 1);
    }
    return Result<UriView>::Success(uri);
}

Result<usize> TryPercentDecode(StringView text, Span<char> output,
                               PercentDecodeMode mode) noexcept {
    const char* data = text.Data();
    const usize length = text.ByteCount();
    // 1단계: 형식 검증과 결과 크기 계산. 실패하면 output을 건드리지 않는다.
    usize decodedSize = 0;
    for (usize index = 0; index < length; ++decodedSize) {
        if (data[index] == '%') {
            if (length - index < 3 || HexValue(data[index + 1]) < 0 ||
                HexValue(data[index + 2]) < 0) {
                return Result<usize>::Failure(ErrorCode::InvalidArgument);
            }
            index += 3;
        } else {
            ++index;
        }
    }
    if (output.Size() < decodedSize) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }
    // 2단계: 쓰기
    usize written = 0;
    for (usize index = 0; index < length;) {
        const char c = data[index];
        if (c == '%') {
            output[written++] =
                static_cast<char>((HexValue(data[index + 1]) << 4) | HexValue(data[index + 2]));
            index += 3;
        } else {
            output[written++] = mode == PercentDecodeMode::FormUrlEncoded && c == '+' ? ' ' : c;
            ++index;
        }
    }
    return Result<usize>::Success(written);
}

Result<usize> PercentEncodedSize(StringView text) noexcept {
    usize size = 0;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        if (!TryAdd(size, IsUnreserved(text.Data()[index]) ? 1 : 3, size)) {
            return Result<usize>::Failure(ErrorCode::Overflow);
        }
    }
    return Result<usize>::Success(size);
}

Result<usize> TryPercentEncode(StringView text, Span<char> output) noexcept {
    auto required = PercentEncodedSize(text);
    if (!required) {
        return Result<usize>::Failure(required.Error());
    }
    if (output.Size() < required.Value()) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }
    constexpr char Digits[] = "0123456789ABCDEF";
    usize written = 0;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        const char c = text.Data()[index];
        if (IsUnreserved(c)) {
            output[written++] = c;
        } else {
            const auto byte = static_cast<uint8>(c);
            output[written++] = '%';
            output[written++] = Digits[byte >> 4];
            output[written++] = Digits[byte & 0x0f];
        }
    }
    return Result<usize>::Success(written);
}

bool FindQueryParameter(StringView query, StringView name, StringView& value) noexcept {
    const char* data = query.Data();
    const usize length = query.ByteCount();
    usize begin = 0;
    while (begin <= length) {
        usize end = begin;
        while (end < length && data[end] != '&') {
            ++end;
        }
        usize separator = begin;
        while (separator < end && data[separator] != '=') {
            ++separator;
        }
        const usize keyLength = separator - begin;
        bool matches = keyLength == name.ByteCount();
        for (usize index = 0; matches && index < keyLength; ++index) {
            matches = data[begin + index] == name.Data()[index];
        }
        if (matches && (keyLength != 0 || end != begin)) {
            value = separator < end ? StringView(data + separator + 1, end - separator - 1)
                                    : StringView(data + end, 0);
            return true;
        }
        if (end == length) {
            break;
        }
        begin = end + 1;
    }
    return false;
}

} // namespace rocks

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
        uri.authority = StringView(data + authorityBegin, authorityEnd - authorityBegin);
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

namespace {

// 용량을 미리 확인한 output에 순서대로 쓴다.
struct UriWriter {
    char* data;
    usize size = 0;

    void Append(const char* source, usize count) noexcept {
        for (usize index = 0; index < count; ++index) {
            data[size + index] = source[index];
        }
        size += count;
    }

    void Append(StringView text) noexcept {
        Append(text.Data(), text.ByteCount());
    }

    void Append(char c) noexcept {
        data[size++] = c;
    }
};

bool StartsWith(const char* data, usize begin, usize end, const char* prefix) noexcept {
    usize index = 0;
    for (; prefix[index] != '\0'; ++index) {
        if (begin + index >= end || data[begin + index] != prefix[index]) {
            return false;
        }
    }
    return true;
}

// RFC 3986 §5.2.4 remove_dot_segments를 data[begin, end)에서 제자리로 수행하고 새 끝을 반환한다.
// 출력 커서(write)는 항상 입력 커서(read) 이하이므로 앞으로 복사해도 아직 읽지 않은 입력을
// 덮어쓰지 않는다. "/."·"/.."가 입력 끝이면 RFC의 "'/'로 바꾸기"를 마지막 byte를 '/'로 바꿔
// 표현한다.
usize RemoveDotSegments(char* data, usize begin, usize end) noexcept {
    usize read = begin;
    usize write = begin;
    while (read < end) {
        if (StartsWith(data, read, end, "../")) {
            read += 3; // A
        } else if (StartsWith(data, read, end, "./")) {
            read += 2; // A
        } else if (StartsWith(data, read, end, "/./")) {
            read += 2; // B: 입력이 '/'로 시작하게 된다.
        } else if (end - read == 2 && StartsWith(data, read, end, "/.")) {
            read += 1; // B: 입력 끝의 "/." → "/"
            data[read] = '/';
        } else if (StartsWith(data, read, end, "/../") ||
                   (end - read == 3 && StartsWith(data, read, end, "/.."))) {
            // C: 출력의 마지막 segment와 그 앞 '/'를 지운다.
            if (end - read == 3) {
                read += 2;
                data[read] = '/';
            } else {
                read += 3;
            }
            while (write > begin && data[write - 1] != '/') {
                --write;
            }
            if (write > begin) {
                --write;
            }
        } else if ((end - read == 1 && data[read] == '.') ||
                   (end - read == 2 && data[read] == '.' && data[read + 1] == '.')) {
            read = end; // D
        } else {
            // E: 첫 segment(앞 '/' 포함, 다음 '/' 전까지)를 출력으로 옮긴다.
            if (data[read] == '/') {
                data[write++] = data[read++];
            }
            while (read < end && data[read] != '/') {
                data[write++] = data[read++];
            }
        }
    }
    return write;
}

} // namespace

Result<usize> ResolvedUriMaxSize(StringView base, StringView reference) noexcept {
    usize size = 0;
    if (!TryAdd(base.ByteCount(), reference.ByteCount(), size) || !TryAdd(size, 2, size)) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }
    return Result<usize>::Success(size);
}

Result<usize> TryResolveUriReference(StringView base, StringView reference,
                                     Span<char> output) noexcept {
    auto parsedBase = TryParseUri(base);
    auto parsedReference = TryParseUri(reference);
    if (!parsedBase || !parsedReference || parsedBase.Value().scheme.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    auto required = ResolvedUriMaxSize(base, reference);
    if (!required) {
        return Result<usize>::Failure(required.Error());
    }
    if (output.Size() < required.Value()) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    const UriView& b = parsedBase.Value();
    const UriView& r = parsedReference.Value();
    UriWriter writer{output.Data()};

    // §5.2.2: 대상의 scheme·authority·query를 고른다. path는 아래에서 output에 직접 만든다.
    const bool referenceAbsolute = !r.scheme.IsEmpty();
    const bool useReferenceAuthority = referenceAbsolute || r.hasAuthority;
    const bool hasAuthority = useReferenceAuthority ? r.hasAuthority : b.hasAuthority;
    writer.Append(referenceAbsolute ? r.scheme : b.scheme);
    writer.Append(':');
    if (hasAuthority) {
        writer.Append("//", 2);
        writer.Append(useReferenceAuthority ? r.authority : b.authority);
    }

    const usize pathBegin = writer.size;
    bool hasQuery = r.hasQuery;
    StringView query = r.query;
    if (useReferenceAuthority) {
        writer.Append(r.path);
    } else if (r.path.IsEmpty()) {
        writer.Append(b.path);
        if (!r.hasQuery) {
            hasQuery = b.hasQuery;
            query = b.query;
        }
    } else if (r.path.Data()[0] == '/') {
        writer.Append(r.path);
    } else {
        // §5.2.3 merge: base에 authority가 있고 path가 비면 "/" + reference path, 아니면 base
        // path의 마지막 '/'까지(없으면 빈 값) 뒤에 reference path를 붙인다.
        if (b.hasAuthority && b.path.IsEmpty()) {
            writer.Append('/');
        } else {
            usize keep = b.path.ByteCount();
            while (keep > 0 && b.path.Data()[keep - 1] != '/') {
                --keep;
            }
            writer.Append(b.path.Data(), keep);
        }
        writer.Append(r.path);
    }
    // base path를 그대로 쓰는 경우(reference path가 빈 경우)도 RFC는 dot segment를 제거하지 않는다.
    const bool removeDots = useReferenceAuthority || !r.path.IsEmpty();
    if (removeDots) {
        writer.size = RemoveDotSegments(writer.data, pathBegin, writer.size);
    }
    if (!hasAuthority && writer.size - pathBegin >= 2 && writer.data[pathBegin] == '/' &&
        writer.data[pathBegin + 1] == '/') {
        // "/."를 끼우려면 path를 2 byte 뒤로 민다(뒤에서부터 복사).
        for (usize index = writer.size; index > pathBegin; --index) {
            writer.data[index + 1] = writer.data[index - 1];
        }
        writer.data[pathBegin] = '/';
        writer.data[pathBegin + 1] = '.';
        writer.size += 2;
    }

    if (hasQuery) {
        writer.Append('?');
        writer.Append(query);
    }
    if (r.hasFragment) {
        writer.Append('#');
        writer.Append(r.fragment);
    }
    return Result<usize>::Success(writer.size);
}

namespace {

char ToLowerAscii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// %hh를 정규화해 쓴다. unreserved 문자를 나타내면 해제하고, 아니면 16진 숫자를 대문자로 바꾼다.
// lowercase면 결과 글자(해제된 글자 포함)를 소문자로 쓴다(host). 입력은 TryParseUri로 검증되어
// %hh가 항상 완전하다.
void AppendPercentNormalized(UriWriter& writer, StringView text, bool lowercase) noexcept {
    constexpr char Digits[] = "0123456789ABCDEF";
    const char* data = text.Data();
    const usize length = text.ByteCount();
    for (usize index = 0; index < length;) {
        const char c = data[index];
        if (c == '%' && length - index >= 3) {
            const int32 value = (HexValue(data[index + 1]) << 4) | HexValue(data[index + 2]);
            const char decoded = static_cast<char>(value);
            if (value < 0x80 && IsUnreserved(decoded)) {
                writer.Append(lowercase ? ToLowerAscii(decoded) : decoded);
            } else {
                writer.Append('%');
                writer.Append(Digits[value >> 4]);
                writer.Append(Digits[value & 0x0f]);
            }
            index += 3;
        } else {
            writer.Append(lowercase ? ToLowerAscii(c) : c);
            ++index;
        }
    }
}

bool EqualsIgnoreCase(StringView text, const char* expected) noexcept {
    usize index = 0;
    for (; expected[index] != '\0'; ++index) {
        if (index >= text.ByteCount() || ToLowerAscii(text.Data()[index]) != expected[index]) {
            return false;
        }
    }
    return index == text.ByteCount();
}

// §6.2.3 scheme 기반 정규화를 적용하는 scheme의 기본 포트다. 해당 없으면 0이다.
uint32 DefaultPortOf(StringView scheme) noexcept {
    if (EqualsIgnoreCase(scheme, "http") || EqualsIgnoreCase(scheme, "ws")) {
        return 80;
    }
    if (EqualsIgnoreCase(scheme, "https") || EqualsIgnoreCase(scheme, "wss")) {
        return 443;
    }
    return 0;
}

void AppendDecimal(UriWriter& writer, uint32 value) noexcept {
    char digits[10];
    usize count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0) {
        writer.Append(digits[--count]);
    }
}

} // namespace

Result<usize> NormalizedUriMaxSize(StringView text) noexcept {
    usize size = 0;
    if (!TryAdd(text.ByteCount(), 2, size)) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }
    return Result<usize>::Success(size);
}

Result<usize> TryNormalizeUri(StringView text, Span<char> output) noexcept {
    auto parsed = TryParseUri(text);
    if (!parsed) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    auto required = NormalizedUriMaxSize(text);
    if (!required) {
        return Result<usize>::Failure(required.Error());
    }
    if (output.Size() < required.Value()) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    const UriView& uri = parsed.Value();
    UriWriter writer{output.Data()};
    const bool hasScheme = !uri.scheme.IsEmpty();
    if (hasScheme) {
        for (usize index = 0; index < uri.scheme.ByteCount(); ++index) {
            writer.Append(ToLowerAscii(uri.scheme.Data()[index]));
        }
        writer.Append(':');
    }
    const uint32 defaultPort = hasScheme ? DefaultPortOf(uri.scheme) : 0;
    if (uri.hasAuthority) {
        writer.Append("//", 2);
        if (uri.hasUserInfo) {
            AppendPercentNormalized(writer, uri.userInfo, false);
            writer.Append('@');
        }
        if (uri.isIpLiteral) {
            writer.Append('[');
            AppendPercentNormalized(writer, uri.host, true);
            writer.Append(']');
        } else {
            AppendPercentNormalized(writer, uri.host, true);
        }
        // "host:"처럼 숫자가 없는 포트와 scheme 기본 포트는 생략한다.
        const bool emptyPort = uri.hasPort && uri.authority.ByteCount() > 0 &&
                               uri.authority.Data()[uri.authority.ByteCount() - 1] == ':';
        if (uri.hasPort && !emptyPort && !(defaultPort != 0 && uri.port == defaultPort)) {
            writer.Append(':');
            AppendDecimal(writer, uri.port);
        }
    }

    const usize pathBegin = writer.size;
    AppendPercentNormalized(writer, uri.path, false);
    if (hasScheme) {
        writer.size = RemoveDotSegments(writer.data, pathBegin, writer.size);
    }
    if (uri.hasAuthority && writer.size == pathBegin && defaultPort != 0) {
        writer.Append('/');
    }
    if (!uri.hasAuthority && writer.size - pathBegin >= 2 && writer.data[pathBegin] == '/' &&
        writer.data[pathBegin + 1] == '/') {
        for (usize index = writer.size; index > pathBegin; --index) {
            writer.data[index + 1] = writer.data[index - 1];
        }
        writer.data[pathBegin] = '/';
        writer.data[pathBegin + 1] = '.';
        writer.size += 2;
    }
    if (uri.hasQuery) {
        writer.Append('?');
        AppendPercentNormalized(writer, uri.query, false);
    }
    if (uri.hasFragment) {
        writer.Append('#');
        AppendPercentNormalized(writer, uri.fragment, false);
    }
    return Result<usize>::Success(writer.size);
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

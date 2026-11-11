#include <Core/Text/Path.h>

#include <Core/Containers/Array.h>
#include <Core/Move.h>
#include <Platform/Path.h>

namespace rocks {
namespace {

struct ParsedRoot {
    PathRoot kind = PathRoot::Relative;
    usize end = 0;
    usize body = 0;
    usize serverEnd = 0;
    usize shareStart = 0;
};

struct Segment {
    usize start;
    usize size;
    bool parent;
};

bool Separator(char value, PathStyle style) noexcept {
    return value == '/' || (style == PathStyle::Windows && value == '\\');
}

bool Letter(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

char UpperDrive(char value) noexcept {
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A') : value;
}

StringView Slice(StringView input, usize start, usize size) noexcept {
    return input.Data() ? StringView(input.Data() + start, size) : StringView();
}

Result<ParsedRoot> Parse(StringView path, PathStyle style) noexcept {
    if (style != PathStyle::Windows && style != PathStyle::Posix) {
        return Result<ParsedRoot>::Failure(ErrorCode::InvalidArgument);
    }

    if (!path.ValidateUtf8()) {
        return Result<ParsedRoot>::Failure(ErrorCode::InvalidArgument);
    }

    const char* text = path.Data();
    const usize size = path.ByteCount();
    for (usize i = 0; i < size; ++i) {
        if (text[i] == '\0') {
            return Result<ParsedRoot>::Failure(ErrorCode::InvalidArgument);
        }
    }

    ParsedRoot root;
    if (size == 0) {
        return Result<ParsedRoot>::Success(root);
    }

    if (style == PathStyle::Posix) {
        if (text[0] == '/') {
            root.kind = PathRoot::PosixRoot;
            root.end = 1;
            root.body = 1;
            while (root.body < size && text[root.body] == '/') {
                ++root.body;
            }
        }

        return Result<ParsedRoot>::Success(root);
    }

    if (size >= 2 && Letter(text[0]) && text[1] == ':') {
        root.end = 2;
        root.body = 2;
        root.kind = PathRoot::WindowsDriveRelative;
        if (size > 2 && Separator(text[2], style)) {
            root.kind = PathRoot::WindowsDriveAbsolute;
            root.end = 3;
            root.body = 3;
            while (root.body < size && Separator(text[root.body], style)) {
                ++root.body;
            }
        }

        return Result<ParsedRoot>::Success(root);
    }

    if (!Separator(text[0], style)) {
        return Result<ParsedRoot>::Success(root);
    }

    if (size < 2 || !Separator(text[1], style)) {
        root.kind = PathRoot::WindowsCurrentDriveRoot;
        root.end = 1;
        root.body = 1;
        return Result<ParsedRoot>::Success(root);
    }

    if (size >= 4 && (text[2] == '?' || text[2] == '.') && Separator(text[3], style)) {
        return Result<ParsedRoot>::Failure(ErrorCode::Unsupported);
    }

    usize cursor = 2;
    if (cursor == size || Separator(text[cursor], style)) {
        return Result<ParsedRoot>::Failure(ErrorCode::InvalidArgument);
    }

    while (cursor < size && !Separator(text[cursor], style)) {
        ++cursor;
    }
    root.serverEnd = cursor;
    if (cursor == size) {
        return Result<ParsedRoot>::Failure(ErrorCode::InvalidArgument);
    }

    while (cursor < size && Separator(text[cursor], style)) {
        ++cursor;
    }
    root.shareStart = cursor;
    if (cursor == size) {
        return Result<ParsedRoot>::Failure(ErrorCode::InvalidArgument);
    }

    while (cursor < size && !Separator(text[cursor], style)) {
        ++cursor;
    }

    const StringView server = Slice(path, 2, root.serverEnd - 2);
    const StringView share = Slice(path, root.shareStart, cursor - root.shareStart);
    if (server == "." || server == ".." || share == "." || share == "..") {
        return Result<ParsedRoot>::Failure(ErrorCode::InvalidArgument);
    }
    root.kind = PathRoot::WindowsUnc;
    root.end = cursor;
    root.body = cursor;
    while (root.body < size && Separator(text[root.body], style)) {
        ++root.body;
    }

    return Result<ParsedRoot>::Success(root);
}

bool FullyQualified(PathRoot root) noexcept {
    return root == PathRoot::PosixRoot || root == PathRoot::WindowsDriveAbsolute ||
           root == PathRoot::WindowsUnc;
}

Result<void> Append(String& output, StringView value) noexcept {
    return output.TryAppend(value);
}

Result<void> AppendRoot(String& output, StringView path, const ParsedRoot& root) noexcept {
    switch (root.kind) {
    case PathRoot::Relative:
        return Result<void>::Success();
    case PathRoot::PosixRoot:
    case PathRoot::WindowsCurrentDriveRoot:
        return Append(output, "/");
    case PathRoot::WindowsDriveRelative:
    case PathRoot::WindowsDriveAbsolute: {
        const char drive[] = {UpperDrive(path.Data()[0]), ':'};
        auto appended = Append(output, StringView(drive, 2));
        if (!appended || root.kind == PathRoot::WindowsDriveRelative) {
            return appended;
        }

        return Append(output, "/");
    }
    case PathRoot::WindowsUnc: {
        auto a = Append(output, "//");
        if (!a) {
            return a;
        }

        auto b = Append(output, Slice(path, 2, root.serverEnd - 2));
        if (!b) {
            return b;
        }

        auto c = Append(output, "/");
        if (!c) {
            return c;
        }

        return Append(output, Slice(path, root.shareStart, root.end - root.shareStart));
    }
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<void> AppendComponent(String& output, StringView component,
                             bool driveRoot = false) noexcept {
    if (!output.IsEmpty() && output.Data()[output.ByteCount() - 1] != '/' && !driveRoot) {
        auto separator = Append(output, "/");
        if (!separator) {
            return separator;
        }
    }

    return Append(output, component);
}

bool SameRoot(StringView first, ParsedRoot a, StringView second, ParsedRoot b) noexcept {
    if (a.kind != b.kind) {
        return false;
    }

    if (a.kind == PathRoot::WindowsDriveAbsolute || a.kind == PathRoot::WindowsDriveRelative) {
        return UpperDrive(first.Data()[0]) == UpperDrive(second.Data()[0]);
    }

    if (a.kind == PathRoot::WindowsUnc) {
        return Slice(first, 0, a.end) == Slice(second, 0, b.end);
    }

    return true;
}

} // namespace

PathStyle Path::HostStyle() noexcept {
    return platform::HostPathStyle();
}

Result<PathRoot> Path::Root(StringView path, PathStyle style) noexcept {
    auto result = Parse(path, style);
    return result ? Result<PathRoot>::Success(result.Value().kind)
                  : Result<PathRoot>::Failure(result.Error());
}

Result<String> Path::Normalize(Allocator& allocator, StringView path, PathStyle style) noexcept {
    auto parsed = Parse(path, style);
    if (!parsed) {
        return Result<String>::Failure(parsed.Error());
    }

    const ParsedRoot root = parsed.Value();
    Array<Segment> components(allocator);
    usize used = 0;
    usize cursor = root.body;
    while (cursor < path.ByteCount()) {
        const usize begin = cursor;
        while (cursor < path.ByteCount() && !Separator(path.Data()[cursor], style)) {
            ++cursor;
        }

        const StringView part = Slice(path, begin, cursor - begin);
        while (cursor < path.ByteCount() && Separator(path.Data()[cursor], style)) {
            ++cursor;
        }

        if (part.IsEmpty() || part == ".") {
            continue;
        }

        const bool parent = part == "..";
        if (parent && used != 0 && !components[used - 1].parent) {
            --used;
            continue;
        }

        if (parent && used == 0 && root.kind != PathRoot::Relative &&
            root.kind != PathRoot::WindowsDriveRelative) {
            return Result<String>::Failure(ErrorCode::InvalidArgument);
        }

        const Segment segment{begin, part.ByteCount(), parent};
        if (used < components.Size()) {
            components[used] = segment;
        } else {
            auto pushed = components.TryPushBack(segment);
            if (!pushed) {
                return Result<String>::Failure(pushed.Error());
            }
        }
        ++used;
    }

    String output(allocator);
    auto rootResult = AppendRoot(output, path, root);
    if (!rootResult) {
        return Result<String>::Failure(rootResult.Error());
    }
    // 상대 첫 컴포넌트가 드라이브 표기처럼 보이면 재파싱 시 root가 바뀌지 않게 한다.
    if (style == PathStyle::Windows && root.kind == PathRoot::Relative && used != 0 &&
        components[0].size >= 2 && Letter(path.Data()[components[0].start]) &&
        path.Data()[components[0].start + 1] == ':') {
        auto prefix = Append(output, "./");
        if (!prefix) {
            return Result<String>::Failure(prefix.Error());
        }
    }

    for (usize i = 0; i < used; ++i) {
        const bool driveRoot = root.kind == PathRoot::WindowsDriveRelative && i == 0;
        auto appended = AppendComponent(
            output, Slice(path, components[i].start, components[i].size), driveRoot);
        if (!appended) {
            return Result<String>::Failure(appended.Error());
        }
    }

    if (output.IsEmpty() && !path.IsEmpty()) {
        auto dot = Append(output, ".");
        if (!dot) {
            return Result<String>::Failure(dot.Error());
        }
    }

    return Result<String>::Success(Move(output));
}

namespace {

Result<void> ValidateCompatibilityPath(StringView path, PathStyle style) noexcept {
    // 문법 선택과 UTF-8을 검사하되 드라이브/UNC로 미리 분해하지 않는다. UE 공용
    // 유틸은 중간 상태의 문자열에도 적용되며 파일 시스템에 접근하지 않는다.
    auto validStyle = Parse(StringView(), style);
    if (!validStyle || !path.ValidateUtf8()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    for (usize index = 0; index < path.ByteCount(); ++index) {
        if (path.Data()[index] == '\0') {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    // 장치 네임스페이스는 일반 경로의 구분자 정리만으로 안전하게 변환할 수 없다.
    if (style == PathStyle::Windows && path.ByteCount() >= 4 && Separator(path.Data()[0], style) &&
        Separator(path.Data()[1], style) && (path.Data()[2] == '?' || path.Data()[2] == '.') &&
        Separator(path.Data()[3], style)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
    return Result<void>::Success();
}

Result<void> EraseCompatibilityRange(String& text, usize start, usize count) noexcept {
    const StringView input = text.View();
    if (start > input.ByteCount() || count > input.ByteCount() - start) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    String replacement(text.GetAllocator());
    auto reserved = replacement.TryReserve(input.ByteCount() - count);
    if (!reserved) {
        return reserved;
    }
    auto prefix = replacement.TryAppend(Slice(input, 0, start));
    if (!prefix) {
        return prefix;
    }
    auto suffix =
        replacement.TryAppend(Slice(input, start + count, input.ByteCount() - start - count));
    if (!suffix) {
        return suffix;
    }
    text.Swap(replacement);
    return Result<void>::Success();
}

} // namespace

Result<String> Path::NormalizeFilename(Allocator& allocator, StringView path,
                                       PathStyle style) noexcept {
    auto valid = ValidateCompatibilityPath(path, style);
    if (!valid) {
        return Result<String>::Failure(valid.Error());
    }

    String output(allocator);
    auto reserved = output.TryReserve(path.ByteCount());
    if (!reserved) {
        return Result<String>::Failure(reserved.Error());
    }
    usize begin = 0;
    for (usize index = 0; index < path.ByteCount(); ++index) {
        if (path.Data()[index] != '\\') {
            continue;
        }
        auto segment = output.TryAppend(Slice(path, begin, index - begin));
        if (!segment) {
            return Result<String>::Failure(segment.Error());
        }
        auto separator = output.TryAppend("/");
        if (!separator) {
            return Result<String>::Failure(separator.Error());
        }
        begin = index + 1;
    }
    auto tail = output.TryAppend(Slice(path, begin, path.ByteCount() - begin));
    if (!tail) {
        return Result<String>::Failure(tail.Error());
    }
    return Result<String>::Success(Move(output));
}

Result<String> Path::CollapseRelativeDirectories(Allocator& allocator, StringView path,
                                                 PathStyle style) noexcept {
    auto valid = ValidateCompatibilityPath(path, style);
    if (!valid) {
        return Result<String>::Failure(valid.Error());
    }

    String output(allocator);
    auto copied = output.TryAssign(path);
    if (!copied) {
        return Result<String>::Failure(copied.Error());
    }
    for (;;) {
        const StringView value = output.View();
        const usize size = value.ByteCount();
        if ((size >= 2 && value.Data()[0] == '.' && value.Data()[1] == '.') ||
            (size >= 3 && value.Data()[0] == '/' && value.Data()[1] == '.' &&
             value.Data()[2] == '.')) {
            return Result<String>::Failure(ErrorCode::InvalidArgument);
        }
        usize parent = size;
        for (usize i = 0; i + 2 < size; ++i) {
            if (value.Data()[i] == '/' && value.Data()[i + 1] == '.' &&
                value.Data()[i + 2] == '.' && (i + 3 == size || value.Data()[i + 3] == '/')) {
                parent = i;
                break;
            }
        }
        if (parent == size) {
            break;
        }

        usize previous = parent;
        for (;;) {
            usize search = previous;
            while (search > 0 && value.Data()[search - 1] != '/') {
                --search;
            }
            if (search == 0) {
                previous = 0;
                break;
            }
            previous = search - 1;
            if (previous + 2 >= size || value.Data()[previous + 1] != '.' ||
                value.Data()[previous + 2] != '/') {
                break;
            }
        }
        for (usize i = previous; i < parent + 3; ++i) {
            if (value.Data()[i] == ':') {
                return Result<String>::Failure(ErrorCode::InvalidArgument);
            }
        }
        auto removed = EraseCompatibilityRange(output, previous, parent + 3 - previous);
        if (!removed) {
            return Result<String>::Failure(removed.Error());
        }
    }

    // 원본처럼 이전 일치 지점 앞만 다시 찾는다. 제거로 새로 생긴 중첩은 재처리하지 않는다.
    usize searchEnd = output.ByteCount();
    for (;;) {
        const StringView value = output.View();
        usize match = value.ByteCount();
        for (usize i = searchEnd; i > 1; --i) {
            if (value.Data()[i - 2] == '.' && value.Data()[i - 1] == '/') {
                match = i - 2;
                break;
            }
        }
        if (match == value.ByteCount()) {
            break;
        }
        auto removed = EraseCompatibilityRange(output, match, 2);
        if (!removed) {
            return Result<String>::Failure(removed.Error());
        }
        searchEnd = match;
    }
    return Result<String>::Success(Move(output));
}

Result<String> Path::Combine(Allocator& allocator, StringView left, StringView right) noexcept {
    if (!left.ValidateUtf8() || !right.ValidateUtf8()) {
        return Result<String>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize i = 0; i < left.ByteCount(); ++i) {
        if (left.Data()[i] == '\0') {
            return Result<String>::Failure(ErrorCode::InvalidArgument);
        }
    }
    for (usize i = 0; i < right.ByteCount(); ++i) {
        if (right.Data()[i] == '\0') {
            return Result<String>::Failure(ErrorCode::InvalidArgument);
        }
    }

    bool needsSeparator = false;
    if (!left.IsEmpty()) {
        const char last = left.Data()[left.ByteCount() - 1];
        needsSeparator = last != '/' && last != '\\' && (right.IsEmpty() || right.Data()[0] != '/');
    }

    if (right.ByteCount() > MaxSize - left.ByteCount()) {
        return Result<String>::Failure(ErrorCode::Overflow);
    }
    usize total = left.ByteCount() + right.ByteCount();
    if (needsSeparator) {
        if (total == MaxSize) {
            return Result<String>::Failure(ErrorCode::Overflow);
        }
        ++total;
    }
    // String의 종단 NUL을 위한 크기도 표현 가능해야 한다.
    if (total == MaxSize) {
        return Result<String>::Failure(ErrorCode::Overflow);
    }

    String output(allocator);
    auto reserved = output.TryReserve(total);
    if (!reserved) {
        return Result<String>::Failure(reserved.Error());
    }
    auto copied = output.TryAssign(left);
    if (!copied) {
        return Result<String>::Failure(copied.Error());
    }
    if (needsSeparator) {
        auto separator = output.TryAppend("/");
        if (!separator) {
            return Result<String>::Failure(separator.Error());
        }
    }
    auto appended = output.TryAppend(right);
    if (!appended) {
        return Result<String>::Failure(appended.Error());
    }
    return Result<String>::Success(Move(output));
}

Result<void> Path::Append(String& left, StringView right) noexcept {
    const StringView original = left.View();
    if (!original.ValidateUtf8() || !right.ValidateUtf8()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    for (usize i = 0; i < original.ByteCount(); ++i) {
        if (original.Data()[i] == '\0') {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }
    for (usize i = 0; i < right.ByteCount(); ++i) {
        if (right.Data()[i] == '\0') {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    const char last = original.IsEmpty() ? '\0' : original.Data()[original.ByteCount() - 1];
    const bool separator = !original.IsEmpty() && last != '/' && last != '\\' &&
                           (right.IsEmpty() || right.Data()[0] != '/');
    if (!separator && right.IsEmpty()) {
        return Result<void>::Success();
    }
    if (right.ByteCount() > MaxSize - original.ByteCount()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }
    usize total = original.ByteCount() + right.ByteCount();
    if (separator) {
        if (total == MaxSize) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        ++total;
    }
    if (total == MaxSize) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (total <= left.ByteCapacity()) {
        // suffix를 먼저 이동해 종단 NUL 뒤의 자기 buffer 별칭도 할당 없이 처리한다.
        left.AppendPathInPlace(right, separator);
        return Result<void>::Success();
    }

    auto combined = Combine(left.GetAllocator(), original, right);
    if (!combined) {
        return Result<void>::Failure(combined.Error());
    }
    left.Swap(combined.Value());
    return Result<void>::Success();
}

Result<String> operator/(const String& left, StringView right) noexcept {
    return Path::Combine(left.GetAllocator(), left.View(), right);
}

Result<String> operator/(const String& left, const String& right) noexcept {
    return Path::Combine(left.GetAllocator(), left.View(), right.View());
}

Result<String> operator/(StringView left, const String& right) noexcept {
    return Path::Combine(right.GetAllocator(), left, right.View());
}

Result<String> operator/(Result<String>&& left, StringView right) noexcept {
    if (!left) {
        return Result<String>::Failure(left.Error());
    }
    return left.Value() / right;
}

Result<String> operator/(Result<String>&& left, const String& right) noexcept {
    if (!left) {
        return Result<String>::Failure(left.Error());
    }
    return left.Value() / right;
}

Result<void> operator/=(String& left, StringView right) noexcept {
    return Path::Append(left, right);
}

Result<void> operator/=(String& left, const String& right) noexcept {
    return Path::Append(left, right.View());
}

Result<String> Path::Join(Allocator& allocator, StringView base, StringView relative,
                          PathStyle style) noexcept {
    auto rhs = Parse(relative, style);
    if (!rhs) {
        return Result<String>::Failure(rhs.Error());
    }

    if (rhs.Value().kind != PathRoot::Relative) {
        return Result<String>::Failure(ErrorCode::InvalidArgument);
    }

    auto lhs = Normalize(allocator, base, style);
    if (!lhs) {
        return Result<String>::Failure(lhs.Error());
    }

    if (relative.IsEmpty()) {
        return lhs;
    }

    String joined(allocator);
    auto copied = joined.TryAssign(lhs.Value().View());
    if (!copied) {
        return Result<String>::Failure(copied.Error());
    }

    auto joinedRoot = Parse(joined.View(), style);
    if (!joinedRoot) {
        return Result<String>::Failure(joinedRoot.Error());
    }

    const bool driveRoot =
        joinedRoot.Value().kind == PathRoot::WindowsDriveRelative && joined.ByteCount() == 2;
    auto appended = AppendComponent(joined, relative, driveRoot);
    if (!appended) {
        return Result<String>::Failure(appended.Error());
    }

    return Normalize(allocator, joined.View(), style);
}

Result<String> Path::MakeAbsolute(Allocator& allocator, StringView base, StringView path,
                                  PathStyle style) noexcept {
    auto normalizedBase = Normalize(allocator, base, style);
    if (!normalizedBase) {
        return Result<String>::Failure(normalizedBase.Error());
    }

    auto baseRoot = Parse(normalizedBase.Value().View(), style);
    if (!baseRoot) {
        return Result<String>::Failure(baseRoot.Error());
    }

    if (!FullyQualified(baseRoot.Value().kind)) {
        return Result<String>::Failure(ErrorCode::InvalidArgument);
    }

    auto targetRoot = Parse(path, style);
    if (!targetRoot) {
        return Result<String>::Failure(targetRoot.Error());
    }

    if (FullyQualified(targetRoot.Value().kind)) {
        return Normalize(allocator, path, style);
    }

    if (targetRoot.Value().kind != PathRoot::Relative) {
        return Result<String>::Failure(ErrorCode::InvalidArgument);
    }

    return Join(allocator, normalizedBase.Value().View(), path, style);
}

Result<String> Path::MakeRelative(Allocator& allocator, StringView base, StringView path,
                                  PathStyle style) noexcept {
    auto first = Normalize(allocator, base, style);
    if (!first) {
        return Result<String>::Failure(first.Error());
    }

    auto second = Normalize(allocator, path, style);
    if (!second) {
        return Result<String>::Failure(second.Error());
    }

    const StringView origin = first.Value().View();
    const StringView target = second.Value().View();
    auto a = Parse(origin, style);
    auto b = Parse(target, style);
    if (!a) {
        return Result<String>::Failure(a.Error());
    }

    if (!b) {
        return Result<String>::Failure(b.Error());
    }

    if (!FullyQualified(a.Value().kind) || !FullyQualified(b.Value().kind) ||
        !SameRoot(origin, a.Value(), target, b.Value())) {
        return Result<String>::Failure(ErrorCode::InvalidArgument);
    }

    usize left = a.Value().body;
    usize right = b.Value().body;
    while (left < origin.ByteCount() && right < target.ByteCount()) {
        usize leftEnd = left;
        usize rightEnd = right;
        while (leftEnd < origin.ByteCount() && origin.Data()[leftEnd] != '/') {
            ++leftEnd;
        }

        while (rightEnd < target.ByteCount() && target.Data()[rightEnd] != '/') {
            ++rightEnd;
        }

        if (!(Slice(origin, left, leftEnd - left) == Slice(target, right, rightEnd - right))) {
            break;
        }
        left = leftEnd < origin.ByteCount() ? leftEnd + 1 : leftEnd;
        right = rightEnd < target.ByteCount() ? rightEnd + 1 : rightEnd;
    }

    String output(allocator);
    while (left < origin.ByteCount()) {
        auto added = AppendComponent(output, "..");
        if (!added) {
            return Result<String>::Failure(added.Error());
        }

        while (left < origin.ByteCount() && origin.Data()[left] != '/') {
            ++left;
        }

        if (left < origin.ByteCount()) {
            ++left;
        }
    }

    if (right < target.ByteCount()) {
        if (output.IsEmpty() && style == PathStyle::Windows && target.ByteCount() - right >= 2 &&
            Letter(target.Data()[right]) && target.Data()[right + 1] == ':') {
            auto prefix = Append(output, "./");
            if (!prefix) {
                return Result<String>::Failure(prefix.Error());
            }
        }

        auto added = AppendComponent(output, Slice(target, right, target.ByteCount() - right));
        if (!added) {
            return Result<String>::Failure(added.Error());
        }
    }

    if (output.IsEmpty()) {
        auto dot = Append(output, ".");
        if (!dot) {
            return Result<String>::Failure(dot.Error());
        }
    }

    return Result<String>::Success(Move(output));
}

Result<bool> Path::IsUnder(Allocator& allocator, StringView path, StringView directory,
                           PathStyle style) noexcept {
    if (path.IsEmpty() || directory.IsEmpty()) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    auto child = Normalize(allocator, path, style);
    if (!child) {
        return Result<bool>::Failure(child.Error());
    }

    auto parent = Normalize(allocator, directory, style);
    if (!parent) {
        return Result<bool>::Failure(parent.Error());
    }

    const StringView candidate = child.Value().View();
    const StringView base = parent.Value().View();
    auto a = Parse(candidate, style);
    auto b = Parse(base, style);
    if (!a) {
        return Result<bool>::Failure(a.Error());
    }

    if (!b) {
        return Result<bool>::Failure(b.Error());
    }

    if ((!FullyQualified(a.Value().kind) && a.Value().kind != PathRoot::Relative) ||
        (!FullyQualified(b.Value().kind) && b.Value().kind != PathRoot::Relative)) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    if (FullyQualified(a.Value().kind) != FullyQualified(b.Value().kind)) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    if (!SameRoot(candidate, a.Value(), base, b.Value())) {
        return Result<bool>::Success(false);
    }

    if (a.Value().kind == PathRoot::Relative && base == ".") {
        const bool outside =
            candidate == ".." || (candidate.ByteCount() > 3 && candidate.Data()[0] == '.' &&
                                  candidate.Data()[1] == '.' && candidate.Data()[2] == '/');
        return Result<bool>::Success(!outside);
    }

    if (a.Value().kind == PathRoot::Relative) {
        usize candidateParents = 0;
        usize candidateRest = 0;
        while (candidateRest + 2 <= candidate.ByteCount() &&
               candidate.Data()[candidateRest] == '.' &&
               candidate.Data()[candidateRest + 1] == '.' &&
               (candidateRest + 2 == candidate.ByteCount() ||
                candidate.Data()[candidateRest + 2] == '/')) {
            ++candidateParents;
            candidateRest += 2;
            if (candidateRest < candidate.ByteCount()) {
                ++candidateRest;
            }
        }

        usize baseParents = 0;
        usize baseRest = 0;
        while (baseRest + 2 <= base.ByteCount() && base.Data()[baseRest] == '.' &&
               base.Data()[baseRest + 1] == '.' &&
               (baseRest + 2 == base.ByteCount() || base.Data()[baseRest + 2] == '/')) {
            ++baseParents;
            baseRest += 2;
            if (baseRest < base.ByteCount()) {
                ++baseRest;
            }
        }

        if (candidateParents > baseParents) {
            return Result<bool>::Success(false);
        }

        if (candidateParents < baseParents) {
            return Result<bool>::Success(baseRest == base.ByteCount());
        }
    }

    if (base.ByteCount() > candidate.ByteCount()) {
        return Result<bool>::Success(false);
    }

    bool matches = true;
    for (usize i = 0; i < base.ByteCount(); ++i) {
        char left = candidate.Data()[i];
        char right = base.Data()[i];
        if (i == 0 && a.Value().kind == PathRoot::WindowsDriveAbsolute) {
            left = UpperDrive(left);
            right = UpperDrive(right);
        }

        if (left != right) {
            matches = false;
            break;
        }
    }

    if (!matches) {
        return Result<bool>::Success(false);
    }

    const bool exact = base.ByteCount() == candidate.ByteCount();
    if (!exact && a.Value().kind == PathRoot::Relative &&
        candidate.Data()[base.ByteCount()] == '/' &&
        candidate.ByteCount() - base.ByteCount() >= 3 &&
        candidate.Data()[base.ByteCount() + 1] == '.' &&
        candidate.Data()[base.ByteCount() + 2] == '.' &&
        (candidate.ByteCount() == base.ByteCount() + 3 ||
         candidate.Data()[base.ByteCount() + 3] == '/')) {
        return Result<bool>::Success(false);
    }

    const bool rootWithSlash = !base.IsEmpty() && base.Data()[base.ByteCount() - 1] == '/';
    return Result<bool>::Success(exact || rootWithSlash ||
                                 candidate.Data()[base.ByteCount()] == '/');
}

Result<StringView> Path::FileName(StringView path, PathStyle style) noexcept {
    auto valid = ValidateCompatibilityPath(path, style);
    if (!valid) {
        return Result<StringView>::Failure(valid.Error());
    }
    const usize size = path.ByteCount();
    usize begin = size;
    while (begin > 0 && !Separator(path.Data()[begin - 1], style)) {
        --begin;
    }
    return Result<StringView>::Success(Slice(path, begin, size - begin));
}

Result<StringView> Path::Directory(StringView path, PathStyle style) noexcept {
    auto valid = ValidateCompatibilityPath(path, style);
    if (!valid) {
        return Result<StringView>::Failure(valid.Error());
    }
    usize end = path.ByteCount();
    while (end > 0 && !Separator(path.Data()[end - 1], style)) {
        --end;
    }
    return Result<StringView>::Success(Slice(path, 0, end == 0 ? 0 : end - 1));
}

Result<StringView> Path::Parent(StringView path, PathStyle style) noexcept {
    auto root = Parse(path, style);
    if (!root) {
        return Result<StringView>::Failure(root.Error());
    }

    const usize size = path.ByteCount();
    const usize rootEnd = root.Value().end;
    if (size <= rootEnd) {
        return Result<StringView>::Success(path);
    }

    if (Separator(path.Data()[size - 1], style)) {
        usize end = size;
        while (end > rootEnd && Separator(path.Data()[end - 1], style)) {
            --end;
        }

        return Result<StringView>::Success(Slice(path, 0, end));
    }

    usize begin = size;
    while (begin > rootEnd && !Separator(path.Data()[begin - 1], style)) {
        --begin;
    }

    if (begin <= rootEnd) {
        return Result<StringView>::Success(Slice(path, 0, rootEnd));
    }

    usize end = begin;
    while (end > rootEnd && Separator(path.Data()[end - 1], style)) {
        --end;
    }

    return Result<StringView>::Success(Slice(path, 0, end));
}

namespace {

// 확장자 구분점이 없으면 name.ByteCount()를 반환한다.
usize ExtensionDot(StringView name) noexcept {
    for (usize index = name.ByteCount(); index != 0; --index) {
        if (name.Data()[index - 1] == '.') {
            return index - 1;
        }
    }
    return name.ByteCount();
}

Result<String> ReplaceExtension(Allocator& allocator, StringView path, StringView extension,
                                PathStyle style, bool addIfMissing) noexcept {
    auto filename = Path::FileName(path, style);
    if (!filename) {
        return Result<String>::Failure(filename.Error());
    }

    const StringView name = filename.Value();
    if (!extension.ValidateUtf8()) {
        return Result<String>::Failure(ErrorCode::InvalidArgument);
    }
    for (usize i = 0; i < extension.ByteCount(); ++i) {
        if (extension.Data()[i] == '\0' || Separator(extension.Data()[i], style)) {
            return Result<String>::Failure(ErrorCode::InvalidArgument);
        }
    }

    const usize dot = ExtensionDot(name);
    const bool hasDot = dot < name.ByteCount();
    const usize prefix = !hasDot && !addIfMissing ? path.ByteCount()
                                                  : path.ByteCount() - name.ByteCount() +
                                                        (hasDot ? dot : name.ByteCount());
    String output(allocator);
    auto assigned = output.TryAssign(Slice(path, 0, prefix));
    if (!assigned) {
        return Result<String>::Failure(assigned.Error());
    }
    if ((hasDot || addIfMissing) && !extension.IsEmpty()) {
        if (extension.Data()[0] != '.') {
            auto period = Append(output, ".");
            if (!period) {
                return Result<String>::Failure(period.Error());
            }
        }
        auto added = Append(output, extension);
        if (!added) {
            return Result<String>::Failure(added.Error());
        }
    }
    return Result<String>::Success(Move(output));
}

} // namespace

Result<StringView> Path::Extension(StringView path, PathStyle style) noexcept {
    auto file = FileName(path, style);
    if (!file) {
        return Result<StringView>::Failure(file.Error());
    }

    const StringView name = file.Value();
    const usize dot = ExtensionDot(name);
    if (dot == name.ByteCount()) {
        return Result<StringView>::Success(Slice(name, name.ByteCount(), 0));
    }
    return Result<StringView>::Success(Slice(name, dot + 1, name.ByteCount() - dot - 1));
}

Result<StringView> Path::BaseFileName(StringView path, PathStyle style) noexcept {
    auto file = FileName(path, style);
    if (!file) {
        return Result<StringView>::Failure(file.Error());
    }
    const StringView name = file.Value();
    return Result<StringView>::Success(Slice(name, 0, ExtensionDot(name)));
}

Result<PathParts> Path::Split(StringView path, PathStyle style) noexcept {
    auto directory = Directory(path, style);
    if (!directory) {
        return Result<PathParts>::Failure(directory.Error());
    }
    auto name = BaseFileName(path, style);
    if (!name) {
        return Result<PathParts>::Failure(name.Error());
    }
    auto extension = Extension(path, style);
    if (!extension) {
        return Result<PathParts>::Failure(extension.Error());
    }
    auto clean = FileName(path, style);
    if (!clean) {
        return Result<PathParts>::Failure(clean.Error());
    }
    const StringView partName = clean.Value() == ".." ? clean.Value() : name.Value();
    return Result<PathParts>::Success(PathParts{directory.Value(), partName, extension.Value()});
}

Result<String> Path::ChangeExtension(Allocator& allocator, StringView path, StringView extension,
                                     PathStyle style) noexcept {
    return ReplaceExtension(allocator, path, extension, style, false);
}

Result<String> Path::SetExtension(Allocator& allocator, StringView path, StringView extension,
                                  PathStyle style) noexcept {
    return ReplaceExtension(allocator, path, extension, style, true);
}

} // namespace rocks

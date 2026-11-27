#include "GalleryStrings.h"

#include "../DemoFiles.h"

namespace rocks::ui::gallery {

namespace {

// 엔진 폴더 기준 갤러리 문자열 파일이다.
constexpr char GalleryTextPath[] = "../Programs/UiDemo/Content/GalleryText.txt";
constexpr char Separator[] = " = ";
constexpr usize SeparatorLength = sizeof(Separator) - 1;

bool IsSpace(uint8 byte) noexcept {
    return byte == ' ' || byte == '\t' || byte == '\r';
}

} // namespace

GalleryStrings::GalleryStrings(Allocator& allocator) noexcept
    : bytes_(allocator), entries_(allocator) {
}

Result<void> GalleryStrings::Load() noexcept {
    entries_.Clear();
    if (auto read = ReadEngineFile(GalleryTextPath, bytes_); !read) {
        return read;
    }

    return Parse();
}

Result<void> GalleryStrings::Parse() noexcept {
    usize line = 0;
    while (line < bytes_.Size()) {
        usize end = line;
        while (end < bytes_.Size() && bytes_[end] != '\n') {
            ++end;
        }

        usize last = end;
        while (last > line && IsSpace(bytes_[last - 1])) {
            --last;
        }

        const usize next = end + 1;
        if (last == line || bytes_[line] == '#') {
            line = next;
            continue;
        }

        // "key = value"의 첫 " = "에서 나눈다.
        usize split = line;
        while (split + SeparatorLength <= last &&
               !(bytes_[split] == Separator[0] && bytes_[split + 1] == Separator[1] &&
                 bytes_[split + 2] == Separator[2])) {
            ++split;
        }

        if (split + SeparatorLength > last || split == line) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 값의 "\n"을 줄바꿈 한 byte로 바꾼다. 바꾼 값은 원래 자리에서 앞쪽으로 당겨 쓴다.
        const usize valueStart = split + SeparatorLength;
        usize write = valueStart;
        for (usize read = valueStart; read < last; ++read) {
            if (bytes_[read] == '\\' && read + 1 < last && bytes_[read + 1] == 'n') {
                bytes_[write++] = '\n';
                ++read;
            } else {
                bytes_[write++] = bytes_[read];
            }
        }

        auto added = entries_.TryPushBack({line, split - line, valueStart, write - valueStart});
        if (!added) {
            return added;
        }

        line = next;
    }

    return Result<void>::Success();
}

StringView GalleryStrings::View(usize start, usize length) const noexcept {
    return StringView(reinterpret_cast<const char*>(bytes_.Data()) + start, length);
}

StringView GalleryStrings::Get(StringView key) const noexcept {
    for (const Entry& entry : entries_) {
        if (View(entry.keyStart, entry.keyLength) == key) {
            return View(entry.valueStart, entry.valueLength);
        }
    }

    return key;
}

} // namespace rocks::ui::gallery

#include <Core/Text/StringBuilder.h>
#include "LspFraming.h"

// 언어 서버의 Content-Length 틀이다(Docs/Design/SparksTools.ko.md 10.2절).
namespace rocks::sparks::tools::detail {

namespace {

constexpr StringView LspLengthHeader = "content-length";
constexpr StringView LspLineEnd = "\r\n";

// 틀 머리 builder의 inline 크기다.
constexpr usize LspHeaderInlineBytes = 64;

// 10진 자리 하나의 기수다.
constexpr usize LspDecimalBase = 10;

bool LspSameName(StringView left, StringView right) noexcept {
    return EqualsIgnoringAsciiCase(left, right);
}

StringView LspTrim(StringView text) noexcept {
    usize begin = 0;
    usize end = text.ByteCount();
    while (begin < end && (text.Data()[begin] == ' ' || text.Data()[begin] == '\t')) {
        ++begin;
    }

    while (end > begin && (text.Data()[end - 1] == ' ' || text.Data()[end - 1] == '\t')) {
        --end;
    }

    return StringView(text.Data() + begin, end - begin);
}

} // namespace

Result<bool> LspReader::Fill(MessageChannel& channel) noexcept {
    // 쓴 입력을 앞에서 지운다(버퍼가 커지지 않게).
    if (start_ > 0) {
        const usize remaining = buffer_.Size() - start_;
        for (usize at = 0; at < remaining; ++at) {
            buffer_[at] = buffer_[start_ + at];
        }

        static_cast<void>(buffer_.TryResize(remaining)); // 축소는 실패하지 않는다
        start_ = 0;
    }

    const usize used = buffer_.Size();
    if (auto grown = buffer_.TryResize(used + LspReadChunkBytes); !grown) {
        return Result<bool>::Failure(grown.Error());
    }

    auto read = channel.Read(Span<char>(buffer_.Data() + used, LspReadChunkBytes));
    if (!read) {
        static_cast<void>(buffer_.TryResize(used));
        return Result<bool>::Failure(read.Error());
    }

    static_cast<void>(buffer_.TryResize(used + read.Value()));
    return Result<bool>::Success(read.Value() > 0);
}

Result<LspFrame> LspReader::Next(MessageChannel& channel, StringView& body,
                                 StringView& problem) noexcept {
    // 머리: 빈 줄까지 줄마다 `이름: 값`.
    usize length = 0;
    bool hasLength = false;
    usize cursor = start_;
    for (;;) {
        // 지금 버퍼에서 줄 하나를 찾는다.
        usize end = cursor;
        while (end + 1 < buffer_.Size() && !(buffer_[end] == '\r' && buffer_[end + 1] == '\n')) {
            ++end;
        }

        if (end + 1 >= buffer_.Size()) {
            if (end - cursor > MaxLspHeaderBytes || cursor - start_ > MaxLspHeaderBlockBytes) {
                problem = "a message header is too long";
                return Result<LspFrame>::Success(LspFrame::Broken);
            }

            const usize offset = cursor - start_;
            auto filled = Fill(channel);
            if (!filled) {
                return Result<LspFrame>::Failure(filled.Error());
            }

            cursor = start_ + offset;
            if (!filled.Value()) {
                if (buffer_.Size() == start_) {
                    return Result<LspFrame>::Success(LspFrame::End);
                }

                problem = "the input ended inside a message";
                return Result<LspFrame>::Success(LspFrame::Broken);
            }

            continue;
        }

        const StringView line(buffer_.Data() + cursor, end - cursor);
        if (line.ByteCount() > MaxLspHeaderBytes) {
            problem = "a message header is too long";
            return Result<LspFrame>::Success(LspFrame::Broken);
        }

        // 머리 전체 상한은 줄을 다 읽은 뒤에도 본다(조각 크기와 상관없이 같은 판정).
        cursor = end + LspLineEnd.ByteCount();
        if (cursor - start_ > MaxLspHeaderBlockBytes) {
            problem = "a message header is too long";
            return Result<LspFrame>::Success(LspFrame::Broken);
        }

        if (line.IsEmpty()) {
            break;
        }

        usize colon = 0;
        while (colon < line.ByteCount() && line.Data()[colon] != ':') {
            ++colon;
        }

        if (colon == line.ByteCount()) {
            problem = "a message header has no ':'";
            return Result<LspFrame>::Success(LspFrame::Broken);
        }

        if (LspSameName(LspTrim(StringView(line.Data(), colon)), LspLengthHeader)) {
            const StringView value =
                LspTrim(StringView(line.Data() + colon + 1, line.ByteCount() - colon - 1));
            usize parsed = 0;
            bool digits = !value.IsEmpty();
            for (usize at = 0; at < value.ByteCount() && digits; ++at) {
                const char c = value.Data()[at];
                digits = c >= '0' && c <= '9' &&
                         parsed <= (MaxLspMessageBytes - usize(c - '0')) / LspDecimalBase;
                parsed = digits ? parsed * LspDecimalBase + usize(c - '0') : parsed;
            }

            if (!digits || hasLength) {
                problem = hasLength ? "Content-Length is given twice"
                                    : "Content-Length is not a number up to 64 MiB";
                return Result<LspFrame>::Success(LspFrame::Broken);
            }

            length = parsed;
            hasLength = true;
        }
    }

    if (!hasLength) {
        problem = "a message has no Content-Length";
        return Result<LspFrame>::Success(LspFrame::Broken);
    }

    // 본문.
    while (buffer_.Size() - cursor < length) {
        const usize offset = cursor - start_;
        auto filled = Fill(channel);
        if (!filled) {
            return Result<LspFrame>::Failure(filled.Error());
        }

        cursor = start_ + offset;
        if (!filled.Value()) {
            problem = "the input ended inside a message";
            return Result<LspFrame>::Success(LspFrame::Broken);
        }
    }

    // byte 그대로 넘긴다(잘못된 UTF-8도 — JSON 해석이 ParseError로 답한다).
    body = StringView(buffer_.Data() + cursor, length);
    start_ = cursor + length;
    return Result<LspFrame>::Success(LspFrame::Message);
}

Result<void> LspWriteFrame(MessageChannel& channel, Allocator& allocator,
                           StringView body) noexcept {
    StringBuilder<LspHeaderInlineBytes> frame(allocator);
    frame.AppendFormat("Content-Length: {}\r\n\r\n", body.ByteCount());
    frame.Append(body);
    if (frame.HasFailed()) {
        return Result<void>::Failure(frame.Status().Error());
    }

    return channel.Write(frame.View());
}

} // namespace rocks::sparks::tools::detail

#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <SparksTools/LanguageServer.h>

// 언어 서버의 Content-Length 틀이다(Docs/Design/SparksTools.ko.md 10.2절).
namespace rocks::sparks::tools::detail {

// 본문 상한(64 MiB), 머리 줄 하나의 상한, 머리 전체의 상한, 한 번에 읽는 byte 수다.
inline constexpr usize MaxLspMessageBytes = usize(64) << 20;
inline constexpr usize MaxLspHeaderBytes = 1024;
inline constexpr usize MaxLspHeaderBlockBytes = 4 * MaxLspHeaderBytes;
inline constexpr usize LspReadChunkBytes = usize(64) << 10;

enum class LspFrame : uint8 {
    Message, // 본문 하나를 읽었다
    End,     // 메시지 경계에서 입력이 끝났다
    Broken   // 틀이 깨졌다(problem — 영어 한 줄)
};

// 채널에서 메시지 본문을 하나씩 꺼낸다. 남은 입력은 다음 호출이 쓴다.
class LspReader final {
public:
    explicit LspReader(Allocator& allocator) noexcept : buffer_(allocator) {
    }

    // 다음 본문(byte 그대로 — UTF-8 검사 없음)을 body에 둔다. body는 reader 버퍼를 가리키며 다음
    // Next 호출까지 유효하다. 실패: 입출력(DependencyFailed 등 — 채널 오류 그대로)·할당.
    Result<LspFrame> Next(MessageChannel& channel, StringView& body, StringView& problem) noexcept;

private:
    // 버퍼에 더 읽는다. 값 false는 입력 끝이다.
    Result<bool> Fill(MessageChannel& channel) noexcept;

    Array<char> buffer_;
    usize start_ = 0; // 아직 쓰지 않은 입력의 시작
};

// body에 틀을 붙여 한 번에 쓴다. 실패: 입출력·할당.
Result<void> LspWriteFrame(MessageChannel& channel, Allocator& allocator, StringView body) noexcept;

} // namespace rocks::sparks::tools::detail

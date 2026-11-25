#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>

// Sparks 언어 서버 `sparks-lsp`다(Docs/Design/SparksTools.ko.md 10절 — LC8e).
// LSP 3.17·JSON-RPC 2.0을 Content-Length 틀로 읽고 쓰며, 열린 문서를 덮은 작업 공간을 CLI `check`와
// 같은 Compilation으로 분석해 진단과 의미 강조를 낸다.
//
// 언제 쓰나: 프로그램 `SparksLsp`가 표준 입력·출력·오류를 잇는 채널로 부른다. 시험은 각본 입력을
// 주는 채널로 같은 프로세스에서 부른다.
//
// 사용 예:
//   class StdioChannel final : public sparks::tools::MessageChannel { ... };
//   StdioChannel channel;
//   const int32 code = sparks::tools::RunLanguageServer(allocator, platform::PhysicalFileSystem(),
//                                                       context.Arguments(), channel);
//
// 스레드: 한 호출은 한 스레드에서 메시지를 받은 순서대로 끝까지 처리한다(분석도 그 자리에서 —
// 결정적이다). 전역 상태를 쓰지 않는다.
namespace rocks::sparks::tools {

// 언어 서버의 입출력이다. 바이트는 그대로 오간다(변환 없음 — Content-Length가 byte 수다).
class MessageChannel {
public:
    virtual ~MessageChannel() = default;

    // buffer에 읽은 byte 수를 돌려준다(0은 입력 끝). 실패는 입출력 오류다(서버는 3으로 끝난다).
    virtual Result<usize> Read(Span<char> buffer) noexcept = 0;

    // 출력 byte(틀을 붙인 메시지 전체)를 쓴다. 실패는 입출력 오류다.
    virtual Result<void> Write(StringView bytes) noexcept = 0;

    // 사람이 읽는 기록 한 줄(끝 LF 포함 — 영어)을 쓴다(표준 오류). 실패는 무시한다.
    virtual void Log(StringView line) noexcept = 0;
};

// 언어 서버를 돈다. arguments는 실행 파일 뒤 인자다: `--schema <폴더>`(여러 번), `--stdio`(받고
// 무시). 종료 코드: 0(shutdown 뒤 exit), 1(shutdown 없는 exit·입력 끝·틀 오류), 2(인자 오류),
// 3(입출력·할당 실패). 원문·schema는 files로 읽는다.
ROCKS_SPARKSTOOLS_API int32 RunLanguageServer(Allocator& allocator, platform::FileSystem& files,
                                              Span<const StringView> arguments,
                                              MessageChannel& channel) noexcept;

} // namespace rocks::sparks::tools

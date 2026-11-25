#include <Core/Result.h>
#include <Launch/EntryPoint.h>
#include <Platform/File.h>
#include <Platform/StandardStream.h>
#include <SparksTools/LanguageServer.h>

// Sparks 언어 서버 프로그램이다(Docs/Design/SparksTools.ko.md 10.1절 — LC8e): 서버는 SparksTools가
// 실행하고 이 프로그램은 표준 입력·출력·오류(SE-B7)를 잇는다.
namespace {

using namespace rocks;

// 표준 스트림 채널이다.
class StandardChannel final : public sparks::tools::MessageChannel {
public:
    Result<usize> Read(Span<char> buffer) noexcept override {
        const platform::FileTransfer read =
            platform::ReadStandardInput(buffer.Data(), buffer.Size());
        if (read.status.code == platform::FileCode::EndOfFile) {
            return Result<usize>::Success(0);
        }

        return read.status ? Result<usize>::Success(read.bytes)
                           : Result<usize>::Failure(ErrorCode::DependencyFailed);
    }

    Result<void> Write(StringView bytes) noexcept override {
        const platform::FileTransfer written = platform::WriteStandardStream(
            platform::StandardStream::Output, bytes.Data(), bytes.ByteCount());
        return written.status ? Result<void>::Success()
                              : Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    void Log(StringView line) noexcept override {
        static_cast<void>(platform::WriteStandardStream(platform::StandardStream::Error,
                                                        line.Data(), line.ByteCount()));
    }
};

// 실행 본문이다(Launch 공통 실행 계층이 크래시 수집·명령줄 캡처 뒤 부른다).
int RunMain(launch::ApplicationContext& context) noexcept {
    StandardChannel channel;
    return sparks::tools::RunLanguageServer(
        context.DefaultAllocator(), platform::PhysicalFileSystem(), context.Arguments(), channel);
}

const launch::ApplicationDescription& Describe() noexcept {
    static constexpr launch::ApplicationDescription Description{
        .name = "SparksLsp", .kind = launch::ApplicationKind::Tool, .run = &RunMain};
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)

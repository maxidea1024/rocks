#include <Core/Result.h>
#include <Launch/EntryPoint.h>
#include <Platform/File.h>
#include <Platform/StandardStream.h>
#include <SparksTools/SparksCommand.h>

// Sparks CLI 프로그램이다(Docs/Design/SparksTools.ko.md 2절 — LC8): 명령은 SparksTools가 실행하고
// 이 프로그램은 결과를 표준 스트림(SE-B7)에 잇는다.
namespace {

using namespace rocks;

// 표준 출력·오류에 쓰는 명령 출력 대상이다.
class StandardOutput final : public sparks::tools::CommandOutput {
public:
    Result<void> WriteOutput(StringView text) noexcept override {
        return Write(platform::StandardStream::Output, text);
    }

    Result<void> WriteError(StringView text) noexcept override {
        return Write(platform::StandardStream::Error, text);
    }

private:
    static Result<void> Write(platform::StandardStream stream, StringView text) noexcept {
        const platform::FileTransfer written =
            platform::WriteStandardStream(stream, text.Data(), text.ByteCount());
        return written.status ? Result<void>::Success()
                              : Result<void>::Failure(ErrorCode::DependencyFailed);
    }
};

// 실행 본문이다(Launch 공통 실행 계층이 크래시 수집·명령줄 캡처 뒤 부른다).
int RunMain(launch::ApplicationContext& context) noexcept {
    StandardOutput output;
    return sparks::tools::RunCommand(context.DefaultAllocator(), platform::PhysicalFileSystem(),
                                     context.Arguments(), output);
}

const launch::ApplicationDescription& Describe() noexcept {
    static constexpr launch::ApplicationDescription Description{
        .name = "SparksCli", .kind = launch::ApplicationKind::Tool, .run = &RunMain};
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)

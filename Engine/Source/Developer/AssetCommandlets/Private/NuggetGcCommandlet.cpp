#include <AssetCommandlets/AssetCookCommandlet.h>
#include <AssetCommandlets/AssetCommandletSupport.h>
#include <AssetNugget/NuggetCacheSettings.h>
#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Base/NumericLimits.h>
#include <Platform/File.h>

// nugget-gc 명령이다(Docs/Design/AssetSystem 13.1절 A6, 9.1절 9·16): 프로젝트 너겟 설정의 로컬
// backend를 열고 Collect({maxBytes, unusedDays})를 부른다. 잠금은 없다(여러 프로세스가 동시에 써도
// 무해 — 9.1절 9).
namespace rocks::commandlet_host {

namespace {

constexpr StringView NuggetGcName = "nugget-gc";
constexpr StringView NuggetGcUsage =
    "usage: nugget-gc --max-bytes=<n> --unused-days=<n> [--project=<dir>]\n";
constexpr uint64 DecimalBase = 10;

struct NuggetGcArgs {
    StringView project;
    StringView maxBytes;
    StringView unusedDays;
    bool help = false;
};

// 10진 정수(부호·공백 없음, 1자 이상)를 max 안에서 읽는다.
bool ParseDecimal(StringView text, uint64 max, uint64& out) noexcept {
    if (text.IsEmpty()) {
        return false;
    }

    uint64 value = 0;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        const char c = text.Data()[index];
        if (c < '0' || c > '9') {
            return false;
        }

        const uint64 digit = static_cast<uint64>(c - '0');
        if (value > (max - digit) / DecimalBase) {
            return false;
        }

        value = value * DecimalBase + digit;
    }

    out = value;
    return true;
}

// 인자를 읽는다. 사용법 위반은 false다.
bool ParseNuggetGc(Span<const StringView> args, NuggetGcArgs& out) noexcept {
    if (args.Size() == 1 && args[0] == "--help") {
        out.help = true;
        return true;
    }

    for (StringView argument : args) {
        StringView* const slots[] = {&out.project, &out.maxBytes, &out.unusedDays};
        const StringView names[] = {"--project=", "--max-bytes=", "--unused-days="};
        constexpr usize SlotCount = 3;
        bool matched = false;
        for (usize index = 0; index < SlotCount && !matched; ++index) {
            if (argument.StartsWith(names[index], StringComparison::CaseSensitive)) {
                const StringView value = argument.SliceBytes(
                    names[index].ByteCount(), argument.ByteCount() - names[index].ByteCount());
                if (!slots[index]->IsEmpty() || value.IsEmpty()) {
                    return false;
                }

                *slots[index] = value;
                matched = true;
            }
        }

        if (!matched) {
            return false;
        }
    }

    // 두 옵션 모두 필수다(13.1절 1).
    return !out.maxBytes.IsEmpty() && !out.unusedDays.IsEmpty();
}

} // namespace

Result<int32> RunNuggetGcCommand(void* context, Span<const StringView> args,
                                 CommandletOutput& output) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    NuggetGcArgs parsed;
    uint64 maxBytes = 0;
    uint64 unusedDays = 0;
    if (!ParseNuggetGc(args, parsed) ||
        (!parsed.help &&
         (!ParseDecimal(parsed.maxBytes, NumericLimits<uint64>::Max(), maxBytes) ||
          !ParseDecimal(parsed.unusedDays, NumericLimits<uint32>::Max(), unusedDays)))) {
        return Write(output, CommandletStream::Error, NuggetGcUsage, 2);
    }

    if (parsed.help) {
        return Write(output, CommandletStream::Output, NuggetGcUsage, 0);
    }

    ProjectLayout layout(allocator);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenProject(output, allocator, NuggetGcName, parsed.project, layout, failure)) {
        return failure;
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    NuggetSettingsFailure settingsFailure;
    auto settings = ReadNuggetCacheSettings(allocator, files, layout.Root(), &settingsFailure);
    if (!settings) {
        if (settings.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(settings.Error());
        }

        // 거부 칸이 있으면 InvalidSetting(상한 넘은 파일 포함 — asset-cook과 같은 판정), 없으면
        // 읽기 오류 이름이다.
        const char* code = settingsFailure.field != NuggetSettingsField::None
                               ? AssetDiagnosticName(AssetDiagnosticCode::InvalidSetting)
                               : ErrorCodeName(settings.Error());
        return Fail(output, allocator, NuggetGcName, 1,
                    "{}: {}/{}: cannot read the nugget settings", code, layout.Root(),
                    NuggetSettingsFile);
    }

    // off는 할 일이 없고, rockpile은 아직 backend가 없다(생기면 `rockpile gc`로 위임 — 13절).
    if (settings.Value().backend == NuggetBackendKind::Off) {
        return Write(output, CommandletStream::Output, "nugget-gc: the nugget cache is off\n", 0);
    }

    if (settings.Value().backend == NuggetBackendKind::Rockpile) {
        return Fail(output, allocator, NuggetGcName, 1,
                    "Unsupported: rockpile: the Rockpile nugget backend is not available");
    }

    String root(allocator);
    if (auto resolved = ResolveNuggetLocalPath(settings.Value(), layout.Root(), root); !resolved) {
        return resolved.Error() == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(resolved.Error())
                   : Fail(output, allocator, NuggetGcName, 1, "{}: {}: invalid localPath",
                          ErrorCodeName(resolved.Error()), layout.Root());
    }

    NuggetSystemClock clock;
    NuggetFileSystemBackend backend(allocator, files, root.View(), clock);
    if (auto opened = backend.Open(); !opened) {
        return opened.Error() == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(opened.Error())
                   : Fail(output, allocator, NuggetGcName, 1,
                          "{}: {}: cannot open the nugget cache", ErrorCodeName(opened.Error()),
                          root.View());
    }

    // 읽기 전용(tmp를 만들 수 없는 공유 루트 등)은 지우지 않는다(13.1절 4).
    if (backend.IsReadOnly()) {
        return Fail(output, allocator, NuggetGcName, 1,
                    "Unsupported: {}: the nugget cache is read-only", root.View());
    }

    auto collected =
        backend.Collect(NuggetCollectOptions{maxBytes, static_cast<uint32>(unusedDays)});
    if (!collected) {
        return collected.Error() == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(collected.Error())
                   : Fail(output, allocator, NuggetGcName, 1, "{}: {}: cannot collect",
                          ErrorCodeName(collected.Error()), root.View());
    }

    const NuggetCollectResult& result = collected.Value();
    StringBuilder<LineBytes> line(allocator);
    if (auto made = line.TryAppendFormat(
            "{}: removed {} ({} bytes) kept {} ({} bytes) skipped {} temporaries {}\n",
            NuggetGcName, result.removed, result.removedBytes, result.kept, result.keptBytes,
            result.skipped, result.temporaries);
        !made) {
        return Result<int32>::Failure(made.Error());
    }

    return Write(output, CommandletStream::Output, line.View(), 0);
}

} // namespace rocks::commandlet_host

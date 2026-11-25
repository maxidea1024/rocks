#include <AssetCommandlets/ClassDefaultsCommandlet.h>
#include <AssetCommandlets/AssetCommandletSupport.h>

#include <Core/Text/StringBuilder.h>
#include <ObjectSerialization/ClassDefaults.h>

// 클래스 기본값 덤프다(RuntimeClassMetadata 'A5c 지문 연결' 대조 2). ForEachClassDefaults의
// 레코드 유무 분류를 TypeId 순 줄로 쓴다.
namespace rocks::commandlet_host {

namespace {

constexpr StringView DumpName = "class-defaults-dump";

constexpr StringView DumpUsage =
    "usage: class-defaults-dump\n"
    "  Print one line per concrete class registered by the asset type module (and its\n"
    "  dependencies) in TypeId order:\n"
    "  '<TypeId> present <cooked fingerprint>', '<TypeId> without <reason>' or\n"
    "  '<TypeId> failed <reason> <error>'. Compare the output of build configurations and\n"
    "  platforms: a difference means a constructor default depends on the build.\n";

// 방문 상태다: 줄을 모으고 일시 실패는 오류로 돌려준다.
struct DumpState {
    explicit DumpState(Allocator& allocator) noexcept : text(allocator) {
    }

    StringBuilder<LineBytes> text;
};

Result<void> AppendDumpLine(void* user, TypeId type, const ClassDefaults* defaults,
                            const ClassDefaultsDiagnostic& diagnostic) noexcept {
    auto& state = *static_cast<DumpState*>(user);
    if (defaults != nullptr) {
        char hex[Sha256Digest::HexBytes + 1];
        defaults->Fingerprint(ClassDefaultsConfiguration::Cooked).ToHex(hex);
        return state.text.TryAppendFormat("{} present {}\n", type, StringView(hex));
    }

    if (!IsRememberedClassDefaultsFailure(diagnostic)) {
        return Result<void>::Failure(diagnostic.error != ErrorCode::None ? diagnostic.error
                                                                         : ErrorCode::OutOfMemory);
    }

    if (IsClassWithoutDefaults(diagnostic.reason)) {
        return state.text.TryAppendFormat("{} without {}\n", type,
                                          StringView(ClassDefaultsReasonName(diagnostic.reason)));
    }

    return state.text.TryAppendFormat("{} failed {} {}\n", type,
                                      StringView(ClassDefaultsReasonName(diagnostic.reason)),
                                      StringView(ErrorCodeName(diagnostic.error)));
}

} // namespace

Result<int32> RunClassDefaultsDumpCommand(void* context, Span<const StringView> args,
                                          CommandletOutput& output) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    if (args.Size() == 1 && args[0] == "--help") {
        return Write(output, CommandletStream::Output, DumpUsage, 0);
    }

    if (!args.IsEmpty()) {
        return Write(output, CommandletStream::Error, DumpUsage, 2);
    }

    AssetCommandTypes types(allocator, *command.modules, command.typeModule);
    if (auto started = types.Start(); !started) {
        // 할당 실패는 줄로 바꾸지 않는다(다른 명령과 같다 — 호스트가 쓴다).
        if (started.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(started.Error());
        }

        return Fail(output, allocator, DumpName, 1, "cannot start the asset type module ({})",
                    ErrorCodeName(started.Error()));
    }

    DumpState state(allocator);
    if (auto visited = ForEachClassDefaults(types.Types(), allocator, &AppendDumpLine, &state);
        !visited) {
        if (visited.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(visited.Error());
        }

        return Fail(output, allocator, DumpName, 1, "cannot build class defaults ({})",
                    ErrorCodeName(visited.Error()));
    }

    return Write(output, CommandletStream::Output, state.text.View(), 0);
}

} // namespace rocks::commandlet_host

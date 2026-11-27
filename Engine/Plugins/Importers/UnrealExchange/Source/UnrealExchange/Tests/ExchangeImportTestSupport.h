#pragma once

#include "../../../../../../Source/Developer/AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "ExchangeTestSupport.h"

#include <AssetCommandlets/AssetEditCommandlet.h>
#include <Core/Commandlet/Commandlet.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>
#include <UnrealExchange/ExchangeCommand.h>
#include <UnrealExchange/ExchangeServiceKind.h>
#include <UnrealExchange/UnrealExchangeImporterSet.h>
#include <stdio.h>

// 교환 임포트 명령 시험 공용 도구다(Docs/Design/UnrealExchange 5.5절 12번·6.4절 10번): 교환
// 임포터를 건 명령 문맥, 디스크 프로젝트(golden 묶음 경로), golden 에셋의 asset-import 원본 인자.
namespace rocks::exchange_test {

using namespace rocks::asset_database_test;

struct Captured {
    explicit Captured(Allocator& allocator) noexcept : out(allocator), error(allocator) {
    }

    static Result<void> Write(void* context, CommandletStream stream, StringView text) noexcept {
        Captured& captured = *static_cast<Captured*>(context);
        return (stream == CommandletStream::Output ? captured.out : captured.error).TryAppend(text);
    }

    String out;
    String error;
};

// 교환 임포터(텍스처·정적 메시)를 건 명령 문맥이다(에셋 타입 모듈은 기본 목록).
struct Tool {
    explicit Tool(Allocator& allocator) noexcept : captured(allocator) {
        assets.allocator = &allocator;
        assets.modules = &test::ProcessModules();
        assets.importers = {&RegisterUnrealExchangeImporters, &importers};
        unreal.assets = &assets;
        unreal.kinds = {&RegisterExchangeServiceKinds, &kind};
    }

    int32 Call(Result<int32> (*command)(void*, Span<const StringView>, CommandletOutput&),
               void* context, Span<const StringView> args) noexcept {
        captured.out.Clear();
        captured.error.Clear();
        CommandletOutput output{&Captured::Write, &captured};
        auto ran = command(context, args, output);
        return ran ? ran.Value() : -1;
    }

    int32 Unreal(Span<const StringView> args) noexcept {
        return Call(&RunAssetImportUnrealCommand, &unreal, args);
    }

    int32 Import(Span<const StringView> args) noexcept {
        return Call(&commandlet_host::RunAssetImportCommand, &assets, args);
    }

    int32 Reimport(Span<const StringView> args) noexcept {
        return Call(&commandlet_host::RunAssetReimportCommand, &assets, args);
    }

    int32 Info(Span<const StringView> args) noexcept {
        return Call(&commandlet_host::RunAssetInfoCommand, &assets, args);
    }

    int32 Set(Span<const StringView> args) noexcept {
        return Call(&commandlet_host::RunAssetSetCommand, &assets, args);
    }

    bool Out(StringView text) const noexcept {
        return StringView(captured.out.View()).Contains(text, StringComparison::CaseSensitive);
    }

    bool Error(StringView text) const noexcept {
        return StringView(captured.error.View()).Contains(text, StringComparison::CaseSensitive);
    }

    void Print(const char* label) const noexcept {
        printf("  [%s] out:\n%.*s  error:\n%.*s\n", label,
               static_cast<int>(captured.out.View().ByteCount()), captured.out.View().Data(),
               static_cast<int>(captured.error.View().ByteCount()), captured.error.View().Data());
    }

    UnrealExchangeImporterSet importers;
    ExchangeServiceKind kind;
    commandlet_host::AssetCommandContext assets;
    UnrealExchangeCommandContext unreal;
    Captured captured;
};

// 디스크 프로젝트다.
struct Scene {
    explicit Scene(Allocator& allocator, const char* tag) noexcept
        : allocator(allocator), root(tag), project(allocator), bundle(allocator) {
        ready = root.made && project.TryAppendFormat("--project={}", root.Root()) &&
                WriteText("Scene.rocksproject", "{}\n") &&
                FixtureSourcePath("Golden.rxbundle", bundle);
    }

    bool WriteText(StringView relative, StringView text) noexcept {
        char path[1024] = {};
        FixedStringBuilder builder{Span<char>(path)};
        if (!builder.TryAppendFormat("{}/{}", root.Root(), relative)) {
            return false;
        }

        const StringView full = builder.View();
        usize slash = full.ByteCount();
        while (slash != 0 && full.Data()[slash - 1] != '/') {
            --slash;
        }

        platform::File file;
        return (slash == 0 || platform::MakeDirectoryTree(platform::PhysicalFileSystem(),
                                                          full.Data(), slash - 1)) &&
               static_cast<bool>(file.Open(platform::PhysicalFileSystem(), full.Data(),
                                           full.ByteCount(),
                                           platform::FileOpenMode::CreateAlways)) &&
               static_cast<bool>(file.WriteAll(text.Data(), text.ByteCount()).status) &&
               static_cast<bool>(file.Close());
    }

    // 프로젝트 안 파일의 내용이 text를 담는지다(저장된 패키지의 태그 문자열 확인).
    bool FileContains(StringView relative, StringView text) noexcept {
        StringBuilder<1024> path(allocator);
        platform::File file;
        uint64 size = 0;
        Array<char> bytes(allocator);
        if (!path.TryAppendFormat("{}/{}", root.Root(), relative) ||
            !file.Open(platform::PhysicalFileSystem(), path.CString(), path.ByteCount(),
                       platform::FileOpenMode::ReadExisting) ||
            !file.Size(size) || !bytes.TryResize(static_cast<usize>(size)) ||
            !file.ReadExact(bytes.Data(), bytes.Size()).status) {
            return false;
        }

        return StringView(bytes.Data(), bytes.Size())
            .Contains(text, StringComparison::CaseSensitive);
    }

    Allocator& allocator;
    TempProject root;
    StringBuilder<512> project;
    StringBuilder<512> bundle;
    bool ready = false;
};

inline bool StartExchangeModule() noexcept {
    return test::ProcessModules().Load("UnrealExchange").IsOk();
}

inline bool Keep(const Tool& tool, String& copy) noexcept {
    return static_cast<bool>(copy.TryAssign(tool.captured.out.View()));
}

// golden 정적 메시 하나의 asset-import 원본 인자(`--source.asset=…`, blob마다 `--source.blob:…`)다.
struct SourceArgs {
    explicit SourceArgs(Allocator& allocator) noexcept : texts(allocator), bundle(allocator) {
    }

    Array<String> texts;
    ExchangeBundle bundle;
};

inline bool MakeSourceArgs(Allocator& allocator, StringView directory, StringView objectPath,
                           SourceArgs& out) noexcept {
    ExchangeDiagnostics diagnostics(allocator);
    if (!ReadExchangeBundle(platform::PhysicalFileSystem(), directory, {}, out.bundle,
                            diagnostics)) {
        return false;
    }

    for (const ExchangeAsset& asset : out.bundle.Assets()) {
        if (asset.objectPath != objectPath) {
            continue;
        }

        String document(allocator);
        StringBuilder<1024> text(allocator);
        if (!text.TryAppendFormat("--source.asset={}/{}", directory, asset.file) ||
            !document.TryAssign(text.View()) || !out.texts.TryPushBack(Move(document))) {
            return false;
        }

        for (usize index = 0; index < asset.blobs.Size(); ++index) {
            // 원본 역할은 digest마다 하나다(같은 byte를 다른 배치로 가리키는 참조가 있다).
            const ExchangeBlobRef& blob = asset.blobs[index];
            bool repeated = false;
            for (usize earlier = 0; earlier < index; ++earlier) {
                repeated = repeated || asset.blobs[earlier].digest == blob.digest;
            }

            if (repeated) {
                continue;
            }

            char hex[Sha256Digest::HexBytes + 1];
            blob.digest.ToHex(hex);
            StringBuilder<1024> argument(allocator);
            String copy(allocator);
            if (!argument.TryAppendFormat("--source.blob:{}=",
                                          StringView(hex, Sha256Digest::HexBytes)) ||
                !AppendExchangeBlobPath(directory, blob.digest, argument) ||
                !copy.TryAssign(argument.View()) || !out.texts.TryPushBack(Move(copy))) {
                return false;
            }
        }

        return true;
    }

    return false;
}

} // namespace rocks::exchange_test

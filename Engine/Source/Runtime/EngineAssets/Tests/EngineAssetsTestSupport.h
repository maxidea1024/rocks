#pragma once

#include "../../AssetRuntime/Tests/LoaderTestSupport.h"

#include <Config/Tests/ConfigTestSupport.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <EngineAssets/EngineAssetsSubsystem.h>
#include <Object/ModuleRegistration.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <stdio.h>
#include <string.h>

// EngineAssets 시험 공용 도구다(Docs/Design/AssetSystem 11.8절): 시험 임시 디렉터리에 쓰는
// 디스크 쿠킹 장면, asset.* CVar 준비, 엔진 설정.
namespace rocks::engine_assets_test {

using namespace rocks::asset_test;

constexpr uint64 TestFrames = 6;

inline bool WriteWholeFile(StringView path, Span<const uint8> bytes) noexcept {
    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                   platform::FileOpenMode::CreateAlways)) {
        return false;
    }

    auto written = file.WriteAll(bytes.Data(), bytes.Size());
    return written.status && written.bytes == bytes.Size() && file.Close();
}

inline bool MakeParent(StringView path) noexcept {
    usize slash = path.ByteCount();
    while (slash > 0 && path.Data()[slash - 1] != '/') {
        --slash;
    }

    if (slash <= 1) {
        return true;
    }

    return static_cast<bool>(
        platform::MakeDirectoryTree(platform::PhysicalFileSystem(), path.Data(), slash - 1));
}

// 시험 콘텐츠 서명이다(ContentSigning.ko.md 7절 7): 고정 seed 키(keyId TestContentKeyId), scope
// ("rocks.engine-assets.test", "main"), 최소 버전 TestMinimumContentVersion. 신뢰 정보는 프로세스
// 수명 동안 유효하다.
inline constexpr uint64 TestMinimumContentVersion = 3;
inline constexpr uint64 TestContentVersion = 5;
crypto::KeyId TestContentKeyId() noexcept;
const ContentTrust& TestContentTrust() noexcept;
// manifest를 시험 키로 서명한 봉투를 out에 쓴다(version은 contentVersion, scope가 있으면 시험
// scope 대신 그것).
bool SignTestManifest(Allocator& allocator, Span<const uint8> manifest, Array<uint8>& out,
                      uint64 version = TestContentVersion,
                      const Sha256Digest* scope = nullptr) noexcept;

// 시험 임시 디렉터리에 쓴 쿠킹 장면이다(패키지 파일 + CookedManifest.rman + 시험 키로 서명한
// CookedManifest.rsig). 1 → 2 Hard, 3은 따로다. 수명 동안 시험 신뢰 정보를 EngineAssets에 등록한다
// (끝나면 해제) — Release 구성은 서명 봉투만 받으므로 기본으로 서명한다.
struct DiskScene {
    explicit DiskScene(const char* tag) noexcept : scene(env) {
        EngineAssetsSubsystem::SetContentTrust(&TestContentTrust());
        const int count = snprintf(root, sizeof(root), "rocks-engine-assets-%s-%u", tag,
                                   static_cast<unsigned>(platform::CurrentProcessId()));
        rootBytes = count > 0 ? static_cast<usize>(count) : 0;
        scene.root = StringView(root, rootBytes);
        made = rootBytes != 0 && rootBytes < sizeof(root) &&
               platform::MakeDirectoryTree(platform::PhysicalFileSystem(), root, rootBytes);
        scene.Add(1);
        scene.Add(2);
        scene.Add(3);
        scene.Hard(1, 2);
    }

    ~DiskScene() noexcept {
        EngineAssetsSubsystem::SetContentTrust(nullptr);
        if (made) {
            CHECK(platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root, rootBytes));
        }
    }

    // 장면을 쿠킹해 파일로 쓴다. writeManifest가 거짓이면 매니페스트(rman·봉투)를 쓰지 않는다.
    bool Write(bool writeManifest = true) noexcept {
        if (!made || !env.Ready() || !scene.Cook()) {
            return false;
        }

        for (usize index = 0; index < scene.FileCount(); ++index) {
            const StringView path = scene.PathAt(index);
            if (!MakeParent(path) || !WriteWholeFile(path, scene.FileAt(index).View())) {
                return false;
            }
        }

        if (!writeManifest) {
            return true;
        }

        Array<uint8> bytes(env.allocator);
        return WriteCookedManifest(env.allocator, scene.manifest, bytes).IsOk() &&
               WriteManifestBytes(bytes.View());
    }

    // 매니페스트 byte를 rman으로 쓰고, signEnvelope면 같은 byte를 서명한 봉투도 쓴다(봉투 검증은
    // 통과하고 매니페스트 판정이 그 byte를 본다).
    bool WriteManifestBytes(Span<const uint8> bytes) noexcept {
        if (!WriteRootFile(CookedManifestFileName, bytes)) {
            return false;
        }

        if (!signEnvelope) {
            return true;
        }

        Array<uint8> envelope(env.allocator);
        return SignTestManifest(env.allocator, bytes, envelope) &&
               WriteRootFile(ContentEnvelopeFileName, envelope.View());
    }

    // 쿠킹 루트 바로 아래 name 파일에 bytes를 쓴다.
    bool WriteRootFile(StringView name, Span<const uint8> bytes) noexcept {
        StringBuilder<160> path(env.allocator);
        return path.TryAppend(scene.root).IsOk() && path.TryAppend("/").IsOk() &&
               path.TryAppend(name).IsOk() && WriteWholeFile(path.View(), bytes);
    }

    StringView Root() const noexcept {
        return StringView(root, rootBytes);
    }

    AssetEnv env;
    LoaderScene scene;
    char root[96] = {};
    usize rootBytes = 0;
    // 거짓이면 봉투를 쓰지 않는다(서명하지 않은 개발 쿠킹 디렉터리).
    bool signEnvelope = true;
    bool made = false;
};

// asset.* CVar를 등록하고 쿠킹 루트·프로파일을 넣은 뒤 시작을 마친다(부팅 값 봉인).
inline bool PrepareSettings(config::ConfigContext& context, StringView root,
                            StringView profile = "Windows") noexcept {
    return RegisterEngineAssetsSettings(context).IsOk() &&
           context.AddAssignment(config::ConfigLayer::Session, "test", "asset.cooked_root", root)
               .IsOk() &&
           context
               .AddAssignment(config::ConfigLayer::Session, "test", "asset.runtime_profile",
                              profile)
               .IsOk() &&
           context.Sync().IsOk() && context.CompleteStartup().IsOk();
}

// 시험 에셋 타입(AssetTestFixtures)을 등록한다(EngineConfig::registerModuleTypes — 정의는
// EngineAssetsTestSupport.cpp, 모듈 생성 헤더는 .cpp만 include한다).
Result<void> RegisterFixtureTypes(ModuleRegistration& modules) noexcept;

inline AssetRequest RequestOf(uint64 id) noexcept {
    AssetRequest request;
    request.id = FixedId(id);
    return request;
}

inline EngineConfig MakeEngineConfig(config::ConfigContext* settings,
                                     Span<const EngineSubsystemType> subsystems,
                                     EngineClient* client, uint32 gcWorkers = 0) noexcept {
    EngineConfig config;
    config.modules = &test::ProcessModules();
    config.settings = settings;
    config.subsystems = subsystems;
    config.client = client;
    config.registerModuleTypes = &RegisterFixtureTypes;
    config.fixedFrameDeltaSeconds = 1.0 / 60.0;
    config.gcWorkers = gcWorkers;
    return config;
}

// EngineAssets 로그를 복사해 두는 sink다(11.8절 5의 기록 규칙 확인). 수준·템플릿·완성 메시지를
// 남긴다. 크기가 커서 정적 객체 하나(CapturedLogs)를 시험마다 비워 쓴다.
class EngineAssetsLogCapture final : public RuntimeLogSink {
public:
    static constexpr uint32 MaxRecords = 128;
    static constexpr usize TextBytes = 256;

    struct Record {
        LogLevel level = LogLevel::Trace;
        char messageTemplate[TextBytes]{};
        char message[TextBytes]{};
    };

    Result<void> Write(const LogEvent& event) noexcept override {
        if (event.category != "EngineAssets") {
            return Result<void>::Success();
        }

        ++total;
        if (count == MaxRecords) {
            return Result<void>::Success();
        }

        Record& record = records[count++];
        record.level = event.level;
        Copy(record.messageTemplate, event.messageTemplate);
        Copy(record.message, event.message);
        return Result<void>::Success();
    }

    void Reset() noexcept {
        count = 0;
        total = 0;
    }

    // level이고 템플릿이 text인 기록 수다.
    uint32 Count(LogLevel level, const char* text) const noexcept {
        uint32 matches = 0;
        for (uint32 index = 0; index < count; ++index) {
            if (records[index].level == level &&
                strcmp(records[index].messageTemplate, text) == 0) {
                ++matches;
            }
        }

        return matches;
    }

    // level이고 템플릿이 text인 첫 기록의 완성 메시지다. 없으면 빈 문자열이다.
    const char* Message(LogLevel level, const char* text) const noexcept {
        for (uint32 index = 0; index < count; ++index) {
            if (records[index].level == level &&
                strcmp(records[index].messageTemplate, text) == 0) {
                return records[index].message;
            }
        }

        return "";
    }

    void Print() const noexcept {
        for (uint32 index = 0; index < count; ++index) {
            printf("  log[%u] %s\n", index, records[index].message);
        }
    }

    Record records[MaxRecords];
    uint32 count = 0;
    uint32 total = 0;

private:
    static void Copy(char (&out)[TextBytes], StringView text) noexcept {
        const usize bytes = text.ByteCount() < TextBytes - 1 ? text.ByteCount() : TextBytes - 1;
        memcpy(out, text.Data(), bytes);
        out[bytes] = 0;
    }
};

inline EngineAssetsLogCapture CapturedLogs;

// Manual 모드 LogSystem에 CapturedLogs를 붙인다(설치는 시험이 LogInstallScope로 한다). 앞 시험이
// 설치 밖에서 버린 기록의 알림은 버리는 설치 하나로 먼저 소비한다. Pump 뒤에 CapturedLogs를 본다.
struct ManualEngineAssetsLogs {
    ManualEngineAssetsLogs() noexcept : logs(allocator, MakeConfig()) {
        {
            LogSystem reset(allocator, LogSystemConfig::ForTool());
            (void)reset.Start();
            LogInstallScope drained(reset);
        }

        CapturedLogs.Reset();
        if (!logs.Start() || !logs.AddSink(CapturedLogs)) {
            platform::Fatal("Manual log system did not start");
        }
    }

    static LogSystemConfig MakeConfig() noexcept {
        LogSystemConfig config;
        config.mode = LogMode::Manual;
        config.globalMinimum = LogLevel::Debug;
        return config;
    }

    void Pump() noexcept {
        (void)logs.Pump();
    }

    SystemAllocator allocator;
    LogSystem logs;
};

// EngineAssets 로그 템플릿이다(구현의 문구와 같아야 한다).
inline constexpr const char* InitFailedLog =
    "EngineAssets initialization failed at {Stage}: {Error} (diagnostic {Code}, cooked root "
    "'{Root}', profile '{Profile}')";
inline constexpr const char* StartedLog =
    "EngineAssets started: cooked root '{Root}', profile '{Profile}', {Entries} entries";
inline constexpr const char* NoSettingsLog = "EngineAssets requires an engine config context";
inline constexpr const char* PumpFailedLog = "Asset pump failed: {Error}";
inline constexpr const char* GcFailedLog = "Asset object GC step failed: {Error}";

} // namespace rocks::engine_assets_test

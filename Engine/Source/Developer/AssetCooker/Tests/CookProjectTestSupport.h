#pragma once

#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../AssetDatabase/Tests/EditorPackageSetTestSupport.h"

#include <AssetCooker/CookProject.h>

// 쿠킹 전체 흐름 시험 장면이다(CookProjectTests·CookArtifactTests가 함께 쓴다).
namespace rocks::cook_project_test {

using namespace rocks::editor_set_test;

// AssetCooker 타입(보고서 struct)을 env의 모듈 등록에 넣는다(CookProjectTests.cpp — 모듈 생성
// 헤더는 .cpp만 include한다).
bool RegisterCookerTypes(AssetEnv& env) noexcept;

inline constexpr StringView Manifest = "Saved/Cooked/Windows/CookedManifest.rman";
inline constexpr StringView Report = "Saved/Cooked/Windows/CookReport.json";

// 프로젝트 장면이다: 디스크 임시 루트(잠금)와 그 루트의 메모리 파일 시스템(Content에 Sources 여덟).
struct ProjectScene {
    // registerTypes가 거짓이면 AssetCooker 타입(보고서 struct)을 등록하지 않는다.
    ProjectScene(AssetEnv& sceneEnv, const char* tag, bool registerTypes = true) noexcept
        : env(sceneEnv), sources(env.allocator), root(tag), layout(env.allocator),
          files(env.allocator, Initialized(layout, root)), index(env.allocator, layout),
          set(env.allocator, SetConfig()), processors(env.allocator, env.types, processor),
          profile(env.allocator), settings(env.allocator) {
        String everything(env.allocator);
        ready = (!registerTypes || RegisterCookerTypes(env)) && MakeSources(env, sources) &&
                root.made && files.Ready() && files.AddDirectory("Content") &&
                files.AddDirectory("Content/Characters") &&
                files.AddFile("Content/Base.rasset", sources.base.View()) &&
                files.AddFile("Content/Derived.rasset", sources.derived.View()) &&
                files.AddFile("Content/Characters/Hero.rasset", sources.hero.View()) &&
                files.AddFile("Content/Characters/Villain.rasset", sources.villain.View()) &&
                files.AddFile("Content/Leaf.rasset", sources.leaf.View()) &&
                files.AddFile("Content/CycleA.rasset", sources.cycleA.View()) &&
                files.AddFile("Content/CycleB.rasset", sources.cycleB.View()) &&
                files.AddFile("Content/Hub.rasset", sources.hub.View()) &&
                static_cast<bool>(index.Rebuild(files, ReadOptions())) &&
                static_cast<bool>(set.Initialize()) && static_cast<bool>(lock.Acquire(layout, 0)) &&
                profile.id.TryAssign("Windows") && everything.TryAssign("/Game/") &&
                settings.alwaysCook.TryPushBack(Move(everything));
    }

    static StringView Initialized(ProjectLayout& layout, const TempProject& root) noexcept {
        return layout.Initialize(root.Root()) ? layout.Root() : root.Root();
    }

    static PackageReadOptions ReadOptions() noexcept {
        PackageReadOptions options;
        options.engineVersion = TestEngineVersion;
        return options;
    }

    EditorPackageSetConfig SetConfig() noexcept {
        EditorPackageSetConfig config;
        config.types = &env.types;
        config.files = &files;
        config.layout = &layout;
        config.index = &index;
        config.read.engineVersion = TestEngineVersion;
        config.registryContext = ContextId{0x434f4f4b50524a31ull, 1};
        config.registryOptions.names = &env.names;
        return config;
    }

    CookProjectRequest Request(Span<const StringView> paths = {}) noexcept {
        CookProjectRequest request;
        request.files = &files;
        request.layout = &layout;
        request.lock = &lock;
        request.index = &index;
        request.packages = &set;
        request.types = &env.types;
        request.processors = &processors;
        request.profile = &profile;
        request.profileSettings =
            Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("W"), 1));
        request.settings = &settings;
        request.cookSettings =
            Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("S"), 1));
        request.cookedBy = TestEngineVersion;
        request.paths = paths;
        return request;
    }

    Array<uint8> Copy(StringView relative) noexcept {
        Array<uint8> bytes(env.allocator);
        CHECK(bytes.TryAppend(files.Bytes(relative)));
        return bytes;
    }

    // 공백을 뺀 보고서 텍스트다.
    String ReportText() noexcept {
        String text(env.allocator);
        for (uint8 byte : files.Bytes(Report)) {
            if (byte != ' ' && byte != '\n' && byte != '\r' && byte != '\t') {
                const char character = static_cast<char>(byte);
                CHECK(text.TryAppend(StringView(&character, 1)));
            }
        }

        return text;
    }

    AssetEnv& env;
    Sources sources;
    TempProject root;
    ProjectLayout layout;
    MemoryFileSystem files;
    AssetIndex index;
    EditorPackageSet set;
    DefaultCookProcessor processor;
    CookProcessorTable processors;
    TargetProfile profile;
    CookSettings settings;
    AssetDatabaseLock lock;
    bool ready = false;
};

} // namespace rocks::cook_project_test

#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Text/Name.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// 설정 계층 시험의 설정 struct fixture다(Docs/Design/ConfigSystem.ko.md 4·5·7절 — Config 모듈
// 시험). GraphSettings는 병합 모양(단일 값, enum, 중첩 struct, 배열, Set, Map, struct 원소 배열,
// enum·실수 배열)과 설정 지정자(콘솔 이름, RenderRead, UserConfig, DevelopmentOnly,
// RestartRequired)를 담는다. GameplaySettings는 다른 branch의 섹션과 콘솔 설정 종류(float, int64,
// String, Name)와 StartupOnly를 담는다.
namespace rocks::config_fixture {

ROCKS_ENUM()
enum class FixtureQuality : uint8 {
    Low,
    Medium,
    High
};

ROCKS_STRUCT()
struct FixturePoint {
    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
};

// struct 검증 hook이 있는 값이다(low ≤ high — 설정 층 적용이 게시 전에 거부하는지, Docs/Design/
// CurveData.ko.md 9.1·17.2절).
ROCKS_STRUCT()
struct FixtureSpan {
    ROCKS_PROPERTY() int32 low = 0;
    ROCKS_PROPERTY() int32 high = 0;

    static Result<void> Validate(const FixtureSpan& value) noexcept {
        return value.low <= value.high ? Result<void>::Success()
                                       : Result<void>::Failure(ErrorCode::InvalidArgument);
    }
};

// hook 멤버를 담은 설정 섹션이다(기본 fixture에 등록하지 않는다 — 그 시험이 따로 등록한다).
ROCKS_STRUCT(Settings(Branch = "Render", Section = "SpanCheck"))
struct SpanSettings {
    ROCKS_PROPERTY() FixtureSpan span;
};

// byte 배열 설정 섹션이다(기본 fixture에 등록하지 않는다 — 그 시험이 따로 등록한다). 값은 Base64
// 문자열이고 배열 연산 객체는 InvalidArrayOperation이다(Docs/Design/CurveData.ko.md 17.3절 B5).
ROCKS_STRUCT(Settings(Branch = "Render", Section = "BlobCheck"))
struct BlobSettings {
    explicit BlobSettings(Allocator& allocator) noexcept : blob(allocator) {
    }

    ROCKS_PROPERTY() Array<uint8> blob;
};

// 배열 원소 struct다(원소 안의 배열 연산은 지원하지 않는다 — UnsupportedOperation 시험).
ROCKS_STRUCT()
struct FixtureLayer {
    explicit FixtureLayer(Allocator& allocator) noexcept : names(allocator) {
    }

    ROCKS_PROPERTY() Array<String> names;
};

ROCKS_STRUCT(Settings(Branch = "Render", Section = "RenderGraph"))
struct GraphSettings {
    explicit GraphSettings(Allocator& allocator) noexcept
        : passes(allocator), tags(allocator), cacheDir(allocator), points(allocator),
          byName(allocator), qualities(allocator), scales(allocator), layers(allocator) {
    }

    ROCKS_PROPERTY(ConsoleName = "render.graph.immediate", RenderRead) bool immediate = false;
    ROCKS_PROPERTY(UserConfig) int32 maxPasses = 64;
    ROCKS_PROPERTY(ConsoleName = "render.graph.quality", RenderRead)
    FixtureQuality quality = FixtureQuality::Medium;
    ROCKS_PROPERTY() Array<String> passes;
    ROCKS_PROPERTY() OrderedSet<String> tags;
    ROCKS_PROPERTY() String cacheDir;
    ROCKS_PROPERTY() FixturePoint origin;
    ROCKS_PROPERTY() Array<FixturePoint> points;
    ROCKS_PROPERTY() OrderedMap<String, int32> byName;
    ROCKS_PROPERTY() Array<FixtureQuality> qualities;
    ROCKS_PROPERTY() Array<float> scales;
    ROCKS_PROPERTY() Array<FixtureLayer> layers;
    ROCKS_PROPERTY(DevelopmentOnly, ConsoleName = "render.graph.debug_draw") bool debugDraw = false;
    ROCKS_PROPERTY(RestartRequired) int32 workers = 2;
    // 콘솔 이름 없는 RenderRead 필드(snapshot 이름 `Render.RenderGraph.lodBias`).
    ROCKS_PROPERTY(RenderRead) float lodBias = 1.0f;
};

ROCKS_STRUCT(Settings(Branch = "Game", Section = "Gameplay"))
struct GameplaySettings {
    explicit GameplaySettings(Allocator& allocator) noexcept : title(allocator) {
    }

    ROCKS_PROPERTY(UserConfig, ConsoleName = "game.input.sensitivity") float sensitivity = 1.0f;
    ROCKS_PROPERTY(ConsoleName = "game.rules.score_limit") int64 scoreLimit = 100;
    ROCKS_PROPERTY(ConsoleName = "game.rules.title") String title;
    ROCKS_PROPERTY(ConsoleName = "game.rules.map") Name map;
    ROCKS_PROPERTY(StartupOnly, ConsoleName = "game.rules.seed") int32 seed = 7;
    ROCKS_PROPERTY() int32 lives = 3;
};

// 콘솔 명령 인자 struct다(`render.reload count=2 target=all`).
ROCKS_STRUCT()
struct ReloadArgs {
    explicit ReloadArgs(Allocator& allocator) noexcept : target(allocator) {
    }

    ROCKS_PROPERTY() int32 count = 1;
    ROCKS_PROPERTY() String target;
};

} // namespace rocks::config_fixture

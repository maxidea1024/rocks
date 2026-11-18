#pragma once

#include <Config/ConfigContext.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Text/Json.h>
#include <Object/TypeLease.h>
#include <Platform/File.h>
#include <ReflectionText/TextCodecOptions.h>

// 설정 문맥의 내부 상태다(ConfigContext의 pimpl — 모듈 안 구현만 쓴다).
namespace rocks::config::detail {

// 설정 계층의 진단(약한 쓰기)과 콘솔 명령 실패를 남기는 로그 카테고리다.
inline constexpr LogCategory LogConfig{"Config"};

// 필드 경로 하나의 출처 스택이다. entries는 적용 순서(약한 것부터)의 입력
// 순번(ConfigEntry::sequence)이다.
struct SourceStack {
    explicit SourceStack(Allocator& allocator) noexcept : path(allocator), entries(allocator) {
    }

    String path;
    Array<uint64> entries;
};

// 등록한 설정 struct(섹션) 하나다. value는 현재 값(Sync에서만 바뀐다 — 주소는 고정)이다.
// exported는 마지막 Sync 값의 정규 JSON(변경 감지용)이다.
struct SectionState {
    explicit SectionState(Allocator& allocator) noexcept
        : section(allocator), sources(allocator), exported(allocator) {
    }

    usize branch = 0;
    String section;
    TypeLease lease;
    SettingsTypeOps ops;
    void* value = nullptr;
    Array<UniquePtr<SourceStack>> sources;
    String exported;
};

// 입력 하나의 종류다.
enum class EntryKind : uint8 {
    // branch 파일 내용(섹션 여러 개와 `Settings`).
    Document,
    // 대입 하나(`<Branch>.<Section>.<경로>=<값>`).
    Assignment,
    // 콘솔 설정 이름 대입 하나(`render.graph.immediate=true` — 콘솔 입력·세션). path는 이름이다.
    Console
};

// 층 입력 하나다. Sync가 적용 순서(층, sequence)로 정렬해 쓴다.
struct ConfigEntry {
    explicit ConfigEntry(Allocator& allocator) noexcept
        : tag(allocator), sourceName(allocator), path(allocator), value(allocator) {
    }

    EntryKind kind = EntryKind::Document;
    ConfigLayer layer = ConfigLayer::Engine;
    uint64 sequence = 0;
    String tag;
    String sourceName;
    // Document: branch 번호와 읽은 문서.
    usize branch = 0;
    UniquePtr<JsonDocument> document;
    // Assignment: 섹션 번호·필드 경로·값 텍스트. Console: path가 콘솔 이름.
    usize section = 0;
    String path;
    String value;
};

// 큐 항목이다: 입력 추가 또는 (층, 태그) 빼기.
struct QueuedChange {
    explicit QueuedChange(Allocator& allocator) noexcept : tag(allocator) {
    }

    UniquePtr<ConfigEntry> entry;
    ConfigLayer layer = ConfigLayer::Engine;
    String tag;
};

// 콘솔 설정 하나다(독립 설정 또는 설정 struct 필드). 필드 설정이면 section·field가 그 필드를
// 가리키고 값·출처는 섹션의 것이다(value는 Sync 때 필드에서 읽은 사본).
struct ConsoleSettingState {
    explicit ConsoleSettingState(Allocator& allocator) noexcept
        : name(allocator), help(allocator), allowed(allocator), defaultValue(allocator),
          value(allocator), text(allocator), sources(allocator) {
    }

    String name;
    String help;
    ConsoleValueType type = ConsoleValueType::Bool;
    ValueKind kind = ValueKind::Bool;
    TypeLease enumLease;
    const EnumSchema* enumSchema = nullptr;
    uint32 flags = 0;
    bool hasRange = false;
    double minimum = 0;
    double maximum = 0;
    Array<String> allowed;
    // String·Name 허용 값을 대소문자 무시로 맞춘다(Enum은 늘 무시).
    bool allowedIgnoreCase = false;
    // 필드 설정: 섹션 번호와 스키마 필드 번호(독립 설정은 IndexNone).
    usize section = IndexNone;
    usize field = IndexNone;
    FieldValue defaultValue;
    // 현재 값과 그 콘솔 표기(변경 감지·출력)다.
    FieldValue value;
    String text;
    // 독립 설정의 출처 스택(입력 순번 — 약한 것부터).
    Array<uint64> sources;
};

// 콘솔 명령 하나다. args.create가 있으면 인자 struct 스키마가 있다.
struct ConsoleCommandState {
    explicit ConsoleCommandState(Allocator& allocator) noexcept : name(allocator), help(allocator) {
    }

    String name;
    String help;
    ConsoleCommandFunction execute = nullptr;
    void* context = nullptr;
    TypeLease lease;
    SettingsTypeOps args;
};

// 다음 Sync에 실행할 명령 하나다(인자 값은 args.create로 만들었다).
struct PendingCommand {
    usize command = 0;
    void* arguments = nullptr;
    ConsoleOutput output;
};

// 재시작 대기 값 하나다(시작 뒤 RestartRequired 값이 바뀐 것 — 8절). name은 콘솔 설정 이름이거나
// `<Branch>.<Section>.<필드>`이고 pending·current는 값 표기다(필드는 compact JSON, 콘솔 설정은 콘솔
// 표기).
struct PendingRestart {
    explicit PendingRestart(Allocator& allocator) noexcept
        : name(allocator), pending(allocator), current(allocator) {
    }

    String name;
    String pending;
    String current;
};

// sync 지점 앞 hook 하나다(ConfigContext::AddSyncHook).
struct SyncHook {
    ConfigSyncHook hook = nullptr;
    void* context = nullptr;
};

// 변경 구독 하나다(setting 또는 section 하나를 가리킨다).
struct Subscription {
    uint64 id = 0;
    usize setting = IndexNone;
    usize section = IndexNone;
    ConfigChangeCallback callback = nullptr;
    void* context = nullptr;
};

struct ConfigState {
    ConfigState(Allocator& allocator, ObjectRegistry& objects) noexcept;
    ~ConfigState();

    Allocator& allocator;
    ObjectRegistry& objects;
    ObjectAccessContext access;
    uint64 owner = 0;
    ConfigMode mode = ConfigMode::Sealed;
    bool releaseRules = false;
    String platform;
    Array<String> platformParents;
    // ConfigPathToken 순서의 디렉터리 값.
    String directories[4];
    String projectName;
    Array<String> branches;
    Array<UniquePtr<SectionState>> sections;
    // 적용 중인 입력(넣은 순서).
    Array<UniquePtr<ConfigEntry>> entries;
    Array<UniquePtr<QueuedChange>> queue;
    uint64 nextSequence = 1;
    // 섹션 등록처럼 큐 밖의 변경이 있어 다음 Sync가 다시 합쳐야 한다.
    bool dirty = false;
    ConfigDiagnostics diagnostics;
    // 콘솔 설정·명령(CF2).
    Array<String> regions;
    Array<UniquePtr<ConsoleSettingState>> settings;
    Array<UniquePtr<ConsoleCommandState>> commands;
    Array<PendingCommand> pendingCommands;
    Array<Subscription> subscriptions;
    uint64 nextSubscription = 1;
    SharedPtr<ConfigSnapshot> snapshot;
    uint64 snapshotGeneration = 0;
    // 마지막 snapshot 값의 서명(이름 순서 `이름=표기` 줄)이다. 같으면 snapshot을 재사용한다.
    String renderSignature;
    bool startupComplete = false;
    bool syncing = false;
    // 재시작 대기 값(마지막 Sync 기준 — CF3).
    Array<UniquePtr<PendingRestart>> pendingRestarts;
    // 진단을 이미 낸 무시된 시작 뒤 쓰기(이름·층·출처를 줄바꿈으로 이은 키) — 같은 것에 한 번만
    // 낸다.
    Array<String> ignoredBootWrites;
    // sync 지점 앞 hook(등록 순서).
    Array<SyncHook> syncHooks;

    // 텍스트 codec 옵션(Edit 용도, Map 키별 병합, 대상 Registry의 Name pool).
    reflection_text::TextCodecOptions CodecOptions() const noexcept;

    // 이름이 같은 branch 번호다(대소문자 구분). 없으면 IndexNone.
    usize FindBranch(StringView name) const noexcept;

    // (branch, section) 섹션 번호다(섹션은 ASCII 대소문자 무시). 없으면 IndexNone.
    usize FindSection(usize branch, StringView section) const noexcept;

    // 입력 순번의 입력이다(없으면 null).
    const ConfigEntry* FindEntry(uint64 sequence) const noexcept;

    // 이름이 같은 콘솔 설정 번호다(출시 규칙에서 숨긴 것 포함). 없으면 IndexNone.
    usize FindSetting(StringView name) const noexcept;

    // 이름이 같은 콘솔 명령 번호다. 없으면 IndexNone.
    usize FindCommand(StringView name) const noexcept;
};

// 섹션 하나를 다시 합친 결과다(Sync·사용자 층 차이 계산이 만든다). value는 ops.create로 만든
// 값이고 소유자가 ops.destroy로 돌려준다.
struct SectionResult {
    explicit SectionResult(Allocator& allocator) noexcept : sources(allocator) {
    }

    void* value = nullptr;
    Array<UniquePtr<SourceStack>> sources;
};

// 섹션 하나를 코드 기본값에서 order 순서의 입력으로 다시 합친다(거부되는 입력은 그 섹션에서
// 건너뛰고 진단을 남긴다). 실패하면 out.value가 만들어졌을 수 있다(호출자가 돌려준다).
Result<void> RecomputeSection(ConfigState& state, SectionState& section, usize sectionIndex,
                              Span<ConfigEntry* const> order, SectionResult& out) noexcept;

// 적용 중인 입력(state.entries)에서 keep이 참인 것을 적용 순서(층, 넣은 순서)로 out에 쓴다.
Result<void> SortedEntries(ConfigState& state, bool (*keep)(const ConfigEntry& entry) noexcept,
                           Array<ConfigEntry*>& out) noexcept;

// 섹션 값 storage의 정규 JSON(ExportJson — 변경 감지·차이 계산용)을 out에 쓴다.
Result<void> ExportSection(ConfigState& state, const SectionState& section, void* storage,
                           String& out) noexcept;

// 시작 뒤 RestartRequired 값이 바뀌었으면 지금 값(sections[i]->value·settings[i]->value)을
// 유지한다(Sync가 값 계산 뒤, 맞바꾸기 전에 부른다). 새 값의 가장 강한 출처가 파일 층(코드
// 기본값·엔진·엔진 플랫폼·프로젝트·프로젝트 플랫폼·사용자 — 다음 실행에도 같은 입력이 다시
// 온다)이면 재시작 대기로 기록하고, 그 밖의 층(기기 프로필·품질·기능 묶음 같은 파생 층, 세션·콘솔
// 같은 휘발 층)이면 대기 없이 Warning BootValueIgnored를 같은 이름·출처에 한 번 낸다. order는 이번
// Sync의 적용 입력이다(출처 층 조회). values·sources는 섹션 순서의 새 값과 출처 스택, settings는
// 콘솔 설정 순서의 새 값이다(필드 설정은 values를 고친 뒤 다시 읽어야 하므로 호출자가
// ComputeSettingResults 전에 HoldRestartSections를, 뒤에 HoldRestartSettings를 부른다). pending에
// 새 대기 목록을 쓴다.
Result<void> HoldRestartSections(ConfigState& state, Span<ConfigEntry* const> order,
                                 Span<void* const> values,
                                 Span<Array<UniquePtr<SourceStack>>* const> sources,
                                 Array<UniquePtr<PendingRestart>>& pending) noexcept;

// path 파일을 out에 통째로 읽는다(files의 파일 시스템). 반환: 없으면 false(out은 비운다). 오류:
// 파일 상태의 대응 ErrorCode, 할당 실패.
Result<bool> ReadWholeFile(platform::FileSystem& files, StringView path, Array<char>& out) noexcept;

// 진단 하나를 더한다(실패는 할당 오류).
Result<void> Report(ConfigState& state, ConfigDiagnosticCode code, ConfigSeverity severity,
                    ConfigLayer layer, StringView source, const JsonPosition* position,
                    StringView path, StringView message) noexcept;

// 설정 필드의 층 허용 판정이다(UserConfig·DevelopmentOnly, incoming이면 시작 뒤 StartupOnly). 쓸 수
// 없으면 거부 사유 문장을, 쓸 수 있으면 빈 view를 돌려준다.
StringView CheckLayerAllowed(const ConfigState& state, ConfigLayer layer, uint32 settingsFlags,
                             bool incoming) noexcept;

// 콘솔 이름으로 쓰는 값의 층 허용 판정이다(콘솔 플래그 — 읽기 전용·품질 구성원·개발 전용·사용자
// 설정·시작 뒤 고정). code에 거부 진단 종류를 쓴다. 쓸 수 있으면 빈 view다.
StringView CheckConsoleLayer(const ConfigState& state, ConfigLayer layer, uint32 consoleFlags,
                             bool incoming, ConfigDiagnosticCode& code) noexcept;

// 설정 필드 플래그(SettingsFieldFlags)를 콘솔 플래그(ConsoleSettingFlags)로 옮긴다.
uint32 ConsoleFlagsOfField(uint32 settingsFlags) noexcept;

// 섹션의 직접 멤버 필드 ID에 붙은 설정 플래그다(없으면 0).
uint32 SettingsFlagsOf(const SectionState& section, Guid field) noexcept;

// 섹션에서 콘솔 이름이 붙은 필드의 스키마 번호다(없으면 IndexNone).
usize ConsoleFieldOf(const SectionState& section, StringView name) noexcept;

// 입력이 섹션에 닿는지다(섹션 객체, 그 섹션 필드의 콘솔 이름 — `Settings` 멤버·콘솔 대입, 대입
// 대상).
bool EntryTouchesSection(const ConfigState& state, usize sectionIndex,
                         const ConfigEntry& entry) noexcept;

// 입력 하나를 섹션 값(storage)에 적용한다(입력의 그 섹션 부분 — 섹션 객체, 그 섹션 필드를 가리키는
// `Settings` 멤버와 콘솔 대입, 대입). incoming이면 새로 들어오는 입력의 검사다(시작 뒤 StartupOnly
// 거부). paths에 override 경로 텍스트를 더한다. 반환: true면 적용했다, false면 진단을 남기고
// storage를 바꾸지 않았다(부분 적용 없음은 아니다 — 섹션 객체를 적용한 뒤 콘솔 값이 거부될 수 있다.
// 호출자는 false면 그 storage를 버린다). 오류: 할당 실패 등 호출 오류.
Result<bool> ApplyEntryToSection(ConfigState& state, SectionState& section, void* storage,
                                 const ConfigEntry& entry, bool incoming,
                                 Array<String>& paths) noexcept;

// 입력 하나를 닿는 섹션마다 코드 기본값 위에 적용해 보고 독립 콘솔 설정 값을 검사한다(큐에 넣기 전
// — 진단은 state에 남긴다). 반환: true면 문제없음.
Result<bool> ValidateEntry(ConfigState& state, const ConfigEntry& entry) noexcept;

// Document 입력의 `Settings` 섹션 모양을 검사한다(이름 문법·값 모양). 반환: true면 문제없음.
Result<bool> CheckSettingsSection(ConfigState& state, const ConfigEntry& entry,
                                  JsonValue settings) noexcept;

// 섹션 객체에서 키(대소문자 무시)로 찾은 값이다(없으면 무효 값).
JsonValue FindMemberIgnoringCase(JsonValue object, StringView key) noexcept;

// Document 입력의 `Settings` 섹션이다(없으면 무효 값).
JsonValue SettingsSectionOf(const ConfigEntry& entry) noexcept;

// 콘솔 값 JSON(bool·수·문자열)을 콘솔 표기로 out에 쓴다. kind와 JSON 종류가 맞지 않으면 false다.
Result<bool> ConsoleTextOfJson(JsonValue value, ValueKind kind, String& out) noexcept;

// 독립 콘솔 설정 하나의 값을 order(적용 순서) 입력으로 다시 계산해 value·text·sources에 쓴다.
// report면 거부한 입력마다 진단을 남긴다(등록 때 대기 값 이관). 오류: 할당 실패.
Result<void> RecomputeConsoleSetting(ConfigState& state, const ConsoleSettingState& setting,
                                     Span<ConfigEntry* const> order, bool report, FieldValue& value,
                                     String& text, Array<uint64>& sources) noexcept;

// 콘솔 값 하나를 검사한다(층 허용·형식·범위·허용 값). 받으면 out에 값을 쓰고 빈 view를, 거부하면
// code·사유를 돌려준다. 오류: 할당 실패.
Result<StringView> CheckConsoleValue(ConfigState& state, const ConsoleSettingState& setting,
                                     ConfigLayer layer, bool incoming, StringView text,
                                     FieldValue& out, ConfigDiagnosticCode& code) noexcept;

// 값의 콘솔 표기(리플렉션 명령행 단일 값)를 out에 쓴다.
Result<void> FormatConsoleValue(ConfigState& state, const ConsoleSettingState& setting,
                                const FieldValue& value, String& out) noexcept;

// value를 out에 복사한다(문자열 포함).
Result<void> CopyFieldValue(const FieldValue& value, FieldValue& out) noexcept;

// 섹션 하나의 콘솔 이름 필드를 콘솔 설정으로 등록한다(이름 문법·영역·유일성을 먼저 모두 본다 —
// 실패하면 아무것도 등록하지 않는다). 오류: 문법·영역 InvalidArgument, 겹침 AlreadyExists, 할당
// 실패.
Result<void> RegisterFieldSettings(ConfigState& state, usize section) noexcept;

// Sync가 계산한 콘솔 설정 하나의 새 값이다.
struct SettingResult {
    explicit SettingResult(Allocator& allocator) noexcept
        : value(allocator), text(allocator), sources(allocator) {
    }

    FieldValue value;
    String text;
    Array<uint64> sources;
    bool changed = false;
};

// 모든 콘솔 설정의 새 값을 계산한다(독립 설정은 order로 다시 합치고, 필드 설정은 새 섹션 값
// sectionValues에서 읽는다). out은 state.settings와 같은 순서다.
Result<void> ComputeSettingResults(ConfigState& state, Span<ConfigEntry* const> order,
                                   Span<void* const> sectionValues,
                                   Array<UniquePtr<SettingResult>>& out) noexcept;

// HoldRestartSections의 콘솔 설정 짝이다(독립 콘솔 설정만 — 필드 설정은 섹션 값을 따른다). 대기로
// 돌린 결과는 지금 값·표기·출처로 되돌리고 changed를 끈다.
Result<void> HoldRestartSettings(ConfigState& state, Span<ConfigEntry* const> order,
                                 Span<const UniquePtr<SettingResult>> settings,
                                 Array<UniquePtr<PendingRestart>>& pending) noexcept;

// 새 값으로 설정 snapshot을 만든다. RenderRead 값의 서명(signature에 쓴다)이 이전과 같고 snapshot이
// 이미 있으면 out은 null(재사용)이다. 아니면 세대 +1의 새 snapshot이다.
Result<void> PrepareSnapshot(ConfigState& state, Span<const UniquePtr<SettingResult>> settings,
                             Span<void* const> sectionValues, SharedPtr<ConfigSnapshot>& out,
                             String& signature) noexcept;

// 맞바꾼 뒤의 일이다: 큐에 든 콘솔 명령을 실행하고, 값이 바뀐 대상의 구독 콜백을 구독 순서대로
// 부른다.
void RunCommandsAndCallbacks(ConfigState& state, Span<const bool> sectionChanged,
                             Span<const bool> settingChanged) noexcept;

} // namespace rocks::config::detail

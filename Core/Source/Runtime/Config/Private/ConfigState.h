#pragma once

#include <Config/CVarGroup.h>
#include <Config/ConfigContext.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Text/Json.h>
#include <Object/TypeLease.h>
#include <Platform/File.h>
#include <Platform/Sync.h>
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
    // 봉인 뒤 바로 쓰는 층(콘솔·코드·범위 임시 변경)으로 들어온 입력이다(Sync가 순번을 매길 때
    // 정한다). 그 뒤에 등록한 부팅 독립 콘솔 설정은 이 입력을 받지 않는다(BootSealed — CVar 9절).
    bool sealedDirectWrite = false;
    // 프리셋 묶음 펼침이 만든 품질 묶음 층 입력이다(Console 종류 — tag는 묶음 이름, sourceName은
    // `quality.shadow@2`). Sync가 묶음 해결 때마다 모두 다시 만든다(CVar 10·11절).
    bool groupExpansion = false;
};

// 큐 항목이다: 입력 추가 또는 빼기.
// entry가 있으면 입력을 더한다. replace면 그 앞에 같은 (층, 태그, 콘솔 이름)의 콘솔 이름 입력을
// 뺀다
// (`CVar::Set` — 같은 자리의 이전 쓰기를 바꾼다). validateAtApply면 Sync가 적용 전에 소유
// 스레드에서 다시 검사하고(진단을 남긴다) 거부되면 항목 전체를 버린다(다른 스레드에서 넣은 쓰기
// — 6.2절). entry가 없으면 빼기다. allTags면 층 전체, 아니면 (층, 태그)이고 path가 있으면 그 콘솔
// 이름 입력만 뺀다(`CVar::Clear`·`ClearLayer`·Unset).
// output은 그 입력을 낸 콘솔 sink다(다른 스레드의 콘솔 입력 — 꺼낼 때 거부하면 여기에 알린다).
// line이 있으면 쓰기가 아니라 게임 lane이 실행할 콘솔 입력 한 줄이다(다른 스레드의 `<이름>`·`<이름>
// ?`·명령 — CVar 15절).
struct QueuedChange {
    explicit QueuedChange(Allocator& allocator) noexcept
        : tag(allocator), path(allocator), line(allocator) {
    }

    UniquePtr<ConfigEntry> entry;
    ConfigLayer layer = ConfigLayer::Engine;
    String tag;
    String path;
    bool replace = false;
    bool allTags = false;
    bool validateAtApply = false;
    SharedPtr<ConsoleSink> sink;
    String line;
};

// 문맥의 쓰기 통로다(10.1절 — 다른 스레드가 문맥에 들어오는 길은 이것 하나다). 문맥과 그 문맥이
// 만든 snapshot들이 참조 계수로 공유하므로 문맥보다 오래 살 수 있다(open이 거짓이면 쓰기를 받지
// 않는다). mutex가 queue·current·open을 지킨다. 읽기 경로(`Get`)는 이 잠금을 잡지 않는다.
struct WriteChannel {
    explicit WriteChannel(Allocator& allocator) noexcept : allocator(allocator), queue(allocator) {
    }

    Allocator& allocator;
    platform::Mutex mutex;
    // 넣은 순서의 쓰기(소유 스레드의 층 입력 포함 — 하나의 순서다).
    Array<SharedPtr<QueuedChange>> queue;
    // queue 안의 다른 스레드 쓰기 수(MaxQueuedChanges 한도는 이것에만 건다 — 소유 스레드 입력은
    // 소유자가 스스로 조절한다).
    usize limitedChanges = 0;
    // 미리 만들어 둔 범위 임시 변경 지우기 수다. queue는 늘 이만큼 자리를 더 잡아 두어 지우기를
    // 넣을 때 할당하지 않는다(실패 경로가 없다).
    usize reservedRemovals = 0;
    // 문맥 소유 스레드(게임 lane)다.
    uint64 owner = 0;
    // 마지막으로 게시한 snapshot(Acquire가 돌려준다).
    SharedPtr<const ConfigSnapshot> current;
    bool open = true;
    // 문맥 옵션의 사본(다른 스레드의 쓰기 검사가 읽는다 — 만든 뒤 바뀌지 않는다).
    bool releaseRules = false;
    // 네트워크 역할(NetworkRole 값 — 원자, 소유 스레드가 SetNetworkRole로 쓴다).
    uint8 networkRole = 0;
    // 살아 있는 cvar::SnapshotBinding 수(원자 — 문맥 파괴 때 0이어야 한다).
    uint64 bindings = 0;
    // 프로세스 기본 문맥을 거쳐 이 통로를 쓰는 중인 묶음 없는 쓰기 수다(mutex가 지킨다 — CVar
    // 6.2절). 문맥 파괴는 기본 문맥을 푼 뒤 이 수가 0이 될 때까지 defaultIdle로 기다린다(그래서
    // 통로의 마지막 해제가 소유 스레드에서 일어난다).
    usize defaultWriters = 0;
    platform::ConditionVariable defaultIdle;
};

// 이름 블록이다(snapshot의 RenderRead 이름 값 — 만든 뒤 바뀌지 않는다).
struct NamedBlock {
    explicit NamedBlock(Allocator& allocator) noexcept : entries(allocator) {
    }

    struct Entry {
        explicit Entry(Allocator& allocator) noexcept : name(allocator), value(allocator) {
        }

        String name;
        FieldValue value;
    };

    Array<UniquePtr<Entry>> entries;
};

// 콘솔 설정 하나다(CVar — 핸들·동적 변수, 또는 설정 struct 필드). 필드 설정이면 section·field가
// 그 필드를 가리키고 값·출처는 섹션의 것이다(value는 Sync 때 필드에서 읽은 사본).
struct ConsoleSettingState {
    explicit ConsoleSettingState(Allocator& allocator) noexcept
        : name(allocator), help(allocator), defaultValue(allocator), value(allocator),
          text(allocator), sources(allocator) {
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
    // 필드 설정: 섹션 번호와 스키마 필드 번호(독립 설정은 IndexNone).
    usize section = IndexNone;
    usize field = IndexNone;
    FieldValue defaultValue;
    // 현재 값과 그 콘솔 표기(변경 감지·출력)다.
    FieldValue value;
    String text;
    // 독립 설정의 출처 스택(입력 순번 — 약한 것부터).
    Array<uint64> sources;
    // CVar면 그 정의다(범위·보정·Validate 검사와 멱등 등록 판정 — Docs/Design/CVarSystem.ko.md
    // 5절). 핸들 정의는 정적 저장 기간이라 문맥보다 오래 살고, 동적 변수 정의는 문맥이
    // 소유한다(dynamicVars). 설정 struct 필드는 null이다.
    const cvar::detail::VarDefinition* definition = nullptr;
};

// 콘솔 명령 하나다(ConsoleCommand 핸들 — CVar 13·15절). target은 BindCommand로 묶은 대상이다.
struct ConsoleCommandState {
    explicit ConsoleCommandState(Allocator& allocator) noexcept : name(allocator), help(allocator) {
    }

    String name;
    String help;
    const cvar::detail::CommandDefinition* definition = nullptr;
    void* target = nullptr;
    // 묶은 대상의 타입 표지(cvar::detail::TargetTypeTag<T>의 주소)다.
    const void* targetType = nullptr;
};

// 다음 Sync 5단계에 게임 lane이 할 콘솔 일 하나다: 명령 실행(command — 인자는 arguments 텍스트를
// 실행 때 다시 읽는다), 또는 다른 스레드가 낸 콘솔 입력 한 줄(command가 IndexNone — line을 게임
// lane에서 처리한다).
struct PendingCommand {
    explicit PendingCommand(Allocator& allocator) noexcept : arguments(allocator) {
    }

    usize command = IndexNone;
    String arguments;
    SharedPtr<ConsoleSink> sink;
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

// 프리셋 묶음 항목 하나다(코드 값의 콘솔 표기 — 등록 때 만든다).
struct GroupItemState {
    explicit GroupItemState(Allocator& allocator) noexcept : name(allocator), text(allocator) {
    }

    const cvar::detail::VarDefinition* target = nullptr;
    // 항목 변수의 콘솔 이름이다.
    String name;
    String text;
};

// 프리셋 묶음 등급 하나의 항목이다.
struct GroupLevelState {
    explicit GroupLevelState(Allocator& allocator) noexcept : items(allocator) {
    }

    int32 level = 0;
    Array<UniquePtr<GroupItemState>> items;
};

// 등록한 프리셋 묶음 하나다(CVar 11절). 묶음 변수 자체는 같은 이름의 콘솔 설정(정의 =
// definition)이다. 마지막 Sync가 펼친 결과(requested·applied·reason·unmet)를 둔다.
struct GroupState {
    explicit GroupState(Allocator& allocator) noexcept
        : name(allocator), levels(allocator), levelValues(allocator), reason(allocator) {
    }

    const cvar::detail::GroupDefinition* definition = nullptr;
    String name;
    // 등급 오름차순.
    Array<UniquePtr<GroupLevelState>> levels;
    // 정의된 등급(오름차순 — 조합 검증 규칙의 levels).
    Array<int32> levelValues;
    bool expanded = false;
    int32 requested = 0;
    int32 applied = 0;
    String reason;
    bool unmet = false;
};

// sync 지점 앞 hook 하나다(ConfigContext::AddSyncHook).
struct SyncHook {
    ConfigSyncHook hook = nullptr;
    void* context = nullptr;
};

// 동적 변수 하나의 정의다(ConfigContext::RegisterDynamic — CVar 5절). definition의 view는 이
// 객체의 문자열을 가리키므로 객체는 힙에 두고 옮기지 않는다.
struct DynamicVarState {
    explicit DynamicVarState(Allocator& allocator) noexcept
        : name(allocator), help(allocator), category(allocator), defaultText(allocator) {
    }

    String name;
    String help;
    String category;
    String defaultText;
    cvar::detail::VarDefinition definition;
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
    // 통로에서 꺼냈지만 아직 적용하지 않은 쓰기(넣은 순서 — Sync가 실패하면 다음 Sync가 이어서
    // 적용한다).
    Array<SharedPtr<QueuedChange>> queue;
    uint64 nextSequence = 1;
    // 섹션 등록처럼 큐 밖의 변경이 있어 다음 Sync가 다시 합쳐야 한다.
    bool dirty = false;
    ConfigDiagnostics diagnostics;
    // 콘솔 설정·명령(CF2).
    Array<String> regions;
    Array<UniquePtr<ConsoleSettingState>> settings;
    Array<UniquePtr<ConsoleCommandState>> commands;
    Array<PendingCommand> pendingCommands;
    // 네트워크 역할(CVar 13.2절)과, 등록이 바뀌어 다음 게시가 콘솔 이름 색인을 다시 만들어야
    // 하는지다.
    NetworkRole networkRole = NetworkRole::Standalone;
    bool namesChanged = true;
    // 이 상태를 가진 문맥이다(명령 실행 문맥 CommandContext가 준다 — 문맥은 옮길 수 없다).
    ConfigContext* self = nullptr;
    // Register가 게시하는 동안 아직 넣지 않은 새 설정·명령이다(그 게시의 콘솔 이름 색인에 넣는다).
    Span<const UniquePtr<ConsoleSettingState>> registeringSettings{};
    Span<const UniquePtr<ConsoleCommandState>> registeringCommands{};
    // 입력을 넣기 전 검사 중이다(ValidateEntry — 봉인 뒤 바로 쓰는 층의 부팅 필드 거부는 이때만
    // 한다. Sync의 다시 합치기는 아니다).
    bool validatingWrite = false;
    // 동적 변수 정의(등록 순서 — 문맥이 소유한다).
    Array<UniquePtr<DynamicVarState>> dynamicVars;
    // 쓰기 통로(문맥과 snapshot들이 공유)와 마지막으로 게시한 snapshot이다.
    SharedPtr<WriteChannel> channel;
    SharedPtr<const ConfigSnapshot> snapshot;
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
    // ReportUnresolved가 이미 보고한 이름이다(문맥마다 이름당 한 번).
    Array<String> reportedUnresolved;
    // 프리셋 묶음(등록 순서 — CVar 11절)과 조합 검증 입력(SetQualityValidation — 빌린다), 마지막
    // Sync의 넘친 예산 사유다.
    Array<UniquePtr<GroupState>> groups;
    Span<const QualityGroupRule> qualityRules{};
    Span<const QualityBudgetRule> qualityBudgets{};
    const DeviceTraits* qualityTraits = nullptr;
    Array<String> unmetQualityBudgets;
    // 진단을 이미 낸 `Quality.json` 값 문제(입력 순번·묶음·등급·항목 키) — 같은 것에 한 번만 낸다.
    // Sync가 살아 있는 입력의 것만 남긴다(PruneQualityProblems).
    Array<String> reportedQualityProblems;
    // 진단을 이미 낸 코드 프리셋 값 문제(묶음·등급·항목 키 — 정의에 묶여 있어 늘지 않는다).
    Array<String> reportedPresetProblems;
    // 마지막으로 GroupNoConverge를 낸 상태의 서명(요청·펼친 등급 — 같은 상태에 한 번만 낸다).
    String groupNoConverge;
    // sync 지점 앞 hook(등록 순서).
    Array<SyncHook> syncHooks;
    // Sync마다 한 번 부르는 변경 알림(7.3절).
    MulticastDelegate<void(const SyncChanges&)> onSynced;

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

// 시작 뒤 부팅 값(RestartRequired·StartupOnly — 이번 실행 동안 고정, CVar BootCVar)이 바뀌었으면
// 지금 값(sections[i]->value·settings[i]->value)을 유지한다(Sync가 값 계산 뒤, 맞바꾸기 전에
// 부른다). 새 값의 가장 강한 출처가 파일 층(코드 기본값·엔진·엔진 플랫폼·프로젝트·프로젝트
// 플랫폼·사용자 — 다음 실행에도 같은 입력이 다시 온다)이면 재시작 대기로 기록하고, 그 밖의 층(기기
// 프로필·품질·기능 묶음 같은 파생 층, 세션·콘솔 같은 휘발 층)이면 대기 없이 Warning
// BootValueIgnored를 같은 이름·출처에 한 번 낸다. order는 이번 Sync의 적용 입력이다(출처 층 조회).
// values·sources는 섹션 순서의 새 값과 출처 스택, settings는 콘솔 설정 순서의 새 값이다(필드 설정은
// values를 고친 뒤 다시 읽어야 하므로 호출자가 ComputeSettingResults 전에 HoldBootSections를, 뒤에
// HoldBootSettings를 부른다). pending에 새 대기 목록을 쓴다.
Result<void> HoldBootSections(ConfigState& state, Span<ConfigEntry* const> order,
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

// 설정 필드의 층 허용 판정이다(UserConfig·DevelopmentOnly). 쓸 수 없으면 거부 사유 문장과 code를,
// 쓸 수 있으면 빈 view를 돌려준다. 넣을 때의 검사(state.validatingWrite)이고 봉인 뒤면 부팅 필드
// (StartupOnly·RestartRequired)의 바로 쓰는 층(콘솔·코드·범위 임시 변경) 쓰기를 BootSealed로
// 거부한다(CVar 20절 CVR6 결정). 그 밖의 시작 뒤 변경은 여기서 거부하지 않고 Sync가 지금 값을
// 지킨다(HoldBootSections).
StringView CheckLayerAllowed(const ConfigState& state, ConfigLayer layer, uint32 settingsFlags,
                             ConfigDiagnosticCode& code) noexcept;

// 봉인 뒤 부팅 변수에 바로 쓰려 할 때의 안내 문구다(이름 뒤에 붙인다 — CVar 9절, 콘솔이 쓴다).
// 지금 진단 형식(`error BootSealed <이름>: <문구>`)과 스펙 콘솔 형식(`<이름> <문구>`)의 차이는 CVR6
// 콘솔이 맞춘다.
inline constexpr StringView BootSealedMessage =
    "is a boot variable; set it in a config file or with --set and restart";

// 부르는 쪽이 바로 효력을 기대하는 층(콘솔·코드·범위 임시 변경)이다. 봉인 뒤 부팅 값에 이 층으로
// 쓰면 BootSealed로 거부한다(CVar 9절).
inline bool IsDirectWriteLayer(ConfigLayer layer) noexcept {
    return layer == ConfigLayer::Console || layer == ConfigLayer::Code ||
           layer == ConfigLayer::Scoped;
}

// 부팅 값 플래그(StartupOnly·RestartRequired)가 있는지다.
inline bool HasBootFlags(uint32 consoleFlags) noexcept {
    return HasFlag(consoleFlags, ConsoleSettingFlags::StartupOnly) ||
           HasFlag(consoleFlags, ConsoleSettingFlags::RestartRequired);
}

// 콘솔 이름으로 쓰는 값의 층 허용 판정이다(콘솔 플래그 — 읽기 전용·품질 구성원·개발 전용·사용자
// 설정). code에 거부 진단 종류를 쓴다. 쓸 수 있으면 빈 view다. sealedWrite면(봉인 뒤 쓰기 — 넣을
// 때의 검사) 부팅 변수(StartupOnly·RestartRequired)의 콘솔·코드·범위 임시 변경 층 쓰기를
// BootSealed로 거부한다. 그 밖의 시작 뒤 고정 값은 Sync가 지킨다(재시작 대기·BootValueIgnored).
StringView CheckConsoleLayer(bool releaseRules, ConfigLayer layer, uint32 consoleFlags,
                             ConfigDiagnosticCode& code, bool sealedWrite) noexcept;

// CVar 핸들 정의의 성질을 콘솔 설정 플래그로 옮긴다. 부팅 변수는 StartupOnly(CompleteStartup 뒤
// 고정 — 9절), Cheat는 DevelopmentOnly(VD5 — 출시 규칙 시험에서 숨김·고정)다. 모든 CVar는
// snapshot의 CVar 칸에 든다(VD6 — 이름 블록의 RenderRead가 아니다).
uint32 ConsoleFlagsOfDefinition(const cvar::detail::VarDefinition& definition) noexcept;

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
// `Settings` 멤버와 콘솔 대입, 대입). paths에 override 경로 텍스트를 더한다. 반환: true면 적용했다,
// false면 진단을 남기고 storage를 바꾸지 않았다(부분 적용 없음은 아니다 — 섹션 객체를 적용한 뒤
// 콘솔 값이 거부될 수 있다. 호출자는 false면 그 storage를 버린다). 오류: 할당 실패 등 호출 오류.
Result<bool> ApplyEntryToSection(ConfigState& state, SectionState& section, void* storage,
                                 const ConfigEntry& entry, Array<String>& paths) noexcept;

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

// 콘솔 값 하나를 검사한다(층 허용·형식·범위·허용 값, CVar 핸들 설정은 정의의 범위·보정·Validate).
// 받으면 out에 값을 쓰고 빈 view를, 거부하면 code·사유를 돌려준다. `Clamp()` CVar의 범위 밖 값을
// 보정해 받았으면 빈 view에 code가 Clamped다(호출자가 Note 진단을 남긴다). 오류: 할당 실패.
// writing이면 넣을 때의 검사다(봉인 뒤 부팅 변수 직접 쓰기를 BootSealed로 거부한다 — Sync의 다시
// 합치기는 writing이 아니다).
Result<StringView> CheckConsoleValue(ConfigState& state, const ConsoleSettingState& setting,
                                     ConfigLayer layer, StringView text, FieldValue& out,
                                     ConfigDiagnosticCode& code, bool writing) noexcept;

// 새로 만든 독립 콘솔 설정(setting — 아직 state.settings에 넣지 않았다)에 이미 들어온 층 입력(대기
// 값)을 적용 순서로 넘긴다. 거부한 값은 진단을 남기고 건너뛴다. 오류: 할당 실패.
Result<void> AdoptPendingValues(ConfigState& state, ConsoleSettingState& setting) noexcept;

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

// HoldBootSections의 콘솔 설정 짝이다(독립 콘솔 설정만 — 필드 설정은 섹션 값을 따른다). 대기로
// 돌린 결과는 지금 값·표기·출처로 되돌리고 changed를 끈다.
Result<void> HoldBootSettings(ConfigState& state, Span<ConfigEntry* const> order,
                              Span<const UniquePtr<SettingResult>> settings,
                              Array<UniquePtr<PendingRestart>>& pending) noexcept;

// 새 값으로 이름 블록(RenderRead 콘솔 설정·설정 struct 필드 — CVar 핸들 제외)을 만든다. 값의
// 서명(signature에 쓴다)이 이전과 같고 snapshot이 이미 있으면 out은 null(재사용)이다.
Result<void> PrepareNamedBlock(ConfigState& state, Span<const UniquePtr<SettingResult>> settings,
                               Span<void* const> sectionValues, SharedPtr<const NamedBlock>& out,
                               String& signature) noexcept;

// CVar 칸 하나의 새 값이다(value는 그 설정 종류의 값).
struct SlotWrite {
    uint32 index = 0;
    const FieldValue* value = nullptr;
};

// 지금 snapshot을 바탕으로 새 snapshot을 만들어 게시한다(소유 스레드 — 10절 4단계·5절 등록 즉시
// 게시·9절 봉인). named가 null이 아니면 이름 블록을 바꾸고, writes의 칸만 새 값으로 바꾼다(바뀐
// 칸이 든 조각만 새로 만들고 나머지는 공유한다 — 칸의 세대는 이전 세대 + 1, 처음이면 1).
// bootWrites는 부팅 블록 칸이다(봉인 때와 봉인 뒤 등록한 BootCVar — 한 번 쓴 칸은 바뀌지 않는다).
// 봉인 여부는 state.startupComplete다. 세대 번호는 이전 + 1이다. 게시하면 통로의 current를 바꾸고
// 소유 스레드를 다시 묶는다(명시 묶음이 살아 있으면 그대로 둔다). 오류: 할당 실패(그때 게시하지
// 않는다).
Result<void> PublishSnapshot(ConfigState& state, const SharedPtr<const NamedBlock>* named,
                             Span<const SlotWrite> writes,
                             Span<const SlotWrite> bootWrites = {}) noexcept;

// 설정 snapshot의 내부 접근이다(ConfigSnapshot의 friend — Config 모듈 안만 쓴다).
struct SnapshotAccess {
    static const SharedPtr<WriteChannel>& Channel(const ConfigSnapshot& snapshot) noexcept {
        return snapshot.channel_;
    }

    static Result<SharedPtr<ConfigSnapshot>> Make(Allocator& allocator) noexcept;

    static void Assign(ConfigSnapshot& snapshot, uint64 generation,
                       SharedPtr<const NamedBlock> named,
                       SharedPtr<WriteChannel> channel) noexcept {
        snapshot.generation_ = generation;
        snapshot.named_ = Move(named);
        snapshot.channel_ = Move(channel);
    }

    static Array<SharedPtr<const CVarChunk>>& Chunks(ConfigSnapshot& snapshot) noexcept {
        return snapshot.chunks_;
    }

    static const Array<SharedPtr<const CVarChunk>>&
    Chunks(const ConfigSnapshot& snapshot) noexcept {
        return snapshot.chunks_;
    }

    static const SharedPtr<const NamedBlock>& Named(const ConfigSnapshot& snapshot) noexcept {
        return snapshot.named_;
    }

    static Array<SharedPtr<const CVarChunk>>& BootChunks(ConfigSnapshot& snapshot) noexcept {
        return snapshot.bootChunks_;
    }

    static const Array<SharedPtr<const CVarChunk>>&
    BootChunks(const ConfigSnapshot& snapshot) noexcept {
        return snapshot.bootChunks_;
    }

    static SharedPtr<const ConsoleNameIndex>& Names(ConfigSnapshot& snapshot) noexcept {
        return snapshot.names_;
    }

    static const SharedPtr<const ConsoleNameIndex>& Names(const ConfigSnapshot& snapshot) noexcept {
        return snapshot.names_;
    }

    static void Seal(ConfigSnapshot& snapshot, bool sealed) noexcept {
        snapshot.sealed_ = sealed;
    }
};

// 쓰기 큐에 changes를 이 순서대로 한꺼번에 넣는다(어느 스레드에서나). limited면 다른 스레드의
// 쓰기라 MaxQueuedChanges 한도를 건다(소유 스레드 입력은 한도와 무관하다). 오류: 통로가 닫혔으면
// ShuttingDown, limited인데 한도를 넘으면 Busy, 할당 실패. 실패하면 아무것도 넣지 않는다.
Result<void> Enqueue(WriteChannel& channel, Span<SharedPtr<QueuedChange>> changes,
                     bool limited) noexcept;

// 범위 임시 변경 지우기 자리 하나를 잡는다(queue 용량을 미리 늘린다). 오류: 통로가 닫혔으면
// ShuttingDown, 할당 실패.
Result<void> ReserveRemoval(WriteChannel& channel) noexcept;

// 잡아 둔 자리에 지우기를 넣는다(한도와 무관 — 실패하지 않는다). 통로가 닫혔으면 버린다.
void SubmitRemoval(WriteChannel& channel, SharedPtr<QueuedChange> change) noexcept;

// 소유 스레드(게임 lane)를 문맥의 지금 snapshot에 다시 묶는다. 그 스레드에 명시 묶음
// (cvar::SnapshotBinding)이 살아 있으면 그대로 둔다(묶음이 풀릴 때 최신 snapshot으로 되돌아온다).
// 한 스레드가 여러 문맥의 게임 lane이면 가장 최근에 게시하거나 Sync한 문맥이 묶인다(10.1절).
void RebindGameLane(ConfigState& state) noexcept;

// 콘솔 이름 대입 하나(AddAssignment의 콘솔 이름 경로)·섹션 경로 대입을 검사해 큐 항목으로
// 만든다(넣지 않는다 — 여러 항목을 한꺼번에 넣는 쪽이 쓴다). 오류: AddAssignment와 같다.
Result<void> PrepareAssignment(ConfigState& state, ConfigLayer layer, StringView tag,
                               StringView target, StringView value,
                               SharedPtr<QueuedChange>& out) noexcept;

// (layer, tag) 빼기 큐 항목을 만든다. 오류: 할당 실패.
Result<void> PrepareUnset(ConfigState& state, ConfigLayer layer, StringView tag,
                          SharedPtr<QueuedChange>& out) noexcept;

// 층 입력 하나(branch 파일 내용)를 검사하고 쓰기 큐에 넣는다(ConfigContext::AddLayerText의 본체 —
// 소유 스레드). replace면 그 앞에 (층, 태그) 빼기를 함께 넣는다(ReplaceLayerText). 오류는
// AddLayerText와 같다.
Result<void> QueueLayerText(ConfigState& state, ConfigLayer layer, StringView tag,
                            StringView branch, StringView sourceName, StringView text,
                            bool replace) noexcept;

// 쓰기 큐에 든 것이 있는지다.
bool HasQueuedChanges(WriteChannel& channel) noexcept;

// 통로의 쓰기를 넣은 순서대로 꺼내 state.queue 뒤에 붙인다(Sync 1단계 앞 — 소유 스레드). 다른
// 스레드에서 넣은 쓰기(validateAtApply)는 여기서 다시 검사해 진단을 남기고, 거부되면 버린다. 입력
// 순번(sequence)을 여기서 매긴다. 오류: 할당 실패(꺼낸 것은 state.queue에 남는다).
Result<void> DrainQueue(ConfigState& state) noexcept;

// 맞바꾼 뒤의 일이다: 큐에 든 콘솔 일(명령·표시)을 넣은 순서대로 실행한다(10절 5단계).
void RunPendingCommands(ConfigState& state) noexcept;

// 쓰기가 갈 통로다(WriteTarget이 채운다). 프로세스 기본 문맥을 거쳤으면(viaDefault) 소멸 때 통로
// 참조를 먼저 놓고 그 통로의 defaultWriters를 내린다(CVar 6.2절). 쓰기를 마칠 때까지 쥔다.
struct WriteRoute {
    WriteRoute() noexcept = default;
    ~WriteRoute() noexcept;
    WriteRoute(const WriteRoute&) = delete;
    WriteRoute& operator=(const WriteRoute&) = delete;

    SharedPtr<WriteChannel> channel;
    bool viaDefault = false;
};

// 쓰기가 갈 통로를 route에 채운다: 이 스레드에 묶인 문맥, 없으면 프로세스 기본 문맥(6.2절 — 기본
// 문맥이면 그 통로의 defaultWriters를 올린다). 둘 다 없으면 route.channel은 null이다.
void WriteTarget(WriteRoute& route) noexcept;

// 프로세스 기본 문맥이 이 통로면 풀고, 기본 문맥을 거친 쓰기가 모두 끝날 때까지 기다린다(문맥
// 파괴 — CVar 6.2절). 통로는 이미 닫혀 있어야 한다(겹친 쓰기는 ShuttingDown으로 끝난다).
void ReleaseProcessDefault(WriteChannel& channel) noexcept;

// ---- 콘솔 입력·명령(CVar 13·15절 — ConsoleInput.cpp) ----

// 콘솔 이름 색인을 새로 만든다(콘솔 설정·명령 — 이름 순서). 오류: 할당 실패.
Result<SharedPtr<const ConsoleNameIndex>> BuildConsoleNameIndex(ConfigState& state) noexcept;

// 콘솔 층 쓰기의 권한(치트 허용 상태·네트워크 역할)이다. cheats는 판정에 쓸 `cheat.enabled` 값,
// role은 역할이다. 거부면 code·사유, 받으면 빈 view다.
StringView CheckConsolePermission(uint32 varFlags, bool cheats, NetworkRole role,
                                  ConfigDiagnosticCode& code) noexcept;

// 문맥의 지금 `cheat.enabled` 값이다(등록되지 않았으면 true — 단독 실행 기본).
bool CheatsAllowed(const ConfigState& state) noexcept;

// snapshot의 `cheat.enabled` 값이다(칸이 없으면 기본값 true — 다른 스레드의 쓰기 검사가 쓴다).
bool SnapshotCheatsAllowed(const ConfigSnapshot* snapshot) noexcept;

// 문맥이 스스로 등록하는 핸들(`cheat.enabled`·`cvar.find`·`cvar.explain`·`cvar.dump`)을 등록한다
// (Initialize). 오류: Register와 같다.
Result<void> RegisterBuiltinConsoleHandles(ConfigContext& context) noexcept;

// 꺼낸 쓰기 하나의 콘솔 권한을 적용 때 다시 본다(콘솔 층만 — 6.2절 "적용 때 판정"). 거부하면 진단을
// 남기고 그 입력의 sink에 알린다. 반환: 받는지. 오류: 할당 실패.
Result<bool> RecheckConsolePermission(ConfigState& state, const QueuedChange& change,
                                      bool cheats) noexcept;

// 콘솔 층 입력 하나(콘솔 이름 대입·`Settings` 문서)의 권한(치트·복제)이다. cheats는 판정에 쓸
// `cheat.enabled` 값이다. 거부하면 진단을 남기고 sink에 `<이름> <사유>`를 쓴다. 반환: 받는지.
// 오류: 할당 실패.
Result<bool> CheckEntryPermission(ConfigState& state, const ConfigEntry& entry, bool cheats,
                                  ConsoleSink* sink) noexcept;

// 큐 항목 하나를 적용 입력 목록(plan)에 반영한다(빼기·바꾸기 뒤 더하기 — Sync 1단계와 드레인의 치트
// 추적이 같이 쓴다). 오류: 할당 실패.
Result<void> ApplyQueuedChange(Array<ConfigEntry*>& plan, const QueuedChange& change) noexcept;

// 입력이 `cheat.enabled` 값을 바꿀 수 있는지다(그 이름의 대입·`Settings` 멤버, 빼기).
bool ChangeTouchesCheats(const QueuedChange& change) noexcept;

// plan(적용 입력, 순서 무관)으로 계산한 `cheat.enabled` 값이다(등록되지 않았으면 true). 오류: 할당
// 실패.
Result<bool> CheatsOfPlan(ConfigState& state, Span<ConfigEntry* const> plan) noexcept;

// 꺼낸 콘솔 입력 한 줄(다른 스레드 — QueuedChange::line)을 다음 명령 실행 차례에 게임 lane이
// 처리하게 pendingCommands에 넣는다. 오류: 할당 실패.
Result<void> QueueConsoleLine(ConfigState& state, QueuedChange& change) noexcept;

// 콘솔 명령 하나를 실행한다(Sync 5단계 — 인자를 다시 읽고 권한을 보고 부른다). 실패는 sink와 로그에
// 남긴다.
void RunPendingCommand(ConfigContext& context, ConfigState& state,
                       PendingCommand& pending) noexcept;

// ---- 프리셋 묶음(CVar 10·11절 — CVarGroups.cpp) ----

// 품질 묶음 층에 직접 쓰려는 입력을 거부한다(그 층은 묶음 펼침만 쓴다 — 진단 LayerNotAllowed,
// InvalidArgument). layer가 Quality가 아니면 성공이다.
Result<void> RejectQualityLayerWrite(ConfigState& state, ConfigLayer layer,
                                     StringView sourceName) noexcept;

// 입력이 `Quality` branch 문서(`Quality.json` — 묶음 → 등급 → 항목 → 값)인지다.
bool IsQualityDocument(const ConfigState& state, const ConfigEntry& entry) noexcept;

// `Quality.json` 문서의 모양과 층을 검사한다(묶음·항목 이름 문법, 등급 키는 10진 정수, 값은 JSON
// 값 하나, 파일 층만 — 등록 여부는 보지 않는다: 묶음은 늦게 등록될 수 있다). 반환: true면 문제없음.
Result<bool> ValidateQualityDocument(ConfigState& state, const ConfigEntry& entry) noexcept;

// 등록할 묶음(added 안의 Group 정의)을 이미 등록한 묶음과 함께 검사한다: 한 변수는 한 묶음의
// 항목만(두 묶음 이름), 묶음의 묶음은 DAG(자기 참조·순환 경로). 위반이면 Error GroupConflict를
// 남기고 InvalidArgument다(CVar 11절). 오류: 할당 실패.
Result<void> CheckGroupGraph(ConfigState& state,
                             Span<const cvar::detail::VarDefinition* const> added) noexcept;

// 등록한 정의(added — 묶음 또는 항목 변수)가 닿는 코드 프리셋 값을 항목 변수의 규칙으로 검사한다
// (묶음과 항목이 둘 다 등록된 것만 — (묶음, 등급, 항목)마다 Error 한 번). 새 항목 변수가 있으면
// 다음 Sync가 다시 펼친다(dirty). 오류: 할당 실패.
Result<void> CheckPresetValues(ConfigState& state,
                               Span<const cvar::detail::VarDefinition* const> added) noexcept;

// reportedQualityProblems에서 이미 빠진 입력의 키를 버린다(Sync가 입력 목록을 바꾼 뒤 — 할당 없음).
void PruneQualityProblems(ConfigState& state) noexcept;

// 묶음 정의의 등록 상태(등급별 항목의 코드 값 표기)를 만든다. 오류: 할당 실패.
Result<void> MakeGroupState(ConfigState& state, const cvar::detail::GroupDefinition& definition,
                            UniquePtr<GroupState>& out) noexcept;

// Sync 2단계 묶음 해결이다(CVar 10절): 묶음을 위상 순서(부모 → 자식)로, 묶음마다 그때까지의 펼침을
// 합친 입력으로 계산한 유효 등급(요청 등급)으로 펼친다(generated). 조합 검증 결과(resolutions —
// state.groups 순서)가 펼친 등급(expanded)과 다르면 낮춘 등급으로 처음부터 다시 펼친다(앞 차례의
// 결과는 요청 등급이 같은 묶음에만 — MaxGroupPasses 번, 넘으면 마지막 결과와 같은 상태에 한 번
// Warning GroupNoConverge). 마지막 펼침을 order(펼침 제외)와 합친 적용 순서가 settingOrder,
// 그것으로 계산한 콘솔 설정 값이 settingResults다. 묶음 상태는 바꾸지 않는다(CommitGroups가
// 맞바꾼다). 오류: 할당 실패, 조합 검증의 호출 오류.
Result<void> ResolveGroups(ConfigState& state, Span<ConfigEntry* const> order,
                           Span<void* const> sectionValues,
                           Array<UniquePtr<ConfigEntry>>& generated,
                           Array<ConfigEntry*>& settingOrder,
                           Array<UniquePtr<SettingResult>>& settingResults,
                           QualityResolutionSet& resolutions, Array<int32>& expanded) noexcept;

// ResolveGroups 결과를 묶음 상태에 옮긴다(실패하지 않는다 — 맞바꾸기).
void CommitGroups(ConfigState& state, QualityResolutionSet& resolutions,
                  Span<const int32> expanded) noexcept;

// ReportUnresolved의 `Quality.json` 부분이다: 등록되지 않은 묶음(UnresolvedName), 정의되지 않은
// 등급(UndefinedLevel), 그 등급의 코드 항목이 아닌 이름(InvalidSetting)을 Warning으로 한 번씩
// 남기고 reported를 올린다. 오류: 할당 실패.
Result<void> ReportQualityDocuments(ConfigState& state, usize& reported) noexcept;

} // namespace rocks::config::detail

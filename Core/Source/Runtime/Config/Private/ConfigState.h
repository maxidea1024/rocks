#pragma once

#include <Config/ConfigContext.h>
#include <Core/Text/Json.h>
#include <Object/TypeLease.h>
#include <ReflectionText/TextCodecOptions.h>

// 설정 문맥의 내부 상태다(ConfigContext의 pimpl — 모듈 안 구현만 쓴다).
namespace rocks::config::detail {

// 필드 경로 하나의 출처 스택이다. entries는 적용 순서(약한 것부터)의 입력
// 순번(ConfigEntry::sequence)이다.
struct SourceStack {
    explicit SourceStack(Allocator& allocator) noexcept : path(allocator), entries(allocator) {
    }

    String path;
    Array<uint64> entries;
};

// 등록한 설정 struct(섹션) 하나다. value는 현재 값(Sync에서만 바뀐다 — 주소는 고정)이다.
struct SectionState {
    explicit SectionState(Allocator& allocator) noexcept : section(allocator), sources(allocator) {
    }

    usize branch = 0;
    String section;
    TypeLease lease;
    SettingsTypeOps ops;
    void* value = nullptr;
    Array<UniquePtr<SourceStack>> sources;
};

// 입력 하나의 종류다.
enum class EntryKind : uint8 {
    // branch 파일 내용(섹션 여러 개와 `Settings`).
    Document,
    // 대입 하나(`<Branch>.<Section>.<경로>=<값>`).
    Assignment
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
    // Assignment: 섹션 번호·필드 경로·값 텍스트.
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

struct ConfigState {
    ConfigState(Allocator& allocator, ObjectRegistry& objects) noexcept;

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

    // 텍스트 codec 옵션(Edit 용도, Map 키별 병합, 대상 Registry의 Name pool).
    reflection_text::TextCodecOptions CodecOptions() const noexcept;

    // 이름이 같은 branch 번호다(대소문자 구분). 없으면 IndexNone.
    usize FindBranch(StringView name) const noexcept;

    // (branch, section) 섹션 번호다(섹션은 ASCII 대소문자 무시). 없으면 IndexNone.
    usize FindSection(usize branch, StringView section) const noexcept;

    // 입력 순번의 입력이다(없으면 null).
    const ConfigEntry* FindEntry(uint64 sequence) const noexcept;
};

// 진단 하나를 더한다(실패는 할당 오류).
Result<void> Report(ConfigState& state, ConfigDiagnosticCode code, ConfigSeverity severity,
                    ConfigLayer layer, StringView source, const JsonPosition* position,
                    StringView path, StringView message) noexcept;

// 설정 필드의 층 허용 판정이다(UserConfig·DevelopmentOnly). 쓸 수 없으면 거부 사유 문장을, 쓸 수
// 있으면 빈 view를 돌려준다.
StringView CheckLayerAllowed(const ConfigState& state, ConfigLayer layer,
                             uint32 settingsFlags) noexcept;

// 섹션의 직접 멤버 필드 ID에 붙은 설정 플래그다(없으면 0).
uint32 SettingsFlagsOf(const SectionState& section, Guid field) noexcept;

// 입력 하나를 섹션 값(storage)에 적용한다(입력의 그 섹션 부분 — Document는 섹션 객체, Assignment는
// 대입). paths에 override 경로 텍스트를 더한다. 반환: true면 적용했다, false면 진단을 남기고
// storage를 바꾸지 않았다. 오류: 할당 실패 등 호출 오류.
Result<bool> ApplyEntryToSection(ConfigState& state, SectionState& section, void* storage,
                                 const ConfigEntry& entry, Array<String>& paths) noexcept;

// 입력 하나를 섹션마다 코드 기본값 위에 적용해 보아 검사한다(큐에 넣기 전 — 진단은 state에 남긴다).
// 반환: true면 문제없음.
Result<bool> ValidateEntry(ConfigState& state, const ConfigEntry& entry) noexcept;

// Document 입력의 `Settings` 섹션을 검사한다(이름 문법·값 모양). 반환: true면 문제없음.
Result<bool> CheckSettingsSection(ConfigState& state, const ConfigEntry& entry,
                                  JsonValue settings) noexcept;

// 섹션 객체에서 키(대소문자 무시)로 찾은 값이다(없으면 무효 값).
JsonValue FindMemberIgnoringCase(JsonValue object, StringView key) noexcept;

} // namespace rocks::config::detail

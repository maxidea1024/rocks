#pragma once

#include <Config/CVarBuilder.h>

// 프리셋 묶음이다(Docs/Design/CVarSystem.ko.md 11절 — CVR5, UE scalability group 대응). 묶음은
// 코드에서 `cvar::Group` 빌더 한 문장으로 정의하는 int32 등급 변수이고, 등급마다 항목(변수, 값)
// 목록을 가진다. Sync가 묶음의 유효 등급을 Quality 층(Config 3.1 6층)에 펼친다: 그 묶음 태그의 앞
// 펼침을 빼고 새 등급 항목을 쓴다(출처 `quality.shadow@2`). 어떤 묶음의 항목인 변수만 Quality 층
// 값을 받는다(구성원 자동 — VD14). `Quality.json`(Config 6.1)은 코드가 정의한 항목의 값만 바꾼다.
//
// 사용 예:
//   constinit CVarGroup ShadowGroup = cvar::Group("quality.shadow", 2)
//       .Help("Shadow scalability")
//       .Level(0, {{ShadowQualityLevel, ShadowQuality::Off}, {ShadowDistance, 1000}})
//       .Level(1, {{ShadowQualityLevel, ShadowQuality::Low}, {ShadowDistance, 3000}})
//       .Level(2, {{ShadowQualityLevel, ShadowQuality::Medium}, {ShadowDistance, 6000}})
//       .Level(3, {{ShadowQualityLevel, ShadowQuality::Epic}, {ShadowDistance, 15000}});
//
//   // 전체 품질은 다른 묶음을 항목으로 가진다(묶음 항목 — {묶음, 등급}).
//   constinit CVarGroup OverallGroup = cvar::Group("quality.overall", 1)
//       .Help("Overall scalability")
//       .Level(0, {{ShadowGroup, 0}})
//       .Level(1, {{ShadowGroup, 2}});
//
// UE 대응: Scalability::FQualityLevels·SetGroupQualityLevel(Engine/Private/Scalability.cpp:412–446)
// — UE는 묶음 정의가 ini(`[ShadowQuality@2]`)에 있고, Rocks는 정의가 코드이며 파일은 값만 바꾼다.
namespace rocks::cvar {

// 묶음 하나가 정의할 수 있는 등급 수다(consteval 안에서는 할당할 수 없으므로 묶음이 고정 용량을
// 품는다 — 11절).
inline constexpr usize MaxGroupLevels = 8;

// 등급 하나가 가질 수 있는 항목 수다.
inline constexpr usize MaxEntriesPerLevel = 32;

// Sync의 묶음 해결 반복 한도다(10절 2단계 — 펼침 → 다시 계산을 이 횟수 안에 멈추지 않으면 마지막
// 결과를 쓰고 GroupNoConverge를 남긴다).
inline constexpr usize MaxGroupPasses = 8;

class CVarGroup;
class GroupBuilder;

// 프리셋 항목 하나다: 항목 변수의 정의 주소와 값이다. 주소는 상수이므로 다른 .cpp에 정의된 변수를
// 가리켜도 정적 초기화 순서와 무관하다(번호는 Sync에서 읽는다). 값 타입은 변수 타입과 같아야 한다
// (`1000` → float 같은 암시 변환은 된다 — String·Name 변수의 값은 StringView). `BootCVar`는 항목이
// 될 수 없다(컴파일 오류). 다른 묶음은 `{묶음, 등급}`으로 항목이 된다.
//
// 사용 예:
//   .Level(1, {{ShadowDistance, 3000}, {ShadowGroupOfTerrain, 1}})
class PresetEntry {
public:
    constexpr PresetEntry() noexcept = default;

    template <class T>
    constexpr PresetEntry(const CVar<T>& variable, TypeIdentityT<SetArg<T>> value) noexcept
        : target_(&detail::HandleAccess::Definition(variable)),
          value_(detail::ScalarOfArg<T>(value)) {
    }

    // 값 타입이 변수 타입으로 바뀌지 않으면 컴파일 오류다.
    template <class T, class Value>
        requires(!IsConvertibleV<Value, SetArg<T>>)
    constexpr PresetEntry(const CVar<T>&, Value) noexcept {
        static_assert(detail::DependentFalse<T, Value>,
                      "Preset entry value type must match the CVar type");
    }

    // 부팅 변수는 실행 중 등급으로 바뀌지 않는다(4.3절 표).
    template <class T, class Value>
    constexpr PresetEntry(const BootCVar<T>&, Value) noexcept {
        static_assert(
            detail::DependentFalse<T, Value>,
            "BootCVar is fixed at boot; set it in a config file or with --set and restart");
    }

    // 다른 묶음의 등급 항목이다(아래 CVarGroup 뒤에 정의한다).
    constexpr PresetEntry(const CVarGroup& group, int32 level) noexcept;

    // 항목 변수의 정의다(기본 생성이면 null).
    constexpr const detail::VarDefinition* Target() const noexcept {
        return target_;
    }

    // 항목 값이다(String·Name은 정적 저장소 텍스트).
    constexpr const detail::ScalarValue& Value() const noexcept {
        return value_;
    }

private:
    const detail::VarDefinition* target_ = nullptr;
    detail::ScalarValue value_{};
};

// 등급 하나의 항목 표다.
struct PresetLevel {
    int32 level = 0;
    uint32 count = 0;
    PresetEntry entries[MaxEntriesPerLevel]{};
};

namespace detail {

// 묶음의 상수 정의다(공통 머리 VarDefinition — kind가 Group — 위에 등급 표). 묶음 변수는 int32이고
// 허용 값은 정의된 등급뿐이다.
struct GroupDefinition : TypedDefinition<int32> {
    uint32 levelCount = 0;
    PresetLevel levels[MaxGroupLevels]{};
};

// 묶음 변수의 값 검사다: 정의된 등급만 받는다(사이에 있는 등급도 정의되지 않았으면 거부 —
// UndefinedLevel).
constexpr ValueCheck CheckGroupLevel(const VarDefinition& definition, ScalarValue& value,
                                     StringView& reason) noexcept {
    const auto& group = static_cast<const GroupDefinition&>(definition);
    for (uint32 index = 0; index < group.levelCount; ++index) {
        if (group.levels[index].level == value.integer) {
            return ValueCheck::Accepted;
        }
    }

    reason = "the level is not defined by the preset group";
    return ValueCheck::UndefinedLevel;
}

} // namespace detail

// 프리셋 묶음 핸들이다(11절). `cvar::Group` 빌더의 consteval 변환으로만 만들어지는 상수 초기화
// 값이고, 모듈 등록 함수에서 ConfigContext::Register로 올린다(CVar와 섞어도 된다). 묶음 자체가
// int32 변수다: `Get()`은 이 스레드에 묶인 snapshot의 등급(사용자가 고른 등급 — 조합 검증으로 낮춘
// 펼침 등급이 아니다), 쓰기는 정의된 등급만 받는다(UndefinedLevel). 늘 사용자
// 설정(`UserSetting()`)이다.
//
// 사용 예:
//   const int32 level = ShadowGroup.Get();
//   Result<void> written = ShadowGroup.Set(3);   // 다음 Sync에 항목이 등급 3 값으로 펼쳐진다
//
// 수명·스레드: CVar와 같다(정적 저장 기간, 힙 생성 금지, 어느 스레드에서나 Get·Set).
class CVarGroup {
public:
    CVarGroup(const CVarGroup&) = delete;
    CVarGroup& operator=(const CVarGroup&) = delete;
    CVarGroup(CVarGroup&&) = delete;
    CVarGroup& operator=(CVarGroup&&) = delete;

    static void* operator new(usize) = delete;
    static void* operator new[](usize) = delete;
    static void* operator new(usize, void*) = delete;
    static void* operator new[](usize, void*) = delete;

    constexpr StringView Name() const noexcept {
        return definition_.name;
    }

    constexpr StringView Help() const noexcept {
        return definition_.help;
    }

    // 기본 등급이다.
    constexpr int32 DefaultValue() const noexcept {
        return static_cast<int32>(definition_.defaultValue.integer);
    }

    // 정의된 등급 수와 i번째 등급 표다(정의 순서).
    constexpr usize LevelCount() const noexcept {
        return definition_.levelCount;
    }

    constexpr const PresetLevel& LevelAt(usize index) const noexcept {
        return definition_.levels[index];
    }

    cvar::Index Index() const noexcept {
        return cvar::Index{
            platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&definition_.index)};
    }

    // 이 스레드에 묶인 snapshot의 등급이다(CVar::Get과 같은 규칙).
    int32 Get() const noexcept {
        return detail::ReadBound<int32>(definition_);
    }

    // 등급을 쓴다(CVar::Set과 같은 규칙 — 정의되지 않은 등급은 InvalidArgument, 이유
    // UndefinedLevel).
    Result<void> Set(int32 level, const SetOptions& options = {},
                     SourceLocation where = SourceLocation::Current()) const noexcept {
        return detail::SubmitSet(definition_, detail::ScalarOf<int32>(level), options, where);
    }

    // (layer, tag)에 쓴 등급을 지운다(CVar::Clear와 같다).
    Result<void> Clear(const SetOptions& options = {}) const noexcept {
        return detail::SubmitClear(definition_, options);
    }

private:
    friend struct detail::HandleAccess;
    friend class GroupBuilder;

    constexpr explicit CVarGroup(const detail::GroupDefinition& definition) noexcept
        : definition_(definition) {
    }

    detail::GroupDefinition definition_;
};

constexpr PresetEntry::PresetEntry(const CVarGroup& group, int32 level) noexcept
    : target_(&detail::HandleAccess::Definition(group)), value_(detail::ScalarOf<int32>(level)) {
}

// `cvar::Group`이 돌려주는 빌더다. 복사·이동할 수 없고 rvalue 체인으로만 쓴다(VarBuilder와 같다).
// 끝의 consteval 변환이 검사한다: 이름 문법·도움말(CVar와 같다), 같은 등급 두 번
// (CVarError_DuplicateLevel), 한 등급에 같은 변수 두 번(CVarError_DuplicateEntry), 기본 등급
// 미정의(CVarError_DefaultLevelUndefined), 등급이 MaxGroupLevels보다 많음(CVarError_TooManyLevels).
// 항목 수 한도(MaxEntriesPerLevel)와 항목 타입은 static_assert다.
class GroupBuilder : public detail::BuilderCore<int32> {
public:
    constexpr GroupBuilder(StringView name, int32 defaultLevel) noexcept
        : detail::BuilderCore<int32>(name, detail::ScalarOf<int32>(defaultLevel),
                                     HandleKind::Group) {
        this->AddFlag(VarFlags::UserSetting);
    }

    constexpr GroupBuilder&& Help(StringView text) && noexcept {
        this->definition_.help = text;
        return static_cast<GroupBuilder&&>(*this);
    }

    constexpr GroupBuilder&& Category(StringView category) && noexcept {
        this->definition_.category = category;
        return static_cast<GroupBuilder&&>(*this);
    }

    // 등급 하나와 그 항목이다(정의 순서는 상관없다).
    template <usize Count>
    constexpr GroupBuilder&& Level(int32 level, const PresetEntry (&entries)[Count]) && noexcept {
        static_assert(Count <= MaxEntriesPerLevel,
                      "A preset level holds at most MaxEntriesPerLevel entries");
        PresetLevel* slot = AddLevel(level);
        if (slot != nullptr) {
            slot->count = static_cast<uint32>(Count);
            for (usize index = 0; index < Count; ++index) {
                slot->entries[index] = entries[index];
            }
        }

        return static_cast<GroupBuilder&&>(*this);
    }

    // 항목 없는 등급이다(그 등급에서는 아무것도 펼치지 않는다).
    constexpr GroupBuilder&& Level(int32 level) && noexcept {
        static_cast<void>(AddLevel(level));
        return static_cast<GroupBuilder&&>(*this);
    }

    consteval operator CVarGroup() && noexcept {
        detail::GroupDefinition group{};
        static_cast<detail::TypedDefinition<int32>&>(group) = this->Finish();
        if (overflow_) {
            detail::CVarError_TooManyLevels();
        }

        bool defaultDefined = false;
        group.levelCount = levelCount_;
        for (uint32 index = 0; index < levelCount_; ++index) {
            group.levels[index] = levels_[index];
            defaultDefined = defaultDefined || levels_[index].level == group.defaultValue.integer;
            for (uint32 other = 0; other < index; ++other) {
                if (levels_[other].level == levels_[index].level) {
                    detail::CVarError_DuplicateLevel();
                }
            }

            const PresetLevel& level = levels_[index];
            for (uint32 entry = 0; entry < level.count; ++entry) {
                for (uint32 earlier = 0; earlier < entry; ++earlier) {
                    if (level.entries[earlier].Target() == level.entries[entry].Target()) {
                        detail::CVarError_DuplicateEntry();
                    }
                }
            }
        }

        if (!defaultDefined) {
            detail::CVarError_DefaultLevelUndefined();
        }

        group.check = &detail::CheckGroupLevel;
        return CVarGroup(group);
    }

private:
    constexpr PresetLevel* AddLevel(int32 level) noexcept {
        if (levelCount_ == MaxGroupLevels) {
            overflow_ = true;
            return nullptr;
        }

        PresetLevel& slot = levels_[levelCount_++];
        slot.level = level;
        slot.count = 0;
        return &slot;
    }

    PresetLevel levels_[MaxGroupLevels]{};
    uint32 levelCount_ = 0;
    bool overflow_ = false;
};

// 프리셋 묶음 정의의 진입점이다. defaultLevel은 정의할 등급 중 하나여야 한다.
//
// 사용 예:
//   constinit CVarGroup TextureGroup = cvar::Group("quality.texture", 1).Help("Texture quality")
//       .Level(0, {{TexturePoolMiB, 512}}).Level(1, {{TexturePoolMiB, 1024}});
constexpr GroupBuilder Group(StringView name, int32 defaultLevel) noexcept {
    return GroupBuilder(name, defaultLevel);
}

namespace detail {

template <>
inline constexpr bool IsHandle<CVarGroup> = true;

} // namespace detail

} // namespace rocks::cvar

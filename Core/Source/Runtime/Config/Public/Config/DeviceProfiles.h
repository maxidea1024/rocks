#pragma once

#include <Config/ConfigContext.h>
#include <Config/DeviceTraits.h>
#include <Config/DeviceProfileSettings.h>

// 기기 프로필(Docs/Design/ConfigSystem.ko.md 6.2절 — 설정 CF4): 선택 순서, 부모 사슬 상속, 메모리
// 등급, 기기 프로필 층(ConfigLayer::DeviceProfile, 태그 DeviceProfileTag) 적용. 정의는
// DeviceProfileSettings(branch `DeviceProfiles`)의 값이다.
//
// 선택 순서(UE `UDeviceProfileManager::GetPlatformDeviceProfileName`의 명령줄 → 선택기 → 플랫폼
// 이름 순서에 프로젝트 지정을 더했다):
//   1) 명령줄 `--device-profile=<이름>`
//   2) 프로젝트 지정(DeviceProfileSettings::profile)
//   3) 선택 규칙: 이 플랫폼에 적용되는 프로필 가운데 match 조건이 모두 참인 것(matchPriority가 큰
//      것, 같으면 이름 byte 순서로 앞선 것) — 봉인 모드는 이 단계를 건너뛴다(8절)
//   4) 플랫폼 기본(platformDefaults[플랫폼], 없으면 플랫폼 이름과 같은 프로필)
// 지정된 이름(1·2·4)이 정의에 없으면 진단 InvalidDeviceProfile을 남기고 다음 단계로 간다. 모두
// 없으면 프로필 없음이다(기기 프로필 층이 비어 있다).
//
// 상속(UE `DeviceProfileManager.cpp`의 부모 사슬 순회와 같다): 고른 프로필부터 부모로 올라가며
// 아직 정해지지 않은 대상만 채운다. 같은 프로필 안에서는 메모리 등급 값이 일반 값보다 강하다(UE
// `CVars_<Bucket>` 먼저). 그래서 자식 > 자식의 등급 값 > … 순서가 아니라 "자식 등급 값 > 자식 값 >
// 부모 등급 값 > 부모 값"이다. 모르는 부모·순환은 진단을 남기고 거기서 멈춘다.
namespace rocks::config {

// 설정 CF4의 설정 struct(DeviceProfileSettings)를 문맥에 등록한다(호스트 부트의 설정 등록 단계 —
// 파일보다 먼저). 리플렉션 타입은 Config 모듈 등록(generated::Config::RegisterModuleTypes)이 한다.
// 오류: RegisterSettings의 오류.
Result<void> RegisterDeviceProfileSettings(ConfigContext& context) noexcept;

// 기기 프로필 층의 태그다.
inline constexpr StringView DeviceProfileTag = "DeviceProfile";

// 기기 프로필 명령줄 인자의 접두사다(`--device-profile=<이름>`).
inline constexpr StringView DeviceProfileArgumentPrefix = "--device-profile=";

// 프로필을 고른 단계다.
enum class DeviceProfileSource : uint8 {
    // 고른 프로필이 없다.
    None,
    CommandLine,
    Project,
    Rule,
    PlatformDefault
};

// 단계 이름(열거자 철자)이다. 정의되지 않은 값이면 빈 view다.
StringView DeviceProfileSourceName(DeviceProfileSource source) noexcept;

// 고른 프로필과 메모리 등급이다.
struct DeviceProfileSelection {
    explicit DeviceProfileSelection(Allocator& allocator) noexcept
        : profile(allocator), memoryBucket(allocator) {
    }

    // 프로필 이름(없으면 빈 문자열).
    String profile;
    DeviceProfileSource source = DeviceProfileSource::None;
    // 기기의 메모리 등급 이름(시스템 메모리 특성이나 등급 정의가 없으면 빈 문자열).
    String memoryBucket;
    // 기기 프로필 층에 넣은 값 수.
    uint32 values = 0;
};

// arguments에서 마지막 `--device-profile=<이름>`의 이름이다(없으면 빈 view — arguments를 빌린다).
StringView FindDeviceProfileArgument(Span<const StringView> arguments) noexcept;

// 마지막 Sync 값의 프로필 정의로 프로필을 고르고(선택 순서), 부모 사슬·메모리 등급 값을 펼쳐 기기
// 프로필 층을 다시 쓴다(Unset(DeviceProfile, DeviceProfileTag) 뒤 AddAssignment 값마다 — 다음
// Sync에 적용). platform은 문맥 옵션의 플랫폼이다. 선택 규칙과 메모리 등급은 traits를 쓴다(봉인
// 모드는 선택 규칙을 쓰지 않는다). 거부된 값과 정의 오류는 진단을 남기고 건너뛴다. 반환: 선택
// 결과(out). 오류: 초기화 전·DeviceProfileSettings 미등록 InvalidArgument·NotFound, 다른 스레드
// WrongThread, 할당 실패, Unset·AddAssignment의 호출 오류(InvalidArgument 제외).
//
// 사용 예(호스트 부트 — 파일·--set 뒤, 시작 완료 전):
//   context.Sync();
//   DeviceProfileSelection selected(allocator);
//   ApplyDeviceProfile(context, FindDeviceProfileArgument(arguments), traits, selected);
//   context.Sync();
Result<void> ApplyDeviceProfile(ConfigContext& context, StringView commandLineProfile,
                                const DeviceTraits& traits, DeviceProfileSelection& out) noexcept;

} // namespace rocks::config

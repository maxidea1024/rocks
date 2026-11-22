#pragma once

#include <Base/Types.h>

// 엔진 버전이다(Docs/Design/ReflectionFoundations.ko.md 8.2절, UE5.8 FEngineVersionBase 대응).
// 단일 출처는 저장소의 Build/Build.version이고, 빌드가 실행 파일마다 BuildInfo 생성 TU에 상수로
// 주입한다(build::CurrentEngineVersion(), Core/Build/BuildInfo.h). 버전 헤더·매크로·compile
// 정의는 따로 두지 않는다 — 버전을 올려도 각 실행 파일의 생성 TU와 link만 다시 돈다.
//
// 값 규칙: 각 칸은 0–65535다. 주·부가 모두 0인 값(0.0.x)은 버전이 아니라 빈 값이다(0으로
// 초기화된 칸이 호환 판정을 통과하지 않게 — UE도 0.0.0을 빈 값으로 본다). 빌드는 빈 값을 주입하지
// 않으므로 CurrentEngineVersion()은 언제나 버전이다. 필드 이름이 major·minor가 아닌 것은 bionic
// sys/sysmacros.h·macOS sys/types.h가 그 이름을 함수형 매크로로 정의하기 때문이다.
//
// 패치는 데이터 축(formatVersion·schemaVersion·타입 스키마·커스텀 버전·쿠커 버전)과 공개 API를
// 바꾸지 않는 수정만 뜻하므로(Build/README.md 올리는 규칙) 호환 판정은 주·부만 본다.
//
// 사용 예(에디터 패키지 저장·로드, A1):
//   const EngineVersion current = build::CurrentEngineVersion();
//   summary.savedBy = current;                                   // 주·부·패치
//   summary.compatibleWith = current.CompatibleBaseline();       // 주·부·0
//   if (!current.CanLoadPackageCompatibleWith(loaded.compatibleWith)) {
//       return NewerVersion;                                     // 더 새 엔진이 저장한 패키지
//   }
//   char text[EngineVersion::TextBytes];
//   current.ToChars(text);                                       // "0.1.0"
//
// 집합체라 {주, 부, 패치}로 만든다. 모든 연산은 실패하지 않고 공유 상태가 없다(스레드 안전).
namespace rocks {

struct ROCKS_CORE_API EngineVersion {
    // ToChars가 쓰는 가장 긴 표기 "65535.65535.65535"(17자)와 NUL이다.
    static constexpr usize TextBytes = 18;

    uint16 majorVersion = 0;
    uint16 minorVersion = 0;
    uint16 patchVersion = 0;

    // 주·부가 모두 0인 빈 값인지다(0으로 초기화한 EngineVersion{} 포함).
    [[nodiscard]] constexpr bool IsEmpty() const noexcept {
        return majorVersion == 0 && minorVersion == 0;
    }

    // 같은 주·부의 패치 0이다. 에디터 패키지 Summary의 compatibleWithEngineVersion에 쓴다(패치
    // 수정은 저장 형식을 바꾸지 않으므로 같은 주·부의 모든 패치가 그 패키지를 읽을 수 있다).
    [[nodiscard]] constexpr EngineVersion CompatibleBaseline() const noexcept {
        return {majorVersion, minorVersion, 0};
    }

    // 패키지 로드 판정이다. compatibleWith(패키지 Summary의 compatibleWithEngineVersion)의 주·부가
    // 이 엔진보다 새면 false다(그 패키지는 이 엔진이 모르는 형식일 수 있다 — NewerVersion). 같거나
    // 오래되면 true이며, 이때 실제로 읽을 수 있는지는 엔진 버전이 아니라 데이터 축(formatVersion·
    // schemaVersion·타입 스키마)이 판정한다. 패치는 보지 않는다.
    // 프로젝트 연결 판정(주·부가 같아야 함 — 빌드 도구 EngineVersion.CanLinkProject)과 의미가 달라
    // 이름을 나눈다(UE IsCompatibleWith와 같은 이름을 다른 뜻으로 쓰지 않는다).
    [[nodiscard]] constexpr bool
    CanLoadPackageCompatibleWith(EngineVersion compatibleWith) const noexcept {
        return compatibleWith.CompatibleBaseline() <= CompatibleBaseline();
    }

    // "주.부.패치"(정규 10진 — 앞자리 0 없음, 예: "0.1.0")와 NUL을 out에 쓰고 NUL을 뺀 길이(5–17)를
    // 돌려준다. 빌드 도구 manifest의 engine-version 줄과 같은 표기다. CRT를 쓰지 않는다.
    usize ToChars(char (&out)[TextBytes]) const noexcept;

    friend constexpr bool operator==(EngineVersion, EngineVersion) noexcept = default;

    // 순서 비교다(주, 부, 패치 사전식). STL <compare>를 쓰지 않으려고 직접 정의한다.
    friend constexpr bool operator<(EngineVersion left, EngineVersion right) noexcept {
        if (left.majorVersion != right.majorVersion) {
            return left.majorVersion < right.majorVersion;
        }

        if (left.minorVersion != right.minorVersion) {
            return left.minorVersion < right.minorVersion;
        }

        return left.patchVersion < right.patchVersion;
    }

    friend constexpr bool operator<=(EngineVersion left, EngineVersion right) noexcept {
        return !(right < left);
    }

    friend constexpr bool operator>(EngineVersion left, EngineVersion right) noexcept {
        return right < left;
    }

    friend constexpr bool operator>=(EngineVersion left, EngineVersion right) noexcept {
        return !(left < right);
    }
};

} // namespace rocks

#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>

// Sparks 패키지 manifest(`SparksPackage.json5`)다(Docs/Design/SparksLanguage.ko.md 3.4절 "패키지
// manifest", 진단 S1101–S1105).
//
// 사용 예:
//   PackageManifest manifest(allocator);
//   auto read = ReadPackageManifest(sources, manifestId, diagnostics, manifest);
//   if (read && read.Value()) {
//       Use(manifest.Name(), manifest.GetProfile());
//   }
//
// 수명: 값은 manifest 객체가 가진다(원문과 무관). 한 객체는 한 스레드에서 쓴다.

namespace rocks::sparks {

// 이 컴파일러가 아는 언어 버전이다(manifest `sparks`).
inline constexpr uint32 CurrentLanguageVersion = 1;

// 엔진 binding schema의 패키지 이름이다(언제나 의존하고 import하지 않는다 — SD60).
inline constexpr StringView EnginePackageName = "Rocks";

// 패키지 루트의 manifest 파일 이름이다.
inline constexpr StringView ManifestFileName = "SparksPackage.json5";

// logCategory 상한이다. 엔진 카테고리는 `Script.<logCategory>`이므로 LoggingSystem 6.2절
// 상한(8구간, 63 byte)에서 `Script.` 한 구간·7 byte를 뺀 값이다.
inline constexpr usize MaxLogCategorySegments = 7;
inline constexpr usize MaxLogCategoryBytes = 56;

// 패키지 이름의 최대 byte 수다. logCategory를 생략하면 패키지 이름이므로 그 상한과 같다.
inline constexpr usize MaxPackageNameBytes = MaxLogCategoryBytes;

// manifest `profile`이다(1.3절).
enum class Profile : uint8 {
    Core,
    Full
};

namespace detail {
class ManifestReader;
} // namespace detail

// manifest의 이름 값 하나다. 글자는 PackageManifest::Text로 읽는다.
struct ManifestName {
    uint32 offset = 0;
    uint32 length = 0;
    SourceSpan span; // 원문의 그 값(따옴표 포함)
};

class PackageManifest final {
public:
    explicit PackageManifest(Allocator& allocator) noexcept;

    PackageManifest(const PackageManifest&) = delete;
    PackageManifest& operator=(const PackageManifest&) = delete;
    PackageManifest(PackageManifest&&) noexcept = default;
    PackageManifest& operator=(PackageManifest&&) noexcept = default;
    ~PackageManifest() noexcept = default;

    SourceId Source() const noexcept {
        return source_;
    }

    StringView Name() const noexcept {
        return Text(name_);
    }

    // `name` 값의 원문 범위다(S1112 위치).
    SourceSpan NameSpan() const noexcept {
        return name_.span;
    }

    uint32 LanguageVersion() const noexcept {
        return languageVersion_;
    }

    Profile GetProfile() const noexcept {
        return profile_;
    }

    // 의존 패키지 이름(manifest 순서)이다.
    Span<const ManifestName> Dependencies() const noexcept {
        return dependencies_.View();
    }

    // WorldService 클래스 경로(`Combat.Score.ScoreKeeper`, manifest 순서)다.
    Span<const ManifestName> Services() const noexcept {
        return services_.View();
    }

    // 로그 카테고리다(생략하면 패키지 이름).
    StringView LogCategory() const noexcept {
        return Text(logCategory_);
    }

    StringView Text(const ManifestName& name) const noexcept {
        return StringView(text_.Data() + name.offset, name.length);
    }

private:
    friend class detail::ManifestReader;

    Array<char> text_;
    ManifestName name_;
    ManifestName logCategory_;
    Array<ManifestName> dependencies_;
    Array<ManifestName> services_;
    SourceId source_;
    uint32 languageVersion_ = 0;
    Profile profile_ = Profile::Core;
};

// id 파일(manifest)을 읽어 out을 채운다. 형식 오류는 diagnostics에 S1101–S1105로 붙이고(오류마다
// 하나, 계속 읽어 모두 알린다) false를 돌려준다 — 그때도 `name` 값 자체가 맞으면
// Name()·NameSpan()은 그 값이고(패키지 이름 중복 검사용), 나머지 내용은 정해지지 않는다. 파일은
// IsUsable()이어야 한다(아니면 InvalidArgument). 실패: 할당 실패.
Result<bool> ReadPackageManifest(const SourceSet& sources, SourceId id, DiagnosticList& diagnostics,
                                 PackageManifest& out) noexcept;

} // namespace rocks::sparks

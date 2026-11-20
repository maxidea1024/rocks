#include <SparksCompiler/PackageManifest.h>

#include <Core/Text/EditDistance.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>

#include "Names.h"
#include "SchemaInput.h"

// manifest reader다(Docs/Design/SparksLanguage.ko.md 3.4절 "패키지 manifest"). 오류마다 진단 하나를
// 내고 끝까지 읽는다.

namespace rocks::sparks {

namespace {

// 진단 문장 builder의 inline 크기다.
constexpr usize ManifestMessageInlineBytes = 192;

// 알 수 없는 key에 비슷한 key를 제안하는 편집 거리 문턱이다.
constexpr usize MaxManifestKeySuggestionDistance = 2;

constexpr StringView ManifestKeys[] = {"name",         "sparks",   "profile",
                                       "dependencies", "services", "logCategory"};

// 진단 문장에 그대로 넣어도 되는 글자인지다(제어 문자·비ASCII 없음 — 한 줄·터미널 안전).
bool IsQuotable(StringView text) noexcept {
    constexpr char FirstPrintable = 0x20;
    constexpr char LastPrintable = 0x7E;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        const char value = text.Data()[index];
        if (value < FirstPrintable || value > LastPrintable) {
            return false;
        }
    }

    return !text.IsEmpty();
}

} // namespace

PackageManifest::PackageManifest(Allocator& allocator) noexcept
    : text_(allocator), dependencies_(allocator), services_(allocator) {
}

namespace detail {

class ManifestReader final {
public:
    ManifestReader(const SourceSet& sources, SourceId id, DiagnosticList& diagnostics,
                   PackageManifest& out) noexcept
        : file_(sources.Get(id)), id_(id), diagnostics_(diagnostics), out_(out),
          allocator_(out.text_.GetAllocator()) {
    }

    Result<bool> Run() noexcept {
        out_.text_.Clear();
        out_.dependencies_.Clear();
        out_.services_.Clear();
        out_.name_ = ManifestName{};
        out_.logCategory_ = ManifestName{};
        out_.source_ = id_;
        out_.languageVersion_ = 0;
        out_.profile_ = Profile::Core;

        JsonDocument document(allocator_);
        JsonReadOptions options;
        options.syntax = JsonSyntax::Json5;
        const JsonError parsed = document.Parse(file_.Text(), options);
        if (!parsed.Succeeded()) {
            if (parsed.code == JsonErrorCode::OutOfMemory) {
                return Result<bool>::Failure(ErrorCode::OutOfMemory);
            }

            const uint32 at = static_cast<uint32>(parsed.position.byteOffset);
            Report(DiagnosticCode::ManifestSyntax, SourceSpan{at, at},
                   "The package manifest is not valid JSON5.",
                   JsonErrorMessage(parsed.code, JsonSyntax::Json5),
                   "Fix the JSON5 syntax at this position.");
            return Finish();
        }

        const JsonValue root = document.Root();
        if (!root.IsObject()) {
            Report(DiagnosticCode::ManifestSyntax, SpanOf(root),
                   "The package manifest must be one JSON5 object.",
                   "It lists name, sparks, profile and the optional keys in braces.",
                   "Write the manifest as '{ name: \"Game\", sparks: 1, profile: \"core\" }'.");
            return Finish();
        }

        bool hasName = false;
        bool hasVersion = false;
        bool hasProfile = false;
        bool hasLogCategory = false;
        for (usize index = 0; index < root.Size() && error_ == ErrorCode::None; ++index) {
            const StringView key = root.KeyAt(index);
            const JsonValue value = root.ValueAt(index);
            if (key == "name") {
                hasName = true;
                ReadName(value);
            } else if (key == "sparks") {
                hasVersion = true;
                ReadVersion(value);
            } else if (key == "profile") {
                hasProfile = true;
                ReadProfile(value);
            } else if (key == "dependencies") {
                ReadNameList(value, true);
            } else if (key == "services") {
                ReadNameList(value, false);
            } else if (key == "logCategory") {
                hasLogCategory = true;
                ReadLogCategory(value);
            } else {
                ReportUnknownKey(key, value);
            }
        }

        if (!hasName) {
            ReportMissing(root, "The manifest has no 'name'.",
                          "Add a package name, as in 'name: \"Game\"'.");
        }

        if (!hasVersion) {
            ReportMissing(root, "The manifest has no 'sparks'.",
                          "Add the language version, as in 'sparks: 1'.");
        }

        if (!hasProfile) {
            ReportMissing(root, "The manifest has no 'profile'.",
                          "Add the profile, as in 'profile: \"core\"'.");
        }

        CheckDependenciesAgainstName();
        if (!hasLogCategory) {
            out_.logCategory_ = out_.name_;
        }

        return Finish();
    }

private:
    Result<bool> Finish() noexcept {
        if (error_ != ErrorCode::None) {
            return Result<bool>::Failure(error_);
        }

        return Result<bool>::Success(valid_);
    }

    SourceSpan SpanOf(JsonValue value) const noexcept {
        return detail::JsonValueSpan(file_.Text(), value.Position());
    }

    void Report(DiagnosticCode code, SourceSpan span, StringView message, StringView explanation,
                StringView fix) noexcept {
        valid_ = false;
        if (error_ != ErrorCode::None) {
            return;
        }

        if (auto added = diagnostics_.TryAdd(code, id_, span, message, explanation, fix); !added) {
            error_ = added.Error();
        }
    }

    // 값 오류(S1104)다. 이름을 문장에 넣을 수 있으면 넣는다.
    void ReportValue(SourceSpan span, StringView format, StringView quoted, StringView fallback,
                     StringView explanation, StringView fix) noexcept {
        StringBuilder<ManifestMessageInlineBytes> message(allocator_);
        if (IsQuotable(quoted)) {
            message.AppendFormat(format, quoted);
        } else {
            message.Append(fallback);
        }

        if (message.HasFailed()) {
            error_ = message.Status().Error();
            return;
        }

        Report(DiagnosticCode::InvalidManifestValue, span, message.View(), explanation, fix);
    }

    void ReportMissing(JsonValue root, StringView message, StringView fix) noexcept {
        Report(DiagnosticCode::MissingManifestKey, SpanOf(root), message,
               "'name', 'sparks' and 'profile' are required.", fix);
    }

    void ReportUnknownKey(StringView key, JsonValue value) noexcept {
        StringView suggestion;
        usize best = MaxManifestKeySuggestionDistance + 1;
        for (const StringView candidate : ManifestKeys) {
            const usize distance = EditDistance(key, candidate, MaxManifestKeySuggestionDistance,
                                                EditDistanceCase::AsciiInsensitive);
            if (distance < best) {
                best = distance;
                suggestion = candidate;
            }
        }

        StringBuilder<ManifestMessageInlineBytes> message(allocator_);
        StringBuilder<ManifestMessageInlineBytes> fix(allocator_);
        if (IsQuotable(key)) {
            message.AppendFormat("Unknown manifest key '{}'.", key);
        } else {
            message.Append("Unknown manifest key.");
        }

        if (!suggestion.IsEmpty()) {
            fix.AppendFormat("Rename it to '{}'.", suggestion);
        } else {
            fix.Append("Remove the key.");
        }

        if (message.HasFailed() || fix.HasFailed()) {
            error_ = ErrorCode::OutOfMemory;
            return;
        }

        Report(DiagnosticCode::UnknownManifestKey, SpanOf(value), message.View(),
               "A manifest has name, sparks, profile, dependencies, services and logCategory.",
               fix.View());
    }

    // 글자를 저장한다. 실패하면 out은 그대로다(빈 이름이 저장소 없는 글자를 가리키지 않게).
    bool Store(StringView text, SourceSpan span, ManifestName& out) noexcept {
        const uint32 offset = static_cast<uint32>(out_.text_.Size());
        if (auto appended = out_.text_.TryAppend(Span<const char>(text.Data(), text.ByteCount()));
            !appended) {
            error_ = appended.Error();
            return false;
        }

        out.offset = offset;
        out.length = static_cast<uint32>(text.ByteCount());
        out.span = span;
        return true;
    }

    void ReadName(JsonValue value) noexcept {
        const SourceSpan span = SpanOf(value);
        if (!value.IsString()) {
            Report(DiagnosticCode::InvalidManifestValue, span, "'name' must be a string.", "",
                   "Write the package name in quotes, as in 'name: \"Game\"'.");
            return;
        }

        const StringView name = value.GetString();
        if (!detail::IsPascalName(name) || name.ByteCount() > MaxPackageNameBytes) {
            ReportValue(span, "Package name '{}' is not valid.", name,
                        "The package name is not valid.",
                        "A package name starts with an uppercase English letter and uses only "
                        "ASCII letters and digits (at most 56 bytes).",
                        "Use a name such as 'Game' or 'DoorKit'.");
            return;
        }

        if (name == EnginePackageName) {
            Report(DiagnosticCode::InvalidManifestValue, span,
                   "'Rocks' is the engine package name.",
                   "Every package already depends on the engine package 'Rocks'.",
                   "Choose another package name.");
            return;
        }

        Store(name, span, out_.name_);
    }

    void ReadVersion(JsonValue value) noexcept {
        const SourceSpan span = SpanOf(value);
        // 정수 종류(10진·16진)만 받는다. int64를 넘는 양의 정수도 정수이므로 S1105다.
        const auto version = value.TryGetInt64();
        const auto large =
            version ? Result<uint64>::Failure(ErrorCode::InvalidArgument) : value.TryGetUInt64();
        if (!version && !large) {
            Report(DiagnosticCode::InvalidManifestValue, span,
                   "'sparks' must be an integer language version.", "", "Write 'sparks: 1'.");
            return;
        }

        if (!version || version.Value() != static_cast<int64>(CurrentLanguageVersion)) {
            StringBuilder<ManifestMessageInlineBytes> message(allocator_);
            if (version) {
                message.AppendFormat("Sparks language version {} is not supported.",
                                     version.Value());
            } else {
                message.AppendFormat("Sparks language version {} is not supported.", large.Value());
            }

            if (message.HasFailed()) {
                error_ = message.Status().Error();
                return;
            }

            Report(DiagnosticCode::UnsupportedLanguageVersion, span, message.View(),
                   "This compiler knows language version 1 only.", "Write 'sparks: 1'.");
            return;
        }

        out_.languageVersion_ = CurrentLanguageVersion;
    }

    void ReadProfile(JsonValue value) noexcept {
        const SourceSpan span = SpanOf(value);
        if (value.IsString() && value.GetString() == "core") {
            out_.profile_ = Profile::Core;
            return;
        }

        if (value.IsString() && value.GetString() == "full") {
            out_.profile_ = Profile::Full;
            return;
        }

        Report(DiagnosticCode::InvalidManifestValue, span,
               "'profile' must be \"core\" or \"full\".",
               "The profile decides which language features the package may use.",
               "Write 'profile: \"core\"'.");
    }

    // dependencies(패키지 이름)와 services(모듈 경로 + 클래스 이름)다.
    void ReadNameList(JsonValue value, bool dependencies) noexcept {
        const SourceSpan span = SpanOf(value);
        Array<ManifestName>& list = dependencies ? out_.dependencies_ : out_.services_;
        if (!value.IsArray()) {
            Report(DiagnosticCode::InvalidManifestValue, span,
                   dependencies ? StringView("'dependencies' must be an array of package names.")
                                : StringView("'services' must be an array of class paths."),
                   "",
                   dependencies ? StringView("Write 'dependencies: [\"Weapons\"]'.")
                                : StringView("Write 'services: [\"Combat.Score.ScoreKeeper\"]'."));
            return;
        }

        for (usize index = 0; index < value.Size() && error_ == ErrorCode::None; ++index) {
            const JsonValue element = value.At(index);
            const SourceSpan elementSpan = SpanOf(element);
            if (!element.IsString()) {
                Report(DiagnosticCode::InvalidManifestValue, elementSpan,
                       dependencies ? StringView("Each dependency must be a package name string.")
                                    : StringView("Each service must be a class path string."),
                       "", "Write the name in quotes.");
                continue;
            }

            const StringView text = element.GetString();
            if (dependencies) {
                if (!detail::IsPascalName(text) || text.ByteCount() > MaxPackageNameBytes) {
                    ReportValue(elementSpan, "Dependency '{}' is not a valid package name.", text,
                                "The dependency is not a valid package name.",
                                "A package name starts with an uppercase English letter and uses "
                                "only ASCII letters and digits.",
                                "Write the name of a package, as in 'Weapons'.");
                    continue;
                }

                if (text == EnginePackageName) {
                    Report(DiagnosticCode::InvalidManifestValue, elementSpan,
                           "'Rocks' is always a dependency.",
                           "The engine package is available to every package without listing it.",
                           "Remove 'Rocks' from the dependencies.");
                    continue;
                }
            } else {
                usize segments = 0;
                if (!detail::IsDottedPascalName(text, segments) || segments < 2) {
                    ReportValue(elementSpan, "Service '{}' is not a class path.", text,
                                "The service is not a class path.",
                                "A service is a module import path followed by a WorldService "
                                "class name.",
                                "Write a path such as 'Combat.Score.ScoreKeeper'.");
                    continue;
                }
            }

            bool duplicate = false;
            for (const ManifestName& existing : list) {
                if (out_.Text(existing) == text) {
                    duplicate = true;
                }
            }

            if (duplicate) {
                ReportValue(elementSpan,
                            dependencies ? StringView("Dependency '{}' is listed twice.")
                                         : StringView("Service '{}' is listed twice."),
                            text, "The entry is listed twice.", "", "Remove the repeated entry.");
                continue;
            }

            ManifestName stored;
            if (Store(text, elementSpan, stored)) {
                if (auto pushed = list.TryPushBack(stored); !pushed) {
                    error_ = pushed.Error();
                }
            }
        }
    }

    void ReadLogCategory(JsonValue value) noexcept {
        const SourceSpan span = SpanOf(value);
        usize segments = 0;
        if (!value.IsString() || !detail::IsDottedPascalName(value.GetString(), segments) ||
            segments > MaxLogCategorySegments ||
            value.GetString().ByteCount() > MaxLogCategoryBytes) {
            Report(DiagnosticCode::InvalidManifestValue, span,
                   "'logCategory' is not a valid log category.",
                   "It is up to 7 dot-separated parts, each starting with an uppercase English "
                   "letter and using only ASCII letters and digits, and at most 56 bytes.",
                   "Write a category such as 'Game' or 'Game.Doors'.");
            return;
        }

        Store(value.GetString(), span, out_.logCategory_);
    }

    // 자기 자신 의존은 name을 읽은 뒤에야 판정한다(key 순서와 무관하게).
    void CheckDependenciesAgainstName() noexcept {
        if (out_.name_.length == 0) {
            return;
        }

        for (const ManifestName& dependency : out_.dependencies_) {
            if (out_.Text(dependency) == out_.Name()) {
                Report(DiagnosticCode::InvalidManifestValue, dependency.span,
                       "A package cannot depend on itself.",
                       "Modules of the same package import each other without the package name.",
                       "Remove this dependency.");
            }
        }
    }

    const SourceFile& file_;
    SourceId id_;
    DiagnosticList& diagnostics_;
    PackageManifest& out_;
    Allocator& allocator_;
    bool valid_ = true;
    ErrorCode error_ = ErrorCode::None;
};

} // namespace detail

Result<bool> ReadPackageManifest(const SourceSet& sources, SourceId id, DiagnosticList& diagnostics,
                                 PackageManifest& out) noexcept {
    if (!sources.Get(id).IsUsable()) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    detail::ManifestReader reader(sources, id, diagnostics, out);
    return reader.Run();
}

} // namespace rocks::sparks

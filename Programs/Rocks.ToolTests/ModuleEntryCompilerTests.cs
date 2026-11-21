using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 모듈 진입점 매크로 ROCKS_IMPLEMENT_MODULE(Core/Module/ModuleEntry.h, Docs/Design/ModuleLoader.ko.md 3.1·3.2절)의 컴파일 시점
// 검사를 실제 컴파일러로 확인한다(구문 검사만 한다). 빌드가 넣는 ROCKS_MODULE_NAME과 같은 이름은 컴파일되고, 다른 이름·정의가 없는
// 컴파일·잘못된 RegisterSettings 서명·ModuleImplementation이 아닌 클래스는 영어 static_assert 문구로 실패해야 한다. 매크로가 빠진
// 모듈의 링크 오류는 Rocks.Build.Tests(ModuleTableTests)가 실제 링크로 검사한다.
internal static class ModuleEntryCompilerTests
{
    private const string Prelude = """
        #include <Core/Module/ModuleEntry.h>

        namespace rocks::config {
        class ConfigContext {};
        } // namespace rocks::config

        class SettingsModule final : public rocks::ModuleImplementation {
        public:
            static rocks::Result<void> RegisterSettings(rocks::config::ConfigContext&) noexcept {
                return rocks::Result<void>::Success();
            }
        };

        class WrongSettingsModule final : public rocks::ModuleImplementation {
        public:
            static void RegisterSettings(rocks::config::ConfigContext&) noexcept {
            }
        };

        // final이 아닌 클래스는 이름 조회 탐침으로 접근 권한·오버로드와 무관하게 이름을 찾는다.
        class OpenSettingsModule : public rocks::ModuleImplementation {
        public:
            static rocks::Result<void> RegisterSettings(rocks::config::ConfigContext&) noexcept {
                return rocks::Result<void>::Success();
            }
        };

        class PrivateSettingsModule : public rocks::ModuleImplementation {
            static rocks::Result<void> RegisterSettings(rocks::config::ConfigContext&) noexcept {
                return rocks::Result<void>::Success();
            }
        };

        class OverloadedSettingsModule : public rocks::ModuleImplementation {
        public:
            static rocks::Result<void> RegisterSettings(rocks::config::ConfigContext&) noexcept {
                return rocks::Result<void>::Success();
            }

            static rocks::Result<void> RegisterSettings(rocks::config::ConfigContext&, int) noexcept {
                return rocks::Result<void>::Success();
            }
        };

        class MemberSettingsModule : public rocks::ModuleImplementation {
        public:
            rocks::Result<void> RegisterSettings(rocks::config::ConfigContext&) noexcept {
                return rocks::Result<void>::Success();
            }
        };

        class NotAModule {};

        """;

    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        if (options.Optional("compiler") is not string compiler)
        {
            Console.WriteLine("[SKIP] Native.ModuleEntryCompilerContracts: --compiler was not supplied");
            return;
        }

        suite.Add("Native.ModuleEntryCompilerContracts", async () =>
        {
            using TemporaryDirectory temporary = new();
            bool clangCl = Path.GetFileNameWithoutExtension(compiler).Equals("clang-cl", StringComparison.OrdinalIgnoreCase);
            string[] includeDirectories = Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal).ToArray();

            async Task<ProcessResult> Compile(string name, string? moduleName, string body, bool moduleTypes = false, bool sharedLibrary = false)
            {
                temporary.Write(name + ".cpp", Prelude + body + "\n");
                string source = temporary.FilePath(name + ".cpp");
                List<string> arguments = clangCl
                    ? ["/nologo", "/std:c++latest", "/utf-8", "/W4", "/WX", "/Zs", source]
                    : ["-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsyntax-only", source];
                foreach (string directory in includeDirectories)
                {
                    arguments.Add((clangCl ? "/I" : "-I") + directory);
                }

                if (moduleName is not null)
                {
                    arguments.Add((clangCl ? "/D" : "-D") + "ROCKS_MODULE_NAME=" + moduleName);
                }

                if (moduleTypes)
                {
                    arguments.Add((clangCl ? "/D" : "-D") + "ROCKS_MODULE_TYPES=1");
                }

                if (sharedLibrary)
                {
                    arguments.Add((clangCl ? "/D" : "-D") + "ROCKS_MODULE_SHARED_LIBRARY=1");
                }

                arguments.AddRange(options.ClangArguments);
                return await ProcessRunner.RunAsync(new(compiler, arguments, options.Root, environment,
                    Timeout: TimeSpan.FromSeconds(60)));
            }

            // 같은 이름, 설정 등록 함수가 있는 클래스, 여러 실행 파일이 함께 컴파일하는 소스의 ROCKS_MODULE_NAME 이름 자리는 컴파일된다.
            (string Name, string? Module, string Source)[] accepted =
            [
                ("same_name", "Curves", "ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Curves)"),
                ("settings", "Physics", "ROCKS_IMPLEMENT_MODULE(SettingsModule, Physics)\n" +
                    "static_assert(rocks::detail::MakeModuleEntry<SettingsModule>(\"Physics\").registerSettings ==\n" +
                    "              &SettingsModule::RegisterSettings);\n" +
                    "static_assert(rocks::detail::MakeModuleEntry<rocks::DefaultModuleImplementation>(\"Other\").registerSettings == nullptr);"),
                ("settings_open", "Physics", "ROCKS_IMPLEMENT_MODULE(OpenSettingsModule, Physics)\n" +
                    "static_assert(rocks::detail::MakeModuleEntry<OpenSettingsModule>(\"Physics\").registerSettings ==\n" +
                    "              &OpenSettingsModule::RegisterSettings);"),
                ("expanded_name", "FoundationTests", "ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, ROCKS_MODULE_NAME)"),
            ];
            (string Name, string? Module, string Source, string Diagnostic)[] rejected =
            [
                ("name_mismatch", "Curves", "ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Curve)",
                    "ROCKS_IMPLEMENT_MODULE name does not match the module being built"),
                ("name_case", "Curves", "ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, curves)",
                    "ROCKS_IMPLEMENT_MODULE name does not match the module being built"),
                ("name_undefined", null, "ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Curves)",
                    "ROCKS_MODULE_NAME is not defined"),
                ("settings_signature", "Physics", "ROCKS_IMPLEMENT_MODULE(WrongSettingsModule, Physics)",
                    "RegisterSettings must be a public, non-overloaded static member"),
                // requires 식만으로는 private·오버로드·비정적 RegisterSettings가 진단 없이 "없음"이 되어 설정 등록이 빠졌다.
                ("settings_private", "Physics", "ROCKS_IMPLEMENT_MODULE(PrivateSettingsModule, Physics)",
                    "RegisterSettings must be a public, non-overloaded static member"),
                ("settings_overloaded", "Physics", "ROCKS_IMPLEMENT_MODULE(OverloadedSettingsModule, Physics)",
                    "RegisterSettings must be a public, non-overloaded static member"),
                ("settings_member", "Physics", "ROCKS_IMPLEMENT_MODULE(MemberSettingsModule, Physics)",
                    "RegisterSettings must be a public, non-overloaded static member"),
                ("not_a_module", "Physics", "ROCKS_IMPLEMENT_MODULE(NotAModule, Physics)",
                    "ROCKS_IMPLEMENT_MODULE class must derive from rocks::ModuleImplementation"),
            ];

            // 자기 타입 표지(ROCKS_MODULE_TYPES=1, Docs/Design/ModuleLoader.ko.md 3.10절): 매크로가 스스로 선언한 RegisterModuleOwnTypes가
            // 메타 컴파일러의 모듈 생성 헤더 선언·모듈 생성 소스 정의와 같은 서명이고 진입 기술의 registerTypes 칸에 맞는다.
            const string ownTypesSignature =
                "::rocks::Result<void> RegisterModuleOwnTypes(::rocks::ModuleRegistration& registration, const ::rocks::ModuleLease& module) noexcept";
            Task<ProcessResult> moduleTypes = Compile("module_types", "Curves",
                $"namespace rocks::generated::Curves {{ {ownTypesSignature}; }}\n" +
                "ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Curves)\n" +
                $"namespace rocks::generated::Curves {{ {ownTypesSignature} {{ static_cast<void>(registration); static_cast<void>(module); " +
                "return ::rocks::Result<void>::Success(); } }\n" +
                "static_assert(rocks::detail::MakeModuleEntry<rocks::DefaultModuleImplementation>(\"Curves\", &rocks::generated::Curves::RegisterModuleOwnTypes)" +
                ".registerTypes != nullptr);", moduleTypes: true);

            // 공유 라이브러리 표지(ROCKS_MODULE_SHARED_LIBRARY=1, Docs/Design/BuildTargets.ko.md 3.8절 BS8d1): 매크로가 export 진입점
            // RocksModuleEntry와 강제 참조용 모듈별 기호 RocksModuleEntryOf_<Name>을 더 정의한다. 헤더가 export 철자(Base/Compiler.h)를 스스로
            // include해야 PCH 없이도 컴파일된다.
            Task<ProcessResult> sharedLibrary = Compile("shared_library", "Curves",
                "ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Curves)\n" +
                "static_assert(sizeof(&RocksModuleEntry) != 0 && sizeof(&RocksModuleEntryOf_Curves) != 0);", sharedLibrary: true);

            // 서로 독립인 컴파일이라 함께 돌린다.
            Task<ProcessResult>[] controls = accepted.Select(item => Compile(item.Name, item.Module, item.Source)).ToArray();
            Task<ProcessResult>[] failures = rejected.Select(item => Compile(item.Name, item.Module, item.Source)).ToArray();
            for (int index = 0; index < accepted.Length; ++index)
            {
                ProcessResult result = await controls[index];
                Verify.Equal(0, result.ExitCode, accepted[index].Name + " must compile: " + result.StdOut + result.StdErr);
            }

            ProcessResult typesResult = await moduleTypes;
            Verify.Equal(0, typesResult.ExitCode, "module_types must compile: " + typesResult.StdOut + typesResult.StdErr);
            ProcessResult sharedResult = await sharedLibrary;
            Verify.Equal(0, sharedResult.ExitCode, "shared_library must compile: " + sharedResult.StdOut + sharedResult.StdErr);

            for (int index = 0; index < rejected.Length; ++index)
            {
                ProcessResult result = await failures[index];
                string output = result.StdOut + result.StdErr;
                Verify.That(result.ExitCode != 0, rejected[index].Name + " compiled but must be rejected");
                Verify.Contains(rejected[index].Diagnostic, output, rejected[index].Name + " diagnostic");
            }
        });
    }
}

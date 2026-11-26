using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// 셰이더 파라미터 구조체를 생성한다(Docs/Design/ShaderSystem 16.2절 SSD49–SSD52). 리플렉션 메타 단계(<see cref="MetadataRunner"/>)가 게시한 뒤
/// 셰이더 파라미터 헤더를 가진 모듈마다 MetaCompiler 셰이더 파라미터 모드(<c>shader-parameters</c>)를 한 번 돌린다. MetaCompiler가 모듈의 출력
/// 루트(&lt;out&gt;/Generated/&lt;모듈&gt;/ShaderParameters)에 생성 C++ 헤더(Include)와 생성 HLSL 사본(Shaders)을 내용이 바뀔 때만 쓰고, 이번 실행이
/// 내지 않은 낡은 파일을 지운다. 컴파일 인자·정의 헤더·include 순서는 리플렉션 메타 단계와 같다.
/// </summary>
public static class ShaderParameterRunner
{
    /// <summary>
    /// plan의 셰이더 파라미터 모듈마다 생성한다. 반환값은 0(성공 또는 모듈 없음) 또는 MetaCompiler 종료 코드(진단을 출력함)다. gameProject가 참이면
    /// 플러그인이 아닌 모듈의 가상 루트가 /Project다(아니면 /Engine). 플러그인 모듈은 /Plugin/&lt;이름&gt;이다.
    /// </summary>
    public static async Task<int> GenerateAsync(BuildContext context, NativeToolchain toolchain, MetadataPlan plan, TargetRules target, string compilerTool,
        CancellationToken cancellationToken, bool gameProject = false)
    {
        if (plan.ShaderParameters.Count == 0)
        {
            return 0;
        }

        var byName = plan.Modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        HashSet<string> entryModules = ModuleDefinitions.EntryModules(plan.Modules);
        foreach (ShaderParameterModule run in plan.ShaderParameters)
        {
            cancellationToken.ThrowIfCancellationRequested();
            NativeModuleInput owner = byName[run.Name];
            string virtualRoot = owner.Plugin is ModulePlugin plugin ? "/Plugin/" + plugin.Name : gameProject ? "/Project" : "/Engine";
            var arguments = new List<string> { compilerTool, "shader-parameters", "--module", run.Name, "--output-root", run.OutputRoot,
                "--compiler", toolchain.CompilerPath, "--virtual-root", virtualRoot };
            foreach (MetadataHeader header in run.Headers)
            {
                arguments.AddRange(["--input", header.Path, "--include", header.Include]);
                if (header.Private)
                {
                    arguments.AddRange(["--private", header.Include]);
                }
            }

            foreach (string argument in owner.AdditionalCompileArguments.Concat(NativeCompileProfile.ModuleArguments(toolchain, context.Configuration, owner,
                NativeLanguage.Cpp, target.LinkType)))
            {
                arguments.Add("--clang-arg=" + argument);
            }

            List<NativeModuleInput> dependencies = ModuleVisibility.Of(owner, byName, (name, from) =>
                throw new ArgumentException($"Module '{from}' depends on unknown module '{name}'."));
            string[] defines = ModuleDefinitions.Of(owner, dependencies, entryModules.Contains(owner.Name), target.LinkType, toolchain.Platform);
            if (defines.Length != 0)
            {
                string definitions = ModuleDefinitions.PathOf(context.OutputDirectory, owner.Name);
                AtomicFile.WriteIfChanged(definitions, new System.Text.UTF8Encoding(false).GetBytes(ModuleDefinitions.Content(defines)));
                arguments.AddRange(["--definitions-header", definitions]);
            }

            foreach (string directory in MetadataLayout.IncludeOrder(owner, byName))
            {
                arguments.AddRange(["--include-dir", directory]);
            }

            ProcessResult generated = await ProcessRunner.RunAsync(new("dotnet", arguments, context.RootDirectory, toolchain.Environment,
                Timeout: TimeSpan.FromMinutes(3)), cancellationToken);
            Console.Write(generated.StdOut);
            Console.Error.Write(generated.StdErr);
            if (generated.ExitCode != 0)
            {
                return generated.ExitCode;
            }
        }

        return 0;
    }
}

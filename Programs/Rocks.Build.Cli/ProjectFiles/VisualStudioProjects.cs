using System.Security.Cryptography;
using System.Text;
using Microsoft.VisualStudio.SolutionPersistence.Model;
using Microsoft.VisualStudio.SolutionPersistence.Serializer;

namespace Rocks.Build.Cli;

/// <summary>
/// 이 컴퓨터의 도구 모음 값이다(Docs/Design/ProjectFiles.ko.md 8.5절). 두 도구 모음 props에만 들어간다.
/// LlvmInstallDirectory·LlvmToolsVersion·ResourceIncludeDirectory는 컴파일러 resource 디렉터리(<c>&lt;LLVM&gt;/lib/clang/&lt;주 버전&gt;</c>)에서
/// 얻는다. IncludePath는 [ResourceIncludeDirectory] + SystemIncludes(<c>INCLUDE</c>), ExternalIncludePath는 SystemIncludes다.
/// </summary>
internal sealed record ProjectFilesToolchain(string Compiler, string LlvmInstallDirectory, string LlvmToolsVersion,
    IReadOnlyList<string> SystemIncludes, string? AsanRuntimeDirectory, string ResourceIncludeDirectory);

/// <summary>타깃 하나의 프로젝트 정보다. Executables는 구성 이름 → 실행 파일 경로(실행 파일이 아니면 null)다.</summary>
internal sealed record ProjectFilesTarget(string Name, string TargetType, IReadOnlyDictionary<string, string?> Executables);

/// <summary>
/// 프로젝트 파일 렌더링 입력이다. Configurations는 솔루션 구성 순서, Android는 Android 플랫폼을 넣는가, CSharpProjects는 저장소 기준
/// C# 프로젝트 경로(빌드 안 함, 6.4절), SolutionPath는 솔루션 파일 절대 경로다.
/// </summary>
internal sealed record ProjectFilesInput(string Root, string ProjectFilesDirectory, string SolutionPath, IntelliSenseModel Model,
    IReadOnlyDictionary<string, ProjectFilesTarget> Targets, IReadOnlyList<string> Configurations, bool Android,
    ProjectFilesToolchain Toolchain, IReadOnlyList<string> CSharpProjects);

/// <summary>
/// 모델을 Visual Studio 파일(.vcxproj·.filters·.slnx·props·정의 헤더 사본)로 렌더링한다(Docs/Design/ProjectFiles.ko.md 6·7·8절).
/// 결과는 절대 경로 → 내용이고 아무것도 쓰지 않는다(쓰기는 <see cref="ProjectFilesOutput"/>). 같은 입력이면 byte가 같다.
/// </summary>
internal static class VisualStudioProjects
{
    /// <summary>공통 경로 props 파일 이름이다.</summary>
    public const string CommonProps = "Rocks.ProjectFiles.props";

    /// <summary>도구 모음 props(Microsoft.Cpp.Default.props 앞) 파일 이름이다.</summary>
    public const string ToolchainProps = "Rocks.Toolchain.props";

    /// <summary>도구 모음 경로 props(Microsoft.Cpp.props 뒤) 파일 이름이다.</summary>
    public const string ToolchainPathsProps = "Rocks.ToolchainPaths.props";

    // 프로젝트 GUID의 이름 공간이다(UUID v5, 6.5절). 바꾸면 모든 프로젝트 GUID가 바뀐다.
    private static readonly Guid ProjectNamespace = new("7a1f2c3e-5b4d-4e8f-9a0b-1c2d3e4f5a6b");

    // 필터 GUID의 이름 공간이다.
    private static readonly Guid FilterNamespace = new("0e9d8c7b-6a5f-4e3d-8c2b-1a0f9e8d7c6b");

    private const string WindowsPlatform = "Windows";

    private const string AndroidPlatform = "Android";

    // Visual Studio 프로젝트 플랫폼이다(모르는 플랫폼도 이 이름에 구성 이름으로 구분한다, 6.3절).
    private const string ProjectPlatform = "x64";

    // Android 구성 이름의 접두다(UE VCProject.cs:749와 같은 모양).
    private const string AndroidPrefix = "Android_arm64_";

    private const string Utf8Declaration = "<?xml version=\"1.0\" encoding=\"utf-8\"?>";

    public static SortedDictionary<string, byte[]> Render(ProjectFilesInput input)
    {
        var files = new SortedDictionary<string, byte[]>(StringComparer.Ordinal);
        string directory = Path.GetFullPath(input.ProjectFilesDirectory);
        void Add(string path, string text, bool crlf = true) =>
            files[Path.GetFullPath(path)] = new UTF8Encoding(false).GetBytes(crlf ? text.ReplaceLineEndings("\r\n") : text);

        Add(Path.Combine(directory, CommonProps), CommonPropsText(input));
        (byte[] toolchain, byte[] paths) = ToolchainFiles(input.Toolchain);
        files[Path.GetFullPath(Path.Combine(directory, ToolchainProps))] = toolchain;
        files[Path.GetFullPath(Path.Combine(directory, ToolchainPathsProps))] = paths;
        foreach ((string name, string content) in input.Model.DefinitionCopies)
        {
            // 정의 헤더 사본은 빌드가 쓰는 파일과 같은 byte(LF)다.
            Add(Path.Combine(directory, IntelliSenseModel.DefinitionsDirectoryName, name), content, crlf: false);
        }

        foreach (IdeProject project in input.Model.Projects)
        {
            string path = ProjectPath(input, project);
            Add(path, Project(input, project, path));
            Add(path + ".filters", Filters(input, project));
        }

        Add(input.SolutionPath, Solution(input));
        return files;
    }

    /// <summary>프로젝트 파일 경로다: 엔진 프로젝트는 프로젝트 파일 디렉터리, 타깃 프로젝트는 그 아래 Programs.</summary>
    public static string ProjectPath(ProjectFilesInput input, IdeProject project) => project.Engine
        ? Path.Combine(input.ProjectFilesDirectory, project.Name + ".vcxproj")
        : Path.Combine(input.ProjectFilesDirectory, "Programs", project.Name + ".vcxproj");

    /// <summary>이름 기반 UUID v5(RFC 9562, SHA-1)다.</summary>
    public static Guid NameGuid(Guid space, string name)
    {
        byte[] prefix = space.ToByteArray(bigEndian: true);
        byte[] hash = SHA1.HashData([.. prefix, .. Encoding.UTF8.GetBytes(name)]);
        hash[6] = (byte)((hash[6] & 0x0F) | 0x50);
        hash[8] = (byte)((hash[8] & 0x3F) | 0x80);
        return new Guid(hash.AsSpan(0, 16), bigEndian: true);
    }

    /// <summary>프로젝트 GUID다(저장소 기준 경로, '/' 구분, 대소문자 그대로).</summary>
    public static Guid ProjectGuid(string root, string projectPath) => NameGuid(ProjectNamespace, Relative(root, projectPath));

    // 솔루션·프로젝트 구성 이름이다(6.3절). 엔진 프로젝트의 Game이 아닌 종류는 _종류를 붙인다.
    private static string ProjectConfiguration(IdeProject project, ProjectFilesInput input, IdeProjectConfiguration configuration, string platform)
    {
        string type = input.Targets[configuration.Target].TargetType;
        string name = project.Engine && type != "Game" ? configuration.Configuration + "_" + type : configuration.Configuration;
        return platform == AndroidPlatform ? AndroidPrefix + name : name;
    }

    private static IEnumerable<string> Platforms(ProjectFilesInput input) => input.Android ? [WindowsPlatform, AndroidPlatform] : [WindowsPlatform];

    private static string CommonPropsText(ProjectFilesInput input)
    {
        string up = string.Join("\\", Path.GetRelativePath(input.ProjectFilesDirectory, input.Root).Split(Path.DirectorySeparatorChar)
            .Where(part => part.Length != 0 && part != "."));
        return $"""
            {Utf8Declaration}
            <!-- Generated by Rocks.Build projectfiles (Docs/Design/ProjectFiles.ko.md). Do not edit. -->
            <Project>
              <PropertyGroup>
                <RocksRoot>$([MSBuild]::NormalizeDirectory('$(MSBuildThisFileDirectory){up}'))</RocksRoot>
                <RocksProjectFiles>$(MSBuildThisFileDirectory)</RocksProjectFiles>
              </PropertyGroup>
            </Project>

            """;
    }

    /// <summary>
    /// 두 도구 모음 props의 내용(UTF-8, CRLF)이다. 생성과 빌드의 낡음 검사(9절)가 같은 렌더링을 쓴다 — 도구 모음이 바뀌면 지문이 바뀐다.
    /// </summary>
    public static (byte[] Toolchain, byte[] Paths) ToolchainFiles(ProjectFilesToolchain toolchain)
    {
        var encoding = new UTF8Encoding(false);
        return (encoding.GetBytes(ToolchainPropsText(toolchain).ReplaceLineEndings("\r\n")),
            encoding.GetBytes(ToolchainPathsPropsText(toolchain).ReplaceLineEndings("\r\n")));
    }

    private static string ToolchainPropsText(ProjectFilesToolchain toolchain) => $"""
        {Utf8Declaration}
        <!-- Generated by Rocks.Build projectfiles. Machine-specific values; imported before Microsoft.Cpp.Default.props. Do not edit. -->
        <Project>
          <PropertyGroup>
            <LLVMInstallDir>{Xml(Escape(toolchain.LlvmInstallDirectory))}</LLVMInstallDir>
            <LLVMToolsVersion>{Xml(toolchain.LlvmToolsVersion)}</LLVMToolsVersion>
            <RocksCompiler>{Xml(Escape(toolchain.Compiler))}</RocksCompiler>
            <RocksAsanRuntimeDirectory>{Xml(Escape(toolchain.AsanRuntimeDirectory ?? ""))}</RocksAsanRuntimeDirectory>
          </PropertyGroup>
        </Project>

        """;

    private static string ToolchainPathsPropsText(ProjectFilesToolchain toolchain) => $"""
        {Utf8Declaration}
        <!-- Generated by Rocks.Build projectfiles. Machine-specific values; imported after Microsoft.Cpp.props. Do not edit. -->
        <Project>
          <PropertyGroup>
            <IncludePath>{Xml(string.Join(";", new[] { toolchain.ResourceIncludeDirectory }.Concat(toolchain.SystemIncludes).Select(Escape)))}</IncludePath>
            <ExternalIncludePath>{Xml(string.Join(";", toolchain.SystemIncludes.Select(Escape)))}</ExternalIncludePath>
          </PropertyGroup>
        </Project>

        """;

    private static string Project(ProjectFilesInput input, IdeProject project, string path)
    {
        var text = new StringBuilder();
        text.AppendLine(Utf8Declaration);
        text.AppendLine("<!-- Generated by Rocks.Build projectfiles (Docs/Design/ProjectFiles.ko.md). Do not edit; regenerate with GenerateProjectFiles.bat. -->");
        text.AppendLine("<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">");
        text.AppendLine("  <ItemGroup Label=\"ProjectConfigurations\">");
        var rows = new List<(IdeProjectConfiguration Configuration, string Platform, string Name)>();
        foreach (string platform in Platforms(input))
        {
            foreach (IdeProjectConfiguration configuration in project.Configurations)
            {
                rows.Add((configuration, platform, ProjectConfiguration(project, input, configuration, platform)));
            }
        }

        foreach (var row in rows)
        {
            text.AppendLine($"    <ProjectConfiguration Include=\"{Xml(row.Name)}|{ProjectPlatform}\">");
            text.AppendLine($"      <Configuration>{Xml(row.Name)}</Configuration>");
            text.AppendLine($"      <Platform>{ProjectPlatform}</Platform>");
            text.AppendLine("    </ProjectConfiguration>");
        }

        text.AppendLine("  </ItemGroup>");
        text.AppendLine("  <PropertyGroup Label=\"Globals\">");
        text.AppendLine($"    <ProjectGuid>{{{ProjectGuid(input.Root, path).ToString().ToUpperInvariant()}}}</ProjectGuid>");
        text.AppendLine("    <Keyword>MakeFileProj</Keyword>");
        text.AppendLine($"    <RootNamespace>{Xml(project.Name)}</RootNamespace>");
        text.AppendLine("    <MinimumVisualStudioVersion>18.0</MinimumVisualStudioVersion>");
        text.AppendLine("  </PropertyGroup>");
        string up = project.Engine ? "" : "..\\";
        text.AppendLine($"  <Import Project=\"{up}{CommonProps}\" />");
        text.AppendLine($"  <Import Project=\"{up}{ToolchainProps}\" />");
        text.AppendLine("  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />");
        text.AppendLine("  <PropertyGroup Label=\"Configuration\">");
        text.AppendLine("    <ConfigurationType>Makefile</ConfigurationType>");
        text.AppendLine("    <PlatformToolset>ClangCL</PlatformToolset>");
        text.AppendLine("  </PropertyGroup>");
        text.AppendLine("  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />");
        text.AppendLine($"  <Import Project=\"{up}{ToolchainPathsProps}\" />");
        int defaultBundle = project.DefaultBundle ?? -1;
        foreach (var row in rows)
        {
            ProjectFilesTarget target = input.Targets[row.Configuration.Target];
            // Android 행은 컴파일러를 넘기지 않는다(CLI가 --platform android와 --compiler를 함께 받지 않는다, 7.1절).
            string arguments = $"-Target {target.Name} -Configuration {row.Configuration.Configuration} -Platform {row.Platform}" +
                (row.Platform == WindowsPlatform ? " -Compiler \"$(RocksCompiler)\"" : "");
            string? executable = row.Platform == WindowsPlatform ? target.Executables.GetValueOrDefault(row.Configuration.Configuration) : null;
            text.AppendLine($"  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='{Xml(row.Name)}|{ProjectPlatform}'\">");
            foreach ((string property, string action) in new[] { ("NMakeBuildCommandLine", "Build"), ("NMakeReBuildCommandLine", "Rebuild"), ("NMakeCleanCommandLine", "Clean") })
            {
                text.AppendLine($"    <{property}>{Xml($"\"$(RocksRoot)Build\\IdeBuild.bat\" -Action {action} {arguments}")}</{property}>");
            }

            text.AppendLine($"    <NMakeOutput>{(executable is null ? "" : Xml(Msb(input.Root, executable)))}</NMakeOutput>");
            string intermediate = Msb(input.Root, Path.Combine(input.ProjectFilesDirectory, "Intermediate", project.Name, row.Name + "_" + ProjectPlatform)) + "\\";
            text.AppendLine($"    <IntDir>{Xml(intermediate)}</IntDir>");
            text.AppendLine($"    <OutDir>{Xml(intermediate)}</OutDir>");
            text.AppendLine("    <LocalDebuggerWorkingDirectory>$(RocksRoot)</LocalDebuggerWorkingDirectory>");
            if (row.Configuration.Configuration == nameof(NativeConfiguration.Asan))
            {
                text.AppendLine("    <LocalDebuggerEnvironment>PATH=$(RocksAsanRuntimeDirectory);$(PATH)</LocalDebuggerEnvironment>");
            }

            foreach (IdeBundle bundle in project.Bundles)
            {
                IntelliSenseValues? values = bundle.Values[row.Configuration.Key];
                text.AppendLine($"    <RocksIncludes_{bundle.Number}>{Xml(List(input.Root, values?.Includes))}</RocksIncludes_{bundle.Number}>");
                text.AppendLine($"    <RocksDefines_{bundle.Number}>{Xml(string.Join(";", (values?.Defines ?? []).Select(Escape)))}</RocksDefines_{bundle.Number}>");
                text.AppendLine($"    <RocksForcedIncludes_{bundle.Number}>{Xml(List(input.Root, values?.ForcedIncludes))}</RocksForcedIncludes_{bundle.Number}>");
                text.AppendLine($"    <RocksOptions_{bundle.Number}>{Xml(Options(input.Root, values?.Options))}</RocksOptions_{bundle.Number}>");
            }

            text.AppendLine("  </PropertyGroup>");
            if (defaultBundle >= 0)
            {
                // 가장 많은 파일이 쓰는 묶음이 ClCompile 기본값이다(8.3절). 항목 메타데이터는 기본값을 바꾸므로 다른 묶음은 네 값을 모두 단다.
                text.AppendLine($"  <ItemDefinitionGroup Condition=\"'$(Configuration)|$(Platform)'=='{Xml(row.Name)}|{ProjectPlatform}'\">");
                text.AppendLine("    <ClCompile>");
                AppendMetadata(text, defaultBundle, "      ");
                text.AppendLine("    </ClCompile>");
                text.AppendLine("  </ItemDefinitionGroup>");
            }
        }

        text.AppendLine("  <ItemGroup>");
        foreach (IdeSourceFile file in project.Sources)
        {
            string include = Xml(Msb(input.Root, file.Path));
            if (file.Bundle is null)
            {
                text.AppendLine($"    <None Include=\"{include}\" />");
            }
            else if (file.Bundle == defaultBundle)
            {
                text.AppendLine($"    <ClCompile Include=\"{include}\" />");
            }
            else
            {
                text.AppendLine($"    <ClCompile Include=\"{include}\">");
                AppendMetadata(text, file.Bundle.Value, "      ");
                text.AppendLine("    </ClCompile>");
            }
        }

        foreach (string header in project.Headers)
        {
            text.AppendLine($"    <ClInclude Include=\"{Xml(Msb(input.Root, header))}\" />");
        }

        text.AppendLine("  </ItemGroup>");
        text.AppendLine("  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />");
        text.AppendLine("</Project>");
        return text.ToString();
    }

    private static void AppendMetadata(StringBuilder text, int bundle, string indent)
    {
        text.AppendLine($"{indent}<AdditionalIncludeDirectories>$(RocksIncludes_{bundle})</AdditionalIncludeDirectories>");
        text.AppendLine($"{indent}<PreprocessorDefinitions>$(RocksDefines_{bundle})</PreprocessorDefinitions>");
        text.AppendLine($"{indent}<ForcedIncludeFiles>$(RocksForcedIncludes_{bundle})</ForcedIncludeFiles>");
        text.AppendLine($"{indent}<AdditionalOptions>$(RocksOptions_{bundle})</AdditionalOptions>");
    }

    private static string Filters(ProjectFilesInput input, IdeProject project)
    {
        var text = new StringBuilder();
        text.AppendLine(Utf8Declaration);
        text.AppendLine("<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">");
        string FilterOf(string path) => Path.GetRelativePath(input.Root, Path.GetDirectoryName(path)!).Replace('/', '\\');
        var filters = new SortedSet<string>(StringComparer.Ordinal);
        foreach (string path in project.Sources.Select(file => file.Path).Concat(project.Headers))
        {
            for (string? filter = FilterOf(path); !string.IsNullOrEmpty(filter) && filter != "."; filter = Path.GetDirectoryName(filter))
            {
                filters.Add(filter);
            }
        }

        text.AppendLine("  <ItemGroup>");
        foreach (string filter in filters)
        {
            text.AppendLine($"    <Filter Include=\"{Xml(filter)}\">");
            text.AppendLine($"      <UniqueIdentifier>{{{NameGuid(FilterNamespace, filter.Replace('\\', '/')).ToString().ToUpperInvariant()}}}</UniqueIdentifier>");
            text.AppendLine("    </Filter>");
        }

        text.AppendLine("  </ItemGroup>");
        text.AppendLine("  <ItemGroup>");
        foreach (IdeSourceFile file in project.Sources)
        {
            string kind = file.Bundle is null ? "None" : "ClCompile";
            text.AppendLine($"    <{kind} Include=\"{Xml(Msb(input.Root, file.Path))}\">");
            text.AppendLine($"      <Filter>{Xml(FilterOf(file.Path))}</Filter>");
            text.AppendLine($"    </{kind}>");
        }

        foreach (string header in project.Headers)
        {
            text.AppendLine($"    <ClInclude Include=\"{Xml(Msb(input.Root, header))}\">");
            text.AppendLine($"      <Filter>{Xml(FilterOf(header))}</Filter>");
            text.AppendLine("    </ClInclude>");
        }

        text.AppendLine("  </ItemGroup>");
        text.AppendLine("</Project>");
        return text.ToString();
    }

    private static string Solution(ProjectFilesInput input)
    {
        var model = new SolutionModel();
        string[] engineTypes = [.. input.Model.Projects.Where(project => project.Engine).SelectMany(project => project.Configurations)
            .Select(configuration => input.Targets[configuration.Target].TargetType).Where(type => type != "Game").Distinct(StringComparer.Ordinal)
            .Order(StringComparer.Ordinal)];
        // 솔루션 구성: 구성마다 하나, 엔진 프로젝트에 Game이 아닌 종류가 있으면 "구성 종류"도 하나(6.3절).
        var buildTypes = new List<(string Name, string Configuration, string? Type)>();
        foreach (string configuration in input.Configurations)
        {
            buildTypes.Add((configuration, configuration, null));
            buildTypes.AddRange(engineTypes.Select(type => (configuration + " " + type, configuration, (string?)type)));
        }

        foreach (var buildType in buildTypes)
        {
            model.AddBuildType(buildType.Name);
        }

        foreach (string platform in Platforms(input))
        {
            model.AddPlatform(platform);
        }

        string solutionDirectory = Path.GetDirectoryName(input.SolutionPath)!;
        foreach (IdeProject project in input.Model.Projects)
        {
            SolutionFolderModel folder = model.AddFolder(project.Engine ? "/Engine/" : "/Programs/");
            SolutionProjectModel entry = model.AddProject(Path.GetRelativePath(solutionDirectory, ProjectPath(input, project)).Replace('\\', '/'), null, folder);
            entry.Id = ProjectGuid(input.Root, ProjectPath(input, project));
            entry.AddProjectConfigurationRule(new ConfigurationRule(BuildDimension.Platform, "*", "*", ProjectPlatform));
            foreach (var buildType in buildTypes)
            {
                foreach (string platform in Platforms(input))
                {
                    IdeProjectConfiguration? configuration = project.Configurations.FirstOrDefault(value => value.Configuration == buildType.Configuration &&
                        (!project.Engine || ((input.Targets[value.Target].TargetType == "Game") ? buildType.Type is null : input.Targets[value.Target].TargetType == buildType.Type)));
                    if (configuration is null)
                    {
                        entry.AddProjectConfigurationRule(new ConfigurationRule(BuildDimension.Build, buildType.Name, platform, "false"));
                        continue;
                    }

                    string name = ProjectConfiguration(project, input, configuration, platform);
                    if (name != buildType.Name)
                    {
                        entry.AddProjectConfigurationRule(new ConfigurationRule(BuildDimension.BuildType, buildType.Name, platform, name));
                    }
                }
            }
        }

        foreach (string csharp in input.CSharpProjects)
        {
            bool rules = csharp.EndsWith(".Rules.csproj", StringComparison.Ordinal);
            SolutionFolderModel folder = model.AddFolder(rules ? "/Programs/Rules/" : "/Programs/Tools/");
            SolutionProjectModel entry = model.AddProject(Path.GetRelativePath(solutionDirectory, Path.Combine(input.Root, csharp)).Replace('\\', '/'), null, folder);
            entry.Id = NameGuid(ProjectNamespace, csharp);
            // 솔루션 빌드는 C# 도구를 빌드하지 않는다(6.4절 — IdeBuild의 도구 준비와 같은 출력을 두고 경합한다).
            entry.AddProjectConfigurationRule(new ConfigurationRule(BuildDimension.BuildType, "*", "*", "Debug"));
            entry.AddProjectConfigurationRule(new ConfigurationRule(BuildDimension.Platform, "*", "*", "Any CPU"));
            entry.AddProjectConfigurationRule(new ConfigurationRule(BuildDimension.Build, "*", "*", "false"));
        }

        using var stream = new MemoryStream();
        SolutionSerializers.SlnXml.SaveAsync(stream, model, CancellationToken.None).GetAwaiter().GetResult();
        return Encoding.UTF8.GetString(stream.ToArray()).TrimStart('﻿');
    }

    // 저장소 기준 경로(슬래시)다.
    private static string Relative(string root, string path) => Path.GetRelativePath(root, Path.GetFullPath(path)).Replace('\\', '/');

    // MSBuild 값: 저장소 안 경로는 $(RocksRoot) 기준, 그 밖은 그대로(이스케이프).
    private static string Msb(string root, string path)
    {
        string full = Path.GetFullPath(path);
        string relative = Path.GetRelativePath(root, full);
        bool inside = !Path.IsPathRooted(relative) && relative != ".." && !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal);
        return inside ? "$(RocksRoot)" + Escape(relative) : Escape(full);
    }

    private static string List(string root, IReadOnlyList<string>? paths) => string.Join(";", (paths ?? []).Select(path => Msb(root, path)));

    // 옵션 안의 따옴표 경로(/Yu"…")도 $(RocksRoot) 기준으로 쓴다.
    private static string Options(string root, IReadOnlyList<string>? options) => string.Join(" ", (options ?? []).Select(option =>
        option.StartsWith("/Yu\"", StringComparison.Ordinal) ? "/Yu\"" + Msb(root, option[4..^1]) + "\"" : Escape(option)));

    // MSBuild 특수 문자 이스케이프다(%XX).
    public static string Escape(string value)
    {
        var text = new StringBuilder(value.Length);
        foreach (char character in value)
        {
            text.Append(character switch
            {
                '%' => "%25",
                '$' => "%24",
                '@' => "%40",
                '\'' => "%27",
                ';' => "%3B",
                '?' => "%3F",
                '*' => "%2A",
                _ => character.ToString()
            });
        }

        return text.ToString();
    }

    private static string Xml(string value) => System.Security.SecurityElement.Escape(value) ?? "";
}

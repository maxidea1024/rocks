param([Parameter(Mandatory = $true)][string]$MetaCompiler)

$ErrorActionPreference = 'Stop'

function Invoke-Git([string[]]$GitArguments) {
    $start = [System.Diagnostics.ProcessStartInfo]::new('git')
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false, $true)
    $start.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false, $true)
    foreach ($argument in $GitArguments) { $start.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($start)
    if ($null -eq $process) { throw 'Could not start git.' }
    try {
        $out = $process.StandardOutput.ReadToEndAsync()
        $err = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill($true)
            throw 'Git identity history query timed out.'
        }
        if ($process.ExitCode -ne 0) { throw "Git identity history query failed: $($err.GetAwaiter().GetResult())" }
        return $out.GetAwaiter().GetResult()
    } finally { $process.Dispose() }
}

# PR checkout의 실제 merge commit과 양 부모 및 유일한 merge base를 사용한다.
# 히스토리가 없거나 복수 merge-base이면 임의로 선택하지 않고 실패한다.
$parents = (Invoke-Git -GitArguments @('rev-list', '--parents', '-n', '1', 'HEAD')).Trim().Split(' ', [StringSplitOptions]::RemoveEmptyEntries)
if ($parents.Length -ne 3) { throw 'Identity merge check requires a two-parent PR merge commit.' }
$bases = (Invoke-Git -GitArguments @('merge-base', '--all', $parents[1], $parents[2])).Trim().Split("`n", [StringSplitOptions]::RemoveEmptyEntries)
if ($bases.Length -ne 1) { throw 'Identity merge check requires exactly one merge base.' }
$revisions = @($bases[0].Trim(), $parents[1], $parents[2], $parents[0])
$sets = @()
$all = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
foreach ($revision in $revisions) {
    $names = Invoke-Git -GitArguments @('ls-tree', '-r', '--name-only', '-z', $revision)
    $set = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($name in $names.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)) {
        if ($name.EndsWith('.rocksids.json', [StringComparison]::Ordinal)) {
            [void]$set.Add($name)
            [void]$all.Add($name)
        }
    }
    $sets += ,$set
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) ('rocks-identity-merge-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($temporary)
try {
    $counter = 0
    foreach ($name in $all) {
        $files = @()
        for ($index = 0; $index -lt 4; $index++) {
            $file = Join-Path $temporary ("$counter-$index.json")
            $content = if ($sets[$index].Contains($name)) {
                Invoke-Git -GitArguments @('show', ($revisions[$index] + ':' + $name))
            } else { '{"schema":1,"entries":[]}' }
            [IO.File]::WriteAllText($file, $content, [Text.UTF8Encoding]::new($false, $true))
            $files += $file
        }
        Write-Host "Checking stable identity history: $name"
        & dotnet $MetaCompiler ids merge-check --base $files[0] --ours $files[1] --theirs $files[2] --merged $files[3]
        if ($LASTEXITCODE -ne 0) { throw "Identity merge check failed: $name" }
        $counter++
    }
    Write-Host "Stable identity history checked: $counter manifest(s)."
} finally {
    # 이 스크립트가 생성한 무작위 하위 디렉터리만 제거한다.
    if ([IO.Directory]::Exists($temporary)) { [IO.Directory]::Delete($temporary, $true) }
}

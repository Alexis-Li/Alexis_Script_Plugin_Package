<#
.SYNOPSIS
Builds the installed MtoULiveLink plugin against a Topia Unreal Engine without modifying engine or project source files.

.DESCRIPTION
Stages the project plugin and a writable view of the installed engine in a temporary
directory, builds the two editor modules, verifies their BuildId, and installs only
the Win64 editor binaries back into the project's plugin directory. The command is a
dry run unless -Apply is specified.

.PARAMETER EngineRoot
Unreal Engine root containing the Engine directory.

.PARAMETER ProjectFile
Host .uproject file. Its Plugins/MtoULiveLink directory is the build input and output.

.PARAMETER TempRoot
Optional parent for temporary build directories. Defaults to the system temp directory.

.PARAMETER Apply
Run the build and install the generated plugin binaries. Without this switch, only
validate inputs and print the planned operation.

.PARAMETER Json
Write one stable JSON result to stdout. Build progress is sent to stderr.

.EXAMPLE
pwsh ./tools/build_mtou_topia.ps1 -EngineRoot $env:TOPIA_ENGINE_ROOT -ProjectFile $env:ATHENA_UPROJECT

.EXAMPLE
pwsh ./tools/build_mtou_topia.ps1 -EngineRoot $env:TOPIA_ENGINE_ROOT -ProjectFile $env:ATHENA_UPROJECT -Apply -Json

.NOTES
Exit codes: 0 success, 2 invalid input, 3 build failure, 4 install failure.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EngineRoot,

    [Parameter(Mandatory = $true)]
    [string]$ProjectFile,

    [string]$TempRoot = [IO.Path]::GetTempPath(),

    [switch]$Apply,

    [switch]$Json
)

$ErrorActionPreference = "Stop"
$PluginName = "MtoULiveLink"
$ExpectedModules = @("MtoULiveLink", "MtoULiveLinkEditor")
$ExpectedFiles = @(
    "UnrealEditor-MtoULiveLink.dll",
    "UnrealEditor-MtoULiveLink.pdb",
    "UnrealEditor-MtoULiveLinkEditor.dll",
    "UnrealEditor-MtoULiveLinkEditor.pdb",
    "UnrealEditor.modules"
)

function Write-Diagnostic([string]$Message) {
    if ($Json) {
        [Console]::Error.WriteLine($Message)
    }
    else {
        Write-Host $Message
    }
}

function Resolve-ExistingDirectory([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label directory not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path.TrimEnd("\")
}

function Resolve-ExistingFile([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label file not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Copy-PluginSource([string]$Source, [string]$Destination) {
    $Excluded = @("Binaries", "DerivedDataCache", "Intermediate", "Saved", ".vs", ".git")
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($Item in Get-ChildItem -LiteralPath $Source -Force) {
        if ($Excluded -notcontains $Item.Name) {
            Copy-Item -LiteralPath $Item.FullName -Destination $Destination -Recurse -Force
        }
    }
}

function New-DirectoryJunction([string]$Path, [string]$Target) {
    New-Item -ItemType Junction -Path $Path -Target $Target -Force | Out-Null
    return $Path
}

function Test-FileLocked([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    try {
        $Stream = [IO.File]::Open($Path, "Open", "ReadWrite", "None")
        $Stream.Dispose()
        return $false
    }
    catch {
        return $true
    }
}

function Remove-Junctions([System.Collections.Generic.List[string]]$Paths) {
    for ($Index = $Paths.Count - 1; $Index -ge 0; $Index--) {
        $Path = $Paths[$Index]
        if (Test-Path -LiteralPath $Path) {
            [IO.Directory]::Delete($Path)
        }
    }
}

function Write-Result([hashtable]$Result, [int]$ExitCode) {
    if ($Json) {
        $Result | ConvertTo-Json -Depth 6 -Compress
    }
    elseif ($Result.ok) {
        Write-Host ("{0}: {1}" -f $Result.mode, $Result.message)
        if ($Result.files) {
            $Result.files | ForEach-Object { Write-Host "  $_" }
        }
    }
    else {
        [Console]::Error.WriteLine($Result.error)
        if ($Result.log) {
            [Console]::Error.WriteLine("Build log: $($Result.log)")
        }
        if ($Result.temp_dir) {
            [Console]::Error.WriteLine("Temporary files kept at: $($Result.temp_dir)")
        }
    }
    exit $ExitCode
}

$RunDirectory = $null
$BuildLog = $null
$Junctions = [System.Collections.Generic.List[string]]::new()
$CleanupRunDirectory = $false
$ExitCode = 0
$PhaseExitCode = 2
$Result = $null

try {
    $ResolvedEngineRoot = Resolve-ExistingDirectory $EngineRoot "Engine root"
    $EngineDirectory = Resolve-ExistingDirectory (Join-Path $ResolvedEngineRoot "Engine") "Engine"
    $BuildBat = Resolve-ExistingFile (Join-Path $EngineDirectory "Build\BatchFiles\Build.bat") "Build.bat"
    $EngineManifestPath = Resolve-ExistingFile (Join-Path $EngineDirectory "Binaries\Win64\UnrealEditor.modules") "Engine module manifest"
    $ResolvedProjectFile = Resolve-ExistingFile $ProjectFile "Project"
    if ([IO.Path]::GetExtension($ResolvedProjectFile) -ne ".uproject") {
        throw "Project file must use the .uproject extension: $ResolvedProjectFile"
    }

    $ProjectRoot = Split-Path -Parent $ResolvedProjectFile
    $PluginRoot = Resolve-ExistingDirectory (Join-Path $ProjectRoot "Plugins\$PluginName") "Installed plugin"
    $PluginDescriptor = Resolve-ExistingFile (Join-Path $PluginRoot "$PluginName.uplugin") "Plugin descriptor"
    $ResolvedTempRoot = Resolve-ExistingDirectory $TempRoot "Temporary root"
    $EditorProcesses = @(Get-Process UnrealEditor, UnrealEditor-Cmd -ErrorAction SilentlyContinue)
    $LockedFiles = @(
        $ExpectedFiles |
            Where-Object { $_.EndsWith(".dll", [StringComparison]::OrdinalIgnoreCase) } |
            ForEach-Object { Join-Path $PluginRoot "Binaries\Win64\$_" } |
            Where-Object { Test-FileLocked $_ }
    )

    if (-not $Apply) {
        $Result = @{
            ok = $true
            mode = "dry-run"
            message = "Inputs are valid; rerun with -Apply to build and install the plugin binaries."
            engine_root = $ResolvedEngineRoot
            project_file = $ResolvedProjectFile
            plugin_root = $PluginRoot
            editor_running = ($EditorProcesses.Count -gt 0)
            binaries_locked = ($LockedFiles.Count -gt 0)
            planned_files = $ExpectedFiles
        }
        $CleanupRunDirectory = $true
    }
    else {
        if ($LockedFiles.Count -gt 0) {
            throw "Close the Unreal Editor using this plugin before installing binaries: $($LockedFiles -join ', ')"
        }

        $PhaseExitCode = 3
        $RunDirectory = Join-Path $ResolvedTempRoot ("mtou-topia-" + [guid]::NewGuid().ToString("N"))
        $HostProjectRoot = Join-Path $RunDirectory "HostProject"
        $StagedPluginRoot = Join-Path $HostProjectRoot "Plugins\$PluginName"
        $HostSourceRoot = Join-Path $HostProjectRoot "Source"
        $OverlayEngine = Join-Path $RunDirectory "Topia\Engine"
        $BuildLog = Join-Path $RunDirectory "build.log"

        New-Item -ItemType Directory -Path $HostSourceRoot, $OverlayEngine -Force | Out-Null
        Copy-PluginSource $PluginRoot $StagedPluginRoot
        Get-ChildItem -LiteralPath $StagedPluginRoot -File -Recurse |
            Where-Object IsReadOnly |
            ForEach-Object { $_.IsReadOnly = $false }

        $HostProjectFile = Join-Path $HostProjectRoot "HostProject.uproject"
        [IO.File]::WriteAllText(
            $HostProjectFile,
            '{ "FileVersion": 3, "Plugins": [ { "Name": "MtoULiveLink", "Enabled": true } ] }'
        )
        $TargetFile = Join-Path $HostSourceRoot "MtoUHostEditor.Target.cs"
        [IO.File]::WriteAllText($TargetFile, @'
using UnrealBuildTool;

public class MtoUHostEditorTarget : TargetRules
{
    public MtoUHostEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V6;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
        BuildEnvironment = TargetBuildEnvironment.Shared;
        bBuildAllModules = false;
        ExtraModuleNames.Add("UnrealGame");
    }
}
'@)

        foreach ($Name in @("Binaries", "Build", "Config", "Plugins", "Source")) {
            $SourcePath = Resolve-ExistingDirectory (Join-Path $EngineDirectory $Name) "Engine $Name"
            $Junctions.Add((New-DirectoryJunction (Join-Path $OverlayEngine $Name) $SourcePath))
        }
        foreach ($Name in @("Content", "Documentation", "Extras", "Platforms", "Programs", "Shaders")) {
            $SourcePath = Join-Path $EngineDirectory $Name
            if (Test-Path -LiteralPath $SourcePath -PathType Container) {
                $Junctions.Add((New-DirectoryJunction (Join-Path $OverlayEngine $Name) $SourcePath))
            }
        }

        New-Item -ItemType Directory -Path (Join-Path $OverlayEngine "Saved") -Force | Out-Null
        $OverlayBuildRoot = Join-Path $OverlayEngine "Intermediate\Build"
        $OverlayWin64Root = Join-Path $OverlayBuildRoot "Win64"
        $OverlayUnrealEditorRoot = Join-Path $OverlayWin64Root "UnrealEditor"
        New-Item -ItemType Directory -Path $OverlayUnrealEditorRoot -Force | Out-Null

        $EngineInc = Resolve-ExistingDirectory (Join-Path $EngineDirectory "Intermediate\Build\Win64\UnrealEditor\Inc") "Engine generated headers"
        Copy-Item -LiteralPath $EngineInc -Destination (Join-Path $OverlayUnrealEditorRoot "Inc") -Recurse -Force
        foreach ($RelativePath in @("Intermediate\Build\BuildRules", "Intermediate\ScriptModules")) {
            $SourcePath = Join-Path $EngineDirectory $RelativePath
            if (Test-Path -LiteralPath $SourcePath -PathType Container) {
                $DestinationParent = Split-Path -Parent (Join-Path $OverlayEngine $RelativePath)
                New-Item -ItemType Directory -Path $DestinationParent -Force | Out-Null
                Copy-Item -LiteralPath $SourcePath -Destination (Join-Path $OverlayEngine $RelativePath) -Recurse -Force
            }
        }
        Get-ChildItem -LiteralPath (Join-Path $OverlayEngine "Intermediate") -File -Recurse |
            Where-Object IsReadOnly |
            ForEach-Object { $_.IsReadOnly = $false }

        foreach ($Name in @("x64", "UnrealGame")) {
            $SourcePath = Join-Path $EngineDirectory "Intermediate\Build\Win64\$Name"
            if (Test-Path -LiteralPath $SourcePath -PathType Container) {
                $Junctions.Add((New-DirectoryJunction (Join-Path $OverlayWin64Root $Name) $SourcePath))
            }
        }

        $OverlayBuildBat = Join-Path $OverlayEngine "Build\BatchFiles\Build.bat"
        $StagedDescriptor = Join-Path $StagedPluginRoot "$PluginName.uplugin"
        $BuildArguments = @(
            "MtoUHostEditor",
            "Win64",
            "Development",
            "-Project=$HostProjectFile",
            "-Plugin=$StagedDescriptor",
            "-WaitMutex",
            "-NoHotReloadFromIDE",
            "-UsePrecompiled"
        )

        Write-Diagnostic "Building MtoULiveLink against the staged Topia engine view..."
        if ($Json) {
            & $OverlayBuildBat @BuildArguments *> $BuildLog
        }
        else {
            & $OverlayBuildBat @BuildArguments 2>&1 | Tee-Object -FilePath $BuildLog
        }
        $BuildExitCode = $LASTEXITCODE
        if ($BuildExitCode -ne 0) {
            throw "UnrealBuildTool failed with exit code $BuildExitCode."
        }

        $BuiltBinaries = Join-Path $StagedPluginRoot "Binaries\Win64"
        foreach ($Name in $ExpectedFiles) {
            Resolve-ExistingFile (Join-Path $BuiltBinaries $Name) "Built output" | Out-Null
        }
        $BuiltManifest = Get-Content -LiteralPath (Join-Path $BuiltBinaries "UnrealEditor.modules") -Raw | ConvertFrom-Json
        $EngineManifest = Get-Content -LiteralPath $EngineManifestPath -Raw | ConvertFrom-Json
        if ($BuiltManifest.BuildId -ne $EngineManifest.BuildId) {
            throw "Plugin BuildId does not match the engine BuildId."
        }
        foreach ($Module in $ExpectedModules) {
            if (-not $BuiltManifest.Modules.PSObject.Properties[$Module]) {
                throw "Built module manifest is missing $Module."
            }
        }

        $ProjectManifestPath = Join-Path $ProjectRoot "Binaries\Win64\UnrealEditor.modules"
        if (Test-Path -LiteralPath $ProjectManifestPath -PathType Leaf) {
            $ProjectManifest = Get-Content -LiteralPath $ProjectManifestPath -Raw | ConvertFrom-Json
            if ($ProjectManifest.BuildId -ne $EngineManifest.BuildId) {
                throw "The project's existing BuildId does not match the selected engine."
            }
        }

        $PhaseExitCode = 4
        $InstalledBinaries = Join-Path $PluginRoot "Binaries\Win64"
        New-Item -ItemType Directory -Path $InstalledBinaries -Force | Out-Null
        foreach ($Name in $ExpectedFiles) {
            Copy-Item -LiteralPath (Join-Path $BuiltBinaries $Name) -Destination $InstalledBinaries -Force
        }

        $InstalledFiles = @(
            $ExpectedFiles | ForEach-Object { Join-Path $InstalledBinaries $_ }
        )
        $Result = @{
            ok = $true
            mode = "apply"
            message = "MtoULiveLink was built and installed without modifying engine or project source/configuration files."
            build_id = $BuiltManifest.BuildId
            files = $InstalledFiles
        }
        $CleanupRunDirectory = $true
    }
}
catch {
    $ExitCode = $PhaseExitCode
    $Result = @{
        ok = $false
        mode = if ($Apply) { "apply" } else { "dry-run" }
        error = $_.Exception.Message
        log = if ($BuildLog -and (Test-Path -LiteralPath $BuildLog)) { $BuildLog } else { $null }
        temp_dir = $RunDirectory
    }
}
finally {
    Remove-Junctions $Junctions
    if ($CleanupRunDirectory -and $RunDirectory -and (Test-Path -LiteralPath $RunDirectory)) {
        [IO.Directory]::Delete($RunDirectory, $true)
    }
}

Write-Result $Result $ExitCode

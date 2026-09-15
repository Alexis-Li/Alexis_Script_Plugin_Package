# Uses the Windows PowerShell runtime included with Windows; no pwsh installation needed.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.Application]::EnableVisualStyles()

function Show-Message([string]$Text, [string]$Icon = 'Information') {
    [System.Windows.Forms.MessageBox]::Show($Text, 'MtoU · Topia 插件编译助手', 'OK', $Icon) | Out-Null
}

try {
    $PowerShellExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $Backend = Join-Path $PSScriptRoot 'build_mtou_topia.ps1'
    if (-not (Test-Path -LiteralPath $Backend -PathType Leaf)) {
        throw '缺少 build_mtou_topia.ps1。请将本工具的三个文件放在同一个文件夹中。'
    }
    Show-Message "请先关闭 Unreal Editor，并确认项目 Plugins 文件夹中已安装 MtoULiveLink 源码插件。`n`n接下来选择项目文件和公司 Topia 引擎。工具会使用电脑现有的编译环境。"
    $Picker = New-Object System.Windows.Forms.OpenFileDialog
    try {
        $Picker.Title = '第 1 步：选择要使用 MtoU 的项目'
        $Picker.Filter = 'Unreal 项目 (*.uproject)|*.uproject'
        if ($Picker.ShowDialog() -ne 'OK') { exit 0 }
        $ProjectFile = $Picker.FileName
        $Picker.FileName = ''
        $Picker.Title = '第 2 步：选择公司引擎 Engine\Binaries\Win64\UnrealEditor.exe'
        $Picker.Filter = 'Unreal 编辑器 (UnrealEditor.exe)|UnrealEditor.exe'
        if ($Picker.ShowDialog() -ne 'OK') { exit 0 }
        $EngineRoot = Split-Path (Split-Path (Split-Path (Split-Path $Picker.FileName -Parent) -Parent) -Parent) -Parent
        $ExpectedEditor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
        if ($Picker.FileName -ne $ExpectedEditor) {
            throw '请选择公司引擎 Engine\Binaries\Win64 文件夹中的 UnrealEditor.exe。'
        }
    }
    finally { $Picker.Dispose() }

    $PreviewText = & $PowerShellExe -NoProfile -ExecutionPolicy Bypass -File $Backend -EngineRoot $EngineRoot -ProjectFile $ProjectFile -Json
    $PreviewCode = $LASTEXITCODE
    $Preview = ($PreviewText -join "`n") | ConvertFrom-Json
    if ($PreviewCode -ne 0 -or -not $Preview.ok) { throw $Preview.error }
    if ($Preview.binaries_locked) { throw '插件正在被使用。请关闭使用该插件的 Unreal Editor 后重试。' }
    $Confirmation = "项目：$ProjectFile`n`n引擎：$EngineRoot`n`n安装位置：$($Preview.plugin_root)\Binaries\Win64`n`n将编译并替换此处的插件 DLL、PDB 和模块清单。工程 .uproject、源码和配置不变。`n`n点击「确定」开始；编译期间请保留控制台窗口，等待结果提示。"
    if ([System.Windows.Forms.MessageBox]::Show($Confirmation, '确认编译并安装 MtoU', 'OKCancel', 'Information') -ne 'OK') { exit 0 }

    Write-Host '正在编译 MtoU，请等待。完成后会显示结果；请勿关闭此窗口。'
    # Stream build diagnostics to the console while retaining the backend failure log.
    & $PowerShellExe -NoProfile -ExecutionPolicy Bypass -File $Backend -EngineRoot $EngineRoot -ProjectFile $ProjectFile -Apply
    $BuildCode = $LASTEXITCODE
    if ($BuildCode -ne 0) {
        Show-Message "编译或安装未完成（错误码 $BuildCode）。`n`n请复制控制台中的错误和 Build log 路径给技术同事。修复问题后可重新双击运行。" 'Error'
        exit $BuildCode
    }
    Show-Message 'MtoU 编译并安装成功。现在可以打开项目使用插件。'
}
catch {
    Show-Message $_.Exception.Message 'Error'
    exit 2
}

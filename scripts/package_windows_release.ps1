[CmdletBinding()]
param(
    [string]$SourceDir,
    [string]$BuildDir,
    [string]$StageDir,
    [string]$OutputDir,
    [string]$QtBinDir,
    [string]$OpenCvBinDir,
    [switch]$Clean,
    [switch]$SkipBuild,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-AbsolutePath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$BaseDir
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $BaseDir $Path))
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @()
    )

    Write-Host ("> {0} {1}" -f $FilePath, ($Arguments -join ' '))
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath"
    }
}

function Assert-File {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Description = $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing ${Description}: $Path"
    }
}

function Assert-Directory {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Description = $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Missing ${Description}: $Path"
    }
}

function Get-PeInfo {
    param([Parameter(Mandatory = $true)][string]$Path)

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or [System.BitConverter]::ToUInt16($bytes, 0) -ne 0x5a4d) {
        throw "Not a PE executable: $Path"
    }

    $peOffset = [System.BitConverter]::ToInt32($bytes, 0x3c)
    if ($peOffset -lt 0 -or $peOffset + 24 -gt $bytes.Length -or
        [System.BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
        throw "Invalid PE header: $Path"
    }

    $machine = [System.BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $sectionCount = [System.BitConverter]::ToUInt16($bytes, $peOffset + 6)
    $optionalHeaderSize = [System.BitConverter]::ToUInt16($bytes, $peOffset + 20)
    $optionalHeaderOffset = $peOffset + 24
    $optionalMagic = [System.BitConverter]::ToUInt16($bytes, $optionalHeaderOffset)
    $dataDirectoryOffset = if ($optionalMagic -eq 0x20b) {
        $optionalHeaderOffset + 112
    } elseif ($optionalMagic -eq 0x10b) {
        $optionalHeaderOffset + 96
    } else {
        throw "Unsupported PE optional-header format: $Path"
    }

    $sectionTableOffset = $optionalHeaderOffset + $optionalHeaderSize
    $sections = @()
    for ($index = 0; $index -lt $sectionCount; ++$index) {
        $offset = $sectionTableOffset + $index * 40
        if ($offset + 40 -gt $bytes.Length) {
            throw "Invalid PE section table: $Path"
        }
        $sections += [pscustomobject]@{
            VirtualSize = [System.BitConverter]::ToUInt32($bytes, $offset + 8)
            VirtualAddress = [System.BitConverter]::ToUInt32($bytes, $offset + 12)
            RawSize = [System.BitConverter]::ToUInt32($bytes, $offset + 16)
            RawOffset = [System.BitConverter]::ToUInt32($bytes, $offset + 20)
        }
    }

    function Convert-RvaToOffset {
        param([uint32]$Rva)

        foreach ($section in $sections) {
            $sectionSize = [Math]::Max([uint64]$section.VirtualSize, [uint64]$section.RawSize)
            if ($Rva -ge $section.VirtualAddress -and $Rva -lt ([uint64]$section.VirtualAddress + $sectionSize)) {
                return [int]($section.RawOffset + ($Rva - $section.VirtualAddress))
            }
        }
        throw ("Unable to map PE RVA 0x{0:X}: {1}" -f $Rva, $Path)
    }

    function Read-AsciiString {
        param([int]$Offset)

        $characters = New-Object System.Collections.Generic.List[char]
        while ($Offset -lt $bytes.Length -and $bytes[$Offset] -ne 0) {
            $characters.Add([char]$bytes[$Offset])
            ++$Offset
        }
        return -join $characters
    }

    $imports = @()
    if ($dataDirectoryOffset + 16 -le $bytes.Length) {
        $importDirectoryRva = [System.BitConverter]::ToUInt32($bytes, $dataDirectoryOffset + 8)
        if ($importDirectoryRva -ne 0) {
            $descriptorOffset = Convert-RvaToOffset $importDirectoryRva
            while ($descriptorOffset + 20 -le $bytes.Length) {
                $nameRva = [System.BitConverter]::ToUInt32($bytes, $descriptorOffset + 12)
                $firstThunk = [System.BitConverter]::ToUInt32($bytes, $descriptorOffset + 16)
                if ($nameRva -eq 0 -and $firstThunk -eq 0) {
                    break
                }
                if ($nameRva -ne 0) {
                    $imports += Read-AsciiString (Convert-RvaToOffset $nameRva)
                }
                $descriptorOffset += 20
            }
        }
    }

    return [pscustomobject]@{ Machine = $machine; Imports = $imports }
}

function Copy-OpenCvRuntime {
    param(
        [Parameter(Mandatory = $true)][string]$ExecutablePath,
        [Parameter(Mandatory = $true)][string]$OpenCvBinDirectory,
        [Parameter(Mandatory = $true)][string]$DestinationDirectory
    )

    $peInfo = Get-PeInfo $ExecutablePath
    $openCvNames = @($peInfo.Imports | Where-Object { $_ -match '^opencv.*\.dll$' })
    if ($openCvNames.Count -eq 0) {
        throw 'video_call.exe has no dynamically linked OpenCV DLL dependency.'
    }

    $copied = New-Object System.Collections.Generic.List[string]
    foreach ($name in ($openCvNames | Sort-Object -Unique)) {
        if ($name -match 'd\.dll$') {
            throw "Debug OpenCV dependency detected: $name"
        }
        $source = Join-Path $OpenCvBinDirectory $name
        Assert-File $source "OpenCV Release DLL"
        Copy-Item -LiteralPath $source -Destination (Join-Path $DestinationDirectory $name) -Force
        $copied.Add($name)
    }

    # OpenCV videoio may load a same-version backend DLL at runtime. Keep only
    # matching Release backend files alongside the directly imported DLLs.
    $versionTokens = @($openCvNames | ForEach-Object {
        if ($_ -match '(\d{3,4})') { $Matches[1] }
    } | Sort-Object -Unique)
    foreach ($versionToken in $versionTokens) {
        Get-ChildItem -LiteralPath $OpenCvBinDirectory -File -Filter "opencv_videoio_*$versionToken*.dll" |
            Where-Object { $_.Name -notmatch 'd\.dll$' } |
            ForEach-Object {
                Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $DestinationDirectory $_.Name) -Force
                if (-not $copied.Contains($_.Name)) {
                    $copied.Add($_.Name)
                }
            }
    }

    return @($copied | Sort-Object -Unique)
}

function Find-MsvcRuntimeDirectory {
    $redistRoots = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($env:VCToolsRedistDir)) {
        $redistRoots.Add($env:VCToolsRedistDir)
    }

    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vsWhere) {
        $installationPath = (& $vsWhere -latest -products * -property installationPath)
        if ($null -ne $installationPath -and -not [string]::IsNullOrWhiteSpace(([string]$installationPath).Trim())) {
            $redistRoots.Add((Join-Path ([string]$installationPath).Trim() 'VC\Redist\MSVC'))
        }
    }

    foreach ($root in ($redistRoots | Sort-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) {
            continue
        }
        $directCandidates = Get-ChildItem -LiteralPath $root -Recurse -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\Microsoft\.VC\d+\.CRT$' }
        foreach ($candidate in ($directCandidates | Sort-Object FullName -Descending)) {
            if (@(Get-ChildItem -LiteralPath $candidate.FullName -Filter 'vcruntime140.dll' -File -ErrorAction SilentlyContinue).Count -ne 0) {
                return $candidate.FullName
            }
        }
    }

    throw 'Unable to locate the x64 MSVC Release runtime DLL directory.'
}

function Copy-MsvcRuntime {
    param([Parameter(Mandatory = $true)][string]$DestinationDirectory)

    $runtimeDirectory = Find-MsvcRuntimeDirectory
    $runtimeDlls = @(Get-ChildItem -LiteralPath $runtimeDirectory -Filter '*.dll' -File)
    if ($runtimeDlls.Count -eq 0) {
        throw "No MSVC runtime DLLs found in $runtimeDirectory"
    }
    foreach ($runtimeDll in $runtimeDlls) {
        if ($runtimeDll.Name -match 'd\.dll$') {
            throw "Debug MSVC runtime DLL detected: $($runtimeDll.Name)"
        }
        Copy-Item -LiteralPath $runtimeDll.FullName -Destination (Join-Path $DestinationDirectory $runtimeDll.Name) -Force
    }
    return @($runtimeDlls.Name | Sort-Object)
}

$repositoryDir = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SourceDir)) { $SourceDir = $repositoryDir }
$SourceDir = Get-AbsolutePath $SourceDir $repositoryDir
Assert-Directory $SourceDir 'source directory'

if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = 'build-release-package-x64' }
if ([string]::IsNullOrWhiteSpace($OutputDir)) { $OutputDir = 'dist' }
$BuildDir = Get-AbsolutePath $BuildDir $SourceDir
$OutputDir = Get-AbsolutePath $OutputDir $SourceDir
if ([string]::IsNullOrWhiteSpace($StageDir)) { $StageDir = Join-Path $OutputDir 'video_call-win64' }
$StageDir = Get-AbsolutePath $StageDir $SourceDir

if ([string]::IsNullOrWhiteSpace($QtBinDir)) {
    $QtBinDir = 'C:\Qt\6.11.1\msvc2022_64\bin'
}
if ([string]::IsNullOrWhiteSpace($OpenCvBinDir)) {
    $OpenCvBinDir = 'C:\Opencv\opencv\build\x64\vc16\bin'
}
$QtBinDir = Get-AbsolutePath $QtBinDir $SourceDir
$OpenCvBinDir = Get-AbsolutePath $OpenCvBinDir $SourceDir

$cmake = Get-Command cmake.exe -ErrorAction Stop
$ctest = Get-Command ctest.exe -ErrorAction Stop
$python = Get-Command python.exe -ErrorAction Stop
$windeployqt = Join-Path $QtBinDir 'windeployqt.exe'
Assert-File $windeployqt 'Qt windeployqt.exe'
Assert-Directory $OpenCvBinDir 'OpenCV bin directory'

if ($Clean) {
    foreach ($path in @($BuildDir, $OutputDir)) {
        if (Test-Path -LiteralPath $path) {
            Write-Host "Removing $path"
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }
}

if (-not $SkipBuild) {
    $qtPrefix = Split-Path -Parent $QtBinDir
    $openCvRoot = Split-Path -Parent $OpenCvBinDir
    $openCvConfig = Join-Path $openCvRoot 'lib'
    Assert-Directory $openCvConfig 'OpenCV CMake package directory'

    $configureArguments = @(
        '-S', $SourceDir,
        '-B', $BuildDir,
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_PREFIX_PATH=$qtPrefix",
        "-DOpenCV_DIR=$openCvConfig"
    )
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs2022Path = ''
    if (Test-Path -LiteralPath $vsWhere) {
        $vsWhereOutput = & $vsWhere -latest -products * -version '[17.0,18.0)' -property installationPath
        if ($null -ne $vsWhereOutput) {
            $vs2022Path = ([string]$vsWhereOutput).Trim()
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($vs2022Path)) {
        $configureArguments += @('-G', 'Visual Studio 17 2022', '-A', 'x64')
    } else {
        # A newer MSVC toolset can be ABI-compatible with the Qt MSVC package,
        # but it must be loaded explicitly so NMake/JOM receives the x64 SDK,
        # compiler and linker environment.
        $compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
        $compilerPath = if ($null -ne $compiler) { $compiler.Source } else { '' }
        if ([string]::IsNullOrWhiteSpace($compilerPath) -and -not [string]::IsNullOrWhiteSpace($env:VCToolsInstallDir)) {
            $compilerCandidate = Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64\cl.exe'
            if (Test-Path -LiteralPath $compilerCandidate) {
                $compilerPath = $compilerCandidate
                $env:Path = (Split-Path -Parent $compilerCandidate) + ';' + $env:Path
            }
        }
        $jom = Get-Command jom.exe -ErrorAction SilentlyContinue
        if ($null -eq $jom) {
            $qtRoot = Split-Path -Parent (Split-Path -Parent $QtBinDir)
            $jomCandidate = Join-Path $qtRoot 'Tools\QtCreator\bin\jom\jom.exe'
            if (Test-Path -LiteralPath $jomCandidate) {
                $jom = Get-Item -LiteralPath $jomCandidate
            }
        }
        if ([string]::IsNullOrWhiteSpace($compilerPath) -or $null -eq $jom) {
            throw 'Visual Studio 2022 was not found. Start a 64-bit MSVC Developer Command Prompt before packaging with the NMake/JOM fallback.'
        }
        $jomPath = if ($jom.PSObject.Properties.Name -contains 'Source') { $jom.Source } else { $jom.FullName }
        $sdkBinDirectory = if (-not [string]::IsNullOrWhiteSpace($env:WindowsSdkVerBinPath)) {
            Join-Path $env:WindowsSdkVerBinPath 'x64'
        } else {
            ''
        }
        $resourceCompiler = Join-Path $sdkBinDirectory 'rc.exe'
        $manifestTool = Join-Path $sdkBinDirectory 'mt.exe'
        Assert-File $resourceCompiler 'Windows SDK rc.exe for x64 Release builds'
        Assert-File $manifestTool 'Windows SDK mt.exe for x64 Release builds'
        $resourceCompilerForCmake = $resourceCompiler.Replace('\', '/')
        $manifestToolForCmake = $manifestTool.Replace('\', '/')
        $configureArguments += @(
            '-G', 'NMake Makefiles JOM',
            "-DCMAKE_MAKE_PROGRAM=$jomPath",
            "-DCMAKE_C_COMPILER=$compilerPath",
            "-DCMAKE_CXX_COMPILER=$compilerPath",
            "-DCMAKE_RC_COMPILER=$resourceCompilerForCmake",
            "-DCMAKE_MT=$manifestToolForCmake"
        )
    }

    Invoke-Checked $cmake.Source $configureArguments
    Invoke-Checked $cmake.Source @('--build', $BuildDir, '--config', 'Release', '--parallel')
}

Assert-Directory $BuildDir 'Release build directory'

if (-not $SkipTests) {
    $originalPath = $env:Path
    try {
        # Tests run from the build tree, before windeployqt has created a
        # self-contained staging directory.
        $testRuntimeDirectories = @($QtBinDir, $OpenCvBinDir)
        $testRuntimeDirectories += Find-MsvcRuntimeDirectory
        $env:Path = ($testRuntimeDirectories -join ';') + ';' + $originalPath
        Invoke-Checked $ctest.Source @('--test-dir', $BuildDir, '-C', 'Release', '--output-on-failure')
    } finally {
        $env:Path = $originalPath
    }
    Invoke-Checked $python.Source @('-m', 'py_compile', (Join-Path $SourceDir 'tools\udp_test_receiver.py'))
    Invoke-Checked $python.Source @('-m', 'py_compile', (Join-Path $SourceDir 'tools\udp_jpeg_receiver.py'))
    Invoke-Checked $python.Source @((Join-Path $SourceDir 'tools\udp_test_receiver.py'), '--self-test')
    Invoke-Checked $python.Source @((Join-Path $SourceDir 'tools\udp_jpeg_receiver.py'), '--self-test')
}

if (Test-Path -LiteralPath $StageDir) {
    Remove-Item -LiteralPath $StageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $StageDir -Force | Out-Null
Invoke-Checked $cmake.Source @('--install', $BuildDir, '--config', 'Release', '--prefix', $StageDir)

$appExe = Join-Path $StageDir 'video_call.exe'
Assert-File $appExe 'installed video_call.exe'
$appPeInfo = Get-PeInfo $appExe
if ($appPeInfo.Machine -ne 0x8664) {
    throw ('video_call.exe is not x64 (PE machine 0x{0:X}).' -f $appPeInfo.Machine)
}

# Chinese UI text is compiled into the application, so excluding Qt translation
# catalogs does not remove the program's Chinese interface.
Invoke-Checked $windeployqt @('--release', '--compiler-runtime', '--no-translations', $appExe)
$openCvDlls = Copy-OpenCvRuntime $appExe $OpenCvBinDir $StageDir
$msvcRuntimeDlls = Copy-MsvcRuntime $StageDir

$portableReadme = @"
video_call Windows 便携版
========================

1. 双击 video_call.exe 启动。请不要删除同目录 DLL 或插件子目录。
2. 首次运行时，如 Windows 防火墙提示，请允许“专用网络”访问。
3. 视频默认使用 UDP 5000；音频默认使用 UDP 5002。
4. 两台电脑分别填写对方的局域网 IPv4 地址。
5. 先应用视频网络设置，再应用音频设置。
6. 启动摄像头后，点击“开始发送视频”。
7. 建议佩戴耳机后再开始双向音频；当前没有回声消除。
8. 程序不检测对端是否在线，不发送 ACK，也不重传数据。
9. 没有画面时，请检查摄像头是否被其他程序占用，以及 Windows 防火墙的 UDP 放行规则。
10. 没有声音时，请检查 Windows 默认输入、输出设备，以及是否正确应用音频设置。
"@
$utf8Bom = New-Object System.Text.UTF8Encoding($true)
[System.IO.File]::WriteAllText((Join-Path $StageDir 'README_PORTABLE.txt'), $portableReadme, $utf8Bom)

$requiredFiles = @(
    'video_call.exe',
    'Qt6Core.dll',
    'Qt6Gui.dll',
    'Qt6Widgets.dll',
    'Qt6Network.dll',
    'Qt6Multimedia.dll',
    'README_PORTABLE.txt',
    'platforms\qwindows.dll'
)
foreach ($relativePath in $requiredFiles) {
    Assert-File (Join-Path $StageDir $relativePath) "required staging file"
}
if ((Get-Item -LiteralPath $appExe).Length -le 0) {
    throw 'video_call.exe is empty.'
}
if ($openCvDlls.Count -eq 0) {
    throw 'No OpenCV Release DLL was deployed.'
}
foreach ($runtimeDll in @('vcruntime140.dll', 'msvcp140.dll')) {
    Assert-File (Join-Path $StageDir $runtimeDll) 'MSVC runtime DLL'
}

$stagingFiles = Get-ChildItem -LiteralPath $StageDir -Recurse -File
$debugDllNames = @('Qt6Cored.dll', 'Qt6Guid.dll', 'Qt6Widgetsd.dll', 'Qt6Networkd.dll', 'Qt6Multimediad.dll')
$forbiddenFiles = @($stagingFiles | Where-Object {
    $debugDllNames -contains $_.Name -or
    $_.Name -match '^opencv.*d\.dll$' -or
    $_.Extension -in @('.obj', '.ilk', '.pdb') -or
    $_.Name -eq 'CMakeCache.txt'
})
if ($forbiddenFiles.Count -ne 0) {
    throw ('Forbidden Debug or build artifacts in staging: ' + (($forbiddenFiles | Select-Object -ExpandProperty FullName) -join '; '))
}

$git = Get-Command git.exe -ErrorAction Stop
$shortSha = (& $git.Source -C $SourceDir rev-parse --short HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($shortSha)) {
    throw 'Unable to determine the current Git short SHA.'
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$zipPath = Join-Path $OutputDir ("video_call-win64-release-{0}.zip" -f $shortSha)
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -LiteralPath $StageDir -DestinationPath $zipPath -Force
Assert-File $zipPath 'portable ZIP'

$zipItem = Get-Item -LiteralPath $zipPath
$zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
Write-Host ''
Write-Host "Portable package: $zipPath"
Write-Host "ZIP bytes: $($zipItem.Length)"
Write-Host "ZIP SHA-256: $zipHash"
Write-Host ('OpenCV Release DLLs: ' + ($openCvDlls -join ', '))
Write-Host ('MSVC Runtime DLLs: ' + ($msvcRuntimeDlls -join ', '))

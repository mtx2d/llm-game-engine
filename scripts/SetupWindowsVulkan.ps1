# Native Windows Vulkan CI dependencies. This does not establish platform support:
# AsterRendererTests must subsequently render and check pixels with validation.
# SDK installer usage: https://vulkan.lunarg.com/doc/view/1.4.328.1/windows/getting_started.html
# SwiftShader: https://github.com/google/swiftshader/tree/0f87bb2d3742c2992fd1a7dcb2f051eebf79299c
[CmdletBinding()]
param(
    [string] $BuildRoot = (Join-Path $PSScriptRoot '../build/windows-vulkan'),
    [int] $Parallel = 3
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $IsWindows) { throw 'SetupWindowsVulkan.ps1 requires native Windows PowerShell 7.' }
if (-not [Environment]::Is64BitProcess) { throw 'SetupWindowsVulkan.ps1 requires a 64-bit PowerShell process.' }
if ($Parallel -lt 1 -or $Parallel -gt 16) { throw 'Parallel must be in [1,16].' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
try
{
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    $isElevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
finally { $identity.Dispose() }
if ($isElevated -and ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted'))
{
    throw 'Run this setup in a non-elevated shell. Automatic Vulkan registry registration is limited to ephemeral GitHub-hosted runners.'
}
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildDirectory = [IO.Path]::GetFullPath($BuildRoot)
$allowedPrefix = [IO.Path]::GetFullPath((Join-Path $repositoryRoot 'build')) + [IO.Path]::DirectorySeparatorChar
if (-not $buildDirectory.StartsWith($allowedPrefix, [StringComparison]::OrdinalIgnoreCase))
{
    throw 'Downloaded dependencies must remain in this repository build directory.'
}
$archiveDirectory = Join-Path $buildDirectory 'archives'
$evidenceDirectory = Join-Path $buildDirectory 'evidence'
New-Item -ItemType Directory -Force $archiveDirectory, $evidenceDirectory | Out-Null

function Get-VerifiedArchive([string] $Uri, [string] $Destination, [string] $Sha256)
{
    if (-not (Test-Path -LiteralPath $Destination))
    {
        Write-Host "Downloading $Uri"
        $partial = "$Destination.partial"
        try
        {
            Invoke-WebRequest -Uri $Uri -OutFile $partial
            $actual = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash
            if ($actual -ne $Sha256) { throw "SHA256 mismatch for $Uri`: expected $Sha256, got $actual" }
            Move-Item -LiteralPath $partial -Destination $Destination
        }
        finally
        {
            if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial }
        }
    }
    $actual = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($actual -ne $Sha256) { throw "Cached archive hash mismatch: $Destination" }
}

function Invoke-Checked([string] $Program, [string[]] $Arguments)
{
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with code $LASTEXITCODE" }
}

function Set-ProcessAndActionsEnvironment([string] $Name, [string] $Value)
{
    [Environment]::SetEnvironmentVariable($Name, $Value, 'Process')
    if ($env:GITHUB_ENV) { "$Name=$Value" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append }
}

# Hashes are published in the official LunarG download responses (the `sha`
# header) and SDK download page. Every downloaded byte is verified before use.
$sdkVersion = '1.4.328.1'
$sdkHash = 'a8675df6d538079c2a719a9373994948091db785b48f142e024254e76348d16c'
$runtimeHash = '293efbb2c11e60429df7770f40e3c5c811a7fa42af81de0cc41e8718ddb2b373'
$swiftRevision = '0f87bb2d3742c2992fd1a7dcb2f051eebf79299c'
$swiftHash = 'a477c90a3e83dda8ace49904d9d55f819af324d5c14a1af58d8f838727fdb3c1'
$sdkArchive = Join-Path $archiveDirectory "vulkansdk-windows-X64-$sdkVersion.exe"
$runtimeArchive = Join-Path $archiveDirectory "VulkanRT-X64-$sdkVersion-Components.zip"
$swiftArchive = Join-Path $archiveDirectory "swiftshader-$swiftRevision.tar.gz"
Get-VerifiedArchive "https://vulkan.lunarg.com/sdk/download/$sdkVersion/windows/vulkansdk-windows-X64-$sdkVersion.exe" $sdkArchive $sdkHash
Get-VerifiedArchive "https://vulkan.lunarg.com/sdk/download/$sdkVersion/windows/VulkanRT-X64-$sdkVersion-Components.zip" $runtimeArchive $runtimeHash
Get-VerifiedArchive "https://codeload.github.com/google/swiftshader/tar.gz/$swiftRevision" $swiftArchive $swiftHash

$sdkDirectory = Join-Path $buildDirectory "VulkanSDK-$sdkVersion"
$sdkMarker = Join-Path $sdkDirectory '.aster-installed'
if (-not (Test-Path -LiteralPath $sdkMarker))
{
    # copy_only keeps registry entries, global environment, and system drivers unchanged.
    $installer = Start-Process -FilePath $sdkArchive -Wait -PassThru -ArgumentList @(
        '--root', "`"$sdkDirectory`"", '--accept-licenses', '--default-answer',
        '--confirm-command', 'install', 'copy_only=1')
    if ($installer.ExitCode -ne 0) { throw "Vulkan SDK installation failed: $($installer.ExitCode)" }
    if (-not (Test-Path (Join-Path $sdkDirectory 'Lib/vulkan-1.lib')) -or
        -not (Test-Path (Join-Path $sdkDirectory 'Bin/glslangValidator.exe')) -or
        -not (Test-Path (Join-Path $sdkDirectory 'Bin/VkLayer_khronos_validation.dll')))
    {
        throw 'Vulkan SDK installation omitted its import library, compiler, or validation layer.'
    }
    Set-Content -LiteralPath $sdkMarker -Value $sdkHash
}

$runtimeDirectory = Join-Path $buildDirectory "VulkanRuntime-$sdkVersion"
if (-not (Test-Path -LiteralPath $runtimeDirectory))
{
    Expand-Archive -LiteralPath $runtimeArchive -DestinationPath $runtimeDirectory
}
$loaders = @(Get-ChildItem -LiteralPath $runtimeDirectory -Recurse -File -Filter 'vulkan-1.dll' |
    Where-Object { $_.DirectoryName -match '[\\/](x64|amd64|Bin-X64)([\\/]|$)' })
if ($loaders.Count -ne 1) { throw "Expected exactly one x64 Vulkan loader in $runtimeDirectory" }
$loaderDirectory = $loaders[0].DirectoryName

$swiftSource = Join-Path $buildDirectory "swiftshader-$swiftRevision"
if (-not (Test-Path (Join-Path $swiftSource 'CMakeLists.txt')))
{
    # The verified archive includes the selected bundled LLVM/SPIR-V dependencies.
    # It has no .git directory, so upstream cannot download an unpinned commit hook.
    Invoke-Checked 'tar' @('-xzf', $swiftArchive, '-C', $buildDirectory)
}
$swiftBuild = Join-Path $buildDirectory 'swiftshader-build'
Invoke-Checked 'cmake' @('-S', $swiftSource, '-B', $swiftBuild,
    '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'host=x64',
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
    '-DSWIFTSHADER_BUILD_TESTS=OFF', '-DSWIFTSHADER_BUILD_BENCHMARKS=OFF',
    '-DSWIFTSHADER_BUILD_PVR=OFF', '-DSWIFTSHADER_WARNINGS_AS_ERRORS=OFF',
    '-DSWIFTSHADER_ENABLE_VULKAN_DEBUGGER=OFF', '-DREACTOR_BACKEND=LLVM')
Invoke-Checked 'cmake' @('--build', $swiftBuild, '--config', 'Release',
    '--target', 'vk_swiftshader', '--parallel', "$Parallel")
$icdManifest = Join-Path $swiftBuild 'Windows/vk_swiftshader_icd.json'
if (-not (Test-Path -LiteralPath $icdManifest)) { throw 'SwiftShader did not produce its Windows ICD manifest.' }
$icd = Get-Content -LiteralPath $icdManifest -Raw | ConvertFrom-Json
$icdLibrary = [IO.Path]::GetFullPath((Join-Path (Split-Path $icdManifest) $icd.ICD.library_path))
if (-not (Test-Path -LiteralPath $icdLibrary)) { throw "SwiftShader ICD library is missing: $icdLibrary" }

Get-CimInstance Win32_VideoController | Select-Object Name, AdapterCompatibility, DriverVersion, VideoProcessor |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidenceDirectory 'WindowsAdapters.json')
Set-ProcessAndActionsEnvironment 'VULKAN_SDK' $sdkDirectory
Set-ProcessAndActionsEnvironment 'VK_LAYER_PATH' (Join-Path $sdkDirectory 'Bin')
Set-ProcessAndActionsEnvironment 'VK_DRIVER_FILES' $icdManifest
Set-ProcessAndActionsEnvironment 'VK_ICD_FILENAMES' $icdManifest
Set-ProcessAndActionsEnvironment 'ASTER_VULKAN_LOADER' $loaders[0].FullName
Set-ProcessAndActionsEnvironment 'ASTER_VULKAN_EVIDENCE' $evidenceDirectory
# Elevated applications ignore driver/layer environment overrides by design.
# Use the loader's documented machine registration only on the disposable hosted
# runner. Add our exact manifest values without replacing any other entries.
# https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#exception-for-elevated-privileges
$registrations = @()
if ($isElevated)
{
    $registrations = @(
        @{ Key = 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'; Manifest = $icdManifest },
        @{ Key = 'HKLM:\SOFTWARE\Khronos\Vulkan\ExplicitLayers'; Manifest = (Join-Path $sdkDirectory 'Bin/VkLayer_khronos_validation.json') }
    )
    foreach ($registration in $registrations)
    {
        if (-not (Test-Path -LiteralPath $registration.Manifest -PathType Leaf))
        {
            throw "Vulkan registry manifest is missing: $($registration.Manifest)"
        }
        if (-not (Test-Path -LiteralPath $registration.Key))
        {
            New-Item -Path $registration.Key -Force | Out-Null
        }
        New-ItemProperty -LiteralPath $registration.Key -Name $registration.Manifest -PropertyType DWord -Value 0 -Force | Out-Null
        Write-Host "Registered Vulkan manifest $($registration.Manifest) at $($registration.Key)"
    }
}
# Never add SwiftShader's directory to PATH: it contains a drop-in vulkan-1.dll
# that bypasses the Vulkan loader and would prevent validation layer loading.
foreach ($directory in @((Join-Path $sdkDirectory 'Bin'), $loaderDirectory))
{
    $env:PATH = "$directory;$env:PATH"
    if ($env:GITHUB_PATH) { $directory | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append }
}
$informationTools = @('vulkaninfoSDK.exe', 'vulkaninfo.exe') | ForEach-Object { Join-Path $sdkDirectory "Bin/$_" } |
    Where-Object { Test-Path -LiteralPath $_ }
if (@($informationTools).Count -eq 0) { throw 'The SDK contains no Vulkan device information executable.' }
$informationTool = @($informationTools)[0]
# Select the verified loader beside the executable, ahead of a runner image's
# system loader. Keep the same application-local arrangement used by Aster.
Copy-Item -LiteralPath $loaders[0].FullName -Destination (Join-Path $sdkDirectory 'Bin/vulkan-1.dll') -Force
@{
    SdkVersion = $sdkVersion; SdkSha256 = $sdkHash; RuntimeSha256 = $runtimeHash
    SwiftShaderRevision = $swiftRevision; SwiftShaderSha256 = $swiftHash
    IcdManifest = $icdManifest; IcdLibrary = $icdLibrary; Loader = $loaders[0].FullName
    Elevated = $isElevated; RegistryEntries = $registrations
    Note = 'Software Vulkan on native Windows; renderer execution and validation are separate required gates.'
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidenceDirectory 'Dependencies.json')
$previousLoaderDebug = [Environment]::GetEnvironmentVariable('VK_LOADER_DEBUG', 'Process')
try
{
    $env:VK_LOADER_DEBUG = 'error,warn,driver'
    & $informationTool '--summary' 2>&1 | Tee-Object -FilePath (Join-Path $evidenceDirectory 'VulkanInfo.txt')
    $enumerationExitCode = $LASTEXITCODE
}
finally { [Environment]::SetEnvironmentVariable('VK_LOADER_DEBUG', $previousLoaderDebug, 'Process') }
if ($enumerationExitCode -ne 0) { throw "Vulkan device enumeration failed: $enumerationExitCode" }
Write-Host 'Windows Vulkan dependencies are prepared. Run the renderer tests with validation before claiming support.'

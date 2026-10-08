[CmdletBinding()]
param(
    [string]$GameFolder,
    [string]$MemoryBackingDirectory,
    [ValidateRange(320,7680)][int]$ScreenWidth = 2560,
    [ValidateRange(240,4320)][int]$ScreenHeight = 1440,
    [switch]$CheckOnly
)

# Portable experimental profile; no game or user files are bundled.
# GPL-2.0: this wrapper may be modified; the emulator can also run directly.
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This package requires Windows x64.' }
# Use this host's own modules even when launched from a different PS edition.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Management\Microsoft.PowerShell.Management.psd1') -ErrorAction Stop
$packageRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$emulatorPath = Join-Path $packageRoot 'kyty_emulator.exe'
$expectedExeHash = '177D7FDB6AD6A351E472A2A5A8E5D012754D74DB61433CFB3B7E360EA56DC318'
if ((Get-FileHash -LiteralPath $emulatorPath -Algorithm SHA256).Hash -ine $expectedExeHash) {
    throw 'The emulator differs from this experimental package. Use its matching starter or run your replacement directly.'
}

function Quote-WindowsArgument([string]$Value) {
    # Microsoft CRT argument quoting. Never interpolate a game path into a shell.
    if ($Value.IndexOf([char]0) -ge 0) { throw 'Arguments cannot contain NUL.' }
    $result = New-Object System.Text.StringBuilder
    [void]$result.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') {
            [void]$result.Append(('\' * (2 * $slashes + 1)))
            [void]$result.Append('"')
        } else {
            [void]$result.Append(('\' * $slashes))
            [void]$result.Append($character)
        }
        $slashes = 0
    }
    [void]$result.Append(('\' * (2 * $slashes)))
    [void]$result.Append('"')
    return $result.ToString()
}

function Invoke-ConsoleChild([Diagnostics.ProcessStartInfo]$StartInfo) {
    # Drain both pipes concurrently; all output goes to this console, not files.
    if (-not ('KytyPortableConsoleChild' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Threading.Tasks;
public static class KytyPortableConsoleChild {
    public static int Run(ProcessStartInfo info) {
        info.RedirectStandardOutput = true;
        info.RedirectStandardError = true;
        using (var child = new Process()) {
            child.StartInfo = info;
            child.Start();
            var output = Task.Factory.StartNew(() => {
                string line; while ((line = child.StandardOutput.ReadLine()) != null) Console.Out.WriteLine(line);
            });
            var error = Task.Factory.StartNew(() => {
                string line; while ((line = child.StandardError.ReadLine()) != null) Console.Error.WriteLine(line);
            });
            child.WaitForExit();
            Task.WaitAll(output, error);
            return child.ExitCode;
        }
    }
}
'@
    }
    return [KytyPortableConsoleChild]::Run($StartInfo)
}

if ($CheckOnly) {
    if ($GameFolder) {
        $candidateGame = Join-Path ([IO.Path]::GetFullPath($GameFolder)) 'eboot.bin'
        if (-not (Test-Path -LiteralPath $candidateGame -PathType Leaf)) { throw 'GameFolder must contain eboot.bin.' }
    }
    Write-Output 'Package identity checked. No game, memory backing, or GPU initialized.'
    Write-Output 'Profile: experimental 2x EQAA + resolve; structured terminal external-call probe; diagnostics off.'
    Write-Output 'A game folder containing eboot.bin is required when starting. The default backing folder is local to this extracted package.'
    return
}
if (Get-Process -Name kyty_emulator -ErrorAction SilentlyContinue) { throw 'Close the other Kyty emulator before starting this copy.' }

if (-not $GameFolder) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Select your own extracted GT7 application folder containing eboot.bin. This package does not include a game.'
    $dialog.ShowNewFolderButton = $false
    try {
        if ($dialog.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { return }
        $GameFolder = $dialog.SelectedPath
    } finally { $dialog.Dispose() }
}
$gameRoot = [IO.Path]::GetFullPath($GameFolder)
$gamePath = Join-Path $gameRoot 'eboot.bin'
if (-not (Test-Path -LiteralPath $gamePath -PathType Leaf)) { throw 'Choose an application folder containing eboot.bin.' }
if (-not $MemoryBackingDirectory) { $MemoryBackingDirectory = Join-Path $packageRoot '_MemoryBacking' }
$backingRoot = [IO.Path]::GetFullPath($MemoryBackingDirectory)
$driveRoot = [IO.Path]::GetPathRoot($backingRoot)
$drive = New-Object IO.DriveInfo($driveRoot)
if ($drive.DriveType -ne [IO.DriveType]::Fixed -or $drive.DriveFormat -ne 'NTFS') {
    throw 'The backing folder must be on a local fixed NTFS volume.'
}
if ($drive.AvailableFreeSpace -lt 14495514624L) { throw 'The backing volume needs at least 14 GB of free space.' }
if ($backingRoot -eq $gameRoot -or $backingRoot.StartsWith($gameRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Keep memory backing separate from the game installation.'
}
$runId = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N')
$backingRun = Join-Path $backingRoot $runId
$runtimeRoot = Join-Path $packageRoot '_Runtime'
$tempRoot = Join-Path $runtimeRoot 'Temp'
foreach ($directory in @($backingRoot,$backingRun,$runtimeRoot,$tempRoot)) {
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory | Out-Null }
    $item = Get-Item -LiteralPath $directory
    if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Runtime directories must be regular writable directories.' }
}

$arguments = @(
    '--game', $gamePath,
    '--screen-width', [string]$ScreenWidth, '--screen-height', [string]$ScreenHeight,
    '--present-mode','Mailbox', '--vblank-frequency','60',
    '--shader-optimization-type','Performance', '--shader-log-direction','Silent',
    '--graphics-debug-dump','false', '--command-buffer-dump','false',
    '--printf-direction','Silent', '--spirv-debug-printf','false',
    '--vulkan-validation','false', '--shader-validation','false',
    '--readback-linear-images','false'
)
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $emulatorPath
$start.WorkingDirectory = $runtimeRoot
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.Arguments = (@($arguments | ForEach-Object { Quote-WindowsArgument $_ }) -join ' ')
foreach ($key in @($start.EnvironmentVariables.Keys)) {
    if ([string]$key -like 'KYTY_*') { $start.EnvironmentVariables.Remove([string]$key) }
}
$start.EnvironmentVariables['KYTY_EXPERIMENTAL_EQAA_2X'] = '1'
$start.EnvironmentVariables['KYTY_EXPERIMENTAL_EQAA_2X_RESOLVE'] = '1'
$start.EnvironmentVariables['KYTY_PROBE_EXTERNAL_CALL_TARGET'] = '1'
$start.EnvironmentVariables['KYTY_PROBE_EXTERNAL_STRUCTURED'] = '1'
$start.EnvironmentVariables['KYTY_DIRECT_MEMORY_BACKING_DIR'] = $backingRun
$start.EnvironmentVariables['TEMP'] = $tempRoot
$start.EnvironmentVariables['TMP'] = $tempRoot
Write-Output 'Starting the experimental profile. Graphics may be incomplete. A race load can stop at the deliberate external-call probe.'
$exitCode = Invoke-ConsoleChild $start
Write-Output ('Emulator exit code: ' + $exitCode)
exit $exitCode

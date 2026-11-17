# Where this PC keeps what the game needs (the PowerShell side of tools\port_paths.py).
#   . "$PSScriptRoot\port_paths.ps1";  Get-PortPath rpcs3
# Order: environment variable (PORT_RPCS3 ...), then config.local.json in the project
# folder (written by Setup.cmd, not in git).
. (Join-Path $PSScriptRoot "safe_paths.ps1")   # Get-BuildRoot, New-Junction, Invoke-Cmd (any folder name)
function Get-PortPath([string]$Key, [switch]$Optional) {
    $envName = @{ rpcs3 = "PORT_RPCS3"; vcvars = "PORT_VCVARS"; ninja = "PORT_NINJA"; llvm = "PORT_LLVM" }[$Key]
    $v = if ($envName) { [Environment]::GetEnvironmentVariable($envName) } else { $null }
    if (-not $v) {
        $cfg = Join-Path (Split-Path -Parent $PSScriptRoot) "config.local.json"
        if (Test-Path -LiteralPath $cfg) { $v = (Get-Content -LiteralPath $cfg -Raw | ConvertFrom-Json).$Key }
    }
    if (-not $v -and -not $Optional) {
        throw "no '$Key' path set: run Setup.cmd (or set $envName, or `"$Key`" in config.local.json)"
    }
    $v
}

# --- where RPCS3 keeps dev_hdd0 and dev_flash (GitHub #50) -----------------------------------------
# RPCS3 reads config\vfs.yml: "/dev_hdd0/: <path>" and "/dev_flash/: <path>", each possibly on another
# drive, with "$(EmulatorDir)" standing for RPCS3's data folder (the folder of rpcs3.exe when portable,
# else %APPDATA%\rpcs3). An empty value means the default (<data folder>\dev_hdd0). Players who moved
# their game data to another drive have a vfs.yml like that; looking only next to rpcs3.exe missed it.
function Get-Rpcs3DataDirs([string]$Rpcs3) {
    $c = New-Object System.Collections.ArrayList
    foreach ($d in @($Rpcs3, $(if ($env:APPDATA) { Join-Path $env:APPDATA "rpcs3" }), $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA "rpcs3" }))) {
        if ($d -and (Test-Path -LiteralPath $d -PathType Container) -and ($c -notcontains $d)) { [void]$c.Add($d) }
    }
    , $c.ToArray()
}

# The vfs.yml entry for "dev_hdd0" / "dev_flash" of one data folder as a full path, or $null (none / empty).
function Get-Rpcs3VfsPath([string]$DataDir, [string]$Name) {
    $yml = Join-Path $DataDir "config\vfs.yml"
    if (-not (Test-Path -LiteralPath $yml -PathType Leaf)) { return $null }
    foreach ($line in Get-Content -LiteralPath $yml -ErrorAction SilentlyContinue) {
        if ($line -notmatch '^\s*"?/?(\w+)/?"?\s*:\s*(.*?)\s*$' -or $Matches[1] -ne $Name) { continue }
        $v = $Matches[2].Trim().Trim('"', "'")
        if (-not $v) { return $null }
        $emu = $DataDir.TrimEnd('\', '/') + "\"
        if ($v.StartsWith('$(EmulatorDir)', [StringComparison]::OrdinalIgnoreCase)) { $v = $emu + $v.Substring(14).TrimStart('/', '\') }
        $v = $v.Replace('/', '\')
        if (-not [IO.Path]::IsPathRooted($v)) { $v = $emu + $v }
        return $v.TrimEnd('\')
    }
    $null
}

# RPCS3's dev_hdd0 / dev_flash. $Probe is a path inside it that proves it is the right one (the game's
# update folder, the firmware's liblv2.sprx): of the places RPCS3 may keep it (vfs.yml, the default
# beside rpcs3.exe or in %APPDATA%\rpcs3) the first holding it; none holds it = the first place.
function Find-Rpcs3Dir([string]$Rpcs3, [string]$Name, [string]$Probe) {
    $cands = New-Object System.Collections.ArrayList
    foreach ($d in (Get-Rpcs3DataDirs $Rpcs3)) {
        $v = Get-Rpcs3VfsPath $d $Name
        if ($v -and ($cands -notcontains $v)) { [void]$cands.Add($v) }
        $def = [IO.Path]::Combine($d, $Name)
        if ($cands -notcontains $def) { [void]$cands.Add($def) }
    }
    if ($cands.Count -eq 0) { return (Join-Path $Rpcs3 $Name) }
    if ($Probe) { foreach ($c in $cands) { if (Test-Path -LiteralPath ([IO.Path]::Combine($c, $Probe))) { return $c } } }
    $cands[0]
}

# The PS3 firmware files the game reads while it runs (fonts, system data): Setup's copy in
# game\dev_flash (then RPCS3 is no longer needed), else RPCS3's own dev_flash.
function Get-DevFlash {
    $own = Join-Path (Split-Path -Parent $PSScriptRoot) "game\dev_flash"
    if (Test-Path -LiteralPath (Join-Path $own "sys")) { return $own }
    $rp = Get-PortPath rpcs3 -Optional
    if (-not $rp) { return $own }   # (v0.6.0: Setup always puts them there; no RPCS3 path is kept)
    Find-Rpcs3Dir $rp "dev_flash" "sys\external\liblv2.sprx"
}

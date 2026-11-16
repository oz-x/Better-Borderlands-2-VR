# Instalador interactivo de Better Borderlands 2 VR.
# Busca Borderlands 2 (Steam y Epic), pide confirmacion y copia el mod en Binaries\Win32.
#
#   powershell -ExecutionPolicy Bypass -File instalar.ps1
#   powershell -ExecutionPolicy Bypass -File instalar.ps1 -GamePath "D:\Juegos\Borderlands 2" -Yes
#
# -GamePath: carpeta del juego (o su Binaries\Win32) sin buscarla.  -Yes: no preguntar nada.
param(
    [string]$GamePath,
    [switch]$Yes
)

$ErrorActionPreference = 'Stop'
$SteamAppId = '49520'
$ModFiles = @('d3d9.dll', 'openxr_loader.dll')
$Source = $PSScriptRoot

function Write-Title([string]$text) {
    Write-Host ''
    Write-Host $text -ForegroundColor Cyan
}

function Finish([int]$code) {
    if (-not $Yes) {
        Write-Host ''
        Read-Host 'Pulsa Enter para salir' | Out-Null
    }
    exit $code
}

function Fail([string]$message) {
    Write-Host ''
    Write-Host "ERROR: $message" -ForegroundColor Red
    Finish 1
}

function Ask-YesNo([string]$question, [bool]$default) {
    if ($Yes) { return $true }
    $hint = if ($default) { '[S/n]' } else { '[s/N]' }
    while ($true) {
        $answer = (Read-Host "$question $hint").Trim().ToLower()
        if ($answer -eq '') { return $default }
        if ($answer -in @('s', 'si', 'y', 'yes')) { return $true }
        if ($answer -in @('n', 'no')) { return $false }
        Write-Host 'Responde s o n.'
    }
}

# Accepts the game folder, its Binaries\Win32 folder or Borderlands2.exe itself and returns
# the Binaries\Win32 folder, or $null if it is not a Borderlands 2 installation.
function Resolve-GameBinaries([string]$path) {
    if ([string]::IsNullOrWhiteSpace($path)) { return $null }
    $path = $path.Trim().Trim('"')
    if (Test-Path -LiteralPath $path -PathType Leaf) { $path = Split-Path -Parent $path }
    foreach ($candidate in @($path, (Join-Path $path 'Binaries\Win32'))) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'Borderlands2.exe') -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).ProviderPath
        }
    }
    return $null
}

function Get-SteamLibraries {
    $roots = New-Object System.Collections.Generic.List[string]
    foreach ($key in @(
            @{ Path = 'HKCU:\Software\Valve\Steam'; Name = 'SteamPath' },
            @{ Path = 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam'; Name = 'InstallPath' },
            @{ Path = 'HKLM:\SOFTWARE\Valve\Steam'; Name = 'InstallPath' })) {
        $value = (Get-ItemProperty -Path $key.Path -Name $key.Name -ErrorAction SilentlyContinue).($key.Name)
        if ($value) { $roots.Add(($value -replace '/', '\')) }
    }
    $libraries = New-Object System.Collections.Generic.List[string]
    foreach ($root in $roots) {
        $libraries.Add($root)
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($match in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries.Add(($match.Groups[1].Value -replace '\\\\', '\'))
            }
        }
    }
    return $libraries | Where-Object { Test-Path -LiteralPath $_ } | Sort-Object -Unique
}

function Find-Installations {
    $found = New-Object System.Collections.Generic.List[object]
    foreach ($library in Get-SteamLibraries) {
        $folder = 'Borderlands 2'
        $manifest = Join-Path $library "steamapps\appmanifest_$SteamAppId.acf"
        if (Test-Path -LiteralPath $manifest) {
            $match = [regex]::Match((Get-Content -LiteralPath $manifest -Raw), '"installdir"\s+"([^"]+)"')
            if ($match.Success) { $folder = $match.Groups[1].Value }
        }
        $binaries = Resolve-GameBinaries (Join-Path $library "steamapps\common\$folder")
        if ($binaries) { $found.Add([pscustomobject]@{ Store = 'Steam'; Binaries = $binaries }) }
    }
    $epic = Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
    if (Test-Path -LiteralPath $epic) {
        foreach ($item in Get-ChildItem -LiteralPath $epic -Filter '*.item' -ErrorAction SilentlyContinue) {
            try { $data = Get-Content -LiteralPath $item.FullName -Raw | ConvertFrom-Json } catch { continue }
            if ($data.DisplayName -notlike '*Borderlands 2*') { continue }
            $binaries = Resolve-GameBinaries $data.InstallLocation
            if ($binaries) { $found.Add([pscustomobject]@{ Store = 'Epic'; Binaries = $binaries }) }
        }
    }
    # The same folder can be reached through several registry keys / library entries.
    return @($found | Sort-Object Binaries -Unique)
}

function Select-Installation {
    if ($GamePath) {
        $binaries = Resolve-GameBinaries $GamePath
        if (-not $binaries) { Fail "No encuentro Borderlands2.exe en '$GamePath'." }
        return $binaries
    }
    Write-Title 'Buscando Borderlands 2...'
    $found = @(Find-Installations)  # @(): a single result would otherwise lose .Count
    if ($found.Count -eq 1) {
        Write-Host "Encontrado ($($found[0].Store)): $(Split-Path -Parent (Split-Path -Parent $found[0].Binaries))"
        if (Ask-YesNo 'Es esta la instalacion correcta?' $true) { return $found[0].Binaries }
    } elseif ($found.Count -gt 1) {
        Write-Host 'He encontrado varias instalaciones:'
        for ($i = 0; $i -lt $found.Count; $i++) {
            $root = Split-Path -Parent (Split-Path -Parent $found[$i].Binaries)
            Write-Host ("  [{0}] {1} ({2})" -f ($i + 1), $root, $found[$i].Store)
        }
        if ($Yes) { return $found[0].Binaries }
        while ($true) {
            $choice = Read-Host "Elige un numero (1-$($found.Count)), o Enter para escribir otra ruta"
            if ($choice -eq '') { break }
            $index = 0
            if ([int]::TryParse($choice, [ref]$index) -and $index -ge 1 -and $index -le $found.Count) {
                return $found[$index - 1].Binaries
            }
        }
    } else {
        Write-Host 'No he encontrado Borderlands 2 automaticamente.'
    }
    if ($Yes) { Fail 'No se ha encontrado el juego; indica la ruta con -GamePath.' }
    while ($true) {
        $typed = Read-Host 'Ruta de la carpeta de Borderlands 2 (vacio para cancelar)'
        if ([string]::IsNullOrWhiteSpace($typed)) { Write-Host 'Cancelado.'; Finish 1 }
        $binaries = Resolve-GameBinaries $typed
        if ($binaries) { return $binaries }
        Write-Host "No encuentro Borderlands2.exe ahi (ni en su Binaries\Win32)." -ForegroundColor Yellow
    }
}

Write-Host '=============================================' -ForegroundColor Cyan
Write-Host '   Better Borderlands 2 VR - instalador' -ForegroundColor Cyan
Write-Host '=============================================' -ForegroundColor Cyan

foreach ($file in $ModFiles + 'bl2vr.ini') {
    if (-not (Test-Path -LiteralPath (Join-Path $Source $file))) {
        Fail "Falta '$file' junto al instalador. Descomprime el ZIP completo antes de ejecutarlo."
    }
}

$target = Select-Installation
$existingDll = Join-Path $target 'd3d9.dll'
$existingIni = Join-Path $target 'bl2vr.ini'
# Our DLL always has a bl2vr.ini next to it; a lone d3d9.dll belongs to another mod (ReShade...).
$update = (Test-Path -LiteralPath $existingDll) -and (Test-Path -LiteralPath $existingIni)
$foreign = (Test-Path -LiteralPath $existingDll) -and -not $update
$backup = Join-Path $target ('d3d9.dll.bak-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))

Write-Title 'Resumen'
Write-Host "  Destino:  $target"
Write-Host "  Accion:   $(if ($update) { 'actualizar el mod ya instalado' } else { 'instalar el mod' })"
Write-Host "  Archivos: $($ModFiles -join ', ')"
if (Test-Path -LiteralPath $existingIni) {
    Write-Host '  Config:   se conserva tu bl2vr.ini actual'
} else {
    Write-Host '  Config:   bl2vr.ini con los valores por defecto'
}
if ($foreign) {
    Write-Host "  Aviso:    ya hay otro d3d9.dll (otro mod); se guardara como $(Split-Path -Leaf $backup)" -ForegroundColor Yellow
}
Write-Host ''
if (-not (Ask-YesNo 'Instalar ahora?' $true)) {
    Write-Host 'Cancelado; no se ha cambiado nada.'
    Finish 1
}

$running = Get-Process -Name 'Borderlands2' -ErrorAction SilentlyContinue
if ($running) {
    if (-not (Ask-YesNo 'Borderlands 2 esta abierto y hay que cerrarlo. Cerrarlo ahora?' $false)) {
        Fail 'Cierra Borderlands 2 y vuelve a ejecutar el instalador.'
    }
    $running | Stop-Process -Force
    $running | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
}

try {
    if ($foreign) { Move-Item -LiteralPath $existingDll -Destination $backup }
    foreach ($file in $ModFiles) {
        Copy-Item -LiteralPath (Join-Path $Source $file) -Destination $target -Force
    }
    if (-not (Test-Path -LiteralPath $existingIni)) {
        Copy-Item -LiteralPath (Join-Path $Source 'bl2vr.ini') -Destination $target
    }
} catch {
    Fail "No se pudieron copiar los archivos: $($_.Exception.Message)"
}

Write-Title 'Instalado.'
Write-Host '  1. Abre Virtual Desktop / Steam Link / SteamVR y conecta el Quest.'
Write-Host '  2. Lanza Borderlands 2 desde Steam como siempre.'
Write-Host "  3. Ajustes en: $existingIni"
Write-Host '  Para desinstalar, borra d3d9.dll, openxr_loader.dll y bl2vr.ini de esa carpeta.'
Finish 0

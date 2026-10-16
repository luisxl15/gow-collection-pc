# subir_para_github.ps1
# Sobe o projeto vita-recomp-lab para um repositorio PRIVADO no GitHub.
#
# Uso (PowerShell):
#   .\subir_para_github.ps1
#   .\subir_para_github.ps1 -Path "D:\outro\lugar\vita-recomp-lab" -RepoName "vita-recomp-lab"
#
# Ou de dois cliques em subir_para_github.bat.

param(
    [string]$Path = "C:\Users\Luis\Desktop\projeto_CLAUDE\vita-recomp-lab",
    [string]$RepoName = "",
    [int]$MaxFileMB = 95   # limite do GitHub e 100 MB por arquivo
)

$ErrorActionPreference = "Stop"

function Step($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "    $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "    $msg" -ForegroundColor Yellow }
function Fail($msg) { Write-Host ""; Write-Host "ERRO: $msg" -ForegroundColor Red; exit 1 }

# No Windows PowerShell 5.1, texto que um programa externo escreve no stderr
# vira erro fatal quando $ErrorActionPreference = "Stop". Por isso os
# programas externos rodam com "Continue" e o resultado vem do codigo de saida.
function Run {
    param([string]$Exe, [string[]]$CmdArgs)
    $ErrorActionPreference = "Continue"
    & $Exe @CmdArgs
    if ($LASTEXITCODE -ne 0) { Fail "'$Exe $($CmdArgs -join ' ')' falhou (codigo $LASTEXITCODE)." }
}

# Roda em silencio e devolve so o codigo de saida.
function Test-Native {
    param([string]$Exe, [string[]]$CmdArgs)
    $ErrorActionPreference = "Continue"
    & $Exe @CmdArgs 2>&1 | Out-Null
    return $LASTEXITCODE
}

function Refresh-Path {
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [Environment]::GetEnvironmentVariable("Path", "User")
}

function Ensure-Tool {
    param([string]$Command, [string]$WingetId, [string]$Label)
    if (Get-Command $Command -ErrorAction SilentlyContinue) { Ok "$Label encontrado."; return }

    Warn "$Label nao esta instalado."
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Fail "Instale o $Label manualmente e rode o script de novo."
    }
    $answer = Read-Host "    Instalar $Label com winget agora? (s/n)"
    if ($answer -notmatch '^[sSyY]') { Fail "$Label e necessario." }

    Run "winget" @("install", "--id", $WingetId, "-e", "--accept-source-agreements", "--accept-package-agreements")
    Refresh-Path
    if (-not (Get-Command $Command -ErrorAction SilentlyContinue)) {
        Fail "$Label foi instalado, mas ainda nao esta no PATH. Feche e abra o terminal e rode o script de novo."
    }
    Ok "$Label instalado."
}

# ------------------------------------------------------------------ 1. pasta
Step "Verificando a pasta do projeto"
if (-not (Test-Path -LiteralPath $Path -PathType Container)) { Fail "Pasta nao encontrada: $Path" }
Set-Location -LiteralPath $Path
if ([string]::IsNullOrWhiteSpace($RepoName)) { $RepoName = Split-Path -Leaf $Path }
Ok "Projeto: $Path"
Ok "Repositorio: $RepoName (privado)"

# ------------------------------------------------------------------ 2. ferramentas
Step "Verificando ferramentas"
Ensure-Tool -Command "git" -WingetId "Git.Git"     -Label "Git"
Ensure-Tool -Command "gh"  -WingetId "GitHub.cli"  -Label "GitHub CLI"

# ------------------------------------------------------------------ 3. login no GitHub
Step "Verificando login no GitHub"
if ((Test-Native "gh" @("auth", "status")) -ne 0) {
    Warn "Voce ainda nao esta logado. Vai abrir o navegador para autorizar."
    Run "gh" @("auth", "login", "--hostname", "github.com", "--git-protocol", "https", "--web")
}
Run "gh" @("auth", "setup-git")
$ghUser = (& gh api user --jq .login).Trim()
Ok "Logado como $ghUser"

# ------------------------------------------------------------------ 4. identidade do git
Step "Verificando nome e e-mail dos commits"
$gitName  = (& git config user.name)
$gitEmail = (& git config user.email)
if ([string]::IsNullOrWhiteSpace($gitName)) {
    $gitName = Read-Host "    Seu nome para os commits"
    Run "git" @("config", "--global", "user.name", $gitName)
}
if ([string]::IsNullOrWhiteSpace($gitEmail)) {
    $gitEmail = Read-Host "    Seu e-mail para os commits"
    Run "git" @("config", "--global", "user.email", $gitEmail)
}
Ok "$gitName <$gitEmail>"

# ------------------------------------------------------------------ 5. .gitignore
Step "Preparando .gitignore"
$ignoreEntries = @(
    "# build",
    "build/", "out/", "bin/", "obj/", "x64/", "Debug/", "Release/",
    "cmake-build-*/", "CMakeFiles/", "CMakeCache.txt",
    "*.obj", "*.o", "*.pdb", "*.ilk", "*.idb", "*.exe", "*.dll", "*.lib", "*.exp",
    "# IDEs",
    ".vs/", ".vscode/", ".idea/", "*.user", "*.suo",
    "# arquivos do jogo (nao vao para o repositorio)",
    "*.vpk", "*.iso", "*.psarc", "*.pkg", "*.zip", "*.7z", "*.rar",
    "dump/", "dumps/", "game/", "app0/", "assets_extraidos/",
    "# texturas PBR e dumps de shader (pesados)",
    "textures/", "pbr_dump/",
    "# sistema",
    "Thumbs.db", "desktop.ini"
)
$gitignorePath = Join-Path $Path ".gitignore"
$existing = @()
if (Test-Path -LiteralPath $gitignorePath) { $existing = Get-Content -LiteralPath $gitignorePath }
$toAdd = $ignoreEntries | Where-Object { $_ -notlike "#*" -and $existing -notcontains $_ }
if ($toAdd.Count -gt 0) {
    $block = @("", "# --- adicionado por subir_para_github.ps1 ---") + ($ignoreEntries | Where-Object { $_ -like "#*" -or $toAdd -contains $_ })
    Add-Content -LiteralPath $gitignorePath -Value $block -Encoding UTF8
    Ok "$($toAdd.Count) regras adicionadas ao .gitignore"
} else {
    Ok ".gitignore ja estava completo"
}
Warn "Confira se a pasta com os arquivos extraidos do jogo esta no .gitignore."

# ------------------------------------------------------------------ 6. git init + commit
Step "Preparando o repositorio local"
if (-not (Test-Path -LiteralPath (Join-Path $Path ".git"))) {
    Run "git" @("init")
    Ok "Repositorio git criado"
} else {
    Ok "Ja era um repositorio git"
}
Run "git" @("config", "core.autocrlf", "true")
Run "git" @("add", "-A")

# Arquivos grandes demais para o GitHub saem do commit e vao para o .gitignore.
$staged = & git diff --cached --name-only -z
$bigFiles = @()
if ($staged) {
    foreach ($rel in ($staged -split "`0")) {
        if ([string]::IsNullOrWhiteSpace($rel)) { continue }
        $full = Join-Path $Path $rel
        if (Test-Path -LiteralPath $full -PathType Leaf) {
            $sizeMB = (Get-Item -Force -LiteralPath $full).Length / 1MB
            if ($sizeMB -gt $MaxFileMB) { $bigFiles += [pscustomobject]@{ Path = $rel; MB = [math]::Round($sizeMB, 1) } }
        }
    }
}
if ($bigFiles.Count -gt 0) {
    Warn "Arquivos acima de $MaxFileMB MB foram removidos do commit e adicionados ao .gitignore:"
    foreach ($f in $bigFiles) {
        Warn "  $($f.Path) ($($f.MB) MB)"
        Run "git" @("rm", "--cached", "--quiet", "--", $f.Path)
        Add-Content -LiteralPath $gitignorePath -Value ("/" + ($f.Path -replace '\\', '/')) -Encoding UTF8
    }
    Run "git" @("add", ".gitignore")
}

& git diff --cached --quiet
if ($LASTEXITCODE -ne 0) {
    $fileCount = (& git diff --cached --name-only | Measure-Object).Count
    Run "git" @("commit", "-m", "Estado atual do port")
    Ok "Commit criado com $fileCount arquivo(s)"
} else {
    Ok "Nada novo para commitar"
}
Run "git" @("branch", "-M", "main")

# ------------------------------------------------------------------ 7. GitHub
Step "Enviando para o GitHub"
$hasOrigin = (& git remote) -contains "origin"
if (-not $hasOrigin) {
    if ((Test-Native "gh" @("repo", "view", "$ghUser/$RepoName")) -eq 0) {
        Warn "O repositorio $ghUser/$RepoName ja existe. Vou usar ele."
        Run "git" @("remote", "add", "origin", "https://github.com/$ghUser/$RepoName.git")
        Run "git" @("push", "-u", "origin", "main")
    } else {
        Run "gh" @("repo", "create", $RepoName, "--private", "--source", ".", "--remote", "origin", "--push")
    }
} else {
    Run "git" @("push", "-u", "origin", "main")
}

$fullName = (& gh repo view --json nameWithOwner --jq .nameWithOwner).Trim()
$visibility = (& gh repo view --json visibility --jq .visibility).Trim()
if ($visibility -ne "PRIVATE") { Warn "Atencao: o repositorio esta como $visibility, nao privado." }

try { Set-Clipboard -Value $fullName } catch { }

Write-Host ""
Write-Host "Pronto!" -ForegroundColor Green
Write-Host "Repositorio: https://github.com/$fullName ($visibility)"
Write-Host ""
Write-Host "Mande isto para o Claude (ja esta na area de transferencia):" -ForegroundColor Cyan
Write-Host "    $fullName" -ForegroundColor White
Write-Host ""

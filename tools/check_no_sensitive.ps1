# 提交前安全检查：确保没有把 ROM、字体、构建产物或个人环境信息提交进仓库。
#
# 用法：
#   .\check_no_sensitive.ps1           检查暂存区 + 已跟踪文件
#   .\check_no_sensitive.ps1 -Staged   只检查暂存区（适合做 pre-commit 钩子）
#
# 建议装成 git 钩子：
#   Copy-Item .\check_no_sensitive.ps1 ..\.git\hooks\pre-commit.ps1
#   （或用 git config core.hooksPath 指向一个目录）
param([switch]$Staged)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
Push-Location $repoRoot
try {
    $problems = @()

    # ---------- 1) 禁止出现的文件类型 ----------
    $forbiddenExt = @('.nes', '.unf', '.unif', '.ttf', '.ttc', '.otf', '.woff', '.bin', '.elf', '.map', '.hex')
    $files = if ($Staged) {
        git diff --cached --name-only --diff-filter=ACM
    } else {
        git ls-files
    }
    foreach ($f in $files) {
        $ext = [System.IO.Path]::GetExtension($f).ToLower()
        if ($forbiddenExt -contains $ext) {
            $problems += "被禁止的文件类型: $f"
        }
        if ($f -match '(^|/)(cn_font\.h|build/)') {
            $problems += "生成物不应入库: $f"
        }
    }

    # ---------- 2) 源码里的敏感字符串 ----------
    # 注意：本脚本自身必须包含这些字面量才能做检测，所以要把它自己排除掉，
    # 否则会出现"检查器检查到自己"的误报。
    $selfName = 'check_no_sensitive\.ps1'
    $patterns = @{
        '本机用户名'     = 'b3910'
        'Windows 用户目录' = 'C:\\Users\\'
        '本地绝对路径'   = '[A-Z]:\\(Github|nes|下载)'
        '代理地址'       = '127\.0\.0\.1:7900'
        '第三方字体路径' = 'MCStudioDownload|harry_luckcard'
    }
    foreach ($p in $patterns.GetEnumerator()) {
        $hits = if ($Staged) {
            git grep --cached -n -I -E $p.Value -- . 2>$null
        } else {
            git grep -n -I -E $p.Value -- . 2>$null
        }
        $hits = $hits | Where-Object { $_ -notmatch $selfName }
        if ($hits) {
            $hits | Select-Object -First 3 | ForEach-Object { $problems += "敏感内容 [$($p.Key)]: $_" }
        }
    }

    # ---------- 3) 过大的文本文件（提示性质） ----------
    foreach ($f in $files) {
        if (Test-Path $f) {
            $len = (Get-Item $f).Length
            if ($len -gt 1MB) {
                Write-Host "提示: $f 有 $([math]::Round($len/1KB)) KB，确认它不是数据/资源文件" -ForegroundColor Yellow
            }
        }
    }

    if ($problems.Count -gt 0) {
        Write-Host "`n提交被阻止，发现以下问题：" -ForegroundColor Red
        $problems | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
        Write-Host "`n如果确认是误报，请检查 .gitignore 后再提交。" -ForegroundColor Yellow
        exit 1
    }

    Write-Host "检查通过：没有发现 ROM / 字体 / 构建产物 / 个人环境信息。" -ForegroundColor Green
} finally {
    Pop-Location
}

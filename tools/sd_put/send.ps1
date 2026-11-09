# 把电脑上的 MP3 通过 USB 串口写进板子的 SD 卡。
#
# 前提：板子必须先烧 tools/sd_put/sd_put.ino（临时工具固件）。
# 传完记得烧回主固件。
#
# 用法：
#   pwsh -File tools/sd_put/send.ps1 -Dir "D:\Music" -Max 10
#   pwsh -File tools/sd_put/send.ps1 -Dir "D:\Music" -Max 1 -Verbose
#
# 参数：
#   -Port    串口，默认 COM8
#   -Dir     源目录，默认当前目录
#   -Max     最多传几个文件，默认 10
#   -Skip    跳过前 N 个（配合 -Max 分段传）

param(
    [string]$Port = "COM8",
    [string]$Dir  = ".",
    [int]$Max     = 10,
    [int]$Skip    = 0
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Wait-Line {
    param($Sp, [string]$Pattern, [int]$TimeoutMs = 15000)
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        try {
            $l = $Sp.ReadLine()
            if ($null -ne $l) {
                $t = $l.Trim()
                if ($t -match $Pattern) { return $t }
                if ($t -match '^ERR') { return $t }
            }
        } catch [System.TimeoutException] {
            # 继续等到 deadline
        }
    }
    return $null
}

# 1) 选文件
if (-not (Test-Path $Dir)) { throw "目录不存在: $Dir" }
$all = Get-ChildItem $Dir -File -Filter *.mp3 | Sort-Object Name
$files = $all | Select-Object -Skip $Skip -First $Max
if (-not $files) { throw "目录里没有 mp3: $Dir" }

Write-Host "=== 准备发送 $($files.Count) 个文件 (共 $($all.Count) 个可选) ==="
$totalMB = [math]::Round((($files | Measure-Object Length -Sum).Sum) / 1MB, 1)
Write-Host "    总大小 $totalMB MB"

# 2) 打开串口
$sp = New-Object System.IO.Ports.SerialPort($Port, 921600, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$sp.Encoding = [System.Text.Encoding]::UTF8
$sp.ReadTimeout = 2000
$sp.WriteTimeout = 20000
$sp.WriteBufferSize = 65536
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.Open()
Start-Sleep -Milliseconds 300
$sp.DiscardInBuffer()

# 3) 握手
Write-Host "=== 握手 ..."
$ok = $false
for ($i = 0; $i -lt 3 -and -not $ok; $i++) {
    $sp.WriteLine("PING")
    $r = Wait-Line -Sp $sp -Pattern '^PONG$' -TimeoutMs 4000
    if ($r -eq 'PONG') { $ok = $true } else { Start-Sleep -Milliseconds 500 }
}
if (-not $ok) {
    $sp.Close()
    throw "握手失败：板子没在跑 sd_put 固件？串口 = $Port"
}
Write-Host "    链路正常"

# 4) 逐个发送
$sent = 0
$tStart = Get-Date
foreach ($f in $files) {
    $data = [System.IO.File]::ReadAllBytes($f.FullName)
    $size = $data.Length
    $mb = [math]::Round($size / 1MB, 2)
    Write-Host ("--- {0} ({1} MB)" -f $f.Name, $mb)

    $sp.WriteLine("PUT $size $($f.Name)")
    $r = Wait-Line -Sp $sp -Pattern '^READY$' -TimeoutMs 10000
    if ($r -ne 'READY') {
        Write-Host "    跳过: $r"
        continue
    }

    $t0 = Get-Date
    $chunk = 8192
    for ($off = 0; $off -lt $size; $off += $chunk) {
        $len = [Math]::Min($chunk, $size - $off)
        $sp.Write($data, $off, $len)
    }

    $r = Wait-Line -Sp $sp -Pattern '^OK ' -TimeoutMs 60000
    $dt = ((Get-Date) - $t0).TotalSeconds
    if ($r -and $r -match '^OK (\d+) (\d+)') {
        $written = [long]$matches[1]
        $expect  = [long]$matches[2]
        if ($written -eq $expect) {
            $kb = [math]::Round($size / 1KB / $dt, 0)
            Write-Host ("    OK {0} 字节  {1:N0} KB/s" -f $written, $kb)
            $sent++
        } else {
            Write-Host ("    不完整: 写入 $written / 期望 $expect")
        }
    } else {
        Write-Host "    失败: $r"
    }
}

$sp.Close()
$elapsed = ((Get-Date) - $tStart).TotalSeconds
Write-Host ""
Write-Host ("=== 完成: {0}/{1} 个文件, 用时 {2:N0} 秒 ===" -f $sent, $files.Count, $elapsed)

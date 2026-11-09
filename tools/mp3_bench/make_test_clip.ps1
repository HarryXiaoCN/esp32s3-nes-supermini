# 重新生成 benchmark 用的测试片段（10 秒 320kbps CBR，最坏情况）
# 用法： pwsh -File tools/mp3_bench/make_test_clip.ps1 -Src "D:\Music\song.mp3"
param([Parameter(Mandatory=$true)][string]$Src)

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
ffmpeg -y -loglevel error -ss 30 -t 10 -i $Src -c:a libmp3lame -b:a 320k -ar 44100 -ac 2 "$here\test.mp3"

python -c @"
import io, os
b = r'$here'
data = open(os.path.join(b, 'test.mp3'), 'rb').read()
out = io.open(os.path.join(b, 'test_mp3.h'), 'w', encoding='utf-8', newline='\n')
out.write('// 自动生成，勿手改\n#pragma once\n#include <stdint.h>\n\n')
out.write('static const uint8_t test_mp3[] = {\n')
for i in range(0, len(data), 16):
    out.write(','.join('0x%02X' % x for x in data[i:i+16]) + ',\n')
out.write('};\n\nstatic const unsigned test_mp3_len = %d;\n' % len(data))
print('generated %d bytes' % len(data))
"@

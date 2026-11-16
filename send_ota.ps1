# =============================================================================
#  send_ota.ps1  ——  Wi-Fi OTA 发送工具（CHUNK/ACK 停等协议 + 断点续传）
#
#  协议约定（板子是偏移量的唯一权威）：
#      板子 → 电脑 : READY <已写入字节数>         "从第 N 字节开始发"
#      电脑 → 板子 : CHUNK <偏移> <长度>\r\n + <长度> 字节原始数据
#      板子 → 电脑 : ACK <新的已写入字节数>       "这些字节已经写进 W25Q64"
#      板子 → 电脑 : OK FLASHING                 "收满且 CRC 校验通过，准备重启"
#      板子 → 电脑 : ERR HEAD / ERR CRC / ERR LEN / ERR OFFSET <偏移>
#
#  链路断开时：本脚本不退出，等板子重连后从它报的偏移继续发，
#  不重发、不跳过。
# =============================================================================
param(
    [string]$File        = 'app_ota.bin',
    [int]   $Port        = 6677,
    [int]   $Chunk       = 512,
    [int]   $TimeoutMs   = 12000,
    [int]   $MaxReconnect = 200
)

$ErrorActionPreference = 'Stop'

function Read-OneLine([System.IO.Stream]$s)
{
    $buf = New-Object byte[] 1
    $sb  = New-Object System.Text.StringBuilder
    while ($true)
    {
        $n = $s.Read($buf, 0, 1)
        if ($n -le 0) { throw '对端关闭了连接' }
        if ($buf[0] -eq 10) { return $sb.ToString() }
        if ($buf[0] -ne 13) { [void]$sb.Append([char]$buf[0]) }
        if ($sb.Length -gt 300) { throw '单行超长（协议可能失步）' }
    }
}

# 读板子的行，直到拿到带偏移量/结果的协议行
function Read-Board([System.IO.Stream]$s, [int]$maxLines = 40)
{
    for ($i = 0; $i -lt $maxLines; $i++)
    {
        $line = (Read-OneLine $s).Trim()

        if ($line -match '^READY\s+(\d+)')      { return @{ kind='off';   off=[int64]$Matches[1]; text=$line } }
        if ($line -match '^ACK\s+(\d+)')        { return @{ kind='off';   off=[int64]$Matches[1]; text=$line } }
        if ($line -match '^ERR OFFSET\s+(\d+)') { return @{ kind='off';   off=[int64]$Matches[1]; text=$line } }
        if ($line -match '^OK')                 { return @{ kind='done';  off=[int64]-1;          text=$line } }
        if ($line -match '^ERR CRC')            { Write-Host '[!] 板子：CRC 不符，将从 0 重新开始'; continue }
        if ($line -match '^ERR')                { return @{ kind='fatal'; off=[int64]-1;          text=$line } }
        if ($line.Length -gt 0)                 { Write-Host ("[板子] " + $line) }
    }
    return @{ kind='none'; off=[int64]-1; text='' }
}

# ---------------------------------------------------------------- 读文件
if (-not (Test-Path $File)) { Write-Host "错误：找不到文件 $File"; exit 1 }
$data  = [System.IO.File]::ReadAllBytes((Resolve-Path $File).Path)
$total = $data.Length
if ($total -lt 16) { Write-Host "错误：$File 太小，不像升级包"; exit 1 }

# 检查是不是合法的升级包（"OTA1" + 长度 + CRC）
$magic = [System.Text.Encoding]::ASCII.GetString($data, 0, 4)
$fwLen = [BitConverter]::ToUInt32($data, 4)
$fwCrc = [BitConverter]::ToUInt32($data, 8)
if ($magic -ne 'OTA1') {
    Write-Host "错误：$File 开头不是 OTA1，这不是升级包。"
    Write-Host "      请用 make_ota.exe 生成的 app_ota.bin，而不是 fromelf 出来的 app.bin。"
    exit 1
}
if (($fwLen + 12) -ne $total) {
    Write-Host ("错误：包长度对不上，包头声明 {0}+12，文件实际 {1}" -f $fwLen, $total)
    exit 1
}

Write-Host ''
Write-Host '================ Wi-Fi OTA 发送工具 ================'
Write-Host ("文件      : {0}" -f (Resolve-Path $File).Path)
Write-Host ("升级包    : {0} 字节（固件 {1} + 包头 12）" -f $total, $fwLen)
Write-Host ("固件 CRC  : 0x{0:X8}" -f $fwCrc)
Write-Host ("分包大小  : {0} 字节/包，停等确认" -f $Chunk)
Write-Host ''
Write-Host '本机 IPv4 地址（net.h 里的服务器 IP 必须是其中之一）：'
[System.Net.Dns]::GetHostAddresses([System.Net.Dns]::GetHostName()) |
    Where-Object { $_.AddressFamily -eq 'InterNetwork' } |
    ForEach-Object { Write-Host ("    " + $_.IPAddressToString) }
Write-Host ''

# ---------------------------------------------------------------- 监听
# 端口自检：Windows 允许两个程序同时监听一个端口，板子的连接会被随机分走。
$owners = @(Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty OwningProcess -Unique |
            Where-Object { $_ -gt 0 })
if ($owners.Count -gt 0) {
    foreach ($op in $owners) {
        $proc = Get-Process -Id $op -ErrorAction SilentlyContinue
        Write-Host ("错误：端口 {0} 已被 PID {1}（{2}）占用" -f $Port, $op, $proc.ProcessName)
    }
    Write-Host '      请关掉那个程序，或者用 -Port 换一个端口。'
    exit 3
}

$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Any, $Port)
$listener.Server.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::Socket,
                                 [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
try { $listener.Start() }
catch { Write-Host ("错误：无法监听端口 {0}：{1}" -f $Port, $_.Exception.Message); exit 1 }

Write-Host ("正在监听端口 {0}，等待板子连接..." -f $Port)
Write-Host '（板子上电或复位后会自动连过来）'
Write-Host ''

$reconnects = 0
$lastOff    = -1
$finished   = $false

while (-not $finished)
{
    $client = $null
    $stream = $null
    try
    {
        $client = $listener.AcceptTcpClient()
        $client.NoDelay = $true
        $stream = $client.GetStream()
        $stream.ReadTimeout  = $TimeoutMs
        $stream.WriteTimeout = $TimeoutMs

        $peer = $client.Client.RemoteEndPoint.ToString()
        Write-Host ("[+] 板子已连接：{0}" -f $peer)

        # ---- 等 READY <偏移>
        $r = Read-Board $stream
        if ($r.kind -ne 'off') {
            Write-Host ("[!] 期望 READY，实际收到：'{0}'，关闭本次连接" -f $r.text)
            throw '没有收到 READY'
        }
        $off = $r.off
        if ($off -ne $lastOff) {
            Write-Host ("[*] 板子已写入 {0} / {1} 字节（{2:N1}%）" -f $off, $total, (100.0 * $off / $total))
            $lastOff = $off
        }
        if ($off -gt 0) { Write-Host '[>] 断点续传：从板子报的位置继续' }

        # ---- 逐包发送
        while ($off -lt $total)
        {
            $len = [Math]::Min($Chunk, $total - $off)
            $hdr = [System.Text.Encoding]::ASCII.GetBytes(("CHUNK {0} {1}`r`n" -f $off, $len))

            $stream.Write($hdr, 0, $hdr.Length)
            $stream.Write($data, [int]$off, [int]$len)
            $stream.Flush()

            $r = Read-Board $stream
            if ($r.kind -eq 'fatal') { throw ("板子拒绝传输：" + $r.text) }
            if ($r.kind -eq 'done')  { Write-Host ("[!] 板子提前完成：" + $r.text); $finished = $true; break }
            if ($r.kind -ne 'off')   { throw '没有收到 ACK' }

            if ($r.off -eq $off) {
                Write-Host ("[!] 偏移 {0} 没有进展，重发这一包" -f $off)
            } else {
                $off = $r.off
                $lastOff = $off
                $pct = 100.0 * $off / $total
                Write-Host ("`r[发送] {0,7} / {1} 字节（{2,5:N1}%）" -f $off, $total, $pct) -NoNewline
            }
        }

        if (-not $finished)
        {
            Write-Host ''
            Write-Host '[*] 全部发完，等待板子校验 CRC...'
            $r = Read-Board $stream 60
            if ($r.kind -eq 'done') {
                Write-Host '[OK] 板子回报：校验通过，正在烧写并重启'
                $finished = $true
            } elseif ($r.kind -eq 'fatal') {
                throw ("板子拒绝传输：" + $r.text)
            } else {
                Write-Host ("[!] 没等到结论（'{0}'），重新连接" -f $r.text)
                throw '没等到结论'
            }
        }
    }
    catch
    {
        Write-Host ''
        Write-Host ("[!] 链路断开：{0}" -f $_.Exception.Message)
        $reconnects++
        if ($reconnects -gt $MaxReconnect) {
            Write-Host '[X] 重连次数太多，放弃'
            break
        }
        Write-Host ("[*] 等板子重连（第 {0}/{1} 次）..." -f $reconnects, $MaxReconnect)
    }
    finally
    {
        if ($stream) { try { $stream.Close() } catch {} }
        if ($client) { try { $client.Close() } catch {} }
    }
}

try { $listener.Stop() } catch {}

Write-Host ''
if ($finished) {
    Write-Host '================ 完成：固件已成功传完 ================'
    Write-Host '板子正在重启并搬运固件，请观察串口和指示灯。'
    exit 0
}
Write-Host '================ 失败：固件没有传完 ================'
exit 2

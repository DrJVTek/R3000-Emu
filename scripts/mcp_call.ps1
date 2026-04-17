# mcp_call.ps1 — spawn r3000_mcp.exe as stdio MCP server, call one tool, print response.
#
# Usage:
#   powershell -File mcp_call.ps1 -Tool "emu.match_render_pattern" -Args '{"max_candidates":3}'
#   powershell -File mcp_call.ps1 -Tool "emu.get_status"
#
# Prerequisites:
#   - r3000_emu.exe must already be running on TCP port 9742
#   - r3000_mcp.exe must be built at lib/Release/r3000_mcp.exe

param(
  [Parameter(Mandatory=$true)][string]$Tool,
  [string]$Args = '{}',
  [string]$McpExe = 'E:\Projects\github\Live\R3000-Emu\lib\Release\r3000_mcp.exe',
  [int]$Port = 9742,
  [int]$TimeoutSec = 10
)

function Write-McpFrame {
  param([System.IO.StreamWriter]$w, [string]$json)
  $bytes = [System.Text.Encoding]::UTF8.GetByteCount($json)
  $w.BaseStream.Write([System.Text.Encoding]::ASCII.GetBytes("Content-Length: $bytes`r`n`r`n"), 0, $bytes.ToString().Length + 20)
  $w.BaseStream.Write([System.Text.Encoding]::UTF8.GetBytes($json), 0, $bytes)
  $w.BaseStream.Flush()
}

function Read-McpFrame {
  param([System.IO.StreamReader]$r)
  $contentLen = 0
  while ($true) {
    $line = $r.ReadLine()
    if ($null -eq $line) { return $null }
    if ($line -match '^Content-Length:\s*(\d+)') {
      $contentLen = [int]$Matches[1]
    }
    if ($line -eq '') { break }  # empty line = end of headers
  }
  if ($contentLen -le 0) { return $null }
  $buf = New-Object char[] $contentLen
  $read = 0
  while ($read -lt $contentLen) {
    $n = $r.Read($buf, $read, $contentLen - $read)
    if ($n -le 0) { break }
    $read += $n
  }
  return (-join $buf)
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $McpExe
$psi.Arguments = "--port $Port"
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8

$proc = [System.Diagnostics.Process]::Start($psi)
try {
  $stdin = $proc.StandardInput
  $stdout = $proc.StandardOutput

  # 1. initialize
  $init = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","clientInfo":{"name":"mcp_call.ps1","version":"1.0"},"capabilities":{}}}'
  Write-McpFrame $stdin $init
  $resp = Read-McpFrame $stdout
  if (-not $resp) { Write-Error "No response to initialize"; exit 2 }

  # 2. initialized notification
  $notif = '{"jsonrpc":"2.0","method":"notifications/initialized"}'
  Write-McpFrame $stdin $notif

  # 3. tools/call
  $call = ConvertTo-Json -Compress -Depth 10 @{
    jsonrpc = "2.0"
    id = 2
    method = "tools/call"
    params = @{
      name = $Tool
      arguments = (ConvertFrom-Json $Args)
    }
  }
  Write-McpFrame $stdin $call
  $resp = Read-McpFrame $stdout
  if ($resp) {
    Write-Output $resp
  } else {
    Write-Error "No response to tools/call"
    exit 3
  }
}
finally {
  if ($proc -and -not $proc.HasExited) {
    $proc.StandardInput.Close()
    if (-not $proc.WaitForExit(2000)) { $proc.Kill() }
  }
}

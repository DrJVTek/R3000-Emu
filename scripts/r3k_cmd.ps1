# r3k_cmd — one-shot helper to send a command to the emu debug server.
#
# Usage:
#   powershell -File r3k_cmd.ps1 -Cmd 'read_cpu'
#   powershell -File r3k_cmd.ps1 -Cmd 'read_memory' -Addr 0x8004E9A0 -Size 64
#   powershell -File r3k_cmd.ps1 -Cmd 'ping'
#
# Prints the JSON response to stdout.

param(
  [Parameter(Mandatory=$true)][string]$Cmd,
  [string]$Addr,
  [int]$Size = 0,
  [string]$Host_ = 'localhost',
  [int]$Port = 9742
)

# Build JSON body.  We always send {"cmd":"..."} plus optional addr/size as
# decimal (the server parses numbers via find_num which reads any numeric
# literal).  We accept Addr as 0xHEX or plain decimal.
$body = '{"cmd":"' + $Cmd + '"'
if ($Addr) {
  $addrInt = 0
  if ($Addr -match '^0[xX]([0-9A-Fa-f]+)$') {
    $addrInt = [Convert]::ToInt64($Matches[1], 16)
  } else {
    $addrInt = [Int64]$Addr
  }
  $body += ',"addr":' + $addrInt
}
if ($Size -gt 0) { $body += ',"size":' + $Size }
$body += '}'

$client = $null
try {
  $client = New-Object System.Net.Sockets.TcpClient($Host_, $Port)
  $stream = $client.GetStream()
  $writer = New-Object System.IO.StreamWriter($stream)
  $reader = New-Object System.IO.StreamReader($stream)
  $writer.AutoFlush = $true

  $writer.WriteLine($body)
  $resp = $reader.ReadLine()
  Write-Output $resp
}
catch [System.Net.Sockets.SocketException] {
  Write-Error "Cannot connect to emu at $Host_ port $Port - is r3000_emu.exe running?"
  exit 2
}
catch {
  Write-Error "TCP error: $_"
  exit 3
}
finally {
  if ($client) { $client.Close() }
}

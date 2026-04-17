# PC sampler — connects to emu TCP debug server, queries read_cpu N times,
# aggregates PC histogram, writes top hotspots to stdout.
#
# Usage: powershell -File pc_sampler.ps1 -Samples 500 -IntervalMs 6
#
# Output: sorted list of (pc_hex, hit_count) for the top 20 hotspots.

param(
  [int]$Samples = 500,
  [int]$IntervalMs = 6,
  [string]$Host_ = 'localhost',
  [int]$Port = 9742,
  [int]$TopN = 20
)

$client = $null
$hist = @{}
$t0 = Get-Date

try {
  $client = New-Object System.Net.Sockets.TcpClient($Host_, $Port)
  $stream = $client.GetStream()
  $writer = New-Object System.IO.StreamWriter($stream)
  $reader = New-Object System.IO.StreamReader($stream)
  $writer.AutoFlush = $true

  for ($i = 0; $i -lt $Samples; $i++) {
    $writer.WriteLine('{"cmd":"read_cpu"}')
    $j = $reader.ReadLine()
    if ($j -match '"pc":"(0x[0-9A-F]+)"') {
      $pc = $Matches[1]
      if ($hist.ContainsKey($pc)) { $hist[$pc] += 1 } else { $hist[$pc] = 1 }
    }
    if ($IntervalMs -gt 0) { Start-Sleep -Milliseconds $IntervalMs }
  }
}
catch [System.Net.Sockets.SocketException] {
  Write-Error "Cannot connect to emu at $Host_ port $Port - is r3000_emu.exe running?"
  exit 2
}
catch {
  Write-Error "Sampler error at iter $i : $_"
  # keep partial histogram and fall through to print
}
finally {
  if ($client) { $client.Close() }
}

$elapsed = ((Get-Date) - $t0).TotalSeconds
Write-Host ("Samples taken: {0} in {1:N2}s ({2:N0} Hz)" -f $Samples, $elapsed, ($Samples / $elapsed))
Write-Host ("Unique PCs:    {0}" -f $hist.Count)
Write-Host ""
Write-Host ("Top {0} hotspots:" -f $TopN)

$sorted = $hist.GetEnumerator() | Sort-Object -Property Value -Descending | Select-Object -First $TopN
foreach ($e in $sorted) {
  $pct = 100.0 * $e.Value / $Samples
  Write-Host ("  {0}  hits={1,-6}  {2,5:N1}%" -f $e.Key, $e.Value, $pct)
}

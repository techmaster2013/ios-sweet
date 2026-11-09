# Run elevated (UAC). Applies a block delta (tools/blockdelta.py) to the iPadOS
# partition (3) of the Pi 5 SD card, then reads every written block back and
# compares it. Only the listed blocks are touched.
#   sd-delta-write.ps1 -Delta \\wsl.localhost\...\x.delta -Log C:\...\sd-delta.log
param([Parameter(Mandatory = $true)][string]$Delta, [Parameter(Mandatory = $true)][string]$Log)
$ErrorActionPreference = 'Stop'
function Log($m) { "{0:HH:mm:ss} {1}" -f (Get-Date), $m | Tee-Object -FilePath $Log -Append }
try {
	Remove-Item $Log -ErrorAction SilentlyContinue
	$p3Offset = 3078619136
	$p3Size = 17179869184
	$disk = Get-Disk | Where-Object { $_.BusType -eq 'SD' -and $_.FriendlyName -match 'SDXC' -and $_.Size -eq 63864569856 }
	if (@($disk).Count -ne 1) { throw "expected exactly one 63864569856-byte SDXC card, found $(@($disk).Count)" }
	$p3 = Get-Partition -DiskNumber $disk.Number | Where-Object PartitionNumber -eq 3
	if (-not $p3 -or $p3.Offset -ne $p3Offset -or $p3.Size -ne $p3Size -or $p3.MbrType -ne 0xAF) { throw "partition 3 is not the iPadOS partition" }

	$in = [System.IO.File]::OpenRead($Delta)
	$br = New-Object System.IO.BinaryReader($in)
	$magic = [System.Text.Encoding]::ASCII.GetString($br.ReadBytes(8))
	$ver = $br.ReadUInt32(); $bs = $br.ReadUInt32(); $size = $br.ReadUInt64(); $count = $br.ReadUInt64()
	if ($magic -ne 'SWEETDLT' -or $ver -ne 1 -or $bs -ne 4096 -or $size -ne $p3Size) { throw "not a delta for this partition" }
	Log "delta: $count blocks of $bs bytes"

	$dev = New-Object System.IO.FileStream("\\.\PhysicalDrive$($disk.Number)", [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)
	$check = New-Object byte[] $bs
	$bad = 0
	try {
		for ($i = 0; $i -lt $count; $i++) {
			$off = $br.ReadUInt64()
			if ($off + $bs -gt $p3Size -or ($off % $bs) -ne 0) { throw "bad block offset $off" }
			$data = $br.ReadBytes($bs)
			$dev.Seek($p3Offset + $off, 'Begin') | Out-Null
			$dev.Write($data, 0, $bs)
			$dev.Flush()
			$dev.Seek($p3Offset + $off, 'Begin') | Out-Null
			$n = 0; while ($n -lt $bs) { $k = $dev.Read($check, $n, $bs - $n); if ($k -le 0) { throw "read back ended early" }; $n += $k }
			for ($j = 0; $j -lt $bs; $j++) { if ($check[$j] -ne $data[$j]) { $bad++; break } }
		}
	} finally { $dev.Close(); $br.Close() }
	if ($bad) { throw "$bad block(s) read back differently" }
	Log "wrote and verified $count blocks"
	Log "DONE OK"
} catch {
	Log "ERROR: $($_.Exception.Message)"
	Log "DONE FAILED"
}

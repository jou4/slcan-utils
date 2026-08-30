<#
	powershell -ExecutionPolicy Bypass examples\run_daemons.ps1
#>

$slcd = 'build/Release/slcd.exe'

$procs = @()

$a = @{
    FilePath     = $slcd
    ArgumentList = @('COM1', 'can0', 6, 2)
    PassThru     = $true
    NoNewWindow  = $true
}
$procs += Start-Process @a

$a = @{
    FilePath     = $slcd
    ArgumentList = @('--vcan', 'can1')
    PassThru     = $true
    NoNewWindow  = $true
}
$procs += Start-Process @a

Write-Host "Started $($procs.Count) process(es). Press Ctrl+C to stop."

try {
    while ($true) { Start-Sleep -Seconds 1 }
} finally {
    foreach ($p in $procs) {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
}


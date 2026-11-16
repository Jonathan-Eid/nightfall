param(
    [ValidateSet('Install','Repair','Check','Remove','Plan')][string]$Action='Install',
    [string]$Root=$PSScriptRoot,
    [ValidateRange(0,65535)][int]$HostHttpPort=0,
    [ValidateRange(0,65535)][int]$ProxyOffset=0,
    [ValidateRange(0,65535)][int]$ControlPort=0,
    [ValidateRange(0,65535)][int]$VideoPort=0,
    [ValidateRange(0,65535)][int]$TrackingPort=0,
    [switch]$EnableDepth,
    [switch]$EnableMicrophone,
    [string]$ResultPath=''
)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'UsbLink-Firewall.psm1') -Force
try {
    $Root=[IO.Path]::GetFullPath($Root)
    $principal=[Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    $administrator=$principal.IsInRole([Security.Principal.WindowsBuiltInRole]'Administrator')
    function Save-UsbPreference([hashtable]$Values) {
        try {Save-NightfallUsbSettings $Root $Values}
        catch [UnauthorizedAccessException] {
            # A protected install folder is written by the elevated child.
            if($administrator){throw}
        }
    }
    $settings=if($Action -ne 'Remove'){Get-NightfallUsbSettings $Root}else{@{}}
    foreach($key in @('HostHttpPort','ProxyOffset','ControlPort','VideoPort','TrackingPort')) {
        $value=Get-Variable -Name $key -ValueOnly
        if($value -and $Action -ne 'Remove'){$settings[$key]=$value}
    }
    if($EnableDepth){$settings.EnableDepth=$true}
    if($EnableMicrophone){$settings.EnableMicrophone=$true}
    # Remember the explicit USB choice even if the cable is absent or UAC is
    # canceled. The tray's repair/check must never fall back to LAN allowances.
    if($Action -in @('Install','Repair') -and -not (Test-Path -LiteralPath (Join-Path $Root 'usb-firewall.json')) -and (Test-Path -LiteralPath (Join-Path $Root 'nightfall-meteor.exe')) -and (Test-Path -LiteralPath (Join-Path $Root 'runtime\host\sunshine.exe'))) {
        Save-UsbPreference (Get-NightfallUsbSettings $Root)
    }
    $plan=if($Action -ne 'Remove'){Get-NightfallUsbFirewallPlan -Root $Root -Settings $settings}
    if($Action -in @('Install','Repair')){Save-UsbPreference $settings}
    if($Action -in @('Install','Repair','Remove')) {
        if(-not $administrator) {
            $temporary=[IO.Path]::GetTempFileName()
            try {
                $arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSCommandPath+'"'),'-Action',$Action,'-Root',('"'+$Root+'"'),'-ResultPath',('"'+$temporary+'"'))
                if($Action -ne 'Remove') {foreach($key in @('HostHttpPort','ProxyOffset','ControlPort','VideoPort','TrackingPort')){$arguments+=@('-'+$key,[string]$settings[$key])}}
                if($settings.EnableDepth){$arguments+='-EnableDepth'}
                if($settings.EnableMicrophone){$arguments+='-EnableMicrophone'}
                $process=Start-Process "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -WindowStyle Hidden -ArgumentList $arguments -Wait -PassThru
                $report=Get-Content -LiteralPath $temporary -Raw | ConvertFrom-Json
                if($process.ExitCode -ne 0 -or -not $report.ok){throw $report.error}
            } finally {Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue}
        } elseif($Action -eq 'Remove') {$report=Remove-NightfallUsbFirewall $Root}
        else {$report=Install-NightfallUsbFirewall $plan}
    } elseif($Action -eq 'Check') {$report=Test-NightfallUsbFirewall $plan}
    else {$report=$plan}
    if($ResultPath){[IO.File]::WriteAllText($ResultPath,($report | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))}
    $report
} catch {
    if($ResultPath){[IO.File]::WriteAllText($ResultPath,(@{ok=$false;error=$_.Exception.Message} | ConvertTo-Json),[Text.UTF8Encoding]::new($false))}
    throw
}

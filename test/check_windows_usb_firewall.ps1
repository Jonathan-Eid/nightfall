param([Parameter(Mandatory)][string]$Result)
# Elevated integration test against the real Windows PersistentStore. Fixture
# programs are inert files and are never run; all fixture rules are removed.
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot
Import-Module (Join-Path $repo 'meteor\tools\windows-release\UsbLink-Firewall.psm1') -Force
$principal=[Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]'Administrator')){throw 'Run this integration test elevated.'}
$artifacts=Join-Path $repo ('build\usb-firewall-integration-'+[guid]::NewGuid().ToString('N'))
$roots=@((Join-Path $artifacts 'First Install'),(Join-Path $artifacts 'Second Install'))
$summary=[ordered]@{ok=$false;artifacts=$artifacts;checks=@();fixture_rules_removed=$false}
try {
    foreach($root in $roots){
        New-Item -ItemType Directory -Force (Join-Path $root 'runtime\host') | Out-Null
        [IO.File]::WriteAllText((Join-Path $root 'nightfall-meteor.exe'),'inert test fixture; never executed')
        [IO.File]::WriteAllText((Join-Path $root 'runtime\host\sunshine.exe'),'inert test fixture; never executed')
    }
    $first=Get-NightfallUsbFirewallPlan -Root $roots[0]
    $second=Get-NightfallUsbFirewallPlan -Root $roots[1]
    Install-NightfallUsbFirewall $first | Out-Null
    Install-NightfallUsbFirewall $first | Out-Null
    if(@(Get-NetFirewallRule -PolicyStore PersistentStore -Group $first.Group).Count -ne 3){throw 'Repeated install duplicated rules.'}
    $summary.checks+='idempotent_install'
    # Narrow one rule incorrectly, then verify Check detects it and Repair
    # restores the required port list using Set-NetFirewallRule.
    Set-NetFirewallRule -PolicyStore PersistentStore -Name $first.rules[0].Name -LocalPort $first.rules[0].LocalPort[0]
    if((Test-NightfallUsbFirewall $first).ok){throw 'Checker missed altered ports.'}
    Install-NightfallUsbFirewall $first | Out-Null
    if(-not (Test-NightfallUsbFirewall $first).ok){throw 'Repair failed.'}
    $summary.checks+='detect_and_repair_changed_filters'
    Install-NightfallUsbFirewall $second | Out-Null
    Remove-NightfallUsbFirewall $roots[0] | Out-Null
    if(-not (Test-NightfallUsbFirewall $second).ok){throw 'Removing one installation affected the other.'}
    $summary.checks+='separate_install_ownership'
    $saved=[IO.File]::ReadAllText($second.Policy)
    if($saved -match 'fe80|Interface|Address|Serial' -or $saved.Contains($roots[1])){throw 'Saved policy contains machine-specific identifiers.'}
    $summary.checks+='portable_saved_settings'
    $summary.ok=$true
} catch {$summary.error=$_.Exception.Message;$summary.stack=$_.ScriptStackTrace}
finally {
    foreach($root in $roots){
        try {Remove-NightfallUsbFirewall $root | Out-Null}
        catch {$summary.ok=$false;$summary.cleanup_error=$_.Exception.Message}
    }
    $remaining=@(Get-NetFirewallRule -PolicyStore PersistentStore -ErrorAction SilentlyContinue | Where-Object {
        $_.Group -in @($first.Group,$second.Group)
    })
    $summary.fixture_rules_removed=$remaining.Count -eq 0
    if(-not $summary.fixture_rules_removed){$summary.ok=$false}
    [IO.File]::WriteAllText($Result,($summary | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
}
$summary
if(-not $summary.ok){throw 'USB firewall integration failed; see the result JSON.'}

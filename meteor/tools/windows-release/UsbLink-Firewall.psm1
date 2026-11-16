# Shared policy for the USB installer, repair shortcut and read-only checker.
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

function Get-NightfallUsbAdapters {
    # Identify the device/driver, not a localized name such as "Ethernet 2".
    $devices = @(Get-CimInstance Win32_NetworkAdapter -ErrorAction Stop)
    @(Get-NetAdapter -IncludeHidden -ErrorAction Stop | Where-Object {
        $index = $_.InterfaceIndex
        $device = $devices | Where-Object InterfaceIndex -EQ $index | Select-Object -First 1
        $device -and $device.ServiceName -ieq 'UsbNcm' -and $device.PNPDeviceID -match '^USB\\VID_2833&'
    } | Select-Object Name, InterfaceIndex, InterfaceGuid)
}

function Get-NightfallUsbIdentity([string]$Root) {
    $path = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $hash = [Security.Cryptography.SHA256]::Create()
    try { $id = ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($path.ToUpperInvariant())))).Replace('-', '').Substring(0, 20) }
    finally { $hash.Dispose() }
    [pscustomobject]@{ Root=$path; Group="Nightfall Meteor USB $id"; Prefix="Nightfall-Meteor-USB-$id"; Policy=(Join-Path $path 'usb-firewall.json') }
}

function Get-NightfallUsbSettings([string]$Root) {
    $identity = Get-NightfallUsbIdentity $Root
    $values = @{ HostHttpPort=47989; ProxyOffset=1000; ControlPort=47910; VideoPort=47911; TrackingPort=47912; EnableDepth=$false; EnableMicrophone=$false }
    if (Test-Path -LiteralPath $identity.Policy -PathType Leaf) {
        if ((Get-Item -LiteralPath $identity.Policy).Length -gt 64KB) { throw 'USB firewall settings exceed 64 KiB.' }
        $saved = Get-Content -LiteralPath $identity.Policy -Raw | ConvertFrom-Json
        if ($saved.format -ne 1 -or $saved.transport -ne 'USB') { throw 'Unsupported USB firewall settings; preserved the existing rules.' }
        foreach ($key in @($values.Keys)) {
            $property = $saved.settings.PSObject.Properties[$key]
            if ($null -eq $property) { throw "Missing USB firewall setting: $key" }
            if ($key -in @('EnableDepth', 'EnableMicrophone')) {
                if ($property.Value -isnot [bool]) { throw "Invalid Boolean setting: $key" }
            } elseif ($property.Value -isnot [int] -and $property.Value -isnot [long]) { throw "Invalid port setting: $key" }
            $values[$key] = $property.Value
        }
    }
    $values
}

function Get-NightfallUsbFirewallPlan {
    param([Parameter(Mandatory)][string]$Root, [hashtable]$Settings, [object[]]$Adapters)
    $identity = Get-NightfallUsbIdentity $Root
    if (-not $Settings) { $Settings = Get-NightfallUsbSettings $Root }
    foreach ($key in @('HostHttpPort', 'ProxyOffset', 'ControlPort', 'VideoPort', 'TrackingPort')) {
        if (-not $Settings.ContainsKey($key) -or $Settings[$key] -lt 1 -or $Settings[$key] -gt 65535) { throw "Invalid USB port setting: $key" }
    }
    $meteor = Join-Path $identity.Root 'nightfall-meteor.exe'
    $hostProgram = Join-Path $identity.Root 'runtime\host\sunshine.exe'
    foreach ($path in @($meteor, $hostProgram)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing installed program: $path" }
    }
    if (-not $PSBoundParameters.ContainsKey('Adapters')) { $Adapters = @(Get-NightfallUsbAdapters) }
    if (-not $Adapters.Count) { throw 'No Meta USB NCM adapter found. Connect the Quest, enable USB Link in Nightfall, then run USB setup again.' }
    $aliases = @($Adapters | ForEach-Object { [WildcardPattern]::Escape([string]$_.Name) } | Sort-Object -Unique)
    if ($aliases | Where-Object { [string]::IsNullOrWhiteSpace($_) }) { throw 'A USB adapter has no interface name.' }
    $base = [int]$Settings.HostHttpPort
    $offset = [int]$Settings.ProxyOffset
    $discovery = $base - 89
    $tcp = @($discovery, ($base+$offset-5), ($base+$offset), ($base+$offset+21), [int]$Settings.ControlPort, [int]$Settings.VideoPort, [int]$Settings.TrackingPort)
    $udp = @(($base+$offset+9), ($base+$offset+10), ($base+$offset+11))
    if ($Settings.EnableDepth) { $tcp += $discovery+1 }
    if ($Settings.EnableMicrophone) { $udp += @(($discovery+2), ($base+$offset+13)) }
    $hostTcp = @(($base-5), $base)
    $xr = @([int]$Settings.ControlPort, [int]$Settings.VideoPort, [int]$Settings.TrackingPort)
    $desktopTcp = @($discovery, ($base+$offset-5), ($base+$offset), ($base+$offset+21), ($base-5), $base)
    if ($Settings.EnableDepth) { $desktopTcp += $discovery+1 }
    if (($xr | Select-Object -Unique).Count -ne 3 -or @($xr | Where-Object { $desktopTcp -contains $_ }).Count) { throw 'XR ports must be distinct and must not overlap desktop TCP services.' }
    foreach ($port in @($tcp + $udp + $hostTcp)) { if ($port -lt 1 -or $port -gt 65535) { throw 'Derived USB port is outside 1..65535.' } }
    $specifications = @(
        @{ Suffix='Meteor-TCP'; Program=$meteor; Protocol='TCP'; Ports=$tcp },
        @{ Suffix='Meteor-UDP'; Program=$meteor; Protocol='UDP'; Ports=$udp },
        @{ Suffix='Host-TCP'; Program=$hostProgram; Protocol='TCP'; Ports=$hostTcp }
    )
    $rules = @($specifications | ForEach-Object {
        [pscustomobject]@{
            Name=($identity.Prefix+'-'+$_.Suffix); Group=$identity.Group
            DisplayName=('Nightfall USB '+$_.Suffix); Program=$_.Program; Protocol=$_.Protocol
            LocalPort=@($_.Ports | Sort-Object -Unique | ForEach-Object { [string]$_ })
            InterfaceAlias=$aliases; Profile='Any'; Direction='Inbound'; Action='Allow'; Enabled='True'
            LocalAddress='fe80::/10'; RemoteAddress='fe80::/10'; EdgeTraversalPolicy='Block'
        }
    })
    [pscustomobject]@{ format=1; transport='USB'; Root=$identity.Root; Group=$identity.Group; Policy=$identity.Policy; settings=$Settings; interfaces=$aliases; rules=$rules }
}

function Save-NightfallUsbSettings([string]$Root, [hashtable]$Settings) {
    $identity=Get-NightfallUsbIdentity $Root
    $saved=@{format=1;transport='USB';settings=$Settings}
    [IO.File]::WriteAllText($identity.Policy,($saved | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
}

function Test-NightfallUsbSameSet($Actual, $Expected) {
    (@($Actual | ForEach-Object { [string]$_ } | Sort-Object -Unique) -join '|') -ieq (@($Expected | ForEach-Object { [string]$_ } | Sort-Object -Unique) -join '|')
}

function Test-NightfallUsbFirewall {
    param([Parameter(Mandatory)]$Plan)
    $issues = @()
    foreach ($spec in $Plan.rules) {
        $rule = Get-NetFirewallRule -PolicyStore PersistentStore -Name $spec.Name -ErrorAction SilentlyContinue
        if (-not $rule) { $issues += "Missing rule $($spec.Name)"; continue }
        if ($rule.Group -ne $Plan.Group -or "$($rule.Enabled)" -ne 'True' -or "$($rule.Direction)" -ne 'Inbound' -or "$($rule.Action)" -ne 'Allow' -or "$($rule.Profile)" -ne 'Any' -or "$($rule.EdgeTraversalPolicy)" -ne 'Block') { $issues += "Unexpected policy for $($spec.Name)" }
        $application = $rule | Get-NetFirewallApplicationFilter
        $address = $rule | Get-NetFirewallAddressFilter
        $port = $rule | Get-NetFirewallPortFilter
        $interface = $rule | Get-NetFirewallInterfaceFilter
        if ($application.Program -ine $spec.Program) { $issues += "Wrong program for $($spec.Name)" }
        if (-not (Test-NightfallUsbSameSet $address.LocalAddress $spec.LocalAddress) -or -not (Test-NightfallUsbSameSet $address.RemoteAddress $spec.RemoteAddress)) { $issues += "Wrong IPv6 scope for $($spec.Name)" }
        if ("$($port.Protocol)" -ine $spec.Protocol -or -not (Test-NightfallUsbSameSet $port.LocalPort $spec.LocalPort) -or -not (Test-NightfallUsbSameSet $port.RemotePort 'Any')) { $issues += "Wrong ports for $($spec.Name)" }
        if (-not (Test-NightfallUsbSameSet $interface.InterfaceAlias $spec.InterfaceAlias)) { $issues += "USB adapters changed; run Repair USB Link for $($spec.Name)" }
    }
    [pscustomobject]@{ ok=($issues.Count -eq 0); transport='USB'; persistent=$true; interfaces=$Plan.interfaces; issues=$issues; rules=@($Plan.rules.Name) }
}

function Install-NightfallUsbFirewall {
    param([Parameter(Mandatory)]$Plan)
    foreach ($spec in $Plan.rules) {
        $existing = Get-NetFirewallRule -PolicyStore PersistentStore -Name $spec.Name -ErrorAction SilentlyContinue
        if ($existing -and $existing.Group -ne $Plan.Group) { throw "Firewall rule name is already owned by another policy: $($spec.Name)" }
    }
    foreach ($spec in $Plan.rules) {
        $parameters = @{}
        foreach ($property in $spec.PSObject.Properties) { if ($property.Name -ne 'Group') { $parameters[$property.Name] = $property.Value } }
        $parameters.Description = 'Persistent Nightfall USB only: exact application, Meta USB interfaces, IPv6 link-local peers and required ports. Repair after adding/renaming an adapter.'
        $parameters.PolicyStore = 'PersistentStore'
        if (Get-NetFirewallRule -PolicyStore PersistentStore -Name $spec.Name -ErrorAction SilentlyContinue) {
            $parameters.NewDisplayName=$parameters.DisplayName
            $parameters.Remove('DisplayName')
            Set-NetFirewallRule @parameters | Out-Null
        } else {
            New-NetFirewallRule @parameters -Group $Plan.Group | Out-Null
        }
    }
    $report = Test-NightfallUsbFirewall $Plan
    if (-not $report.ok) { throw ($report.issues -join '; ') }
    # Save only portable settings, never machine/headset IPs or interface names.
    Save-NightfallUsbSettings $Plan.Root $Plan.settings
    # Replace only the legacy installer's broad rules for these exact programs.
    foreach ($program in @($Plan.rules.Program | Select-Object -Unique)) {
        Get-NetFirewallApplicationFilter -Program $program -ErrorAction SilentlyContinue |
            Get-NetFirewallRule -ErrorAction SilentlyContinue |
            Where-Object { $_.Group -eq 'Nightfall Meteor' -and "$($_.Direction)" -eq 'Inbound' } |
            Remove-NetFirewallRule
    }
    $report
}

function Remove-NightfallUsbFirewall([string]$Root) {
    $identity = Get-NightfallUsbIdentity $Root
    foreach ($suffix in @('Meteor-TCP','Meteor-UDP','Host-TCP')) {
        $rule = Get-NetFirewallRule -PolicyStore PersistentStore -Name ($identity.Prefix+'-'+$suffix) -ErrorAction SilentlyContinue
        if ($rule -and $rule.Group -ne $identity.Group) { throw 'Refusing to remove a firewall rule owned by another policy.' }
        if ($rule) { $rule | Remove-NetFirewallRule }
    }
    if (Test-Path -LiteralPath $identity.Policy -PathType Leaf) { Remove-Item -LiteralPath $identity.Policy -Force }
    [pscustomobject]@{ ok=$true; removed=$true; transport='USB'; Root=$identity.Root }
}

Export-ModuleMember -Function Get-NightfallUsbAdapters, Get-NightfallUsbSettings, Save-NightfallUsbSettings, Get-NightfallUsbFirewallPlan, Test-NightfallUsbFirewall, Install-NightfallUsbFirewall, Remove-NightfallUsbFirewall

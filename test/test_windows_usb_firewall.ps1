# Run with Windows PowerShell's Pester. No real firewall mutations.
Import-Module (Join-Path $PSScriptRoot '..\meteor\tools\windows-release\UsbLink-Firewall.psm1') -Force
InModuleScope UsbLink-Firewall {
    Describe 'Portable USB firewall policy' {
        BeforeEach {
            $script:fixture = Join-Path (Get-PSDrive TestDrive).Root 'Install With Spaces'
            New-Item -ItemType Directory -Force (Join-Path $script:fixture 'runtime\host') | Out-Null
            Set-Content (Join-Path $script:fixture 'nightfall-meteor.exe') 'not executed'
            Set-Content (Join-Path $script:fixture 'runtime\host\sunshine.exe') 'not executed'
            Remove-Item -LiteralPath (Join-Path $script:fixture 'usb-firewall.json') -ErrorAction SilentlyContinue
            $script:adapters = @([pscustomobject]@{ Name='Quest Cable'; InterfaceIndex=71; InterfaceGuid=[guid]::NewGuid() })
        }
        It 'uses exact installed programs, USB interfaces and link-local peers' {
            $plan = Get-NightfallUsbFirewallPlan -Root $script:fixture -Adapters $script:adapters
            $plan.rules.Count | Should Be 3
            foreach ($rule in $plan.rules) {
                $rule.Program.StartsWith($script:fixture+'\') | Should Be $true
                $rule.InterfaceAlias | Should Be 'Quest Cable'
                $rule.LocalAddress | Should Be 'fe80::/10'
                $rule.RemoteAddress | Should Be 'fe80::/10'
                $rule.EdgeTraversalPolicy | Should Be 'Block'
                $rule.LocalPort -contains 'Any' | Should Be $false
            }
            ($plan.rules[0].LocalPort -join ',') | Should Be '47900,47910,47911,47912,48984,48989,49010'
            ($plan.rules[1].LocalPort -join ',') | Should Be '48998,48999,49000'
        }
        It 'does not depend on a PC address or headset address' {
            $plan = Get-NightfallUsbFirewallPlan -Root $script:fixture -Adapters $script:adapters
            ($plan.settings.Keys -join ',') | Should Not Match 'Address|Headset|Interface|Serial'
        }
        It 'round trips portable settings without machine identifiers' {
            $values=Get-NightfallUsbSettings $script:fixture
            $values.ControlPort=50110
            Save-NightfallUsbSettings $script:fixture $values
            (Get-NightfallUsbSettings $script:fixture).ControlPort | Should Be 50110
            $text=Get-Content (Join-Path $script:fixture 'usb-firewall.json') -Raw
            $text | Should Not Match 'Address|Interface|Serial|fe80|Root'
        }
        It 'handles several headsets with separately named adapters' {
            $more = @($script:adapters + [pscustomobject]@{Name='Ethernet 8';InterfaceIndex=80;InterfaceGuid=[guid]::NewGuid()})
            $plan = Get-NightfallUsbFirewallPlan -Root $script:fixture -Adapters $more
            ($plan.rules[0].InterfaceAlias -join ',') | Should Be 'Ethernet 8,Quest Cable'
        }
        It 'uses literal aliases rather than wildcarding other adapters' {
            $script:adapters[0].Name = 'USB [2]'
            $plan = Get-NightfallUsbFirewallPlan -Root $script:fixture -Adapters $script:adapters
            $pattern = [WildcardPattern]::new($plan.interfaces[0])
            $pattern.IsMatch('USB [2]') | Should Be $true
            $pattern.IsMatch('USB 2') | Should Be $false
        }
        It 'keeps a stable identity across case changes and repair' {
            $a = Get-NightfallUsbFirewallPlan -Root $script:fixture -Adapters $script:adapters
            $script:adapters[0].Name = 'Renamed USB'
            $b = Get-NightfallUsbFirewallPlan -Root $script:fixture.ToUpperInvariant() -Adapters $script:adapters
            $a.Group | Should Be $b.Group
            $a.rules[0].Name | Should Be $b.rules[0].Name
            $b.interfaces | Should Be 'Renamed USB'
        }
        It 'does not share rule ownership with another installation' {
            $a = Get-NightfallUsbIdentity $script:fixture
            $b = Get-NightfallUsbIdentity (Join-Path (Split-Path $script:fixture) 'Other Install')
            $a.Prefix | Should Not Be $b.Prefix
        }
        It 'refuses to fall back to Wi-Fi when USB is absent' {
            { Get-NightfallUsbFirewallPlan -Root $script:fixture -Adapters @() } | Should Throw 'No Meta USB NCM adapter found'
        }
        It 'identifies Meta hardware and UsbNcm instead of display names' {
            Mock Get-NetAdapter { @(
                [pscustomobject]@{Name='Different Language';InterfaceIndex=5;InterfaceGuid=[guid]::NewGuid()},
                [pscustomobject]@{Name='UsbNcm Host Device';InterfaceIndex=6;InterfaceGuid=[guid]::NewGuid()},
                [pscustomobject]@{Name='Ethernet';InterfaceIndex=7;InterfaceGuid=[guid]::NewGuid()}) }
            Mock Get-CimInstance { @(
                [pscustomobject]@{InterfaceIndex=5;ServiceName='UsbNcm';PNPDeviceID='USB\VID_2833&PID_0001'},
                [pscustomobject]@{InterfaceIndex=6;ServiceName='OtherDriver';PNPDeviceID='USB\VID_2833&PID_0002'},
                [pscustomobject]@{InterfaceIndex=7;ServiceName='UsbNcm';PNPDeviceID='USB\VID_1234&PID_0001'}) }
            $actual = @(Get-NightfallUsbAdapters)
            $actual.Count | Should Be 1
            $actual[0].Name | Should Be 'Different Language'
        }
        It 'derives ports for another host base and explicit XR ports' {
            $values=Get-NightfallUsbSettings $script:fixture
            $values.HostHttpPort=49989; $values.ProxyOffset=2000
            $values.ControlPort=50110; $values.VideoPort=50111; $values.TrackingPort=50112
            $values.EnableDepth=$true; $values.EnableMicrophone=$true
            $plan=Get-NightfallUsbFirewallPlan -Root $script:fixture -Settings $values -Adapters $script:adapters
            ($plan.rules[0].LocalPort -join ',') | Should Be '49900,49901,50110,50111,50112,51984,51989,52010'
            ($plan.rules[1].LocalPort -join ',') | Should Be '49902,51998,51999,52000,52002'
        }
        It 'rejects an overlapping XR port' {
            $values=Get-NightfallUsbSettings $script:fixture; $values.ControlPort=48989
            {Get-NightfallUsbFirewallPlan -Root $script:fixture -Settings $values -Adapters $script:adapters} | Should Throw 'XR ports must be distinct'
        }
        It 'rejects a derived port outside the valid range' {
            $values=Get-NightfallUsbSettings $script:fixture; $values.ProxyOffset=20000
            {Get-NightfallUsbFirewallPlan -Root $script:fixture -Settings $values -Adapters $script:adapters} | Should Throw 'outside 1..65535'
        }
        It 'refuses to create rules for missing programs' {
            Remove-Item -LiteralPath (Join-Path $script:fixture 'nightfall-meteor.exe')
            {Get-NightfallUsbFirewallPlan -Root $script:fixture -Adapters $script:adapters} | Should Throw 'Missing installed program'
        }
    }
}

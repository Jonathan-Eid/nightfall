param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$BridgeDirectory,
    [Parameter(Mandatory)][string]$Application,
    [Parameter(Mandatory)][string]$ProfileDirectory,
    [string[]]$ApplicationArguments=@(),
    [uint32]$SteamAppId=0
)
$ErrorActionPreference='Stop'
$runtime=(Resolve-Path -LiteralPath $RuntimeDirectory).Path
$bridge=(Resolve-Path -LiteralPath $BridgeDirectory).Path
$app=(Resolve-Path -LiteralPath $Application).Path
foreach($path in @((Join-Path $runtime 'virtualdesktop-openxr.json'),(Join-Path $runtime 'virtualdesktop-openxr.dll'),
    (Join-Path $runtime 'LibOVRRT64_1.dll'),(Join-Path $bridge 'meteor-ovr-bridge.dll'),
    (Join-Path $bridge 'meteor-qsv.dll'),(Join-Path $bridge 'meteor-windows-tracking.dll'),(Join-Path $bridge 'meteor-ovr-service.exe'))){
    if(-not (Test-Path -LiteralPath $path -PathType Leaf)){throw "Missing backend file: $path"}
}
$profile=[IO.Path]::GetFullPath($ProfileDirectory)
$path=Join-Path $profile 'xr-windows.json'
if(Test-Path -LiteralPath $path){throw 'A profile already exists here; choose a new directory to preserve it'}
$config=[ordered]@{
    service=(Join-Path $bridge 'meteor-ovr-service.exe')
    manifest=(Join-Path $runtime 'virtualdesktop-openxr.json')
    encoder=(Join-Path $bridge 'meteor-ovr-bridge.dll')
    ovr_bridge=(Join-Path $bridge 'meteor-ovr-bridge.dll')
    tracking=(Join-Path $bridge 'meteor-windows-tracking.dll')
    application=$app
    arguments=@($ApplicationArguments)
    library_dirs=@($runtime,$bridge)
    working_directory=(Split-Path $app)
    async_delivery=$true
}
if($SteamAppId){$config.steam_app_id=[string]$SteamAppId}
New-Item -ItemType Directory -Path $profile -Force | Out-Null
[IO.File]::WriteAllText($path,($config | ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
"Created $path. Register this profile with Meteor to select Nightfall / VDXR."

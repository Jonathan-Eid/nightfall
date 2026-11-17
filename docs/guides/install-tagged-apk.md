# Install a tagged Nightfall APK on Quest

The **Android tag APK** workflow builds ARM64 APKs on pushes of version tags
such as `v0.9.2` or `v0.9.2-preview.1`. Download the artifact from the successful
run under **GitHub → Actions → Android tag APK → Artifacts**, then extract its
ZIP. It contains the unsigned APK, `SHA256SUMS`, and these instructions.

## Sign the unsigned APK locally

An unsigned APK cannot be installed by ADB or SideQuest. Keep your signing key
private and use the same key for subsequent builds. A new key cannot update an
app installed with a different key. These instructions never uninstall an app:
uninstalling clears its settings and pairing data.

Install Android SDK Build-Tools and Platform-Tools from
[Android's official tools](https://developer.android.com/tools), and Java 17.
The commands below are for PowerShell; change paths, filenames, and key alias
to match your installation. Run them from the extracted artifact folder.

```powershell
$sdk = "$env:LOCALAPPDATA\Android\Sdk"
$tools = Join-Path $sdk 'build-tools\36.1.0'
$adb = Join-Path $sdk 'platform-tools\adb.exe'
$unsigned = (Get-Item '.\Nightfall-*-arm64-v8a-unsigned.apk').FullName
Get-FileHash -LiteralPath $unsigned -Algorithm SHA256
```

Compare the hash with `SHA256SUMS`. For an **update**, use the keystore and alias
that signed the installed Nightfall app. If you do not have its key, stop and
use a build signed by that app's publisher; creating another key will not fix
an update signature mismatch.

For a **first installation only**, create and keep your own keystore if you
do not already have one. `keytool` prompts for passwords; do not put passwords
in a command, commit, or chat. Choose a private backup location for the file.

```powershell
keytool -genkeypair -keystore "$env:USERPROFILE\nightfall-local.jks" `
  -alias nightfall -keyalg RSA -keysize 3072 -validity 10000
```

Sign and verify, substituting your existing keystore/alias when appropriate:

```powershell
& "$tools\zipalign.exe" -P 16 -f -v 4 $unsigned '.\Nightfall-aligned.apk'
if ($LASTEXITCODE) { throw 'APK alignment failed' }
& "$tools\apksigner.bat" sign --ks "$env:USERPROFILE\nightfall-local.jks" `
  --ks-key-alias nightfall --out '.\Nightfall-signed.apk' '.\Nightfall-aligned.apk'
if ($LASTEXITCODE) { throw 'APK signing failed' }
& "$tools\apksigner.bat" verify --verbose '.\Nightfall-signed.apk'
if ($LASTEXITCODE) { throw 'APK signature verification failed' }
```

## Install with ADB

ADB installation **is sideloading**. On the Quest, enable developer mode using
[Meta's device setup instructions](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/).
Connect a USB data cable, put on the headset, and authorize USB debugging for
your PC when prompted.

```powershell
& $adb devices
& $adb install -r '.\Nightfall-signed.apk'
```

If the device says `unauthorized`, accept the debugging prompt inside the
headset. With multiple devices, use `adb -s YOUR_QUEST_SERIAL install -r ...`.
After a successful install, open **Library → Unknown Sources → Nightfall**.
These tagged release APKs use package `app.nightfall.quest`. The separate
**Nightfall USB Test** package used in development is a different app and
retains its own settings and pairing.

If installation reports `INSTALL_FAILED_UPDATE_INCOMPATIBLE`, the signing keys
differ: use the matching key/build. If it reports `VERSION_DOWNGRADE`, use an
equal or newer version. Do not uninstall to bypass either error.

## SideQuest alternative

SideQuest offers a graphical sideloading interface. Enable developer mode and
USB debugging as above, connect the Quest, and use its APK installation option
to select **Nightfall-signed.apk**. It does not make the unsigned artifact
installable or bypass Android's signing/update rules. On a PC already using
ADB, the two ADB commands above are sufficient.

## Scope of this artifact

This is the normal Android release configuration built from the tagged source,
with signing omitted. Optional development-only PCVR/ALVR inputs are not enabled
by this workflow. The locally patched Nightfall USB Test APK and its signing key
are not copied or uploaded by CI.

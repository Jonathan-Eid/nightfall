Nightfall Meteor for Windows x64
================================

Keep this folder together and run nightfall-meteor.exe. Its tray icon opens
the depth model, rate, microphone, and startup controls. Meteor starts with
EdgePad 512x288. Video Depth Anything is offered as a verified download in
the tray's Depth model menu; it is not included in this package.

Requirements: Windows 10/11 x64, a Vulkan-capable NVIDIA or AMD GPU and current driver.
Experimental Intel Arc / standalone VDXR setup is documented in the source
repository's docs/plans/active/intel-arc-vdxr.md and
docs/guides/meteor-usb-pcvr.md. That backend requires the matching source-built
runtime; this package's USB firewall helper does not install it by itself.
The installer includes the built-in desktop host, Windows XR runtimes, and
app-local Microsoft C++ runtime DLLs. An external Sunshine-compatible host is
optional. VB-CABLE is needed
only for the headset microphone; choose CABLE Output in the recording app.
On AMD, Meteor uses D3D11VA for video decoding and ncnn Vulkan for EdgePad.
The included FFmpeg DLLs are required for this path. NVIDIA keeps its NVDEC
path. Video Depth Anything's TensorRT download requires NVIDIA; the Vulkan
VDA option requires compatible model graphs.

The installer creates Start menu and uninstall entries for the current Windows
user and configures Windows Firewall with administrator approval. For USB,
connect the Quest and enable Nightfall's USB Link before setup, then select
"Restrict firewall access to Quest USB Link" in the installer. This creates
persistent rules for these installed programs, the detected Meta USB NCM
adapters, required ports and IPv6 link-local peers. It grants no Wi-Fi access
and does not pin a particular PC/headset IP or expire on a timer. Pairing is
still required. Without that option, the existing LAN firewall setup is used.

Portable folder: run powershell.exe -NoProfile -ExecutionPolicy Bypass -File
.\Firewall-UsbLink.ps1 once to install USB rules. Use -Action Plan to preview,
-Action Check to inspect, -Action Repair after adding a headset/renaming an
adapter, and -Action Remove to undo them. Installed users can choose "Repair
Nightfall USB Link" from Start. Each installation owns its own rules; uninstall
removes those rules and preserves pairing data. USB setup requires Windows'
UsbNcm driver to be available; it does not install a network driver.

Setup offers the built-in host and PCVR. It discovers installed VR games from
Steam's VR application manifest across your Steam libraries. SteamVR handles
every VR game, as other PC streamers do (Linux uses Monado and xrizer instead,
with no SteamVR driver to rely on). It encodes with NVENC on
NVIDIA GPUs and AMF on AMD GPUs; older AMD encoders stop at 4096 pixels per
side, so the headset sends smaller eyes to fit. Install SteamVR through Steam. PCVR setup registers Meteor's bundled
SteamVR driver automatically. SteamVR and games are not redistributed.
After installing a game, quit Meteor and use Set up Nightfall Meteor in Start.
Run Steam first so it can refresh its VR application list. PCVR setup can
enable the SteamVR connection even before any game profiles are discovered.
To pair, select this PC in Nightfall, then choose Game streaming host > Pair
headset... in Meteor's tray (or Pair a Nightfall headset from Start). Enter
the four-digit headset PIN; no host web login is needed. Finish pairing in
Nightfall, then compare and approve the PCVR
headset code in Meteor's tray. VR Apps launches need no Steam launch options.
For direct Moss launches from Steam, use the line in the PCVR profiles folder's
steam-launch-options.txt as Moss's Steam launch options. VRChat uses the
registered SteamVR driver and needs no Meteor launch options, as do other
SteamVR games. Existing Steam
settings are not automatically overwritten by the installer.
Setup diagnostics: AppData\Local\Nightfall Meteor\setup-summary.json.
The built-in multi-monitor backend currently supports SDR displays on one GPU.

Settings and state are stored in AppData\Roaming\Nightfall Meteor. Downloads,
models, the pairing key, and meteor.log are in AppData\Local\Nightfall
Meteor. Keep meteor.key if you move or reinstall Meteor: the Quest trusts
that key. Uninstall leaves these data folders in place. The tray has an Open
log item for troubleshooting.

The source and licence are at https://github.com/tB0nE/nightfall . See
share\doc\nightfall-meteor\LICENSE and THIRD_PARTY_NOTICES.txt in this package.

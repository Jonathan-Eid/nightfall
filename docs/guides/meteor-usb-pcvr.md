# Meteor PCVR over USB

USB Link provides an IPv6 network over the Quest's cable. Connecting ADB or
enabling file transfer alone does not move a Nightfall stream onto it. Both
the client USB control changes and the host scope-preservation changes are
required for this experimental PCVR path. See the
[Arc/VDXR checkpoint](../plans/active/intel-arc-vdxr.md) for build requirements
and measured results.

## Connect

For ADB sideloading of a development APK, enable Quest developer mode and USB
debugging, then authorize the PC in the headset. Meta Quest Developer Hub is
optional; standalone Android platform-tools work. Nightfall's standalone
PCVR runtime does not require Meta Dash or a Horizon Link session.

1. Build and install the updated Android client using `BUILD.md`. Preserve an
   existing installation's signing key, or use the separate development
   package from the export preset. Do not uninstall to bypass a signature
   mismatch: that removes saved pairings.
2. Enable Nightfall's **USB Link** setting with the Quest connected by a USB 3
   data cable. Windows should show a connected **UsbNcm Host Device** network
   adapter. USB Link is separate from Meta Horizon Link.
3. Run the updated Meteor host with the registered Windows XR profile. The
   default XR control listener is `[::]:47910`, accepting IPv4 and IPv6. If an
   existing `meteor.toml` explicitly sets `xr.listen = "0.0.0.0:47910"`, change
   that value to `[::]:47910` and restart Meteor. Binding a specific USB
   address also needs its Windows interface index and requires the adapter
   to exist when Meteor starts.
4. Install the persistent USB firewall policy described below. The Windows
   installer offers **Restrict firewall access to Quest USB Link**; portable
   users can run the same helper from the extracted host folder. It does not
   depend on either device's IP address.
5. Select or pair the PC over its USB address. If automatic discovery does not
   find it, use its link-local IPv6 address with the Quest-side zone `%usb0`,
   for example `fe80::1234%usb0`. The PC's numeric interface index belongs on
   the PC; it is not the zone to enter on the headset.
6. Launch the registered OpenXR application through Meteor's normal broker.
   Keep the same resolution, bitrate and refresh rate when comparing USB
   with Wi-Fi.

## Install, repair and remove the USB firewall policy

Run this from the extracted Meteor host folder containing
`nightfall-meteor.exe`, `runtime/host/sunshine.exe`, `Firewall-UsbLink.ps1` and
`UsbLink-Firewall.psm1`. Connect the Quest and enable Nightfall USB Link first:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Firewall-UsbLink.ps1
```

Accept Windows UAC once. The rules are persistent and have no expiry task.
The helper detects **Meta USB devices using the UsbNcm driver**, independently
of a localized adapter name or a fixed interface index. Each rule combines:

- the full installed path of Meteor or its bundled host;
- the detected USB adapter names, matched literally;
- the exact TCP/UDP port list;
- IPv6 link-local source and destination addresses (`fe80::/10`);
- blocked edge traversal.

Rules apply to all Windows network profiles **on those USB interfaces only**,
so switching the USB adapter between Public and Private does not break Link.
They do not allow Wi-Fi, Ethernet LAN, routed peers or arbitrary ports. A new
headset's link-local IP needs no rule update. If Windows creates a new adapter
for that headset, or an adapter is renamed, use **Repair Nightfall USB Link**
from Start, or rerun:

```powershell
.\Firewall-UsbLink.ps1 -Action Repair
```

Repair redetects adapters and updates the same three rules. It does not add
duplicates or ask for IP addresses. A failed setup retains USB mode so the
tray's firewall repair cannot silently replace it with LAN allowances.
`usb-firewall.json` stores the USB choice and port options beside the installed
program; it contains no PC/headset IP, serial number, saved adapter name or
pairing secret. Do not manually copy keys or host pairing state between PCs.

The following commands need no administrator rights:

```powershell
.\Firewall-UsbLink.ps1 -Action Plan  | ConvertTo-Json -Depth 8
.\Firewall-UsbLink.ps1 -Action Check | ConvertTo-Json -Depth 5
```

Remove just this installation's USB rules with:

```powershell
.\Firewall-UsbLink.ps1 -Action Remove
```

The Windows uninstaller performs that removal automatically and preserves
headset pairing data. Separate install folders have separate rule identities;
removing one does not remove another's rules. Additional firewall allowances
created manually or by other software remain separate and should be reviewed
if a USB-only policy is required. Managed firewall/GPO block rules are not
overridden.

For custom ports, pass `-HostHttpPort`, `-ProxyOffset`, `-ControlPort`,
`-VideoPort` and `-TrackingPort` matching Meteor's settings. The helper saves
them for repair and validates derived port ranges/overlaps. Depth and
microphone ports are opt-in with `-EnableDepth` and `-EnableMicrophone`.

The defaults are:

| Program | Protocol | Local ports | Purpose |
| --- | --- | --- | --- |
| `nightfall-meteor.exe` | TCP | 47900, 47910, 47911, 47912, 48984, 48989, 49010 | Discovery, XR control/video/tracking and desktop proxy |
| `nightfall-meteor.exe` | UDP | 48998, 48999, 49000 | Desktop video/control/audio during connection and handover |
| bundled `sunshine.exe` | TCP | 47984, 47989 | Initial host pairing and host API |

These cover the tested path with microphone/depth disabled. Discovery via
mDNS can require separate multicast permissions; entering the USB address
avoids relying on mDNS. The helper does not modify Windows' system-wide mDNS
rules. The earlier per-IP trial rules and their cleanup timer are superseded
by this persistent installer-managed policy.

The source installer now includes the USB option, helper and repair shortcut.
An updated compiled installer has not been validated in this development
session; the portable helper commands above were tested directly on Windows.

## Development checks

```powershell
Invoke-Pester .\test\test_windows_usb_firewall.ps1
# Elevated; creates inert fixture programs and removes every fixture rule:
.\test\check_windows_usb_firewall.ps1 -Result C:\path\to\usb-firewall-result.json
```

The policy suite covers differing install locations, adapter names, multiple
headsets, custom ports and absent USB hardware. The Windows integration check
installs twice without duplication, detects and repairs changed filters,
isolates two installations, and verifies uninstall cleanup. It does not run
the fixture executables or change the real installation's rules.

## Verify the route

While the XR session is active, run:

```powershell
Get-NetAdapter | Where-Object InterfaceDescription -Like '*UsbNcm*'
Get-NetTCPConnection -LocalPort 47910,47911,47912 -State Established |
    Select-Object LocalAddress,LocalPort,RemoteAddress,OwningProcess
```

All XR connections must use the USB adapter's link-local IPv6 address,
with its interface scope. A session still showing the PC/Quest Wi-Fi IPv4
addresses is still using Wi-Fi. An adapter reporting **Up** is insufficient.
For a physical confirmation, temporarily turn off headset Wi-Fi after
connecting and verify that the XR session keeps rendering and tracking;
restore Wi-Fi afterwards.

With the updated Windows QSV bridge and Android client, a fourth authenticated
connection carries stereo audio on the existing video port (47911 by default).
It needs no additional firewall rule. Hidden desktop streaming is stopped once
this output is ready. Showing the desktop reconnects it; returning to the game
stops it again. Hosts without this audio capability keep desktop streaming for
sound. No ADB command or manual audio-device change is needed for this switch.

At 75% Quest 2 resolution, the November 16 test negotiated 1376×1440 per eye
at 72 Hz and submitted approximately 71 FPS over USB. Both 3,600-frame samples
delivered every packet without transport failure. USB reduced the transport
bottleneck in this scene; application rendering, encoding and headset
presentation still determine performance in other workloads.

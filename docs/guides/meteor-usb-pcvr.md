# Meteor PCVR over USB

USB Link provides an IPv6 network over the Quest's cable. Connecting ADB or
enabling file transfer alone does not move a Nightfall stream onto it. Both
the client USB control changes and the host scope-preservation changes are
required for this experimental PCVR path. See the
[Arc/VDXR checkpoint](../plans/active/intel-arc-vdxr.md) for build requirements
and measured results.

## Connect

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
4. Allow the required traffic through Windows Firewall on the USB interface.
   Use rules scoped to the exact program, interface, current network profile,
   PC USB IPv6 address and Quest USB IPv6 address. Do not disable the firewall.
5. Select or pair the PC over its USB address. If automatic discovery does not
   find it, use its link-local IPv6 address with the Quest-side zone `%usb0`,
   for example `fe80::1234%usb0`. The PC's numeric interface index belongs on
   the PC; it is not the zone to enter on the headset.
6. Launch the registered OpenXR application through Meteor's normal broker.
   Keep the same resolution, bitrate and refresh rate when comparing USB
   with Wi-Fi.

The test used these default ports; custom host base ports change the proxy
ports. Restrict inbound rules by program as well as by the ports below.

| Program | Protocol | Local ports | Purpose |
| --- | --- | --- | --- |
| `nightfall-meteor.exe` | TCP | 47900, 47910, 47911, 47912, 48984, 48989, 49010 | Discovery, XR control/video/tracking and desktop proxy |
| `nightfall-meteor.exe` | UDP | 48998, 48999, 49000 | Desktop video/control/audio during connection and handover |
| bundled `sunshine.exe` | TCP | 47984, 47989 | Initial host pairing and host API |

These cover the tested path with microphone/depth disabled. Discovery via
mDNS can require separate, equally scoped multicast permissions; entering
the USB address avoids relying on mDNS. Test rules should have a documented
expiry or removal procedure. The physical trial's temporary rules were
scheduled for removal at the end of the test window.

## Verify the route

While the XR session is active, run:

```powershell
Get-NetAdapter | Where-Object InterfaceDescription -Like '*UsbNcm*'
Get-NetTCPConnection -LocalPort 47910,47911,47912 -State Established |
    Select-Object LocalAddress,LocalPort,RemoteAddress,OwningProcess
```

All three connections must use the USB adapter's link-local IPv6 address,
with its interface scope. A session still showing the PC/Quest Wi-Fi IPv4
addresses is still using Wi-Fi. An adapter reporting **Up** is insufficient.
For a physical confirmation, temporarily turn off headset Wi-Fi after
connecting and verify that the XR session keeps rendering and tracking;
restore Wi-Fi afterwards.

At 75% Quest 2 resolution, the November 16 test negotiated 1376×1440 per eye
at 72 Hz and submitted approximately 71 FPS over USB. Both 3,600-frame samples
delivered every packet without transport failure. USB reduced the transport
bottleneck in this scene; application rendering, encoding and headset
presentation still determine performance in other workloads.

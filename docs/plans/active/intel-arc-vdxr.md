# Intel Arc encoding and the Nightfall / VDXR backend

## Implementation

Windows PCVR can encode on Intel with oneVPL/QSV. The SteamVR direct/mirror
encoders select QSV on an Intel D3D11 device; the Monado Vulkan bridge selects
QSV for an Intel Vulkan device. The Vulkan path shares a D3D11 texture on the
same adapter, identified by LUID. Both paths convert RGBA/BGRA to limited-range
BT.709 NV12 on the GPU, import it into oneVPL, and synchronously retrieve a
compressed packet. They do not read uncompressed pixels back to the CPU.

The QSV implementation derives from the MIT-licensed
[Quest Link Arc project](https://github.com/Jonathan-Eid/quest-link-intel-arc).
It includes its attribution and license in `test/qsv/`. It uses no Meta runtime
patches or binaries. The pinned MIT-licensed oneVPL dispatcher is linked into `meteor-qsv.dll`;
the Intel graphics driver supplies the hardware runtime. Unsupported drivers
fail capability probing instead of using software.
Pinned headers: Intel libvpl `d77f9195cf495b937631607333288fd917ae8939`.

H.264 and HEVC are advertised only after hardware initialization succeeds.
Dimension limits come from the selected adapter's NV12/D3D11 capability records,
with the smaller limit when advertising both codecs. The test Arc 140V reports
4096×4096 for that common limit. An over-wide stereo offer is refused.
QSV uses one slice, async depth one, no frame reordering, repeated headers,
and explicit color signaling. HEVC generalized B pictures are disabled.
Foveation and live bitrate changes are unavailable. Reconnect to change bitrate.

AMF and QSV share the existing Vulkan packing loop; NVIDIA/AMD selection priority
is preserved. Both NVIDIA/AMF/QSV SteamVR and Vulkan bridge builds were compiled
on Windows. AMD/NVIDIA execution was not tested on this Intel-only PC.

## OpenXR integration

The matching branch of
[Jonathan-Eid/VirtualDesktop-OpenXR](https://github.com/Jonathan-Eid/VirtualDesktop-OpenXR)
adds an explicitly selected OVRNull transport provider. Its standalone runtime
loads Nightfall's `meteor-ovr-bridge.dll` via the per-process
`NIGHTFALL_OVR_BRIDGE` environment variable. The provider supplies the Intel
adapter LUID, negotiated dimensions/refresh, actual headset optics, head/hand
samples, buttons and haptics. OVR projection and quad layers are composed into
a stereo D3D11 texture, then encoded and sent through Meteor's existing bounded
packet queue, acknowledgments and END protocol. Tracking freshness and transport
connection state are separate, and eye-level/floor-level conversions preserve
the headset's adopted STAGE origin.

The Nightfall compositor applies straight/premultiplied/opaque alpha during each
draw instead of mutating a shared source image repeatedly. Quad eye-visibility
flags are preserved. This backend uses synchronous OVR submission plus Meteor's
bounded asynchronous transport, and explicitly transfers the application's
display timestamp for projection, quad-only and empty frames. Grip and aim poses
come directly from Nightfall's OpenXR samples rather than applying vendor raw
Touch-origin offsets a second time.

This uses VDXR as Nightfall's OpenXR backend. It does not add an Intel encoder to
the proprietary Virtual Desktop Streamer. Runtime selection is per application;
the setup below does not alter the global OpenXR registry.

Meteor's Windows catalog and tray expose **Nightfall / VDXR (experimental)** as
a third backend. Profiles include a host-local `ovr_bridge` DLL path and may not
combine it with a SteamVR/OpenVR backend. Headsets continue selecting registered
application IDs; they cannot supply DLL/executable paths.

## Build and configure

Use the native Windows prerequisites in
[the development guide](../../guides/meteor-windows-xr.md). Build Release/x64.
The new standalone native targets are:

```powershell
cmake -S test/qsv -B build/qsv -A x64
cmake --build build/qsv --config Release
cmake -S test/ovr-bridge -B build/ovr -A x64
cmake --build build/ovr --config Release
```

`test/qsv` fetches the pinned oneVPL source unless `ONEVPL_INCLUDE` points to
its `api/` directory. The dispatcher is linked statically; no separate system-wide
`libvpl.dll` installation is needed.
The bridge transport currently uses Vulkan headers/loader for its shared ABI,
so a Vulkan SDK is also required at build time.

Build the standalone `virtualdesktop-openxr` and `OVRNull` Release/x64 projects
from the matching fork branch after initializing its submodules and restoring
NuGet packages. Put `LibOVRRT64_1.dll` beside `virtualdesktop-openxr.dll` and its
JSON manifest. Keep the four bridge outputs together in `build/ovr/Release`.

```powershell
./test/configure_windows_vdxr.ps1 -RuntimeDirectory C:/path/to/vdxr/bin/x64/Release `
    -BridgeDirectory C:/path/to/nightfall/build/ovr/Release `
    -Application C:/path/to/native-openxr-game.exe `
    -SteamAppId 123456 -ProfileDirectory C:/path/to/new-game-profile
```

Use the actual game's Steam ID. The helper refuses to overwrite a profile and
only writes its descriptor. Register it with the updated Meteor build using
`--xr-windows-register --root <catalog> --profile <profile> --name <game name>`,
then enable PCVR as described in the Windows guide. The tray can select the
registered backend. This profile is for native OpenXR applications; OpenVR-only
games still need a separate compatibility path.

The regular Windows staging script now includes `meteor-qsv.dll` for its
Monado/SteamVR encoders. Packaging the optional VDXR backend into the normal
downloadable installer remains separate work; the above is a source-build setup.

## Validation and remaining acceptance

On November 15, 2026, Arc 140V, driver 32.0.101.8626, Windows 11:

- Native QSV tests encoded 72 frames each at 2560×1344: BGRA/H.264 and RGBA/HEVC.
  Independent decoding checked dimensions, every frame, changing RGB order,
  midpoint IDR, no B pictures, and limited-range BT.709/sRGB signaling.
- The real Khronos OpenXR loader and modified VDXR runtime ran the existing
  Vulkan application probe through OVRNull, the D3D11 QSV bridge and Meteor's
  production wire validator: 24 frames at 2560×1344 for H.264 and HEVC, plus a
  24-frame H.264 projection/quad-only/multiple-quad trial. END was acknowledged.
  Decoded quad pixels check geometry, alpha brightness and left/right visibility;
  packet delivery alone is not the quad acceptance check.
  The test checks the independent synthetic floor pose survives OpenXR and
  the transport metadata without an added eye-height offset.
- Meteor's Windows Rust suite passed 184 tests (20 existing ignored integration
  tests). The native tracking socket test checked both controllers, haptic
  acknowledgments, stale rejection, recovery and disconnection. SteamVR geometry
  and bootstrap checks passed. Native compilation includes the AMF headers.

Repeatable native tests: `test/windows_qsv_encode_test.cpp` and
`test/smoke_windows_vdxr.py --help`. The latter takes the standalone runtime
directory, a build directory containing `probe/`, `ovr/` and `wire-sink.exe`, and
a new output directory. It writes `summary.json` and retains matching logs and
bitstream. The wire sink is built from `test/windows_xr_wire_sink.rs`.
The smoke test's `--quad` pixel checks require the test-only Python `av` package.

Those checks use synthetic tracking. On November 16, 2026, the stock
0.9.1-preview.1 Quest 2 client also paired with the Windows host and entered the
standalone VDXR session using real headset tracking. The user confirmed the 3D
scene was visible and smoother after the pacing change. A 3,600-frame GPU scene
trial drained all 3,600 packets without transport failure and acknowledged END.

At 100% Quest 2 resolution (1840×1920 per eye after alignment), the synchronous
path delivered about 35 FPS before, and about 45 FPS after, the matching VDXR
change to consume refresh ticks that arrived while rendering/encoding. These
are single headset trials, not controlled benchmarks or a stable 72 FPS claim.
The current client/control/wire protocol accepts only 20/60/72 Hz; enabling
90 Hz requires coordinated client, host, transport and runtime changes.

For physical performance trials, `test/windows_vdxr_gpu_probe.py` can wrap the
native probe as a registered application's Python script. Pass `--build` with
the directory containing `probe/`, plus a bounded `--frames` and `--timeout`.
Keep the normal session-specific environment supplied by Meteor. This wrapper
enables GPU measurement mode: without `METEOR_XR_GPU_MEASURE=1`, the native probe
generates CPU references, reads images back and appends raw eye images to disk,
which makes it unsuitable for measuring streaming performance.

Pair the stock client with the host's normal HTTP/HTTPS ports before streaming
through Meteor's proxy. A firewall restricted to the proxy alone prevents the
initial PIN exchange. Scope host pairing and proxy rules separately by program,
protocol, required ports, network interface/profile and client address.

### USB validation, November 16

The stock client supported USB desktop streaming, but its XR control path
rejected a zoned IPv6 address and used Godot's `StreamPeerTCP`, which cannot
retain the interface zone. Automatic XR also compared discovery against only
the saved Wi-Fi addresses. The client now uses the existing `NightfallTcpPeer`
for scoped USB control and checks the host address actually selected by the
streaming backend. The host retains the accepted socket's IPv6 scope when
binding video/tracking lanes. Its default control listener accepts IPv4 and
IPv6; an explicit listen address continues to restrict the listener.

A separate local `Nightfall USB Test` package was installed beside the stock
client, retaining the stock client's data and pairing. This diagnostic package
reused the signed release's existing engine/native assets, replaced the two
changed GDScript assets and their sparse-pack index entries, and was signed
locally under a different package ID. It is a local test artifact, not a full
Android source build or a published release. Normal builds use `BUILD.md`.

At 75% Quest 2 resolution, the negotiated eyes were 1376×1440, a packed
2752×1440 H.264 frame, targeting 72 Hz. The earlier Wi-Fi trial submitted
56.2 FPS. Two USB trials submitted 71.2 and 71.1 FPS; each delivered all 3,600
packets, acknowledged completion and reported zero transport failures. The
second trial ran entirely with Quest Wi-Fi disabled. Live TCP connections on
47910/47911/47912 used the USB adapter's scoped link-local IPv6 addresses.
Quest Wi-Fi was restored afterwards. These are sequential trials of a GPU
test scene, not game benchmarks or a motion-to-photon measurement.

The final USB patch passed 189 Windows Rust tests (20 existing ignored),
including IPv4 and IPv6 lane connectivity, dual-stack listener behavior and
scope preservation; all 12 address validation cases also passed inside the
Quest app. See the
[USB PCVR setup and checks](../../guides/meteor-usb-pcvr.md).

Motion-to-photon latency, comfort, controller calibration/haptics,
additional games, other Intel GPUs, Linux Intel, AV1, 90 Hz and conformance
remain unvalidated.


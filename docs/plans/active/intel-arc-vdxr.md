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

The temporary trial firewall was subsequently replaced with the reusable
`Firewall-UsbLink.ps1` / `UsbLink-Firewall.psm1` policy. It discovers Meta USB
NCM interfaces, derives exact program paths and ports, and allows link-local
IPv6 peers on those interfaces without pinning machine/headset addresses.
The rules persist; repair redetects new/renamed adapters, and uninstall removes
only that installation's rules. USB mode remains selected if setup fails.
The stock firewall checker/repair entry point honors the saved USB mode.
Thirteen policy tests passed, and elevated Windows integration verified
idempotent install, filter repair, ownership across two install directories
and removal. On the physical setup the six trial rules and expiry task were
removed; a fresh USB discovery request succeeded while the Wi-Fi request
timed out. Installer source includes the option and repair shortcut; compiling
the updated installer was not validated in this session.
After reconnecting under the permanent policy, another 3,600-frame scene
submitted 71.2 FPS, delivered every packet without transport failure and
acknowledged END.

Motion-to-photon latency, comfort, controller calibration/haptics,
additional games, other Intel GPUs, Linux Intel, AV1, 90 Hz and conformance
remain unvalidated.

### Open Brush pacing and motion artifacts, November 16

The separate Quest test package now also contains a source-built
`nightfall-xr` extension. Its Godot engine and other native libraries still
come from the existing APK; this remains a diagnostic repack, not a full
Android source build. Pairing and the stock package were preserved.

The decoder polls output while waiting on the video socket at 1 ms rather
than 10 ms. ImageReader acquisition uses FIFO with a two-refresh-period
receipt-age limit and catches up to the newest image after a stall. The
MediaCodec Surface has automatic frame dropping disabled so it cannot
silently discard queued images before FIFO acquisition. The GL acquire and
copy fences remain nonblocking on the rendering thread. Local monotonic
histograms measure receipt, codec output and image selection; they do not
measure photons or compare unsynchronized PC/Quest clocks.

Open Brush 2.32.0 exposed Unity's preliminary device-detection instance.
The bridge now retains a connected, never-submitted tracking backend across
that bootstrap gap, without retaining an encoder or submitted texture.
The Nightfall runtime omits unsupported depth/audio extensions and retains
its session when transitioning from headless detection. Asynchronous layer
submission, a first-tick readiness handshake and deferred frame waiting let
application rendering overlap the previous encode. D3D11 commands are flushed
before waiting for the submission worker. QSV still waits for GPU completion
before importing its NV12 surface, using a D3D11 fence/event instead of CPU
polling. Timestamp queries measured conversion itself at about 40 microseconds
in one trial; most of that stage's wait was preceding application GPU work.

At 2752×1440 H.264/32 Mbps/72 Hz, a 21-second Open Brush sample reached
70.7 projection copies per second with a 15 ms cumulative p95 selection
interval. A subsequent 120-second sample fell to 49.3 copies per second,
while the client loop remained near 71.8 Hz. Those sequential, uncontrolled
samples are evidence of remaining workload/pacing variability, not stable
72 FPS. Reducing the PC mirror to 640×360 left the headset resolution intact.
The user reported smoother movement but doubling while turning their head.

A changing stereo pattern then reproduced a host compositor bug that static
geometry tests missed: OVRNull returned the previous writable image index
instead of the next one in its three-image ring. Both eyes' encoded pixels
lagged their submitted frame identity by one frame, while metadata described
the current render pose. The first image was black. The corrected index is
`(lastCommittedIndex + 1) % length`; static single-image chains still return
zero. All 12 images failed the new identity check before the correction;
24/24 H.264 images in both eyes passed afterwards, including startup and
ring wraparound, with the device-detection preflight enabled. A separate
24-frame HEVC identity test and the H.264 quad geometry/alpha/eye-visibility
test passed with the corrected index.

Repeat the decoded-image regression with:

```powershell
python test/smoke_windows_vdxr.py --runtime C:\path\to\vdxr\bin\x64\Release `
    --build build/arc --output build/arc/frame-identity-new --frames 24 --frame-ids --preflight
```

This requires the test-only `av` package. The report includes per-frame
expected/decoded RGB values from both eyes. The corrected build was relaunched
in Open Brush. The user confirmed the doubling was gone while turning their
head. A subsequent 45-second sample averaged 40.7 projection copies per second;
sustained gameplay performance remains unfinished. A fresh Open Brush session
then averaged 66.2 copies per second over 77.6 seconds at the same settings.
This variation is why the faster short sample is not a stable-rate claim.
ALVR comparison was stopped at the user's
request and its temporary settings/driver registration were restored.

### Independent audio and desktop shutdown, November 16

The Windows QSV bridge now advertises optional `audio_pcm`. Android explicitly
negotiates it before accepting a session. The owned VR application's bridge
captures the default Windows render endpoint using shared-mode WASAPI, converts
its mix format to stereo PCM16 and publishes packets of at most 10 ms. Meteor
authenticates and encrypts a separate audio socket using the existing video
listening port. Audio, attach and acknowledgment messages have distinct channel
keys, a fresh connection challenge, bounded lengths and ordered sequences.
There is no new firewall port, virtual audio driver or default-device change.

AAudio playback uses a bounded queue, trims queued latency beyond 40 ms and
outputs silence on underflow. Its callback performs no blocking I/O or memory
allocation. The player stays muted until handover has paused desktop audio.
Once ready, `PCVRDesktopStream` drains decoder GPU references and stops the
Moonlight connection using the existing restart lifecycle. It retains the XR
lease and host/app selection, reconnects when showing desktop or exiting VR,
and requests the existing desktop/audio path again if independent audio fails.
Unupdated clients and hosts without the capability retain desktop streaming.

Physical Quest 2/Arc 140V tests verified independent 48 kHz stereo playback:
the user heard both known test tones while desktop media was disconnected.
Showing desktop reconnected it; hiding it disconnected it again with the same
VR session generation and ongoing audio. The session continued beyond six
minutes. Pairings were preserved. The installed APK remains a diagnostic repack
with a source-built XR extension, not a full Android build.

At unchanged 2752×1440, H.264/32 Mbps/72 Hz, an initial 86.1-second sample
averaged 57.0 projection copies/sec. Later in that same session, sequential
desktop-visible and desktop-stopped samples measured 71.7 copies/sec over
19.5 seconds and 71.5 over 29.5 seconds. Desktop shutdown is functional; these
measurements show no FPS gain in this workload and do not establish a sustained
rate or scanout/motion-to-photon latency. 90 Hz remains unimplemented.

Validation: 195 Rust tests passed (20 environment-specific tests ignored),
including optional audio negotiation, duplicate-lane rejection, ordered PCM
validation, audio forwarding and continued video after audio disconnect.
Portable PCM/ring tests passed with MSVC; workspace handover and desktop
reconnect state tests passed in Godot. Android XR and Windows bridge builds
passed. The read-only WASAPI probe captured 48 kHz stereo without recording it.


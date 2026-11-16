//! DEV lease-owned encrypted socket relays around existing loopback consumers.
//! Prepared control ownership must outlive this object. No app launch is implicit.
use crate::{
    crypto::MeteorKey,
    xr_data::{Channel, HEADER, Kind},
    xr_wire,
};
use ring::rand::{SecureRandom, SystemRandom};
use serde::{Deserialize, Serialize};
use std::{
    io,
    net::{IpAddr, SocketAddr},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
    time::Duration,
};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::{TcpListener, TcpStream},
    sync::watch,
    task::JoinHandle,
};

const WAIT: Duration = Duration::from_secs(20);
const IO: Duration = Duration::from_secs(5);

/// Service-owned runtime tasks outlive connection teardown, but not service drain.
/// The registry is closed only after every connection task has been joined.
#[derive(Clone, Default)]
pub(crate) struct RuntimeTasks(Arc<Mutex<Vec<JoinHandle<()>>>>);
impl RuntimeTasks {
    pub(crate) fn track(&self, task: JoinHandle<()>) {
        let mut tasks = self.0.lock().unwrap();
        tasks.retain(|task| !task.is_finished());
        tasks.push(task);
    }

    pub(crate) async fn drain(&self) -> io::Result<()> {
        // Runtime cancellation allows four seconds for app exit plus up to
        // twenty seconds for post-run validation. Use one service-wide budget.
        let deadline = tokio::time::Instant::now() + Duration::from_secs(26);
        self.drain_until(deadline).await
    }

    async fn drain_until(&self, deadline: tokio::time::Instant) -> io::Result<()> {
        let tasks = std::mem::take(&mut *self.0.lock().unwrap());
        let mut failure = None;
        for mut task in tasks {
            match tokio::time::timeout_at(deadline, &mut task).await {
                Ok(Ok(())) => {},
                Ok(Err(error)) => { failure = Some(io::Error::other(error)); },
                Err(_) => {
                    // Dropping the runtime future invokes owned-process cleanup.
                    // Join the abort before permitting the caller's hard exit.
                    task.abort();
                    let _ = task.await;
                    failure = Some(io::Error::new(io::ErrorKind::TimedOut, "XR runtime drain exceeded shutdown budget"));
                },
            }
        }
        failure.map_or(Ok(()), Err)
    }
}
fn invalid(text: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, text)
}
async fn bounded<T>(future: impl std::future::Future<Output = io::Result<T>>) -> io::Result<T> {
    tokio::time::timeout(IO, future)
        .await
        .map_err(|_| io::Error::new(io::ErrorKind::TimedOut, "XR relay I/O timeout"))?
}

#[derive(Clone)]
pub struct Credentials {
    pub host: Arc<MeteorKey>,
    pub client: [u8; 32],
    pub server_nonce: [u8; 32],
    pub client_nonce: [u8; 32],
    pub session: [u8; 16],
}
impl Credentials {
    fn channel(&self, kind: Kind, binding: [u8; 32]) -> io::Result<Channel> {
        Channel::new_bound(
            &self.host,
            self.client,
            self.server_nonce,
            self.client_nonce,
            self.session,
            kind,
            Some(binding),
        )
    }
}
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Ports {
    pub video: u16,
    pub tracking: u16,
    pub framing: u8,
    #[serde(default)]
    pub audio:bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RuntimePhase {
    #[default]
    WaitingForLanes,
    Starting,
    ReadyForApplication,
    SessionCreated,
    SessionActive,
    SessionEnded,
    Stopping,
    Completed,
    Cancelled,
    Failed,
}
/// Scoped to one relay/accepted control lease. Never a presentation-FPS claim.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct RuntimeStatus {
    pub phase: RuntimePhase,
    pub generation: Option<String>,
    pub event_sequence: u64,
    pub active_sessions: usize,
}
pub(crate) type RuntimeProgress = Arc<Mutex<RuntimeStatus>>;
pub struct Relay {
    pub ports: Ports,
    pub local_video: u16,
    pub local_tracking: u16,
    pub local_audio:Option<u16>,
    failed: Arc<AtomicBool>,
    stop: watch::Sender<bool>,
    tasks: Vec<JoinHandle<()>>,
    attached: Arc<AtomicUsize>,
    runtime: Option<RuntimeProgress>,
    bitrate: crate::xr_bitrate::Control,
    foveation: crate::xr_foveation::Control,
}
impl Relay {
    pub async fn prepare(bind: IpAddr, credentials: Credentials) -> io::Result<Self> {
        Self::prepare_on_ports(bind, credentials, None).await
    }
    pub async fn prepare_on_ports(bind: IpAddr, credentials: Credentials, fixed: Option<(u16, u16)>) -> io::Result<Self> {
        Self::prepare_on_address(SocketAddr::new(bind, 0), credentials, fixed).await
    }
    pub async fn prepare_on_address(bind: SocketAddr, credentials: Credentials, fixed: Option<(u16, u16)>) -> io::Result<Self> {
        Self::prepare_on_address_with_audio(bind,credentials,fixed,false).await
    }
    pub async fn prepare_on_address_with_audio(bind: SocketAddr, credentials: Credentials, fixed: Option<(u16, u16)>, audio:bool) -> io::Result<Self> {
        if fixed.is_some_and(|(video, tracking)| video == 0 || tracking == 0 || video == tracking) {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "XR data ports must be distinct and nonzero"));
        }
        let (video_address, tracking_address) = lane_addresses(bind, fixed);
        let video = TcpListener::bind(video_address).await?;
        let tracking = TcpListener::bind(tracking_address).await?;
        let local_video = TcpListener::bind("127.0.0.1:0").await?;
        let local_tracking = TcpListener::bind("127.0.0.1:0").await?;
        let mut local_audio=if audio {Some(TcpListener::bind("127.0.0.1:0").await?)} else {None};
        let local_audio_port=local_audio.as_ref().map(|listener|listener.local_addr().map(|address|address.port())).transpose()?;
        let ports = Ports {
            video: video.local_addr()?.port(),
            tracking: tracking.local_addr()?.port(),
            framing: 1,
            audio,
        };
        let local_video_port = local_video.local_addr()?.port();
        let local_tracking_port = local_tracking.local_addr()?.port();
        let (stop, _) = watch::channel(false);
        let failed = Arc::new(AtomicBool::new(false));
        let attached = Arc::new(AtomicUsize::new(0));
        let mut tasks = Vec::new();
        for (kind, listener, local) in [
            (Kind::AttachVideo, video, local_video),
            (Kind::AttachTracking, tracking, local_tracking),
        ] {
            let credentials = credentials.clone();
            let mut cancel = stop.subscribe();
            let signal = stop.clone();
            let failure = failed.clone();
            let ready = attached.clone();
            let audio_listener=if kind==Kind::AttachVideo {local_audio.take()} else {None};
            tasks.push(tokio::spawn(async move {
                tokio::select! {
                    result=async {
                        if kind==Kind::AttachVideo {video_lanes(listener,local,audio_listener,credentials,ready).await}
                        else {lane(listener,local,credentials,kind,ready).await}
                    }=> {
                        if let Err(error)=result {failure.store(true,Ordering::Release);eprintln!("Meteor XR relay {kind:?} stopped: {error}");signal.send_replace(true);}
                    },
                    _=cancel.changed()=>{},
                }
            }));
        }
        Ok(Self {
            ports,
            local_video: local_video_port,
            local_tracking: local_tracking_port,
            local_audio:local_audio_port,
            failed,
            stop,
            tasks,
            attached,
            runtime: None,
            bitrate: crate::xr_bitrate::Control::default(),
            foveation: crate::xr_foveation::Control::default(),
        })
    }
    pub fn failed(&self) -> bool {
        self.failed.load(Ordering::Acquire)
    }
    pub(crate) fn update_bitrate(&mut self, mbps:u32) -> io::Result<()> {
        if !self.runtime_status().is_some_and(|s| s.phase==RuntimePhase::SessionActive) {
            return Err(io::Error::other("runtime_not_active"));
        }
        self.bitrate.queue(mbps)
    }
    pub(crate) fn update_foveation(&mut self, enabled:bool) -> io::Result<()> {
        if !self.runtime_status().is_some_and(|s|s.phase==RuntimePhase::SessionActive) { return Err(io::Error::other("runtime_not_active")); }
        self.foveation.directory=self.bitrate.directory.clone();
        self.foveation.queue(u32::from(enabled))
    }
    pub(crate) fn foveation_status(&self) -> Option<crate::xr_foveation::Status> {self.foveation.status()}
    pub(crate) fn bitrate_status(&self) -> Option<crate::xr_bitrate::Status> {self.bitrate.status()}
    pub fn runtime_status(&self) -> Option<RuntimeStatus> {
        self.runtime.as_ref().map(|runtime| runtime.lock().unwrap().clone())
    }
    #[cfg(target_os = "linux")]
    pub fn start_runtime_profile(&mut self, root: std::path::PathBuf, hz: u32, eye_size: u32, long_run: bool) {
        self.start_runtime_supervised(root, hz, eye_size, eye_size, long_run, 0, "h264".into(), false, None, None);
    }
    #[cfg(any(target_os = "linux", windows))]
    pub(crate) fn start_runtime_supervised(&mut self, root: std::path::PathBuf, hz: u32, eye_size: u32, eye_height:u32, long_run: bool, bitrate_mbps: u32, codec: String, foveated_encoding:bool, launch_intent: Option<String>, owner: Option<&RuntimeTasks>) {
        let progress = Arc::new(Mutex::new(RuntimeStatus::default()));
        let bitrate_directory=self.bitrate.directory.clone();
        self.runtime = Some(progress.clone());
        let ready = self.attached.clone();
        let mut cancel = self.stop.subscribe();
        let signal = self.stop.clone();
        let failure = self.failed.clone();
        let (video, tracking) = (self.local_video, self.local_tracking);
        let audio=self.local_audio;
        // Intentionally not aborted by Relay::drop: cancellation is delivered
        // through watch, allowing bounded graceful app teardown before kill.
        let task = tokio::spawn(async move {
            let deadline = tokio::time::Instant::now() + WAIT;
            while ready.load(Ordering::Acquire) != 2+usize::from(audio.is_some()) {
                if *cancel.borrow() {
                    progress.lock().unwrap().phase = RuntimePhase::Cancelled;
                    return;
                }
                tokio::select! {_=cancel.changed()=>{
                    progress.lock().unwrap().phase = RuntimePhase::Cancelled;
                    return;
                },_=tokio::time::sleep(Duration::from_millis(20))=>{}}
                if tokio::time::Instant::now() >= deadline {
                    progress.lock().unwrap().phase = RuntimePhase::Failed;
                    failure.store(true, Ordering::Release);
                    signal.send_replace(true);
                    return;
                }
            }
            progress.lock().unwrap().phase = RuntimePhase::Starting;
            let cancelled = cancel.clone();
            let result = crate::xr_session::runtime_adapter_with_status(root, video, tracking, hz, eye_size, eye_height, long_run, bitrate_mbps, codec, foveated_encoding, launch_intent, cancel, Some(&progress), Some(&bitrate_directory),audio).await;
            // The adapter has returned and its owned child has been reaped.
            {
                let mut status = progress.lock().unwrap();
                status.phase = if result.is_err() { RuntimePhase::Failed }
                    else if *cancelled.borrow() { RuntimePhase::Cancelled }
                    else { RuntimePhase::Completed };
                status.active_sessions = 0;
            }
            if let Err(error) = result {
                eprintln!("Meteor XR runtime adapter stopped: {error}");
                failure.store(true, Ordering::Release);
                signal.send_replace(true);
            }
        });
        if let Some(owner) = owner { owner.track(task); }
    }
}
// Reuse the accepted control socket's interface, including an IPv6 link-local
// scope. Extracting only IpAddr loses the USB interface on Windows.
fn lane_addresses(mut bind: SocketAddr, fixed: Option<(u16, u16)>) -> (SocketAddr, SocketAddr) {
    // A dual-stack control listener reports IPv4 peers as ::ffff:a.b.c.d.
    // Bind their data lanes as IPv4 instead of relying on OS-specific support
    // for binding an IPv4-mapped address on a new IPv6-only socket.
    if let SocketAddr::V6(address) = bind {
        if let Some(ip) = address.ip().to_ipv4_mapped() {
            bind = SocketAddr::new(IpAddr::V4(ip), address.port());
        }
    }
    let (video, tracking) = fixed.unwrap_or((0, 0));
    bind.set_port(video);
    let video_address = bind;
    bind.set_port(tracking);
    (video_address, bind)
}

impl Drop for Relay {
    fn drop(&mut self) {
        self.stop.send_replace(true);
        for task in &self.tasks {
            task.abort();
        }
    }
}

async fn accept(listener: TcpListener) -> io::Result<TcpStream> {
    let (stream, _) = tokio::time::timeout(WAIT, listener.accept())
        .await
        .map_err(|_| io::Error::new(io::ErrorKind::TimedOut, "XR endpoint attach timeout"))??;
    stream.set_nodelay(true)?;
    Ok(stream)
}
async fn read(stream: &mut TcpStream, cipher: &mut Channel) -> io::Result<Vec<u8>> {
    bounded(async {
        let mut header = [0; HEADER];
        stream.read_exact(&mut header).await?;
        let size = cipher.payload_length(&header)?;
        let mut body = vec![0; size];
        stream.read_exact(&mut body).await?;
        cipher.open(&header, body)
    })
    .await
}
async fn write(stream: &mut TcpStream, cipher: &mut Channel, bytes: &[u8]) -> io::Result<()> {
    bounded(stream.write_all(&cipher.seal(bytes)?)).await
}
async fn lane(
    listener: TcpListener,
    local: TcpListener,
    credentials: Credentials,
    kind: Kind,
    ready: Arc<AtomicUsize>,
) -> io::Result<()> {
    let mut remote = accept(listener).await?;
    let mut challenge = [0; 32];
    SystemRandom::new()
        .fill(&mut challenge)
        .map_err(|_| invalid("randomness unavailable"))?;
    bounded(remote.write_all(&challenge)).await?;
    let proof = read(&mut remote, &mut credentials.channel(kind, challenge)?).await?;
    if proof[8..24] != credentials.session {
        return Err(invalid("XR endpoint session mismatch"));
    }
    ready.fetch_add(1, Ordering::Release);
    connected_lane(remote,local,credentials,kind,challenge).await
}
async fn connected_lane(mut remote:TcpStream,local:TcpListener,credentials:Credentials,kind:Kind,challenge:[u8;32])->io::Result<()> {
    // No trusted-local producer is admitted until remote key possession is proved.
    let mut local = if matches!(kind,Kind::AttachVideo|Kind::AttachAudio) {
        let (stream, _) = tokio::time::timeout(Duration::from_secs(45), local.accept()).await
            .map_err(|_| io::Error::new(io::ErrorKind::TimedOut, "XR armed video attach timeout"))??;
        stream.set_nodelay(true)?;
        stream
    } else { accept(local).await? };
    if kind == Kind::AttachVideo {
        video(&mut remote, &mut local, &credentials, challenge).await
    } else if kind==Kind::AttachAudio {
        audio(&mut remote,&mut local,&credentials,challenge).await
    } else {
        tracking(&mut remote, &mut local, &credentials, challenge).await
    }
}
// Audio shares the existing video listening port. Each socket proves possession
// independently with a fresh challenge and a different key-derivation label.
// No additional LAN listener or firewall allowance is introduced.
async fn video_lanes(listener:TcpListener,local_video:TcpListener,local_audio:Option<TcpListener>,credentials:Credentials,ready:Arc<AtomicUsize>)->io::Result<()> {
    let Some(audio_listener)=local_audio else {return lane(listener,local_video,credentials,Kind::AttachVideo,ready).await;};
    let mut video=Some(local_video);let mut audio=Some(audio_listener);
    let mut workers=tokio::task::JoinSet::new();
    for _ in 0..2 {
        let (mut remote,_)=tokio::time::timeout(WAIT,listener.accept()).await
            .map_err(|_|io::Error::new(io::ErrorKind::TimedOut,"XR video/audio attach timeout"))??;
        remote.set_nodelay(true)?;
        let mut challenge=[0;32];SystemRandom::new().fill(&mut challenge).map_err(|_|invalid("randomness unavailable"))?;
        bounded(remote.write_all(&challenge)).await?;
        let mut header=[0;HEADER];bounded(remote.read_exact(&mut header)).await?;
        let kind=match header[5] {5=>Kind::AttachVideo,8=>Kind::AttachAudio,_=>return Err(invalid("XR video/audio attach kind"))};
        let mut cipher=credentials.channel(kind,challenge)?;
        let size=cipher.payload_length(&header)?;let mut body=vec![0;size];
        bounded(remote.read_exact(&mut body)).await?;
        let proof=cipher.open(&header,body)?;
        if proof[8..24]!=credentials.session {return Err(invalid("XR endpoint session mismatch"));}
        let local=if kind==Kind::AttachVideo {video.take()} else {audio.take()}.ok_or_else(||invalid("duplicate XR data lane"))?;
        ready.fetch_add(1,Ordering::Release);
        let credentials=credentials.clone();
        workers.spawn(async move {(kind,connected_lane(remote,local,credentials,kind,challenge).await)});
    }
    while let Some(result)=workers.join_next().await {
        let (kind,result)=result.map_err(io::Error::other)?;
        if kind==Kind::AttachAudio {
            // The headset restores desktop audio if the dedicated PCM lane
            // fails. Losing optional audio must not terminate the VR game.
            if let Err(error)=result {eprintln!("Meteor XR audio lane stopped: {error}");}
        } else {result?;}
    }
    Ok(())
}
async fn audio(remote:&mut TcpStream,local:&mut TcpStream,credentials:&Credentials,binding:[u8;32])->io::Result<()> {
    let mut tx=credentials.channel(Kind::Audio,binding)?;
    let mut rx=credentials.channel(Kind::AudioFeedback,binding)?;
    let mut validator=crate::xr_audio::Reader::default();
    loop {
        let mut header=[0;crate::xr_audio::HEADER];bounded(local.read_exact(&mut header)).await?;
        let info=crate::xr_audio::Header::decode(&header)?;
        let mut packet=header.to_vec();packet.resize(header.len()+info.payload_len(),0);
        bounded(local.read_exact(&mut packet[header.len()..])).await?;
        validator.accept(&packet)?;
        write(remote,&mut tx,&packet).await?;
        let feedback=read(remote,&mut rx).await?;
        let mut expected=b"NFAC".to_vec();expected.extend(info.sequence.to_le_bytes());
        if feedback!=expected {return Err(invalid("XR audio acknowledgment mismatch"));}
        bounded(local.write_all(&feedback)).await?;
    }
}
async fn video(
    remote: &mut TcpStream,
    local: &mut TcpStream,
    credentials: &Credentials,
    binding: [u8; 32],
) -> io::Result<()> {
    let mut tx = credentials.channel(Kind::Video, binding)?;
    let mut rx = credentials.channel(Kind::VideoFeedback, binding)?;
    let mut validator = xr_wire::Reader::default();
    loop {
        let mut header = [0; 152];
        bounded(local.read_exact(&mut header)).await?;
        let info = xr_wire::Header::decode(&header)?;
        let mut packet = header.to_vec();
        let mut body = vec![0; info.payload_len];
        bounded(local.read_exact(&mut body)).await?;
        packet.extend(body);
        validator.read(&mut std::io::Cursor::new(&packet))?;
        write(remote, &mut tx, &packet).await?;
        let feedback = read(remote, &mut rx).await?;
        let mut expected = if info.end {
            b"NFXE".to_vec()
        } else {
            b"NFXA".to_vec()
        };
        expected.extend(info.sequence.to_le_bytes());
        if !info.end {
            expected.extend(info.frame_id.to_le_bytes());
        }
        if feedback != expected {
            return Err(invalid("XR decoder acknowledgment mismatch"));
        }
        bounded(local.write_all(&feedback)).await?;
        if info.end {
            return Ok(());
        }
    }
}
async fn tracking(
    remote: &mut TcpStream,
    local: &mut TcpStream,
    credentials: &Credentials,
    binding: [u8; 32],
) -> io::Result<()> {
    let mut tx = credentials.channel(Kind::HostTracking, binding)?;
    let mut rx = credentials.channel(Kind::HeadsetTracking, binding)?;
    loop {
        let mut request = [0; 32];
        bounded(local.read_exact(&mut request)).await?;
        crate::xr_data::validate_payload(Kind::HostTracking, &request, &mut Default::default())?;
        write(remote, &mut tx, &request).await?;
        if request.starts_with(b"NFXQ") {
            return Ok(());
        }
        let response = read(remote, &mut rx).await?;
        let (magic, size) = match &request[..4] {
            b"NFXC" => (b"NFXC".as_slice(), 32),
            b"NFXP" => (b"NFXT".as_slice(), 64),
            b"NFXO" => (b"NFXO".as_slice(), 112),
            b"NFXJ" => (b"NFXJ".as_slice(), 192),
            b"NFXH" => (b"NFXH".as_slice(), 32),
            _ => return Err(invalid("unsupported tracking request")),
        };
        if response.len() != size || !response.starts_with(magic) {
            return Err(invalid("XR tracking response mismatch"));
        }
        bounded(local.write_all(&response)).await?;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn usb_lane_addresses_preserve_ipv6_scope() {
        let control = SocketAddr::V6(std::net::SocketAddrV6::new(
            "fe80::1234".parse().unwrap(), 47910, 0, 50));
        let (video, tracking) = lane_addresses(control, Some((47911, 47912)));
        assert_eq!(video, "[fe80::1234%50]:47911".parse().unwrap());
        assert_eq!(tracking, "[fe80::1234%50]:47912".parse().unwrap());
        let (video, tracking) = lane_addresses(control, None);
        assert_eq!(video.port(), 0);
        assert_eq!(tracking.port(), 0);
        assert_eq!(match video { SocketAddr::V6(a) => a.scope_id(), _ => 0 }, 50);
    }

    #[tokio::test]
    async fn ipv4_control_on_dual_stack_listener_opens_ipv4_lanes() {
        let control = crate::net::tcp_listener_at("[::]:0".parse().unwrap()).await.unwrap();
        let client = TcpStream::connect((std::net::Ipv4Addr::LOCALHOST, control.local_addr().unwrap().port())).await.unwrap();
        let (accepted, _) = control.accept().await.unwrap();
        let bind = accepted.local_addr().unwrap();
        let (video, tracking) = lane_addresses(bind, None);
        assert!(video.is_ipv4() && tracking.is_ipv4());
        let relay = Relay::prepare_on_address(bind, credentials(), None).await.unwrap();
        TcpStream::connect((std::net::Ipv4Addr::LOCALHOST, relay.ports.video)).await.unwrap();
        TcpStream::connect((std::net::Ipv4Addr::LOCALHOST, relay.ports.tracking)).await.unwrap();
        drop(client);
    }

    #[tokio::test]
    async fn ipv6_relay_listens_on_both_data_lanes() {
        let relay = Relay::prepare_on_address("[::1]:47910".parse().unwrap(), credentials(), None).await.unwrap();
        let video = TcpStream::connect((std::net::Ipv6Addr::LOCALHOST, relay.ports.video)).await.unwrap();
        let tracking = TcpStream::connect((std::net::Ipv6Addr::LOCALHOST, relay.ports.tracking)).await.unwrap();
        assert!(video.peer_addr().unwrap().is_ipv6());
        assert!(tracking.peer_addr().unwrap().is_ipv6());
    }
    struct CleanupFlag(Arc<AtomicBool>);
    impl Drop for CleanupFlag {
        fn drop(&mut self) { self.0.store(true, Ordering::Release); }
    }

    #[tokio::test]
    async fn runtime_drain_waits_for_cooperative_cleanup() {
        let tasks = RuntimeTasks::default();
        let cleaned = Arc::new(AtomicBool::new(false));
        let guard = CleanupFlag(cleaned.clone());
        let (stop, mut cancel) = watch::channel(false);
        tasks.track(tokio::spawn(async move {
            let _guard = guard;
            let _ = cancel.changed().await;
            tokio::time::sleep(Duration::from_millis(10)).await;
        }));
        stop.send_replace(true);
        tasks.drain().await.unwrap();
        assert!(cleaned.load(Ordering::Acquire));
        tasks.drain().await.unwrap();
    }

    #[tokio::test]
    async fn runtime_drain_timeout_joins_abort_cleanup() {
        let tasks = RuntimeTasks::default();
        let cleaned = Arc::new(AtomicBool::new(false));
        let guard = CleanupFlag(cleaned.clone());
        tasks.track(tokio::spawn(async move {
            let _guard = guard;
            std::future::pending::<()>().await;
        }));
        let error = tasks.drain_until(tokio::time::Instant::now() + Duration::from_millis(10)).await.unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::TimedOut);
        assert!(cleaned.load(Ordering::Acquire));
    }

    #[tokio::test]
    #[cfg(target_os = "linux")]
    async fn service_owned_runtime_cancels_before_lane_attachment() {
        let tasks = RuntimeTasks::default();
        let mut relay = Relay::prepare("127.0.0.1".parse().unwrap(), credentials()).await.unwrap();
        relay.start_runtime_supervised("/must-not-launch".into(), 60, 1024, 1024, false, 0, "h264".into(), false, None, Some(&tasks));
        drop(relay);
        tokio::time::timeout(Duration::from_secs(1), tasks.drain()).await.unwrap().unwrap();
    }
    #[tokio::test]
    async fn fixed_data_ports_are_used_without_ephemeral_fallback() {
        let bind = "127.0.0.1".parse().unwrap();
        let reserved_video = TcpListener::bind((bind, 0)).await.unwrap();
        let reserved_tracking = TcpListener::bind((bind, 0)).await.unwrap();
        let ports = (reserved_video.local_addr().unwrap().port(), reserved_tracking.local_addr().unwrap().port());
        let error = Relay::prepare_on_ports(bind, credentials(), Some(ports)).await.err().unwrap();
        assert_eq!(error.kind(), io::ErrorKind::AddrInUse);
        drop(reserved_video); drop(reserved_tracking);
        let relay = Relay::prepare_on_ports(bind, credentials(), Some(ports)).await.unwrap();
        assert_eq!((relay.ports.video, relay.ports.tracking), ports);
        assert!(Relay::prepare_on_ports(bind, credentials(), Some((0, ports.1))).await.is_err());
        assert!(Relay::prepare_on_ports(bind, credentials(), Some((ports.0, ports.0))).await.is_err());
    }

    fn credentials() -> Credentials {
        Credentials {
            host: Arc::new(MeteorKey::from_seed([1; 32])),
            client: MeteorKey::from_seed([2; 32]).public,
            server_nonce: [3; 32],
            client_nonce: [4; 32],
            session: [5; 16],
        }
    }
    async fn attach_audio_or_video(port:u16,kind:Kind)->(TcpStream,[u8;32]) {
        let mut remote=TcpStream::connect(("127.0.0.1",port)).await.unwrap();
        let mut challenge=[0;32];bounded(remote.read_exact(&mut challenge)).await.unwrap();
        let mut proof=vec![0;32];proof[..4].copy_from_slice(b"NFXB");
        proof[4]=if kind==Kind::AttachAudio {3} else {1};
        proof[8..24].copy_from_slice(&credentials().session);
        write(&mut remote,&mut credentials().channel(kind,challenge).unwrap(),&proof).await.unwrap();
        (remote,challenge)
    }
    #[tokio::test]
    async fn audio_shares_video_port_and_its_failure_does_not_stop_video() {
        let relay=Relay::prepare_on_address_with_audio("127.0.0.1:0".parse().unwrap(),credentials(),None,true).await.unwrap();
        assert!(relay.ports.audio);
        let (mut remote_audio,audio_binding)=attach_audio_or_video(relay.ports.video,Kind::AttachAudio).await;
        let (mut remote_video,video_binding)=attach_audio_or_video(relay.ports.video,Kind::AttachVideo).await;
        let mut publisher=TcpStream::connect(("127.0.0.1",relay.local_audio.unwrap())).await.unwrap();
        let mut config=vec![0u8;32];config[..4].copy_from_slice(b"NFAP");config[4]=1;config[6]=32;
        config[8..12].copy_from_slice(&48000u32.to_le_bytes());config[12]=2;config[14]=1;
        bounded(publisher.write_all(&config)).await.unwrap();
        let received=read(&mut remote_audio,&mut credentials().channel(Kind::Audio,audio_binding).unwrap()).await.unwrap();
        assert_eq!(received,config);
        let mut feedback=b"NFAC".to_vec();feedback.extend(0u64.to_le_bytes());
        write(&mut remote_audio,&mut credentials().channel(Kind::AudioFeedback,audio_binding).unwrap(),&feedback).await.unwrap();
        let mut ack=[0;12];bounded(publisher.read_exact(&mut ack)).await.unwrap();assert_eq!(ack.as_slice(),feedback);
        drop(remote_audio);drop(publisher);
        let mut video_publisher=TcpStream::connect(("127.0.0.1",relay.local_video)).await.unwrap();
        bounded(video_publisher.write_all(&frame(0,false))).await.unwrap();
        assert_eq!(read(&mut remote_video,&mut credentials().channel(Kind::Video,video_binding).unwrap()).await.unwrap(),frame(0,false));
        assert!(!relay.failed());
        drop(relay);
        let mut byte=[0];assert_eq!(bounded(remote_video.read(&mut byte)).await.unwrap(),0);
    }
    #[tokio::test]
    async fn duplicate_audio_proof_cancels_lease() {
        let relay=Relay::prepare_on_address_with_audio("127.0.0.1:0".parse().unwrap(),credentials(),None,true).await.unwrap();
        let (_first,_)=attach_audio_or_video(relay.ports.video,Kind::AttachAudio).await;
        let (_second,_)=attach_audio_or_video(relay.ports.video,Kind::AttachAudio).await;
        tokio::time::timeout(Duration::from_secs(1),async {
            while !relay.failed() {tokio::task::yield_now().await;}
        }).await.unwrap();
    }
    fn frame(sequence: u64, end: bool) -> Vec<u8> {
        let payload = [0, 0, 1, 0x67, 1, 0, 0, 1, 0x68, 1, 0, 0, 1, 0x65, 1];
        let mut h = vec![0; 152];
        h[..4].copy_from_slice(b"NFXR");
        h[4] = 1;
        h[6] = 152;
        h[8..16].copy_from_slice(&9u64.to_le_bytes());
        h[16..24].copy_from_slice(&sequence.to_le_bytes());
        h[48] = 1;
        if end {
            h[50] = 1;
            return h;
        }
        h[24..32].copy_from_slice(&(sequence + 1).to_le_bytes());
        h[32..40].copy_from_slice(&(sequence + 1000).to_le_bytes());
        h[40..44].copy_from_slice(&(payload.len() as u32).to_le_bytes());
        h[44..46].copy_from_slice(&512u16.to_le_bytes());
        h[46..48].copy_from_slice(&256u16.to_le_bytes());
        h[49] = 1;
        for eye in 0..2 {
            for (field, value) in [(6, 1f32), (7, -0.8), (8, 0.8), (9, 0.8), (10, -0.8)] {
                let at = 64 + (eye * 11 + field) * 4;
                h[at..at + 4].copy_from_slice(&value.to_le_bytes());
            }
        }
        h.extend(payload);
        h
    }
    #[tokio::test]
    async fn invalid_proof_cancels_other_lane_and_local_admission() {
        let relay = Relay::prepare("127.0.0.1".parse().unwrap(), credentials())
            .await
            .unwrap();
        let mut attacker = TcpStream::connect(("127.0.0.1", relay.ports.video))
            .await
            .unwrap();
        let mut challenge = [0; 32];
        attacker.read_exact(&mut challenge).await.unwrap();
        attacker.write_all(&[0; HEADER]).await.unwrap();
        tokio::time::timeout(Duration::from_secs(1), async {
            while !relay.failed() {
                tokio::task::yield_now().await;
            }
        })
        .await
        .unwrap();
        let mut byte = [0];
        assert_eq!(bounded(attacker.read(&mut byte)).await.unwrap(), 0);
    }
    #[tokio::test]
    async fn dropping_lease_closes_waiting_connections() {
        let relay = Relay::prepare("127.0.0.1".parse().unwrap(), credentials())
            .await
            .unwrap();
        let mut client = TcpStream::connect(("127.0.0.1", relay.ports.video))
            .await
            .unwrap();
        let mut challenge = [0; 32];
        client.read_exact(&mut challenge).await.unwrap();
        drop(relay);
        let mut byte = [0];
        assert_eq!(bounded(client.read(&mut byte)).await.unwrap(), 0);
    }
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    #[ignore = "requires native worker fixture NIGHTFALL_XR_RELAY_CPP_CLIENT"]
    async fn native_live_workers() {
        let binary = std::env::var("NIGHTFALL_XR_RELAY_CPP_CLIENT").unwrap();
        let relay = Relay::prepare("127.0.0.1".parse().unwrap(), credentials())
            .await
            .unwrap();
        let (mut video, mut tracking) = (
            TcpStream::connect(("127.0.0.1", relay.local_video))
                .await
                .unwrap(),
            TcpStream::connect(("127.0.0.1", relay.local_tracking))
                .await
                .unwrap(),
        );
        let child = std::process::Command::new(binary)
            .arg(relay.ports.video.to_string())
            .arg(relay.ports.tracking.to_string())
            .spawn()
            .unwrap();
        for sequence in 0..2 {
            bounded(video.write_all(&frame(sequence, false)))
                .await
                .unwrap();
            let mut ack = [0; 20];
            bounded(video.read_exact(&mut ack)).await.unwrap();
            assert_eq!(&ack[..4], b"NFXA");
            assert_eq!(u64::from_le_bytes(ack[4..12].try_into().unwrap()), sequence);
            let mut request = [0; 32];
            request[..4].copy_from_slice(b"NFXP");
            request[4] = 1;
            request[6] = 32;
            bounded(tracking.write_all(&request)).await.unwrap();
            let mut response = [0; 64];
            bounded(tracking.read_exact(&mut response)).await.unwrap();
            assert_eq!(&response[..4], b"NFXT");
            assert_eq!(
                u64::from_le_bytes(response[16..24].try_into().unwrap()),
                sequence
            );
        }
        bounded(video.write_all(&frame(2, true))).await.unwrap();
        let mut end = [0; 12];
        bounded(video.read_exact(&mut end)).await.unwrap();
        assert_eq!(&end[..4], b"NFXE");
        let mut quit = [0; 32];
        quit[..4].copy_from_slice(b"NFXQ");
        quit[4] = 1;
        quit[6] = 32;
        bounded(tracking.write_all(&quit)).await.unwrap();
        let status = tokio::task::spawn_blocking(move || {
            let mut child = child;
            child.wait().unwrap()
        })
        .await
        .unwrap();
        assert!(status.success());
        assert!(!relay.failed());
    }

    #[cfg(target_os = "linux")]
    #[tokio::test(flavor = "multi_thread", worker_threads = 3)]
    #[ignore = "requires GPU runtime, native worker client and synthetic endpoint"]
    async fn managed_gpu_runtime_through_live_relays() {
        use std::io::BufRead;
        struct Process(std::process::Child);
        impl Drop for Process {
            fn drop(&mut self) {
                let _ = self.0.kill();
                let _ = self.0.wait();
            }
        }
        let root = std::path::PathBuf::from(std::env::var("NIGHTFALL_XR_RELAY_ROOT").unwrap());
        let client = std::env::var("NIGHTFALL_XR_RELAY_CPP_CLIENT").unwrap();
        let reserve = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = reserve.local_addr().unwrap().port();
        drop(reserve);
        let mut publisher = Process(
            std::process::Command::new(
                root.join(".build-cache/meteor-xr/tracking-runtime-spike/synthetic-publisher"),
            )
            .args([port.to_string(), "moving".into()])
            .stdout(std::process::Stdio::piped())
            .spawn()
            .unwrap(),
        );
        let mut line = String::new();
        std::io::BufReader::new(publisher.0.stdout.take().unwrap())
            .read_line(&mut line)
            .unwrap();
        assert!(line.contains("READY"));
        let started = std::time::SystemTime::now();
        let mut relay = Relay::prepare("127.0.0.1".parse().unwrap(), credentials())
            .await
            .unwrap();
        let hz = std::env::var("NIGHTFALL_XR_RELAY_TEST_HZ")
            .ok().map(|s| s.parse().unwrap()).unwrap_or(20);
        let eye_size = std::env::var("NIGHTFALL_XR_RELAY_TEST_EYE_SIZE")
            .ok().map(|s| s.parse().unwrap()).unwrap_or(512);
        let long_run = std::env::var("NIGHTFALL_XR_RELAY_TEST_LONG").as_deref() == Ok("1");
        relay.start_runtime_profile(root.clone(), hz, eye_size, long_run);
        let child = Process(
            std::process::Command::new(client)
                .args([
                    relay.ports.video.to_string(),
                    relay.ports.tracking.to_string(),
                    port.to_string(),
                ])
                .spawn()
                .unwrap(),
        );
        let mut completion = tokio::task::spawn_blocking(move || {
            let mut child = child;
            child.0.wait().unwrap()
        });
        let mut saw_active_session = false;
        let result = loop {
            saw_active_session |= relay.runtime_status().is_some_and(|status| status.phase == RuntimePhase::SessionActive && status.active_sessions > 0);
            tokio::select! {
                result = &mut completion => break result.unwrap(),
                _ = tokio::time::sleep(Duration::from_millis(20)) => {},
            }
        };
        assert!(result.success());
        assert!(saw_active_session, "runtime status never reported an actual active application session");
        assert!(publisher.0.wait().unwrap().success());
        let until = tokio::time::Instant::now() + Duration::from_secs(30);
        loop {
            let proof = std::fs::read_dir(root.join(".build-cache/meteor-xr/lan-sessions"))
                .unwrap()
                .filter_map(Result::ok)
                .map(|entry| entry.path().join("engine-outcome.json"))
                .find(|path| {
                    path.metadata()
                        .ok()
                        .and_then(|m| m.modified().ok())
                        .is_some_and(|time| time >= started)
                        && path.parent().unwrap().join("pipeline-timing.json").exists()
                        && std::fs::read_to_string(path.parent().unwrap().join("runtime.log"))
                            .is_ok_and(|log| log.contains("PASS: Godot OpenXR engine rendered"))
                });
            if let Some(proof) = proof {
                let report: serde_json::Value =
                    serde_json::from_slice(&std::fs::read(&proof).unwrap()).unwrap();
                assert_eq!(report["engine_shutdown_clean"], true);
                assert_eq!(report["encoder_drain_verified"], true);
                let status = relay.runtime_status().unwrap();
                if status.phase == RuntimePhase::Completed {
                    assert!(status.generation.is_some());
                    assert!(status.event_sequence >= 4);
                    assert_eq!(status.active_sessions, 0);
                    break;
                }
            }
            assert!(
                tokio::time::Instant::now() < until,
                "No normal runtime validation proof; see lan-sessions/<attempt>/runtime.log"
            );
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
        assert!(!relay.failed());
    }
}

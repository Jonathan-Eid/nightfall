use std::io;
use std::net::{IpAddr, Ipv6Addr, SocketAddr};

use socket2::{Domain, Protocol, SockAddr, Socket, Type};
use tokio::net::{TcpListener, UdpSocket};

fn dual_stack_socket(ty: Type, protocol: Protocol, port: u16) -> io::Result<Socket> {
    let socket = Socket::new(Domain::IPV6, ty, Some(protocol))?;
    socket.set_only_v6(false)?;
    socket.set_reuse_address(true)?;
    socket.bind(&SockAddr::from(SocketAddr::new(IpAddr::V6(Ipv6Addr::UNSPECIFIED), port)))?;
    Ok(socket)
}

pub async fn tcp_listener(port: u16) -> io::Result<TcpListener> {
    let socket = dual_stack_socket(Type::STREAM, Protocol::TCP, port)?;
    socket.listen(128)?;
    socket.set_nonblocking(true)?;
    TcpListener::from_std(socket.into())
}

/// An unspecified IPv6 listener accepts both Wi-Fi IPv4 and USB IPv6.
/// Explicit interface/loopback addresses retain their configured scope.
pub async fn tcp_listener_at(address: SocketAddr) -> io::Result<TcpListener> {
    if address.is_ipv6() && address.ip().is_unspecified() {
        tcp_listener(address.port()).await
    } else {
        TcpListener::bind(address).await
    }
}

pub async fn udp_socket(port: u16) -> io::Result<UdpSocket> {
    let socket = Socket::new(Domain::IPV6, Type::DGRAM, Some(Protocol::UDP))?;
    socket.set_only_v6(false)?;
    socket.set_reuse_address(true)?;
    // Lets udp_path_socket() open per-path sockets on the same port. Like
    // SO_REUSEADDR before it, this doesn't stop another socket of this user
    // binding the port too; SO_REUSEPORT only changes which one gets packets.
    #[cfg(target_os = "linux")]
    socket.set_reuse_port(true)?;
    socket.bind(&SockAddr::from(SocketAddr::new(IpAddr::V6(Ipv6Addr::UNSPECIFIED), port)))?;
    socket.set_nonblocking(true)?;
    UdpSocket::from_std(socket.into())
}

/// A socket on the listener's port connected to one client address (Linux).
/// The kernel delivers that address's packets to it instead of the listener,
/// and its send queue only holds packets for that path, so a slow path can be
/// told apart from a fast one. `peer` is the address as the dual-stack
/// listener saw it.
#[cfg(target_os = "linux")]
pub fn udp_path_socket(port: u16, peer: SocketAddr) -> io::Result<UdpSocket> {
    let socket = Socket::new(Domain::IPV6, Type::DGRAM, Some(Protocol::UDP))?;
    socket.set_only_v6(false)?;
    socket.set_reuse_address(true)?;
    socket.set_reuse_port(true)?;
    let _ = socket.set_send_buffer_size(4 << 20);
    socket.bind(&SockAddr::from(SocketAddr::new(IpAddr::V6(Ipv6Addr::UNSPECIFIED), port)))?;
    let peer = match peer {
        SocketAddr::V4(v4) => SocketAddr::new(IpAddr::V6(v4.ip().to_ipv6_mapped()), v4.port()),
        v6 => v6,
    };
    socket.connect(&SockAddr::from(peer))?;
    socket.set_nonblocking(true)?;
    UdpSocket::from_std(socket.into())
}

pub fn tcp_listener_std(port: u16) -> io::Result<std::net::TcpListener> {
    let socket = dual_stack_socket(Type::STREAM, Protocol::TCP, port)?;
    socket.listen(128)?;
    socket.set_nonblocking(false)?;
    Ok(socket.into())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn wildcard_control_listener_accepts_ipv4_and_ipv6() {
        let listener = tcp_listener_at("[::]:0".parse().unwrap()).await.unwrap();
        let port = listener.local_addr().unwrap().port();
        for address in [format!("127.0.0.1:{port}"), format!("[::1]:{port}")] {
            let client = tokio::net::TcpStream::connect(address).await.unwrap();
            let (accepted, _) = listener.accept().await.unwrap();
            assert_eq!(accepted.local_addr().unwrap().port(), port);
            drop(client);
        }
    }

    #[tokio::test]
    async fn explicit_loopback_listener_stays_on_loopback() {
        let listener = tcp_listener_at("127.0.0.1:0".parse().unwrap()).await.unwrap();
        assert_eq!(listener.local_addr().unwrap().ip(), "127.0.0.1".parse::<IpAddr>().unwrap());
    }
}

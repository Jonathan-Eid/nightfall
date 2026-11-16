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
    let socket = dual_stack_socket(Type::DGRAM, Protocol::UDP, port)?;
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

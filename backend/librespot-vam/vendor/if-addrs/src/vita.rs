// VitaSpot patch: interface enumeration for the PS Vita.
//
// newlib on the Vita has neither getifaddrs() nor Linux netlink, so the only
// reliable source of the console's IPv4 configuration is SceNetCtl. libmdns
// (and therefore librespot's Zeroconf discovery) only needs the address and
// netmask of the active interface, which is what this module returns.

use std::io;
use std::net::Ipv4Addr;

use crate::{IfAddr, IfOperStatus, Ifv4Addr, Interface};

const SCE_NETCTL_INFO_GET_IP_ADDRESS: i32 = 15;
const SCE_NETCTL_INFO_GET_NETMASK: i32 = 16;
const SCE_NETCTL_STATE_CONNECTED: i32 = 3;

#[repr(C)]
union SceNetCtlInfo {
    raw: [u8; 256],
    ip_address: [u8; 16],
    netmask: [u8; 16],
}

extern "C" {
    fn sceNetCtlInetGetState(state: *mut i32) -> i32;
    fn sceNetCtlInetGetInfo(code: i32, info: *mut SceNetCtlInfo) -> i32;
}

fn parse_ipv4(bytes: &[u8]) -> Option<Ipv4Addr> {
    let end = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
    let text = std::str::from_utf8(&bytes[..end]).ok()?;
    text.trim().parse::<Ipv4Addr>().ok()
}

fn query_ipv4(code: i32) -> Option<Ipv4Addr> {
    unsafe {
        let mut info = SceNetCtlInfo { raw: [0u8; 256] };
        if sceNetCtlInetGetInfo(code, &mut info) < 0 {
            return None;
        }
        parse_ipv4(&info.ip_address)
    }
}

pub fn get_if_addrs() -> io::Result<Vec<Interface>> {
    unsafe {
        let mut state: i32 = 0;
        if sceNetCtlInetGetState(&mut state) < 0 || state != SCE_NETCTL_STATE_CONNECTED {
            return Ok(Vec::new());
        }
    }

    let Some(ip) = query_ipv4(SCE_NETCTL_INFO_GET_IP_ADDRESS) else {
        return Ok(Vec::new());
    };
    if ip.is_loopback() || ip.is_unspecified() {
        return Ok(Vec::new());
    }
    let netmask = query_ipv4(SCE_NETCTL_INFO_GET_NETMASK)
        .unwrap_or(Ipv4Addr::new(255, 255, 255, 0));
    let prefixlen = u32::from_be_bytes(netmask.octets()).count_ones() as u8;

    Ok(vec![Interface {
        name: "wlan0".to_string(),
        addr: IfAddr::V4(Ifv4Addr {
            ip,
            netmask,
            prefixlen,
            broadcast: None,
        }),
        index: Some(1),
        oper_status: IfOperStatus::Up,
    }])
}

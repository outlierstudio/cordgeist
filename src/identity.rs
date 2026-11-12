//! Exact identity routing only. Aliases and snapshots never authorize phone input.
use crate::{
    adb::{Attachment, Authorization, Target},
    api::{AttachmentGeneration, BootId, DeviceId, ErrorCode},
    discovery::{DiscoveryKind, DiscoverySnapshot, MAX_CANDIDATES, UsbDevice},
};
use serde::{Deserialize, Serialize};
use std::{
    collections::{HashMap, HashSet},
    time::{Duration, Instant},
};

pub const ROUTE_MAX_AGE: Duration = Duration::from_secs(5);
fn serial(device: &UsbDevice) -> Option<&str> {
    device.serial.as_deref().filter(|s| !s.trim().is_empty())
}
fn id_valid(id: &DeviceId) -> bool {
    !id.0.is_empty()
        && id.0.len() <= 64
        && id
            .0
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-')
}
fn alias_valid(alias: &str) -> bool {
    !alias.trim().is_empty() && alias.len() <= 128 && !alias.chars().any(char::is_control)
}

/// Private explicit pairing evidence. Never Debug/log/export as public status.
#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Pairing {
    pub device_id: DeviceId,
    pub alias: Option<String>,
    pub topology: String,
    pub vendor_id: u16,
    pub product_id: u16,
    pub usb_serial: Option<String>,
    pub adb_serial: String,
    /// Human-confirmed port association is required for blank/duplicate serials.
    pub port_association: bool,
}
impl Pairing {
    /// Explicitly called by setup after the human identifies this exact physical
    /// port/phone. Never called by automatic discovery or inferred from an alias.
    pub fn confirm(
        device_id: DeviceId,
        alias: Option<String>,
        usb: &UsbDevice,
        adb: &Attachment,
        port_association: bool,
    ) -> Result<Self, ErrorCode> {
        let topology = usb.topology()?;
        match_attachment(usb, adb)?;
        if serial(usb).is_none() && !port_association {
            return Err(ErrorCode::DeviceIdentityAmbiguous);
        }
        let pairing = Self {
            device_id,
            alias,
            topology,
            vendor_id: usb.vendor_id,
            product_id: usb.product_id,
            usb_serial: serial(usb).map(str::to_owned),
            adb_serial: adb.serial.clone(),
            port_association,
        };
        pairing.validate()?;
        Ok(pairing)
    }
    fn validate(&self) -> Result<(), ErrorCode> {
        if !id_valid(&self.device_id)
            || self.alias.as_ref().is_some_and(|s| !alias_valid(s))
            || !valid_topology(&self.topology)
            || self
                .usb_serial
                .as_ref()
                .is_some_and(|s| s.is_empty() || s.len() > 256)
            || self.adb_serial.trim().is_empty()
            || self.adb_serial.len() > 256
            || self
                .usb_serial
                .as_ref()
                .is_some_and(|s| s != &self.adb_serial)
            || (self.usb_serial.is_none() && !self.port_association)
        {
            return Err(ErrorCode::DeviceIdentityAmbiguous);
        }
        Ok(())
    }
    fn matches(&self, u: &UsbDevice, a: &Attachment, topology: &str) -> bool {
        self.topology == topology
            && self.vendor_id == u.vendor_id
            && self.product_id == u.product_id
            && self.usb_serial.as_deref() == serial(u)
            && self.adb_serial == a.serial
    }
}
fn valid_topology(s: &str) -> bool {
    let Some((bus, ports)) = s.split_once('-') else {
        return false;
    };
    if bus
        .parse::<u8>()
        .ok()
        .filter(|v| *v > 0)
        .map(|v| v.to_string())
        .as_deref()
        != Some(bus)
    {
        return false;
    }
    let p = ports.split('.').collect::<Vec<_>>();
    !p.is_empty()
        && p.len() <= 7
        && p.iter().all(|v| {
            v.parse::<u8>()
                .ok()
                .filter(|n| *n > 0)
                .is_some_and(|n| n.to_string() == *v)
        })
}
#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct PairingConfig {
    pub version: u32,
    pub pairings: Vec<Pairing>,
}
impl Default for PairingConfig {
    fn default() -> Self {
        Self {
            version: 1,
            pairings: vec![],
        }
    }
}
impl PairingConfig {
    pub fn validate(&self) -> Result<(), ErrorCode> {
        if self.version != 1 || self.pairings.len() > MAX_CANDIDATES {
            return Err(ErrorCode::InvalidRequest);
        }
        let mut ids = HashSet::new();
        let mut ports = HashSet::new();
        for p in &self.pairings {
            p.validate()?;
            if !ids.insert(&p.device_id) || !ports.insert(&p.topology) {
                return Err(ErrorCode::DeviceIdentityAmbiguous);
            }
        }
        Ok(())
    }
    pub fn set_pairing(&mut self, pairing: Pairing) -> Result<(), ErrorCode> {
        let mut next = self.clone();
        next.pairings.retain(|p| p.device_id != pairing.device_id);
        next.pairings.push(pairing);
        next.validate()?;
        *self = next;
        Ok(())
    }
    pub fn set_alias(&mut self, id: &DeviceId, alias: Option<String>) -> Result<(), ErrorCode> {
        let mut next = self.clone();
        next.pairings
            .iter_mut()
            .find(|p| &p.device_id == id)
            .ok_or(ErrorCode::DeviceIdentityAmbiguous)?
            .alias = alias;
        next.validate()?;
        *self = next;
        Ok(())
    }
    pub fn id_for_alias(&self, alias: &str) -> Result<DeviceId, ErrorCode> {
        self.validate()?;
        let matches = self
            .pairings
            .iter()
            .filter(|p| p.alias.as_deref() == Some(alias))
            .collect::<Vec<_>>();
        if matches.len() != 1 {
            return Err(ErrorCode::DeviceIdentityAmbiguous);
        }
        Ok(matches[0].device_id.clone())
    }
}
fn match_attachment(u: &UsbDevice, a: &Attachment) -> Result<(), ErrorCode> {
    if a.usb.as_deref() != Some(u.topology()?.as_str())
        || a.transport_id.is_none_or(|n| n == 0)
        || a.serial.trim().is_empty()
        || a.serial.len() > 256
        || serial(u).is_some_and(|s| s != a.serial)
    {
        return Err(ErrorCode::DeviceIdentityAmbiguous);
    }
    match a.state {
        Authorization::Authorized => Ok(()),
        Authorization::Unauthorized => Err(ErrorCode::AdbUnauthorized),
        Authorization::Offline => Err(ErrorCode::DeviceDisconnected),
        _ => Err(ErrorCode::BackendUnavailable),
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RouteRef {
    pub boot_id: BootId,
    pub device_id: DeviceId,
    pub attachment: AttachmentGeneration,
}
/// A copied exact target, not authorization. Resolve the RouteRef immediately
/// before each operation; never cache this copy as a live input capability.
#[derive(Clone)]
pub struct ResolvedRoute {
    reference: RouteRef,
    kind: DiscoveryKind,
    usb: UsbDevice,
    adb: Attachment,
}
impl ResolvedRoute {
    pub fn reference(&self) -> &RouteRef {
        &self.reference
    }
    pub fn kind(&self) -> DiscoveryKind {
        self.kind
    }
    pub fn usb(&self) -> &UsbDevice {
        &self.usb
    }
    pub fn adb_attachment(&self) -> &Attachment {
        &self.adb
    }
    pub fn target(&self) -> Target {
        Target {
            transport_id: self.adb.transport_id.expect("validated exact route"),
        }
    }
}
struct Current {
    route: ResolvedRoute,
    captured_at: Instant,
}
pub struct IdentityRegistry {
    boot_id: BootId,
    generation: u64,
    current: HashMap<DeviceId, Current>,
}
impl IdentityRegistry {
    pub fn boot_id(&self) -> &BootId {
        &self.boot_id
    }
    pub fn new(boot_id: BootId) -> Self {
        Self {
            boot_id,
            generation: 0,
            current: HashMap::new(),
        }
    }
    /// Fence first, including when later validation/discovery fails.
    pub fn invalidate(&mut self) -> Result<(), ErrorCode> {
        self.current.clear();
        self.generation = self
            .generation
            .checked_add(1)
            .ok_or(ErrorCode::BackendUnavailable)?;
        Ok(())
    }
    pub fn refresh(
        &mut self,
        snapshot: DiscoverySnapshot,
        adb: &[Attachment],
        config: &PairingConfig,
    ) -> Result<Vec<RouteRef>, ErrorCode> {
        self.invalidate()?;
        config.validate()?;
        if snapshot.devices.len() > MAX_CANDIDATES || adb.len() > MAX_CANDIDATES {
            return Err(ErrorCode::OutputLimit);
        }
        if Instant::now()
            .checked_duration_since(snapshot.captured_at)
            .is_none_or(|age| age > ROUTE_MAX_AGE)
        {
            return Err(ErrorCode::DeviceDisconnected);
        }
        let mut topology_set = HashSet::new();
        let mut targets = HashSet::new();
        for d in &snapshot.devices {
            if !topology_set.insert(d.topology()?) {
                return Err(ErrorCode::DeviceIdentityAmbiguous);
            }
        }
        for a in adb {
            if let Some(t) = a.transport_id
                && (t == 0 || !targets.insert(t))
            {
                return Err(ErrorCode::DeviceIdentityAmbiguous);
            }
        }
        let mut serial_counts = HashMap::<String, usize>::new();
        for u in &snapshot.devices {
            if let Some(s) = serial(u) {
                *serial_counts.entry(s.to_owned()).or_default() += 1;
            }
        }
        let mut current = HashMap::new();
        let mut refs = Vec::new();
        for u in snapshot.devices {
            let topology = u.topology()?;
            let candidates = adb
                .iter()
                .filter(|a| a.usb.as_deref() == Some(topology.as_str()))
                .collect::<Vec<_>>();
            if candidates.len() != 1 {
                return Err(ErrorCode::DeviceIdentityAmbiguous);
            }
            let a = candidates[0];
            match_attachment(&u, a)?;
            let unique = serial(&u).is_some_and(|s| {
                serial_counts.get(s) == Some(&1)
                    && adb.iter().filter(|v| v.serial == s).count() == 1
            });
            let pair = config.pairings.iter().find(|p| p.matches(&u, a, &topology));
            let id = if let Some(p) = pair {
                if !unique && !p.port_association {
                    return Err(ErrorCode::DeviceIdentityAmbiguous);
                }
                p.device_id.clone()
            } else {
                // A moved/replaced explicitly paired device must not silently become
                // a fresh auto-selected identity. Require deliberate re-pairing.
                if config.pairings.iter().any(|p| {
                    p.topology == topology
                        || p.adb_serial == a.serial
                        || (serial(&u).is_some() && p.usb_serial.as_deref() == serial(&u))
                }) || !unique
                {
                    return Err(ErrorCode::DeviceIdentityAmbiguous);
                }
                let mut hasher = blake3::Hasher::new();
                hasher.update(b"cordgeist-auto-device-v1\0");
                hasher.update(&u.vendor_id.to_be_bytes());
                hasher.update(&u.product_id.to_be_bytes());
                hasher.update(a.serial.as_bytes());
                DeviceId(format!("device_{}", &hasher.finalize().to_hex()[..32]))
            };
            let reference = RouteRef {
                boot_id: self.boot_id.clone(),
                device_id: id.clone(),
                attachment: AttachmentGeneration(self.generation),
            };
            if current
                .insert(
                    id,
                    Current {
                        route: ResolvedRoute {
                            reference: reference.clone(),
                            kind: snapshot.kind,
                            usb: u,
                            adb: a.clone(),
                        },
                        captured_at: snapshot.captured_at,
                    },
                )
                .is_some()
            {
                return Err(ErrorCode::DeviceIdentityAmbiguous);
            }
            refs.push(reference);
        }
        refs.sort_by(|a, b| a.device_id.0.cmp(&b.device_id.0));
        self.current = current;
        Ok(refs)
    }
    /// Extend the current exact phone when a new sample is the same attachment.
    /// Any other sample fences the route so a later action sends nothing.
    pub fn renew_if_same(
        &mut self,
        snapshot: DiscoverySnapshot,
        adb: &[Attachment],
        config: &PairingConfig,
    ) -> Result<(), ErrorCode> {
        let mut trial = Self::new(self.boot_id.clone());
        let routes = match trial.refresh(snapshot, adb, config) {
            Ok(routes) => routes,
            Err(code) => {
                let _ = self.invalidate();
                return Err(code);
            }
        };
        if routes.len() != 1 {
            let _ = self.invalidate();
            return Err(if routes.is_empty() {
                ErrorCode::DeviceDisconnected
            } else {
                ErrorCode::DeviceIdentityAmbiguous
            });
        }
        let new = trial.resolve(&routes[0])?;
        let Some(old) = self.current.get(&new.reference().device_id) else {
            let _ = self.invalidate();
            return Err(ErrorCode::DeviceDisconnected);
        };
        let same = old.route.usb().topology().ok() == new.usb().topology().ok()
            && old.route.adb_attachment().transport_id == new.adb_attachment().transport_id
            && old.route.adb_attachment().serial == new.adb_attachment().serial
            && old.route.kind() == new.kind();
        if !same {
            let _ = self.invalidate();
            return Err(ErrorCode::DeviceIdentityAmbiguous);
        }
        let id = new.reference().device_id.clone();
        self.current.get_mut(&id).unwrap().captured_at = Instant::now();
        Ok(())
    }
    pub fn resolve(&self, reference: &RouteRef) -> Result<ResolvedRoute, ErrorCode> {
        if reference.boot_id != self.boot_id {
            return Err(ErrorCode::LeaseInvalid);
        }
        let current = self
            .current
            .get(&reference.device_id)
            .ok_or(ErrorCode::DeviceDisconnected)?;
        if &current.route.reference != reference {
            return Err(ErrorCode::LeaseInvalid);
        }
        if current.captured_at.elapsed() > ROUTE_MAX_AGE {
            return Err(ErrorCode::DeviceDisconnected);
        }
        Ok(current.route.clone())
    }
}

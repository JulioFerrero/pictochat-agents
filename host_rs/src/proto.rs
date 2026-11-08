//! 802.11 frames, the PictoChat beacon and the multiplay (WM port 0xC/0xD/0xE) side of a PictoChat host.
//! A port of the corresponding parts of pictochat.py; pictochat.md documents the protocol.

use std::collections::{BTreeMap, HashMap, HashSet, VecDeque};

pub type Mac = [u8; 6];

pub const LCG_A: u32 = 0x5EED_F715; // arm9 fn 0x0236B24C / 0x0236B1AC
pub const LCG_C: u32 = 0x1B0C_B173;
pub const BEACON_UPDATE_EVERY: u32 = 25; // beacons per counter step (about 2.5 s on a real DS)
pub const NINTENDO_OUI: [u8; 3] = [0x00, 0x09, 0xBF];
pub const BROADCAST: Mac = [0xFF; 6];
pub const MP_CMD: Mac = [0x03, 0x09, 0xBF, 0x00, 0x00, 0x00];
pub const MP_REPLY: Mac = [0x03, 0x09, 0xBF, 0x00, 0x00, 0x10];
pub const MP_ACK: Mac = [0x03, 0x09, 0xBF, 0x00, 0x00, 0x03];
pub const NINTENDO_OUIS: &[[u8; 3]] = &[
    [0x00, 0x09, 0xbf], [0x00, 0x16, 0x56], [0x00, 0x17, 0xab], [0x00, 0x19, 0x1d], [0x00, 0x1a, 0xe9],
    [0x00, 0x1b, 0x7a], [0x00, 0x1b, 0xea], [0x00, 0x1c, 0xbe], [0x00, 0x1d, 0xf0], [0x00, 0x1e, 0x35],
    [0x00, 0x1f, 0x32], [0x00, 0x1f, 0xc5], [0x00, 0x21, 0x47], [0x00, 0x22, 0xaa], [0x00, 0x22, 0xd7],
    [0x00, 0x23, 0x31], [0x00, 0x23, 0xcc], [0x00, 0x24, 0x44], [0x00, 0x24, 0xf3], [0x00, 0x25, 0x3f],
    [0x00, 0x25, 0xa0], [0x00, 0x27, 0x09], [0x40, 0xd2, 0x8a], [0xe8, 0x4e, 0xce], [0xa4, 0xc0, 0xe1],
    [0x7c, 0xbb, 0x8a], [0x9c, 0xba, 0xcf], [0xd8, 0x6b, 0xf7],
];

pub fn mac_str(m: &[u8]) -> String {
    m.iter().map(|b| format!("{:02x}", b)).collect::<Vec<_>>().join(":")
}

pub fn parse_mac(s: &str) -> Option<Mac> {
    let parts: Vec<&str> = s.split(':').collect();
    if parts.len() != 6 {
        return None;
    }
    let mut m = [0u8; 6];
    for (i, p) in parts.iter().enumerate() {
        m[i] = u8::from_str_radix(p, 16).ok()?;
    }
    Some(m)
}

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}

pub fn unhex(s: &str) -> Option<Vec<u8>> {
    let s = s.trim();
    if s.len() % 2 != 0 {
        return None;
    }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).ok()).collect()
}

fn u16le(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}

fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

/// 802.11 header (no FCS): frame control, duration 0, three addresses, sequence number.
pub fn dot11(fc0: u8, fc1: u8, a1: &Mac, a2: &Mac, a3: &Mac, seq: u16, body: &[u8]) -> Vec<u8> {
    let mut f = Vec::with_capacity(24 + body.len());
    f.extend_from_slice(&[fc0, fc1, 0, 0]);
    f.extend_from_slice(a1);
    f.extend_from_slice(a2);
    f.extend_from_slice(a3);
    f.extend_from_slice(&((seq & 0xFFF) << 4).to_le_bytes());
    f.extend_from_slice(body);
    f
}

// ------------------------------------------------------------------ received frames

#[allow(dead_code)]
pub struct Frame<'a> {
    pub ftype: u8,
    pub subtype: u8,
    pub a1: Mac,
    pub a2: Option<Mac>,
    pub a3: Option<Mac>,
    pub seq: u16,
    pub body: &'a [u8],
    pub cls: &'static str,
}

fn mac_at(d: &[u8], o: usize) -> Mac {
    let mut m = [0u8; 6];
    m.copy_from_slice(&d[o..o + 6]);
    m
}

pub fn parse_frame(d: &[u8]) -> Option<Frame<'_>> {
    if d.len() < 10 {
        return None;
    }
    let (fc0, fc1) = (d[0], d[1]);
    let ftype = (fc0 >> 2) & 3;
    let subtype = fc0 >> 4;
    let a1 = mac_at(d, 4);
    let (mut a2, mut a3, mut seq, mut body): (Option<Mac>, Option<Mac>, u16, &[u8]) = (None, None, 0, &[]);
    if ftype != 1 {
        if d.len() < 24 {
            return None;
        }
        a2 = Some(mac_at(d, 10));
        a3 = Some(mac_at(d, 16));
        seq = u16le(d, 22) >> 4;
        let mut hl = 24;
        if ftype == 2 {
            if fc1 & 3 == 3 {
                hl += 6;
            }
            if subtype & 8 != 0 {
                hl += 2;
            }
        }
        body = if d.len() > hl { &d[hl..] } else { &[] };
    } else if d.len() >= 16 {
        a2 = Some(mac_at(d, 10));
    }
    let cls = match ftype {
        0 => match subtype {
            0 => "assoc-req",
            1 => "assoc-resp",
            2 => "reassoc-req",
            3 => "reassoc-resp",
            4 => "probe-req",
            5 => "probe-resp",
            8 => "beacon",
            9 => "atim",
            10 => "disassoc",
            11 => "auth",
            12 => "deauth",
            13 => "action",
            _ => "mgmt",
        },
        1 => "ctrl",
        2 => {
            if a1 == MP_CMD {
                "MP-CMD"
            } else if a1 == MP_REPLY || a3 == Some(MP_REPLY) {
                "MP-REPLY" // real replies are ToDS: a1 = BSSID, a2 = client, a3 = 03:09:BF:00:00:10
            } else if a1 == MP_ACK {
                "MP-ACK"
            } else {
                "data"
            }
        }
        _ => "reserved",
    };
    Some(Frame { ftype, subtype, a1, a2, a3, seq, body, cls })
}

pub fn has_nintendo_ie(f: &Frame) -> bool {
    if f.ftype != 0 || !(f.subtype == 5 || f.subtype == 8) || f.body.len() < 12 {
        return false;
    }
    let ies = &f.body[12..];
    let mut i = 0;
    while i + 2 <= ies.len() {
        let (t, l) = (ies[i], ies[i + 1] as usize);
        if i + 2 + l > ies.len() {
            break;
        }
        if t == 0xDD && l >= 3 && ies[i + 2..i + 5] == NINTENDO_OUI {
            return true;
        }
        i += 2 + l;
    }
    false
}

// ------------------------------------------------------------------ beacon

/// Evolving beacon content: counter, LCG nonce, interval jitter, DTIM toggle, 802.11 sequence.
pub struct HostState {
    lcg: u32,
    pub room: u8,
    pub members: u8,  // host + joined clients (game-info byte 21)
    pub status: u16,  // 0x2348 = room just opened, 0xBD8A once a client connected
    pub counter: u16,
    seq: u16,
    pub nbeacons: u32,
    pub interval: u16,
    frozen: bool,     // a client has connected: keep interval and counter fixed, as a real host does
    pub tsf: u64,     // beacon timestamp, advanced by exactly interval * 1024 us per beacon
    nonce: u16,
}

impl HostState {
    pub fn new(seed: u32, room: u8) -> Self {
        let mut s = HostState {
            lcg: seed, room, members: 1, status: 0x2348, counter: 1, seq: 0, nbeacons: 0, interval: 100,
            frozen: false, tsf: 0, nonce: 0,
        };
        s.update(true);
        s
    }

    fn step(&mut self) -> u32 {
        self.lcg = LCG_A.wrapping_mul(self.lcg).wrapping_add(LCG_C);
        self.lcg
    }

    fn update(&mut self, first: bool) {
        if !first {
            self.counter = self.counter.wrapping_add(1);
        }
        // a real DS advances the LCG 4 times per update (verified on the capture); the first step is the nonce
        self.nonce = self.step() as u16;
        let mut r = 0;
        for _ in 0..3 {
            r = self.step();
        }
        self.interval = 100 + 2 * ((r >> 16) % 5) as u16;
    }

    pub fn next_mgmt(&mut self) -> u16 {
        let s = self.seq;
        self.seq = (self.seq + 1) & 0xFFF;
        s
    }

    /// A client connected or left: freeze the beacon timing and refresh only the nonce (arm9 FUN_0236AA6C).
    pub fn on_members_changed(&mut self) {
        self.frozen = true;
        self.nonce = self.step() as u16;
    }

    fn next(&mut self) -> (u8, u16) {
        if self.nbeacons > 0 && self.nbeacons % BEACON_UPDATE_EVERY == 0 && !self.frozen {
            self.update(false);
        }
        self.tsf += self.interval as u64 * 1024;
        let dtim = (self.nbeacons & 1) as u8;
        let seq = self.seq;
        self.nbeacons += 1;
        self.seq = (self.seq + 1) & 0xFFF;
        (dtim, seq)
    }
}

/// The 24 game-info bytes of a PictoChat beacon:
/// 01 00 00 00 00 00 00 00 | counter | 08 01 c0 00 c0 00 | status | nonce | room | members | 04 00.
fn gameinfo(st: &HostState) -> Vec<u8> {
    let mut g = vec![1, 0, 0, 0, 0, 0, 0, 0];
    g.extend_from_slice(&st.counter.to_le_bytes());
    g.extend_from_slice(&[0x08, 0x01, 0xc0, 0x00, 0xc0, 0x00]);
    g.extend_from_slice(&st.status.to_le_bytes());
    g.extend_from_slice(&st.nonce.to_le_bytes());
    g.extend_from_slice(&[st.room, st.members, 4, 0]);
    g
}

/// 802.11 beacon (no FCS) shaped like the DS Lite's. Advances st.
pub fn build_beacon(src: &Mac, channel: u8, st: &mut HostState) -> Vec<u8> {
    let tsf = st.tsf;
    let (dtim, seq) = st.next();
    let mut vendor = NINTENDO_OUI.to_vec();
    vendor.push(0);
    vendor.extend_from_slice(&0x000Au16.to_le_bytes());
    vendor.extend_from_slice(&0u16.to_le_bytes());
    vendor.extend_from_slice(&gameinfo(st));
    let mut body = Vec::with_capacity(64);
    body.extend_from_slice(&tsf.to_le_bytes());
    body.extend_from_slice(&st.interval.to_le_bytes());
    body.extend_from_slice(&0x0021u16.to_le_bytes());
    body.extend_from_slice(&[0x01, 0x02, 0x82, 0x84]);
    body.extend_from_slice(&[0x03, 0x01, channel]);
    body.extend_from_slice(&[0x05, 0x05, dtim, 2, 0, 0, 0]);
    body.push(0xDD);
    body.push(vendor.len() as u8);
    body.extend_from_slice(&vendor);
    dot11(0x80, 0x00, &BROADCAST, src, src, seq, &body)
}

/// Answer a DS auth / assoc request (open system, accept).
pub fn host_reply(src: &Mac, f: &Frame, st: &mut HostState, aid: u8) -> Option<Vec<u8>> {
    let ta = f.a2?;
    if f.cls == "auth" && f.body.len() >= 6 {
        let (alg, trans) = (u16le(f.body, 0), u16le(f.body, 2));
        if trans == 1 {
            let seq = st.next_mgmt();
            let mut body = alg.to_le_bytes().to_vec();
            body.extend_from_slice(&2u16.to_le_bytes());
            body.extend_from_slice(&0u16.to_le_bytes());
            return Some(dot11(0xB0, 0x00, &ta, src, src, seq, &body));
        }
    }
    if f.cls == "assoc-req" {
        let seq = st.next_mgmt();
        let mut body = 0x0021u16.to_le_bytes().to_vec();
        body.extend_from_slice(&0u16.to_le_bytes());
        body.extend_from_slice(&(0xC000u16 | aid as u16).to_le_bytes());
        body.extend_from_slice(&[0x01, 0x02, 0x82, 0x84]);
        return Some(dot11(0x10, 0x00, &ta, src, src, seq, &body));
    }
    None
}

// ------------------------------------------------------------------ PictoChat packets

/// Swap the two bytes of every 16-bit pair (how MACs appear in the multiplay roster and messages).
pub fn swap16(b: &[u8]) -> Vec<u8> {
    (0..b.len()).map(|i| b[i ^ 1]).collect()
}

/// Split a multiplay body into (port, payload) using the WM word: flags | port << 8 | payload_len / 2.
pub fn wm_entry(body: &[u8]) -> Option<(u8, &[u8])> {
    if body.len() < 2 {
        return None;
    }
    let w = u16le(body, 0);
    let end = (2 + (w as usize & 0xFF) * 2).min(body.len());
    Some((((w >> 8) & 0xF) as u8, &body[2..end]))
}

/// Port 0xD token grant: type 1, 0x14 bytes, sender id, length of the message that sender will send, random.
pub fn type1_payload(cid: u8, length: u32, rand: u32) -> Vec<u8> {
    let mut p = Vec::with_capacity(20);
    p.extend_from_slice(&1u16.to_le_bytes());
    p.extend_from_slice(&0x14u16.to_le_bytes());
    p.extend_from_slice(&[cid, 0]);
    p.extend_from_slice(&0xFFFFu16.to_le_bytes());
    p.extend_from_slice(&length.to_le_bytes());
    p.extend_from_slice(&0u32.to_le_bytes());
    p.extend_from_slice(&rand.to_le_bytes());
    p
}

fn utf16_name(name: &str) -> Vec<u8> {
    let mut b: Vec<u8> = name.encode_utf16().flat_map(|u| u.to_le_bytes()).collect();
    b.truncate(20);
    b
}

/// 84-byte `03 xx` member message (kind 0 = joining, 1 = ready): 03 kind | MAC swapped | UTF-16 name (20 bytes) |
/// zeros | colour u16 | 01 01.
pub fn member_info_message(kind: u8, mac: &Mac, name: &str, color: u16) -> Vec<u8> {
    let mut d = vec![0u8; 84];
    d[0] = 3;
    d[1] = kind;
    d[2..8].copy_from_slice(&swap16(mac));
    let nm = utf16_name(name);
    d[8..8 + nm.len()].copy_from_slice(&nm);
    d[80..82].copy_from_slice(&color.to_le_bytes());
    d[82] = 1;
    d[83] = 1;
    d
}

/// Split a message into port-0xE type-2 payloads (160 data bytes per fragment).
pub fn fragments(msg_id: u8, data: &[u8], junk: u8) -> Vec<Vec<u8>> {
    let mut out = Vec::new();
    let mut off = 0;
    while off < data.len().max(1) {
        let chunk = &data[off.min(data.len())..(off + 160).min(data.len())];
        let last = (off + 160 >= data.len()) as u8;
        let mut p = Vec::with_capacity(12 + chunk.len());
        p.extend_from_slice(&2u16.to_le_bytes());
        p.extend_from_slice(&(12 + chunk.len() as u16).to_le_bytes());
        p.extend_from_slice(&[msg_id, junk, chunk.len() as u8, last]);
        p.extend_from_slice(&(off as u32).to_le_bytes());
        p.extend_from_slice(chunk);
        out.push(p);
        off += 160;
    }
    out
}

/// A drawn chat message: 03 02 | MAC swapped | 00 04 00 00 00 00 | 01 04 09 0b 10 1b | 16 zeros | picture (1-5
/// lines of 2048 bytes: 8x8 tiles of 4bpp, 32 tiles per row).
pub fn chat_message(mac: &Mac, image: &[u8]) -> Vec<u8> {
    let mut m = vec![0x03, 0x02];
    m.extend_from_slice(&swap16(mac));
    m.extend_from_slice(&[0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x09, 0x0b, 0x10, 0x1b]);
    m.extend_from_slice(&[0u8; 16]);
    m.extend_from_slice(image);
    m
}

/// pixels: 80 rows of 256 palette indices (0-15) -> 10240 bytes in the 8x8-tile 4bpp layout used by PictoChat.
pub fn bitmap_to_image(pixels: &[Vec<u8>]) -> Vec<u8> {
    let px = |y: usize, x: usize| -> u8 { pixels.get(y).and_then(|r| r.get(x)).copied().unwrap_or(0) & 15 };
    let mut out = Vec::with_capacity(10240);
    for ty in 0..10 {
        for tx in 0..32 {
            for y in 0..8 {
                for x in (0..8).step_by(2) {
                    out.push(px(ty * 8 + y, tx * 8 + x) | (px(ty * 8 + y, tx * 8 + x + 1) << 4));
                }
            }
        }
    }
    out
}

// ------------------------------------------------------------------ multiplay host logic

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Stage {
    Assoc,
    Hello,
    Joined,
}

pub struct Client {
    pub mac: Mac,
    pub stage: Stage,
}

/// A packet for the polls: sent on up to `repeats` polls, moved on once confirmed after `min_sent` sends.
pub struct Item {
    pub port: u8,
    pub payload: Vec<u8>,
    pub repeats: u8,
    pub min_sent: u8,
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Owner {
    Host,
    Client(u8),
}

/// Packets that must go out back to back: a host message (token grant to itself + fragments), a joined DS's message
/// (the grant we give it + the echo of each fragment as it arrives), or a single packet. PictoChat lets one sender
/// at a time hold the token, so a host message must never start inside a DS message (the first queue was strictly
/// FIFO: a bot reply queued while the DS was sending went out between the DS's echoes, and the DS's remaining
/// echoes waited behind the whole reply, which greyed out the DS's send button).
pub struct Unit {
    pub items: VecDeque<Item>,
    pub owner: Owner,
    pub open: bool,          // a DS message still arriving: more echoes will be appended
    pub started: bool,       // at least one packet handed to the modem
    pub last_activity: f64,
    pub label: Option<(String, f64)>, // host chat message: description, time queued (for the delivery log)
}

/// An open DS message unit with no new fragment for this long is given up (the DS left or lost the token).
const CLIENT_UNIT_TIMEOUT: f64 = 2.0;

/// A message being received from a DS, fragment by fragment. Complete when every byte up to the end of the last
/// fragment has arrived (pictochat.py completed on the last fragment alone, so a late repeat of a finished
/// message's last fragment produced a second, zero-filled copy).
struct Assembly {
    buf: Vec<u8>,
    have: Vec<bool>,
    total: Option<usize>,
}

/// The multiplay (port 0xC/0xD/0xE) side of a PictoChat host, following the traces of a real host. I/O-free:
/// feed it what the clients send (`on_reply`); it queues the packets the polls should carry (`queue`).
/// Per client: assoc -> client hello (0xC type 6) -> host info (0xC type 4) + host member message (`03 00`) ->
/// client token request (0xD type 0) -> grant (0xD type 1, echo) -> client message -> verbatim echo -> after the
/// client's `03 00` the host sends `03 01`; after the client's `03 01` it is joined.
pub struct MultiplayHost {
    pub mac: Mac,
    pub name: String,
    pub color: u16,
    rng: u64,
    pub repeats: u8,
    pub clients: BTreeMap<u8, Client>,
    pub queue: VecDeque<Unit>,
    pub now: f64,            // current time (seconds), set by the caller; used for queue bookkeeping
    /// Minimum sends of a middle fragment (host messages and echoes). PictoChat takes in about one packet per
    /// 60-Hz frame (16.7 ms): a fragment replaced by the next one sooner is silently lost, and the DS shows what
    /// was left in its buffer from the previous message there. The caller sets this from the poll interval so a
    /// fragment stays on the air for at least ~25 ms. First and last fragments always get at least 5.
    pub mid_min_sent: u8,
    held: Vec<Vec<u8>>,
    seen_d0: HashMap<u32, f64>,
    seen_e: HashMap<Vec<u8>, f64>,
    asm: HashMap<u8, Assembly>,
    pub messages: Vec<(u8, Vec<u8>)>,
    sent_ready: HashSet<u8>,
    pub logs: Vec<String>,
}

impl MultiplayHost {
    pub fn new(mac: Mac, name: &str, color: u16, seed: u64, repeats: u8) -> Self {
        MultiplayHost {
            mac, name: name.to_string(), color, rng: seed | 1, repeats, clients: BTreeMap::new(),
            queue: VecDeque::new(), now: 0.0, mid_min_sent: 3, held: Vec::new(), seen_d0: HashMap::new(), seen_e: HashMap::new(),
            asm: HashMap::new(), messages: Vec::new(), sent_ready: HashSet::new(), logs: Vec::new(),
        }
    }

    pub fn rand(&mut self) -> u32 {
        // xorshift64*
        self.rng ^= self.rng >> 12;
        self.rng ^= self.rng << 25;
        self.rng ^= self.rng >> 27;
        (self.rng.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 32) as u32
    }

    fn log(&mut self, s: String) {
        self.logs.push(s);
    }

    pub fn roster(&self) -> Vec<u8> {
        let mut info = vec![0u8; 0x60];
        let macs: Vec<Mac> = std::iter::once(self.mac).chain(self.clients.values().map(|c| c.mac)).collect();
        for (i, m) in macs.iter().take(15).enumerate() {
            info[i * 6..i * 6 + 6].copy_from_slice(&swap16(m));
        }
        info
    }

    fn typed(&self, t: u16, val: u32) -> Vec<u8> {
        let mut p = t.to_le_bytes().to_vec();
        p.extend_from_slice(&0x68u16.to_le_bytes());
        p.extend_from_slice(&val.to_le_bytes());
        p.extend_from_slice(&self.roster());
        p
    }

    fn item(&self, port: u8, payload: Vec<u8>, repeats: Option<u8>, min_sent: u8) -> Item {
        Item { port, payload, repeats: repeats.unwrap_or(self.repeats), min_sent }
    }

    fn host_unit(&self, items: Vec<Item>, label: Option<String>) -> Unit {
        Unit { items: items.into(), owner: Owner::Host, open: false, started: false, last_activity: self.now,
               label: label.map(|l| (l, self.now)) }
    }

    /// Queue a single packet (at the back).
    pub fn put(&mut self, port: u8, payload: Vec<u8>, repeats: Option<u8>, min_sent: u8) {
        let it = self.item(port, payload, repeats, min_sent);
        let u = self.host_unit(vec![it], None);
        self.queue.push_back(u);
    }

    /// Queue a whole message from the host: token grant to itself, then each fragment, as one unit.
    pub fn put_message(&mut self, data: &[u8], msg_id: u8, label: Option<String>) {
        let r = self.rand();
        let mut items = vec![self.item(0xD, type1_payload(msg_id, data.len() as u32, r), None, 5)];
        let junk = self.rand() as u8;
        let frs = fragments(msg_id, data, junk);
        let n = frs.len();
        for (i, fr) in frs.into_iter().enumerate() {
            // middle fragments move on once confirmed and on the air long enough (mid_min_sent); the first and the
            // last keep at least 5, because PictoChat can drop a packet it received while busy with its previous step
            let edge = i == 0 || i + 1 == n;
            items.push(self.item(0xE, fr, None, if edge { self.mid_min_sent.max(5) } else { self.mid_min_sent }));
        }
        let u = self.host_unit(items, label);
        self.queue.push_back(u);
    }

    /// The open unit of a joined DS's message, created if needed: right after the unit being sent (and after
    /// other DS units already waiting there), ahead of host messages that have not started.
    fn client_unit(&mut self, cid: u8) -> &mut Unit {
        if let Some(i) = self.queue.iter().position(|u| u.owner == Owner::Client(cid) && u.open) {
            return &mut self.queue[i];
        }
        let mut pos = match self.queue.front() {
            Some(u) if u.started => 1,
            _ => 0,
        };
        while pos < self.queue.len() && matches!(self.queue[pos].owner, Owner::Client(_)) {
            pos += 1;
        }
        let u = Unit { items: VecDeque::new(), owner: Owner::Client(cid), open: true, started: false,
                       last_activity: self.now, label: None };
        self.queue.insert(pos, u);
        &mut self.queue[pos]
    }

    /// The next packet for the modem, and the label of a host chat message whose last packet this is. Waits (None)
    /// while the unit being sent is a DS message whose next fragment has not arrived yet.
    pub fn pop_item(&mut self, now: f64) -> Option<(Item, Option<(String, f64)>)> {
        loop {
            let f = self.queue.front_mut()?;
            if let Some(it) = f.items.pop_front() {
                f.started = true;
                let label = if f.items.is_empty() && !f.open { f.label.take() } else { None };
                return Some((it, label));
            }
            if f.open && now - f.last_activity < CLIENT_UNIT_TIMEOUT {
                return None;
            }
            self.queue.pop_front();
        }
    }

    pub fn queued_packets(&self) -> usize {
        self.queue.iter().map(|u| u.items.len()).sum()
    }

    pub fn any_joined(&self) -> bool {
        self.clients.values().any(|c| c.stage == Stage::Joined)
    }

    pub fn send_chat(&mut self, image: &[u8]) -> Vec<u8> {
        // like a real DS: send only up to the last 16-px line that contains ink (2048 bytes per line)
        let lines = (0..image.len() / 2048)
            .filter(|i| image[i * 2048..(i + 1) * 2048].iter().any(|&b| b != 0))
            .map(|i| i + 1)
            .max()
            .unwrap_or(1);
        let msg = chat_message(&self.mac, &image[..(lines * 2048).min(image.len())]);
        if self.any_joined() {
            self.put_message(&msg, 0, Some(format!("chat message ({} bytes)", msg.len())));
            self.log(format!("queued chat message ({} bytes, {} fragments)", msg.len(), (msg.len() + 159) / 160));
        } else {
            self.held.push(msg.clone());
            self.log("no client has joined yet; holding the chat message until one has".into());
        }
        msg
    }

    pub fn on_assoc(&mut self, aid: u8, mac: Mac) {
        self.clients.insert(aid, Client { mac, stage: Stage::Assoc });
        self.sent_ready.remove(&aid);
        self.asm.clear();
        self.queue.clear();
        self.log(format!("client {} assoc, AID {}", mac_str(&mac), aid));
    }

    pub fn on_deauth(&mut self, aid: u8) {
        self.clients.remove(&aid);
        if self.clients.is_empty() {
            self.queue.clear();
        }
    }

    /// Forget old duplicate-suppression entries (they only matter for half a second).
    pub fn prune(&mut self, now: f64) {
        self.seen_d0.retain(|_, t| now - *t < 5.0);
        self.seen_e.retain(|_, t| now - *t < 5.0);
    }

    pub fn on_reply(&mut self, aid: u8, port: u8, pl: &[u8], now: f64) {
        let stage = match self.clients.get(&aid) {
            Some(c) => c.stage,
            None => return,
        };
        if pl.len() < 2 {
            return;
        }
        let ptype = u16le(pl, 0);
        if port == 0xC && ptype == 6 && stage == Stage::Assoc {
            self.clients.get_mut(&aid).unwrap().stage = Stage::Hello;
            let r = self.rand();
            let info = self.typed(4, r);
            self.put(0xC, info, Some(9), 5);
            let m = member_info_message(0, &self.mac, &self.name.clone(), self.color);
            self.put_message(&m, 0, None);
            self.log(format!("client {} hello -> host info + host member message", aid));
        } else if port == 0xD && ptype == 0 && pl.len() >= 20 {
            let (cid, want, echo) = (pl[4], u32le(pl, 8), u32le(pl, 16));
            if now - self.seen_d0.get(&echo).copied().unwrap_or(-9.0) > 0.5 {
                self.seen_d0.insert(echo, now);
                let grant = self.item(0xD, type1_payload(cid, want, echo), None, 5);
                if stage == Stage::Joined {
                    // the DS's message goes ahead of host messages that have not started
                    let u = self.client_unit(cid);
                    u.items.push_back(grant);
                    u.last_activity = now;
                } else {
                    // during the join: after what is already queued, as the real host does
                    let u = self.host_unit(vec![grant], None);
                    self.queue.push_back(u);
                }
                self.log(format!("token request from id {} for a {}-byte message (random {:08x}) -> grant",
                                 cid, want, echo));
            }
        } else if port == 0xD && ptype == 3 {
            self.log(format!("ack from client {}", aid));
        } else if port == 0xE && ptype == 2 && pl.len() >= 12 {
            let (mid, flen, last, off) = (pl[4], pl[6] as usize, pl[7], u32le(pl, 8) as usize);
            if now - self.seen_e.get(pl).copied().unwrap_or(-9.0) <= 0.5 {
                return;
            }
            self.seen_e.insert(pl.to_vec(), now);
            // rebroadcast verbatim, with the same repeat rule as the host's own messages (see mid_min_sent)
            let edge = off == 0 || last != 0;
            let min = if edge { self.mid_min_sent.max(5) } else { self.mid_min_sent };
            let echo = self.item(0xE, pl.to_vec(), None, min);
            if stage == Stage::Joined {
                let u = self.client_unit(mid);
                u.items.push_back(echo);
                u.last_activity = now;
                if last != 0 {
                    u.open = false;
                }
            } else {
                let u = self.host_unit(vec![echo], None);
                self.queue.push_back(u);
            }
            let data = &pl[12..(12 + flen).min(pl.len())];
            if off == 0 {
                self.asm.remove(&mid); // a new message from this sender starts
            }
            let a = self.asm.entry(mid).or_insert(Assembly { buf: Vec::new(), have: Vec::new(), total: None });
            let end = off + data.len();
            if a.buf.len() < end {
                a.buf.resize(end, 0);
                a.have.resize(end, false);
            }
            a.buf[off..end].copy_from_slice(data);
            a.have[off..end].iter_mut().for_each(|h| *h = true);
            if last != 0 {
                a.total = Some(end);
            }
            let complete = match a.total {
                Some(t) => a.have.len() >= t && a.have[..t].iter().all(|&h| h),
                None => false,
            };
            if complete {
                let mut a = self.asm.remove(&mid).unwrap();
                a.buf.truncate(a.total.unwrap());
                let msg = a.buf;
                self.log(format!("message from id {} complete: {} bytes, type {:02x} {:02x}", mid, msg.len(),
                                 msg.first().copied().unwrap_or(0), msg.get(1).copied().unwrap_or(0)));
                if msg.starts_with(&[3, 0]) && !self.sent_ready.contains(&aid) {
                    self.sent_ready.insert(aid);
                    let m = member_info_message(1, &self.mac, &self.name.clone(), self.color);
                    self.put_message(&m, 0, None);
                } else if msg.starts_with(&[3, 1]) {
                    // the real host answers a newcomer's 03 01 with its own 03 01 once more
                    let m = member_info_message(1, &self.mac, &self.name.clone(), self.color);
                    self.put_message(&m, 0, None);
                    self.clients.get_mut(&aid).unwrap().stage = Stage::Joined;
                    for h in std::mem::take(&mut self.held) {
                        self.put_message(&h, 0, Some(format!("chat message ({} bytes, held until the join)", h.len())));
                    }
                    self.log(format!("client {} joined", aid));
                }
                self.messages.push((mid, msg));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fragments_roundtrip_and_late_repeat() {
        let me = [0, 0x1b, 0x7a, 0, 0, 1];
        let ds = [0, 0x1b, 0x7a, 0x12, 0x34, 0x56];
        let mut h = MultiplayHost::new(me, "rs", 2, 7, 8);
        h.on_assoc(1, ds);
        let msg: Vec<u8> = (0..1000u32).map(|i| (i * 7) as u8).collect();
        let frs = fragments(1, &msg, 0x55);
        for (i, fr) in frs.iter().enumerate() {
            h.on_reply(1, 0xE, fr, i as f64 * 0.01);
        }
        assert_eq!(h.messages.len(), 1);
        assert_eq!(h.messages[0].1, msg);
        // the last fragment repeated a second later must not produce a second (zero-filled) message
        h.on_reply(1, 0xE, frs.last().unwrap(), 2.0);
        assert_eq!(h.messages.len(), 1);
    }

    /// A joined DS's message goes ahead of a host message that has not started, is never split by one, and
    /// a host message that has started is finished first.
    #[test]
    fn ds_message_has_priority() {
        let me = [0, 0x1b, 0x7a, 0, 0, 1];
        let ds = [0, 0x1b, 0x7a, 1, 2, 3];
        let mut h = MultiplayHost::new(me, "rs", 2, 7, 8);
        h.on_assoc(1, ds);
        h.clients.get_mut(&1).unwrap().stage = Stage::Joined;
        h.put_message(&[0x11; 400], 0, Some("bot A".into()));   // 1 grant + 3 fragments
        h.put_message(&[0x22; 400], 0, Some("bot B".into()));
        let (first, _) = h.pop_item(0.0).unwrap();                // bot A has started
        assert_eq!(first.port, 0xD);
        // the DS asks for the token and starts sending
        let mut req = vec![0u8, 0, 0x14, 0, 1, 0, 0xff, 0xff];
        req.extend_from_slice(&800u32.to_le_bytes());
        req.extend_from_slice(&[0u8; 4]);
        req.extend_from_slice(&0x1234u32.to_le_bytes());
        h.on_reply(1, 0xD, &req, 0.1);
        let frs = fragments(1, &[0x33; 300], 0);
        h.on_reply(1, 0xE, &frs[0], 0.2);
        // the rest of bot A first (3 fragments), then the DS's grant and first echo, then wait for its next fragment
        let ports: Vec<u8> = (0..3).map(|_| h.pop_item(0.3).unwrap().0.port).collect();
        assert_eq!(ports, vec![0xE, 0xE, 0xE]);
        let (g, _) = h.pop_item(0.3).unwrap();
        assert_eq!((g.port, g.payload[0], g.payload[4]), (0xD, 1, 1)); // grant for id 1
        let (e, _) = h.pop_item(0.3).unwrap();
        assert_eq!(e.payload, frs[0]);
        assert!(h.pop_item(0.4).is_none()); // bot B must not start inside the DS's message
        h.on_reply(1, 0xE, &frs[1], 0.5);
        assert_eq!(h.pop_item(0.5).unwrap().0.payload, frs[1]); // last fragment: the DS unit is closed
        let (b, _) = h.pop_item(0.6).unwrap();
        assert_eq!(b.port, 0xD); // now bot B
        let mut last = None;
        while let Some((_, l)) = h.pop_item(0.7) {
            if l.is_some() {
                last = l;
            }
        }
        assert_eq!(last.unwrap().0, "bot B");
    }

    #[test]
    fn beacon_shape() {
        let mut st = HostState::new(1234, 0);
        let b = build_beacon(&[0, 0x1b, 0x7a, 0, 0, 1], 1, &mut st);
        let f = parse_frame(&b).unwrap();
        assert_eq!(f.cls, "beacon");
        assert!(has_nintendo_ie(&f));
        assert_eq!(b.len(), 24 + 12 + 4 + 3 + 7 + 2 + 32);
    }

    /// Byte-for-byte against pictochat.py (values generated with the Python builders).
    #[test]
    fn same_bytes_as_python() {
        let src = [0x00, 0x1b, 0x7a, 0x00, 0x00, 0x01];
        let mut st = HostState::new(1234, 0);
        let b: Vec<String> = (0..30).map(|_| hex(&build_beacon(&src, 1, &mut st))).collect();
        assert_eq!(b[0], "80000000ffffffffffff001b7a000001001b7a00000100000000000000000000660021000102828403010105050002000000dd200009bf000a000000010000000000000001000801c000c0004823adb400010400");
        assert_eq!(b[29], "80000000ffffffffffff001b7a000001001b7a000001d00100382e0000000000660021000102828403010105050102000000dd200009bf000a000000010000000000000002000801c000c0004823d1ac00010400");
        st.on_members_changed();
        st.members = 2;
        st.status = 0xBD8A;
        assert_eq!(hex(&build_beacon(&src, 1, &mut st)), "80000000ffffffffffff001b7a000001001b7a000001e00100d02f0000000000660021000102828403010105050002000000dd200009bf000a000000010000000000000002000801c000c0008abdb52900020400");
        assert_eq!(hex(&member_info_message(1, &src, "claude-s55", 2)), "03011b00007a010063006c0061007500640065002d007300350035000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000101");
        let data: Vec<u8> = (0..200u8).collect();
        let frs: Vec<String> = fragments(0, &data, 0x69).iter().map(|f| hex(f)).collect();
        assert_eq!(frs.join("|"), "0200ac000069a00000000000000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f|0200340000692801a0000000a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7");
        assert_eq!(hex(&type1_payload(1, 10276, 0x8fdca100)), "010014000100ffff242800000000000000a1dc8f");
        let px: Vec<Vec<u8>> = (0..80).map(|y| (0..256).map(|x| ((x * 3 + y) % 16) as u8).collect()).collect();
        let img = bitmap_to_image(&px);
        let sum = img.iter().enumerate().fold(0u32, |s, (i, &b)| s.wrapping_add((i as u32 + 1).wrapping_mul(b as u32)));
        assert_eq!((img.len(), sum), (10240, 2390203904));
        assert_eq!(hex(&img[..16]), "3096fc5241a70d6352b81e7463c92f85");
        let ds = [0x00, 0x1b, 0x7a, 0x12, 0x34, 0x56];
        let mut st2 = HostState::new(1, 0);
        let auth = dot11(0xB0, 0, &src, &ds, &src, 3230, &[0, 0, 1, 0, 0, 0]);
        let f = parse_frame(&auth).unwrap();
        assert_eq!(hex(&host_reply(&src, &f, &mut st2, 1).unwrap()), "b0000000001b7a123456001b7a000001001b7a0000010000000002000000");
        let assoc = dot11(0x00, 0, &src, &ds, &src, 3231, &[0u8; 10]);
        let f = parse_frame(&assoc).unwrap();
        assert_eq!(hex(&host_reply(&src, &f, &mut st2, 1).unwrap()), "10000000001b7a123456001b7a000001001b7a00000110002100000001c001028284");
    }

    #[test]
    fn join_flow_queues_host_info() {
        let me = [0, 0x1b, 0x7a, 0, 0, 1];
        let mut h = MultiplayHost::new(me, "rs", 2, 7, 8);
        h.on_assoc(1, [0, 0x1b, 0x7a, 1, 2, 3]);
        let mut hello = vec![6, 0, 0x68, 0];
        hello.extend_from_slice(&[0u8; 100]);
        h.on_reply(1, 0xC, &hello, 0.0);
        assert_eq!(h.clients[&1].stage, Stage::Hello);
        assert_eq!(h.queue[0].items[0].port, 0xC);
        assert_eq!(h.queue[0].items[0].repeats, 9);
        assert_eq!(&h.queue[0].items[0].payload[..2], &[4, 0]);
    }
}

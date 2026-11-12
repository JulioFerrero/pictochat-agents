//! pictochat-host: PictoChat room host for a real Nintendo DS, using the ESP32-C3 radio modem (esp32c3_modem/)
//! with the multiplay offload: the modem runs the poll / reply / ACK cycle, this program the PictoChat logic
//! (beacon, auth/assoc, join handshake, messages) and the JSON-lines link for pictochat_gui.py and other clients (e.g. a chatbot).
//! A port of `pictochat.py host --serial TTY --offload`; pictochat.py remains for monitor-mode adapters.

mod gui;
mod proto;
mod serial;

use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs::File;
use std::io::{BufWriter, Read, Write};
use std::sync::mpsc::{channel, Receiver, RecvTimeoutError};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde_json::{json, Value};

use proto::*;
use serial::{Modem, MpStats};

pub enum Event {
    Serial(u8, Vec<u8>),
    SerialGone(String),
    Gui(Value),
}

// packets queued on the ESP32 at most (it holds 128). Kept small so a DS's message can still go ahead of a host
// message that has not started (packets already on the ESP32 cannot be reordered); 24 packets are at least
// 24 polls (170 ms at 7 ms), several 50-ms 'M' reports, so the ESP32 does not run dry.
const MP_OUTSTANDING: u32 = 24;
// a DS that has not answered a single poll for this long is treated as gone (its deauth can get lost)
const CLIENT_SILENT_S: f64 = 5.0;
const USAGE: &str = "usage: pictochat-host [options]
  --serial TTY          ESP32-C3 modem with the offload firmware (default /dev/ttyACM0)
  --channel N           Wi-Fi channel (default 1)
  --mac MAC             host MAC / BSSID (default 00:1b:7a:00:00:01)
  --room N              room index for the beacon: 0-3 = Chat Room A-D (default 0)
  --name NAME           host nickname in the room, max 10 characters (default py)
  --color N             host favourite colour 0-15 (default 2)
  --gui-port PORT       JSON-lines port on 127.0.0.1 for pictochat_gui.py and other clients (0 = off; default 0)
  --out BASE            log and pcap file names BASE.log / BASE.pcap (default host_log)
  --no-pcap             do not write the pcap
  --mp-period-ms MS     interval between polls on the ESP32 (default 7)
  --reply-wait MS       wait at most this long for the DS's reply before the ACK (default 4)
  --clienttime N        CMD clienttime, reply window per client in us (default 0x3E6)
  --cmd-word N          third CMD body word of empty polls (default 0)
  --cmdcount N          first word of the ACK (default 0x54)
  --hello-repeats N     maximum number of polls that carry each packet (default 8)
  --min-air-ms MS       keep each message fragment on the air at least this long; the DS takes in about one packet
                        per 16.7-ms frame and loses the rest (default 25)
  --no-confirm          always send every packet --hello-repeats times
  --poll-cycle N        address the clients on every Nth poll (default 1)
  --seed N              beacon LCG seed (default random)
  --all                 forward and log every frame heard";

struct Args {
    serial: String,
    channel: u8,
    mac: Mac,
    room: u8,
    name: String,
    color: u16,
    gui_port: u16,
    out: String,
    pcap: bool,
    mp_period_ms: f64,
    reply_wait: f64,
    clienttime: u16,
    cmd_word: u16,
    cmdcount: u16,
    hello_repeats: u8,
    min_air_ms: f64,
    confirm: bool,
    poll_cycle: u8,
    seed: Option<u32>,
    all: bool,
}

fn parse_int(s: &str) -> Result<u64, String> {
    let r = if let Some(h) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        u64::from_str_radix(h, 16)
    } else {
        s.parse()
    };
    r.map_err(|_| format!("not a number: {}", s))
}

fn parse_args() -> Result<Args, String> {
    let mut a = Args {
        serial: "/dev/ttyACM0".into(), channel: 1, mac: [0x00, 0x1b, 0x7a, 0x00, 0x00, 0x01], room: 0,
        name: "py".into(), color: 2, gui_port: 0, out: "host_log".into(), pcap: true, mp_period_ms: 7.0,
        reply_wait: 4.0, clienttime: 0x3E6, cmd_word: 0, cmdcount: 0x54, hello_repeats: 8, min_air_ms: 25.0, confirm: true,
        poll_cycle: 1, seed: None, all: false,
    };
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < argv.len() {
        let k = argv[i].as_str();
        let flag = matches!(k, "--no-pcap" | "--no-confirm" | "--all" | "-h" | "--help");
        let v = if flag {
            ""
        } else {
            i += 1;
            argv.get(i).map(|s| s.as_str()).ok_or(format!("{} needs a value", k))?
        };
        match k {
            "--serial" => a.serial = v.into(),
            "--channel" => a.channel = parse_int(v)? as u8,
            "--mac" => a.mac = parse_mac(v).filter(|m| m[0] & 1 == 0).ok_or("--mac must be a unicast MAC")?,
            "--room" => a.room = parse_int(v)? as u8,
            "--name" => a.name = v.chars().take(10).collect(),
            "--color" => a.color = parse_int(v)? as u16,
            "--gui-port" => a.gui_port = parse_int(v)? as u16,
            "--out" => a.out = v.into(),
            "--no-pcap" => a.pcap = false,
            "--mp-period-ms" => a.mp_period_ms = v.parse().map_err(|_| "bad --mp-period-ms")?,
            "--reply-wait" => a.reply_wait = v.parse().map_err(|_| "bad --reply-wait")?,
            "--clienttime" => a.clienttime = parse_int(v)? as u16,
            "--cmd-word" => a.cmd_word = parse_int(v)? as u16,
            "--cmdcount" => a.cmdcount = parse_int(v)? as u16,
            "--hello-repeats" => a.hello_repeats = parse_int(v)?.clamp(1, 255) as u8,
            "--min-air-ms" => a.min_air_ms = v.parse().map_err(|_| "bad --min-air-ms")?,
            "--no-confirm" => a.confirm = false,
            "--poll-cycle" => a.poll_cycle = parse_int(v)?.clamp(1, 255) as u8,
            "--seed" => a.seed = Some(parse_int(v)? as u32),
            "--all" => a.all = true,
            "-h" | "--help" => return Err(String::new()),
            _ => return Err(format!("unknown option {}", k)),
        }
        i += 1;
    }
    Ok(a)
}

fn urandom_u64() -> u64 {
    let mut b = [0u8; 8];
    if File::open("/dev/urandom").and_then(|mut f| f.read_exact(&mut b)).is_err() {
        let t = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
        return t.as_nanos() as u64 ^ std::process::id() as u64;
    }
    u64::from_le_bytes(b)
}

/// wifi_phy_rate_t -> rate in 500 kbit/s units (radiotap).
fn rate_500k(r: u8) -> u8 {
    match r {
        0 => 2, 1 | 5 => 4, 2 | 6 => 11, 3 | 7 => 22, 8 => 96, 9 => 48, 10 => 24, 11 => 12, 12 => 108, 13 => 72,
        14 => 36, 15 => 18, _ => 0,
    }
}

struct Host {
    a: Args,
    t0: Instant,
    logf: BufWriter<File>,
    pcap: Option<BufWriter<File>>,
    gui: Option<gui::Gui>,
    modem: Modem,
    st: HostState,
    ml: MultiplayHost,
    aids: BTreeMap<Mac, u8>,
    last_reply: HashMap<u8, f64>, // AID -> last time the ESP32 saw it answer a poll (from the 'M' reports)
    stations: BTreeMap<Mac, u32>,
    answered: HashSet<(Mac, &'static str, u16)>,
    rx_seen: HashMap<(u8, Vec<u8>), f64>,
    ptype_seen: HashSet<(u8, i32)>,
    mp: Option<MpStats>,
    deliveries: Vec<(u16, String, f64)>, // (packet count since the last clear that marks the end, label, time queued)
    tx_errors: u32,
    beacons: u32,
    logged: u32,
    freq: u16,
}

impl Host {
    fn now(&self) -> f64 {
        self.t0.elapsed().as_secs_f64()
    }

    fn emit(&mut self, msg: &str) {
        let t = self.now();
        let line = format!("{:9.3} {}", t, msg);
        println!("{}", line);
        let _ = writeln!(self.logf, "{}", line);
        let _ = self.logf.flush();
        if let Some(g) = &self.gui {
            g.send(json!({"kind": "log", "t": t, "text": msg}));
        }
    }

    fn gui(&self, mut obj: Value) {
        if let Some(g) = &self.gui {
            obj["t"] = json!(self.now());
            g.send(obj);
        }
    }

    fn flush_ml_logs(&mut self) {
        for m in std::mem::take(&mut self.ml.logs) {
            self.emit(&format!("# {}", m));
        }
    }

    fn pcap_write(&mut self, rssi: i8, rate: u8, frame: &[u8]) {
        let Some(p) = self.pcap.as_mut() else { return };
        let t = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
        let mut rec = Vec::with_capacity(31 + frame.len());
        let len = (15 + frame.len()) as u32;
        rec.extend_from_slice(&(t.as_secs() as u32).to_le_bytes());
        rec.extend_from_slice(&t.subsec_micros().to_le_bytes());
        rec.extend_from_slice(&len.to_le_bytes());
        rec.extend_from_slice(&len.to_le_bytes());
        // radiotap: version, pad, length 15, present = rate | channel | dBm signal
        rec.extend_from_slice(&[0, 0, 15, 0]);
        rec.extend_from_slice(&((1u32 << 2) | (1 << 3) | (1 << 5)).to_le_bytes());
        rec.extend_from_slice(&[rate_500k(rate), 0]);
        rec.extend_from_slice(&self.freq.to_le_bytes());
        rec.extend_from_slice(&0x00A0u16.to_le_bytes());
        rec.push(rssi as u8);
        rec.extend_from_slice(frame);
        let _ = p.write_all(&rec);
        let _ = p.flush();
    }

    fn send_frame(&mut self, frame: &[u8], what: &str) {
        if let Err(e) = self.modem.transmit(frame) {
            self.emit(&format!("# {} send error: {}", what, e));
        }
    }

    fn sync_clients(&mut self, reopen: bool) {
        let list: Vec<(Mac, u8)> = self.aids.iter().map(|(m, a)| (*m, *a)).collect();
        if let Err(e) = self.modem.mp_clients(&list, reopen) {
            self.emit(&format!("# modem write error: {}", e));
        }
    }

    fn outstanding(&self) -> u32 {
        match self.mp {
            Some(m) if m.epoch == self.modem.epoch => m.qlen as u32 + self.modem.pushed.wrapping_sub(m.queued) as u32,
            _ => self.modem.pushed as u32,
        }
    }

    /// Hand queued packets to the ESP32, which repeats each one on its polls until the DS confirmed it.
    fn feed(&mut self) {
        while self.outstanding() < MP_OUTSTANDING {
            let now = self.now();
            let Some((it, label)) = self.ml.pop_item(now) else { break };
            let min_sent = if self.a.confirm { it.min_sent } else { it.repeats };
            if let Err(e) = self.modem.mp_push(it.port, &it.payload, min_sent, it.repeats) {
                self.emit(&format!("# modem write error: {}", e));
            }
            let ptype = if it.payload.len() >= 2 { u16::from_le_bytes([it.payload[0], it.payload[1]]) as i64 } else { -1 };
            self.gui(json!({"kind": "tx", "port": it.port, "ptype": ptype, "hex": hex(&it.payload), "repeats": 0}));
            if let Some((desc, t)) = label {
                self.deliveries.push((self.modem.pushed, desc, t));
            }
        }
    }

    /// Log host messages whose last packet the ESP32 has finished (sent and confirmed, or sent the maximum).
    fn check_deliveries(&mut self) {
        let Some(m) = self.mp else { return };
        if m.epoch != self.modem.epoch || self.deliveries.is_empty() {
            return;
        }
        let finished = m.queued.wrapping_sub(m.qlen);
        let now = self.now();
        let (done, wait): (Vec<_>, Vec<_>) =
            self.deliveries.drain(..).partition(|(end, _, _)| finished.wrapping_sub(*end) as i16 >= 0);
        self.deliveries = wait;
        for (_, desc, t) in done {
            self.emit(&format!("# delivered: {} {:.2} s after it was queued", desc, now - t));
        }
    }

    fn clear_modem_queue(&mut self) {
        let _ = self.modem.mp_clear();
        self.deliveries.clear();
    }

    fn mp_config(&mut self) {
        // sends per middle fragment so it stays on the air for --min-air-ms (see MultiplayHost::mid_min_sent)
        let n = (self.a.min_air_ms / self.a.mp_period_ms.max(0.5)).ceil().clamp(1.0, self.a.hello_repeats as f64);
        self.ml.mid_min_sent = n as u8;
        let a = &self.a;
        let r = self.modem.mp_config(true, a.clienttime, a.cmd_word, a.cmdcount, (a.mp_period_ms * 1000.0) as u32,
                                     (a.reply_wait * 1000.0) as u32, a.poll_cycle);
        if let Err(e) = r {
            self.emit(&format!("# modem write error: {}", e));
        }
    }

    fn status(&mut self) {
        let m = self.mp.unwrap_or_default();
        let stations = self.stations.iter().map(|(k, v)| format!("{}({})", mac_str(k), v)).collect::<Vec<_>>();
        let aids = self.aids.iter().map(|(k, v)| format!("{}={}", mac_str(k), v)).collect::<Vec<_>>();
        let line = format!(
            "# status: {} beacons sent, counter={}, interval={}TU, stations heard: {}, frames logged: {}, aids: {}, \
             CMD sent {}, replies heard {}, ACK sent {}, queued on the ESP32 {} (+{} here), ESP32 TX errors {}",
            self.beacons, self.st.counter, self.st.interval,
            if stations.is_empty() { "none".into() } else { stations.join(", ") }, self.logged,
            if aids.is_empty() { "none".into() } else { aids.join(", ") }, m.polls, m.replies, m.acks, m.qlen, self.ml.queued_packets(), m.txerr);
        self.emit(&line);
        let aid_map: serde_json::Map<String, Value> = self.aids.iter().map(|(k, v)| (mac_str(k), json!(v))).collect();
        self.gui(json!({"kind": "state", "version": env!("CARGO_PKG_VERSION"),
                        "commit": option_env!("PICTOCHAT_COMMIT").unwrap_or(""), "impl": "rust",
                        "radio": "ESP32-C3 modem, polls on the ESP32", "uptime": self.t0.elapsed().as_secs(),
                        "aids": aid_map, "cmd": m.polls, "replies": m.replies,
                        "beacons": self.beacons, "echo": false}));
    }

    fn on_gui(&mut self, c: Value) {
        let cmd = c.get("cmd").and_then(|v| v.as_str()).unwrap_or("").to_string();
        match cmd.as_str() {
            "raw" => {
                // {"cmd":"raw","port":14,"hex":"...","repeats":6}: a payload on a port
                let port = c.get("port").and_then(|v| v.as_u64());
                let pl = c.get("hex").and_then(|v| v.as_str()).and_then(unhex);
                match (port, pl) {
                    (Some(port), Some(pl)) => {
                        let reps = c.get("repeats").and_then(|v| v.as_u64()).unwrap_or(self.a.hello_repeats as u64);
                        self.emit(&format!("# gui: queued raw port 0x{:x} payload {}", port, hex(&pl)));
                        self.gui(json!({"kind": "message", "id": 0, "hex": hex(&pl), "dir": "tx"}));
                        self.ml.put(port as u8, pl, Some(reps.clamp(1, 255) as u8), 5);
                    }
                    _ => self.emit(&format!("# gui: bad command {}", c)),
                }
            }
            "announce" => {
                let name = c.get("name").and_then(|v| v.as_str()).filter(|s| !s.is_empty())
                    .map(|s| s.to_string()).unwrap_or(self.a.name.clone());
                let pl = member_info_message(0, &self.a.mac, &name, self.a.color);
                self.ml.put_message(&pl, 0, None);
                self.emit("# gui: queued host member message");
                self.gui(json!({"kind": "message", "id": 0, "hex": hex(&pl), "dir": "tx"}));
            }
            "set_name" => {
                // {"cmd":"set_name","name":"..."}: nickname for the joins that follow (max 10)
                if let Some(n) = c.get("name").and_then(|v| v.as_str()) {
                    let name: String = n.chars().take(10).collect();
                    self.ml.name = name.clone();
                    self.a.name = name.clone();
                    self.emit(&format!("# gui: host nickname is now {:?} (shown to DSes that join from now on)", name));
                }
            }
            "chat" => {
                // {"cmd":"chat","pixels":[[...]*256]*80}: palette indices
                let Some(rows) = c.get("pixels").and_then(|v| v.as_array()) else {
                    self.emit("# gui: chat without pixels");
                    return;
                };
                let px: Vec<Vec<u8>> = rows.iter()
                    .map(|r| r.as_array().map(|r| r.iter().map(|x| x.as_u64().unwrap_or(0) as u8).collect())
                        .unwrap_or_default())
                    .collect();
                let msg = self.ml.send_chat(&bitmap_to_image(&px));
                self.gui(json!({"kind": "message", "id": 0, "hex": hex(&msg), "dir": "tx"}));
            }
            "mp_period" => {
                // {"cmd":"mp_period","ms":5}: change the ESP32 poll interval while running
                if let Some(ms) = c.get("ms").and_then(|v| v.as_f64()) {
                    self.a.mp_period_ms = ms;
                    self.mp_config();
                    self.emit(&format!("# gui: ESP32 poll interval now {:.2} ms, each fragment sent at least {} times",
                                       ms, self.ml.mid_min_sent));
                }
            }
            "echo" => self.emit("# gui: the host always echoes client messages (echo setting ignored)"),
            _ => self.emit(&format!("# gui: unknown command {}", c)),
        }
        self.flush_ml_logs();
    }

    fn on_serial(&mut self, kind: u8, p: Vec<u8>) {
        match kind {
            b'R' if p.len() > 6 => self.on_frame(p[0] as i8, p[1], &p[6..]),
            b'S' => {
                let text = String::from_utf8_lossy(&p).to_string();
                self.emit(&format!("# esp32: {}", text));
            }
            b'E' if p.len() >= 6 => {
                self.tx_errors += 1;
                if self.tx_errors <= 5 || self.tx_errors % 1000 == 0 {
                    let err = i32::from_le_bytes([p[0], p[1], p[2], p[3]]);
                    let fc = u16::from_le_bytes([p[4], p[5]]);
                    self.emit(&format!("# esp32: TX error {} (0x{:x}) for frame control {:04x}, {} errors so far",
                                       err, err, fc, self.tx_errors));
                }
            }
            b'M' => {
                self.mp = MpStats::parse(&p).or(self.mp);
                if let Some(mask) = self.mp.and_then(|m| m.answered) {
                    let now = self.now();
                    for aid in 1..16u8 {
                        if mask & (1 << aid) != 0 && self.last_reply.contains_key(&aid) {
                            self.last_reply.insert(aid, now);
                        }
                    }
                    self.check_silent();
                }
                self.check_deliveries();
            }
            _ => {}
        }
    }

    /// A DS left (deauth/disassoc) or went silent. Clients (e.g. a chatbot) watch for this log line.
    fn remove_client(&mut self, mac: &Mac, reason: &str) {
        let Some(aid) = self.aids.remove(mac) else { return };
        self.last_reply.remove(&aid);
        self.emit(&format!("# station {} deauthenticated (AID {} freed), {}", mac_str(mac), aid, reason));
        self.ml.on_deauth(aid);
        self.st.members = 1 + self.aids.len() as u8;
        self.st.on_members_changed();
        if self.aids.is_empty() {
            self.clear_modem_queue();
        }
        self.sync_clients(false);
    }

    /// Drop DSes that stopped answering polls (needs the firmware's 'answered' field).
    fn check_silent(&mut self) {
        let now = self.now();
        let gone: Vec<Mac> = self.aids.iter()
            .filter(|(_, aid)| self.last_reply.get(aid).map_or(false, |t| now - t > CLIENT_SILENT_S))
            .map(|(m, _)| *m)
            .collect();
        for m in gone {
            self.remove_client(&m, &format!("reason timeout (no answer to any poll for {:.0} s)", CLIENT_SILENT_S));
        }
    }

    fn on_frame(&mut self, rssi: i8, rate: u8, data: &[u8]) {
        let Some(f) = parse_frame(data) else { return };
        let src = self.a.mac;
        let now = self.now();
        if f.a2 == Some(src) && matches!(f.cls, "beacon" | "MP-CMD" | "MP-ACK") {
            return; // our own transmission
        }
        if f.cls == "beacon" && !self.a.all && !(has_nintendo_ie(&f) && f.a2 != Some(src)) {
            return;
        }
        let mine = f.a1 == src || f.a2 == Some(src);
        let from_nin = f.a2.map(|m| NINTENDO_OUIS.contains(&[m[0], m[1], m[2]])).unwrap_or(false);
        if !(self.a.all || mine || from_nin) {
            return;
        }
        self.pcap_write(rssi, rate, data);
        let a2 = f.a2.unwrap_or([0; 6]);
        if f.cls == "MP-REPLY" {
            if let (Some((port, pl)), Some(&aid)) = (wm_entry(f.body), self.aids.get(&a2)) {
                if !pl.is_empty() {
                    let ptype = if pl.len() >= 2 { u16::from_le_bytes([pl[0], pl[1]]) as i32 } else { -1 };
                    if self.ptype_seen.insert((port, ptype)) {
                        let shown = &pl[..pl.len().min(24)];
                        self.emit(&format!("# DS packet: port 0x{:x} type {} len {} payload={}", port, ptype,
                                           pl.len(), hex(shown)));
                    }
                    let key = (port, pl.to_vec());
                    if now - self.rx_seen.get(&key).copied().unwrap_or(-9.0) > 0.5 {
                        self.rx_seen.insert(key, now);
                        self.gui(json!({"kind": "rx", "port": port, "ptype": ptype, "hex": hex(pl),
                                        "mac": mac_str(&a2)}));
                    }
                    let n = self.ml.messages.len();
                    self.ml.on_reply(aid, port, pl, now);
                    self.flush_ml_logs();
                    let new: Vec<(u8, Vec<u8>)> = self.ml.messages.drain(n..).collect();
                    for (mid, msg) in new {
                        self.emit(&format!("# DS message complete: id {}, {} bytes: {}", mid, msg.len(),
                                           hex(&msg[..msg.len().min(48)])));
                        self.gui(json!({"kind": "message", "id": mid, "hex": hex(&msg), "dir": "rx"}));
                    }
                }
            }
            return;
        }
        if matches!(f.cls, "deauth" | "disassoc") {
            let reason = format!("reason {} ({})", hex(&f.body[..f.body.len().min(2)]), f.cls);
            self.remove_client(&a2, &reason);
        }
        if from_nin {
            let c = self.stations.entry(a2).or_insert(0);
            *c += 1;
            if *c == 1 {
                self.emit(&format!("# NEW STATION {} (first frame: {})", mac_str(&a2), f.cls));
            }
        }
        self.logged += 1;
        if f.cls != "beacon" && !f.cls.starts_with("MP-") {
            let shown = &f.body[..f.body.len().min(64)];
            self.emit(&format!("    ch{} {:>4} dBm  {:<12} {} > {} seq={} len={} body={}", self.a.channel, rssi,
                               f.cls, mac_str(&a2), mac_str(&f.a1), f.seq, data.len(), hex(shown)));
        }
        if f.a1 == src && f.a2.is_some() {
            let mut aid = 1;
            if f.cls == "assoc-req" {
                if !self.aids.contains_key(&a2) {
                    let used: HashSet<u8> = self.aids.values().copied().collect();
                    if let Some(free) = (1..16u8).find(|a| !used.contains(a)) {
                        self.aids.insert(a2, free);
                    }
                }
                aid = self.aids.get(&a2).copied().unwrap_or(1);
                self.emit(&format!("# assoc-req from {} -> AID {}", mac_str(&a2), aid));
                if self.aids.contains_key(&a2) {
                    self.last_reply.insert(aid, self.now()); // the timeout starts at the association
                    self.ml.on_assoc(aid, a2);
                    self.flush_ml_logs();
                    self.clear_modem_queue(); // on_assoc dropped the queue: drop the modem's too
                    self.sync_clients(true);
                    self.st.members = 1 + self.aids.len() as u8;
                    self.st.status = 0xBD8A;
                    self.st.on_members_changed();
                }
            }
            if let Some(reply) = host_reply(&src, &f, &mut self.st, aid) {
                let key = (a2, f.cls, f.seq);
                if self.answered.insert(key) {
                    self.send_frame(&reply, "reply");
                    self.pcap_write(0, 1, &reply);
                    self.emit(&format!("# -> replied to {} (seq {}) with {}", f.cls, f.seq, hex(&reply[24..])));
                }
            }
        }
    }
}

fn wait_ready(rx: &Receiver<Event>, host: &mut Host) -> Result<String, String> {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            return Err("no answer from the ESP32 modem (flashed with esp32c3_modem/?)".into());
        }
        match rx.recv_timeout(left) {
            Ok(Event::Serial(b'S', p)) => {
                let text = String::from_utf8_lossy(&p).to_string();
                host.emit(&format!("# esp32: {}", text));
                if text.starts_with("ready") {
                    return Ok(text);
                }
            }
            Ok(Event::Serial(k, p)) => host.on_serial(k, p),
            Ok(Event::SerialGone(e)) => return Err(format!("modem link closed: {}", e)),
            Ok(Event::Gui(_)) => {}
            Err(_) => return Err("no answer from the ESP32 modem (flashed with esp32c3_modem/?)".into()),
        }
    }
}

fn run() -> Result<(), String> {
    let a = match parse_args() {
        Ok(a) => a,
        Err(e) if e.is_empty() => {
            println!("{}", USAGE);
            return Ok(());
        }
        Err(e) => return Err(format!("{}\n{}", e, USAGE)),
    };
    let (tx, rx) = channel::<Event>();
    let modem = serial::open(&a.serial, tx.clone()).map_err(|e| format!("cannot open {}: {}", a.serial, e))?;
    let logf = BufWriter::new(File::create(format!("{}.log", a.out)).map_err(|e| format!("{}.log: {}", a.out, e))?);
    let pcap = if a.pcap {
        let mut p = BufWriter::new(File::create(format!("{}.pcap", a.out)).map_err(|e| format!("{}.pcap: {}", a.out, e))?);
        let mut h = Vec::new();
        h.extend_from_slice(&0xA1B2_C3D4u32.to_le_bytes());
        h.extend_from_slice(&2u16.to_le_bytes());
        h.extend_from_slice(&4u16.to_le_bytes());
        h.extend_from_slice(&[0u8; 8]);
        h.extend_from_slice(&65535u32.to_le_bytes());
        h.extend_from_slice(&127u32.to_le_bytes()); // radiotap
        p.write_all(&h).map_err(|e| e.to_string())?;
        Some(p)
    } else {
        None
    };
    let gui = if a.gui_port != 0 {
        Some(gui::Gui::start(a.gui_port, tx.clone()).map_err(|e| format!("gui port {}: {}", a.gui_port, e))?)
    } else {
        None
    };
    let seed = urandom_u64();
    let st = HostState::new(a.seed.unwrap_or(seed as u32), a.room);
    let ml = MultiplayHost::new(a.mac, &a.name, a.color, seed.rotate_left(17), a.hello_repeats);
    let freq = if a.channel == 14 { 2484 } else { 2407 + 5 * a.channel as u16 };
    let mut h = Host {
        a, t0: Instant::now(), logf, pcap, gui, modem, st, ml, aids: BTreeMap::new(), last_reply: HashMap::new(), stations: BTreeMap::new(),
        answered: HashSet::new(), rx_seen: HashMap::new(), ptype_seen: HashSet::new(), mp: None,
        deliveries: Vec::new(), tx_errors: 0,
        beacons: 0, logged: 0, freq,
    };
    let (ch, mac, all) = (h.a.channel, h.a.mac, h.a.all);
    h.modem.config(ch, &mac, all).map_err(|e| format!("modem write: {}", e))?;
    let ready = wait_ready(&rx, &mut h)?;
    if !ready.contains(" mp=1") {
        return Err("the ESP32 firmware has no multiplay offload (no mp=1): flash esp32c3_modem/ again".into());
    }
    h.mp_config();
    let _ = h.modem.mp_clear();
    h.sync_clients(false);
    h.emit(&format!(
        "# beaconing as {} on channel {}, room index {} (0=A); polling on the ESP32 every {:.1} ms, each fragment \
         sent at least {} times; logging to {}.log{}",
        mac_str(&mac), ch, h.a.room, h.a.mp_period_ms, h.ml.mid_min_sent, h.a.out, if h.a.pcap { " / .pcap" } else { "" }));

    let mut next_beacon = Instant::now();
    let mut next_status = Instant::now() + Duration::from_secs(5);
    let mut next_prune = Instant::now() + Duration::from_secs(5);
    loop {
        h.feed();
        let now = Instant::now();
        if now >= next_beacon {
            let b = build_beacon(&mac, ch, &mut h.st);
            h.send_frame(&b, "beacon");
            h.beacons += 1;
            next_beacon += Duration::from_micros(h.st.interval as u64 * 1024);
            if next_beacon + Duration::from_secs(1) < now {
                next_beacon = now;
            }
        }
        if now >= next_status {
            h.status();
            next_status = now + Duration::from_secs(5);
        }
        if now >= next_prune {
            let t = h.now();
            h.ml.prune(t);
            h.rx_seen.retain(|_, s| t - *s < 5.0);
            next_prune = now + Duration::from_secs(5);
        }
        let wait = next_beacon.min(next_status).saturating_duration_since(Instant::now()).min(Duration::from_millis(20));
        let ev = rx.recv_timeout(wait);
        h.ml.now = h.now();
        match ev {
            Ok(Event::Serial(k, p)) => h.on_serial(k, p),
            Ok(Event::Gui(c)) => h.on_gui(c),
            Ok(Event::SerialGone(e)) => {
                h.emit(&format!("# the ESP32 modem went away ({}); exiting", e));
                return Err(format!("modem link closed: {}", e));
            }
            Err(RecvTimeoutError::Timeout) => {}
            Err(RecvTimeoutError::Disconnected) => return Err("internal channel closed".into()),
        }
    }
}

fn main() {
    if let Err(e) = run() {
        eprintln!("pictochat-host: {}", e);
        std::process::exit(1);
    }
}

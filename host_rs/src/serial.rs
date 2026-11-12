//! USB link to the ESP32-C3 radio modem (esp32c3_modem/). Framing both ways:
//! AA 55 | type u8 | len u16 LE | payload | xor8 of type, len bytes and payload (see modem.c for the types).

use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::io::AsRawFd;
use std::sync::mpsc::Sender;

use crate::proto::Mac;
use crate::Event;

pub struct Modem {
    w: File,
    pub epoch: u8,
    pub pushed: u16, // packets pushed since the last clear (mod 65536)
}

/// Raw mode: no echo, no line discipline, no CR/LF translation (the USB CDC link ignores the baud rate).
fn make_raw(f: &File) -> std::io::Result<()> {
    unsafe {
        let fd = f.as_raw_fd();
        let mut t: libc::termios = std::mem::zeroed();
        if libc::tcgetattr(fd, &mut t) != 0 {
            return Err(std::io::Error::last_os_error());
        }
        libc::cfmakeraw(&mut t);
        t.c_cc[libc::VMIN] = 1;
        t.c_cc[libc::VTIME] = 0;
        if libc::tcsetattr(fd, libc::TCSANOW, &t) != 0 {
            return Err(std::io::Error::last_os_error());
        }
        libc::tcflush(fd, libc::TCIOFLUSH);
    }
    Ok(())
}

pub fn open(path: &str, tx: Sender<Event>) -> std::io::Result<Modem> {
    let f = OpenOptions::new().read(true).write(true).custom_flags(libc::O_NOCTTY).open(path)?;
    // one host per modem: a second program on the same port would steal bytes from this one
    if unsafe { libc::flock(f.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        return Err(std::io::Error::new(std::io::ErrorKind::WouldBlock, "the port is in use by another host"));
    }
    make_raw(&f)?;
    let r = f.try_clone()?;
    std::thread::spawn(move || reader(r, tx));
    Ok(Modem { w: f, epoch: 0, pushed: 0 })
}

fn reader(mut r: File, tx: Sender<Event>) {
    let mut buf: Vec<u8> = Vec::with_capacity(16384);
    let mut chunk = [0u8; 4096];
    loop {
        let n = match r.read(&mut chunk) {
            Ok(0) => {
                let _ = tx.send(Event::SerialGone("end of file".into()));
                return;
            }
            Ok(n) => n,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => {
                let _ = tx.send(Event::SerialGone(e.to_string()));
                return;
            }
        };
        buf.extend_from_slice(&chunk[..n]);
        loop {
            let Some(i) = buf.windows(2).position(|w| w == [0xAA, 0x55]) else {
                let keep = buf.len().min(1);
                buf.drain(..buf.len() - keep);
                break;
            };
            buf.drain(..i);
            if buf.len() < 5 {
                break;
            }
            let (kind, len) = (buf[2], u16::from_le_bytes([buf[3], buf[4]]) as usize);
            if len > 4096 {
                buf.drain(..2);
                continue;
            }
            if buf.len() < 6 + len {
                break;
            }
            let x = buf[2..5 + len].iter().fold(0u8, |a, b| a ^ b);
            if x != buf[5 + len] {
                buf.drain(..2); // not a real frame start: resync
                continue;
            }
            let payload = buf[5..5 + len].to_vec();
            buf.drain(..6 + len);
            if tx.send(Event::Serial(kind, payload)).is_err() {
                return;
            }
        }
    }
}

impl Modem {
    fn write(&mut self, kind: u8, payload: &[u8]) -> std::io::Result<()> {
        let n = payload.len() as u16;
        let mut m = Vec::with_capacity(6 + payload.len());
        m.extend_from_slice(&[0xAA, 0x55, kind]);
        m.extend_from_slice(&n.to_le_bytes());
        m.extend_from_slice(payload);
        let x = m[2..].iter().fold(0u8, |a, b| a ^ b);
        m.push(x);
        self.w.write_all(&m)
    }

    /// 'C': channel, our MAC, rate (wifi_phy_rate_t, 1 = 2 Mbit/s long preamble), forward every frame.
    pub fn config(&mut self, channel: u8, mac: &Mac, all: bool) -> std::io::Result<()> {
        let mut p = vec![channel];
        p.extend_from_slice(mac);
        p.extend_from_slice(&[1, all as u8]);
        self.write(b'C', &p)
    }

    pub fn transmit(&mut self, frame: &[u8]) -> std::io::Result<()> {
        self.write(b'T', frame)
    }

    /// 'm': multiplay offload settings.
    pub fn mp_config(&mut self, on: bool, clienttime: u16, cmd_word: u16, cmdcount: u16, period_us: u32,
                     reply_wait_us: u32, cycle: u8) -> std::io::Result<()> {
        let mut p = vec![on as u8];
        p.extend_from_slice(&clienttime.to_le_bytes());
        p.extend_from_slice(&cmd_word.to_le_bytes());
        p.extend_from_slice(&cmdcount.to_le_bytes());
        p.extend_from_slice(&period_us.to_le_bytes());
        p.extend_from_slice(&reply_wait_us.to_le_bytes());
        p.push(cycle);
        self.write(b'm', &p)
    }

    /// 'A': the associated DSes; reopen = start again with the opening empty poll (re-association).
    pub fn mp_clients(&mut self, aids: &[(Mac, u8)], reopen: bool) -> std::io::Result<()> {
        let mut items = aids.to_vec();
        items.sort_by_key(|&(_, a)| a);
        let mut p = vec![items.len() as u8];
        for (m, a) in items {
            p.push(a);
            p.extend_from_slice(&m);
        }
        p.push(reopen as u8);
        self.write(b'A', &p)
    }

    /// 'Q': a packet for the polls.
    pub fn mp_push(&mut self, port: u8, payload: &[u8], min_sent: u8, max_sent: u8) -> std::io::Result<()> {
        let mut p = vec![port, min_sent, max_sent.max(1)];
        p.extend_from_slice(payload);
        self.pushed = self.pushed.wrapping_add(1);
        self.write(b'Q', &p)
    }

    /// 'X': drop the modem's packet queue.
    pub fn mp_clear(&mut self) -> std::io::Result<()> {
        self.epoch = self.epoch.wrapping_add(1);
        self.pushed = 0;
        self.write(b'X', &[self.epoch])
    }
}

/// The modem's 'M' report.
#[allow(dead_code)]
#[derive(Clone, Copy, Default)]
pub struct MpStats {
    pub epoch: u8,
    pub qlen: u16,
    pub queued: u16,
    pub polls: u32,
    pub replies: u32,
    pub acks: u32,
    pub done: u32,
    pub txerr: u32,
    pub answered: Option<u16>, // AIDs that replied since the previous report (newer firmware only)
}

impl MpStats {
    pub fn parse(p: &[u8]) -> Option<MpStats> {
        if p.len() < 25 {
            return None;
        }
        let u32at = |o: usize| u32::from_le_bytes([p[o], p[o + 1], p[o + 2], p[o + 3]]);
        Some(MpStats {
            epoch: p[0],
            qlen: u16::from_le_bytes([p[1], p[2]]),
            queued: u16::from_le_bytes([p[3], p[4]]),
            polls: u32at(5),
            replies: u32at(9),
            acks: u32at(13),
            done: u32at(17),
            txerr: u32at(21),
            answered: if p.len() >= 27 { Some(u16::from_le_bytes([p[25], p[26]])) } else { None },
        })
    }
}

//! Localhost JSON-lines link to pictochat_gui.py and other clients (e.g. a chatbot), the same protocol as pictochat.py's
//! GuiBridge: host -> client {"kind": "log"|"rx"|"tx"|"message"|"state", ...}; client -> host {"cmd": ...}.
//! A client that connects late first gets the last 600 log/rx/tx/message events. Live events never block the
//! host: a client that does not read loses lines, and one that takes only part of a line is dropped.

use std::collections::VecDeque;
use std::io::{BufRead, BufReader, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::Sender;
use std::sync::{Arc, Mutex};

use crate::Event;

const HISTORY: usize = 600;

struct Client {
    id: u64,
    stream: TcpStream,
    replaying: Arc<AtomicBool>,
}

#[derive(Default)]
struct Inner {
    clients: Vec<Client>,
    history: VecDeque<String>,
}

#[derive(Clone)]
pub struct Gui {
    inner: Arc<Mutex<Inner>>,
}

impl Gui {
    pub fn start(port: u16, tx: Sender<Event>) -> std::io::Result<Gui> {
        let srv = TcpListener::bind(("127.0.0.1", port))?;
        let gui = Gui { inner: Arc::new(Mutex::new(Inner::default())) };
        let g = gui.clone();
        std::thread::spawn(move || {
            let next_id = AtomicU64::new(1);
            for conn in srv.incoming() {
                let Ok(stream) = conn else { continue };
                let _ = stream.set_nodelay(true);
                let id = next_id.fetch_add(1, Ordering::Relaxed);
                let (Ok(rd), Ok(mut wr)) = (stream.try_clone(), stream.try_clone()) else { continue };
                let replaying = Arc::new(AtomicBool::new(true));
                let backlog: String = {
                    let mut inner = g.inner.lock().unwrap();
                    inner.clients.push(Client { id, stream, replaying: replaying.clone() });
                    inner.history.iter().cloned().collect()
                };
                // replay in its own thread: a client that does not read can only stall this helper
                std::thread::spawn(move || {
                    let _ = wr.write_all(backlog.as_bytes());
                    replaying.store(false, Ordering::Release);
                });
                let (g2, tx2) = (g.clone(), tx.clone());
                std::thread::spawn(move || {
                    for line in BufReader::new(rd).lines() {
                        let Ok(line) = line else { break };
                        if let Ok(v) = serde_json::from_str::<serde_json::Value>(&line) {
                            if tx2.send(Event::Gui(v)).is_err() {
                                break;
                            }
                        }
                    }
                    g2.inner.lock().unwrap().clients.retain(|c| c.id != id);
                });
            }
        });
        Ok(gui)
    }

    pub fn send(&self, obj: serde_json::Value) {
        let keep = matches!(obj.get("kind").and_then(|k| k.as_str()), Some("rx" | "tx" | "message" | "log"));
        let line = obj.to_string() + "\n";
        let mut inner = self.inner.lock().unwrap();
        if keep {
            inner.history.push_back(line.clone());
            while inner.history.len() > HISTORY {
                inner.history.pop_front();
            }
        }
        inner.clients.retain(|c| {
            if c.replaying.load(Ordering::Acquire) {
                return true; // backlog still being replayed: skip this line for that client
            }
            let n = unsafe {
                libc::send(c.stream.as_raw_fd(), line.as_ptr() as *const libc::c_void, line.len(),
                           libc::MSG_DONTWAIT | libc::MSG_NOSIGNAL)
            };
            if n < 0 {
                let e = std::io::Error::last_os_error();
                if e.kind() == std::io::ErrorKind::WouldBlock {
                    return true; // socket buffer full: this line is lost for that client
                }
                let _ = c.stream.shutdown(Shutdown::Both);
                return false;
            }
            if (n as usize) < line.len() {
                let _ = c.stream.shutdown(Shutdown::Both); // partial line: the client is not reading
                return false;
            }
            true
        });
    }
}

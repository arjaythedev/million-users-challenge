//! One Linux idle reactor per worker. Real sockets remain open; only active
//! requests allocate Tokio registrations/tasks. No cached request or result data.
use crate::{App, custom_http};
use bytes::BytesMut;
use mio::{Events, Interest, Poll, Token, unix::SourceFd};
use slab::Slab;
use std::{
    cell::{Cell, RefCell},
    io::{self, Read},
    net::TcpStream,
    os::fd::{AsRawFd, RawFd},
    rc::Rc,
    time::Duration,
};
use tokio::io::unix::AsyncFd;

struct Poller(RefCell<Poll>);
impl AsRawFd for Poller {
    fn as_raw_fd(&self) -> RawFd {
        self.0.borrow().as_raw_fd()
    }
}
struct Slot {
    stream: TcpStream,
    generation: u32,
}
pub struct Idle {
    poll: AsyncFd<Poller>,
    slots: RefCell<Slab<Slot>>,
    generation: Cell<u32>,
    app: Rc<App>,
}
impl Idle {
    pub fn start(app: Rc<App>) -> io::Result<Rc<Self>> {
        let idle = Rc::new(Self {
            // SAFETY: Poller owns its epoll descriptor. Its private Poll is never
            // replaced or closed while registered, and Drop follows AsyncFd's
            // deregistration. poll() does not change the owned descriptor.
            poll: unsafe { AsyncFd::register(Poller(RefCell::new(Poll::new()?)))? },
            slots: RefCell::new(Slab::new()),
            generation: Cell::new(0),
            app,
        });
        let owner = idle.clone();
        tokio::task::spawn_local(async move {
            if let Err(e) = owner.run().await {
                eprintln!("idle reactor failed: {e}");
                std::process::exit(1);
            }
        });
        Ok(idle)
    }
    pub fn insert(&self, stream: TcpStream) -> io::Result<()> {
        // TCP_NODELAY is set once at accept. Moving between the idle registry
        // and Tokio preserves the socket and its options.
        let generation = self.generation.get().wrapping_add(1);
        self.generation.set(generation);
        let mut slots = self.slots.borrow_mut();
        let entry = slots.vacant_entry();
        let key = u32::try_from(entry.key()).map_err(|_| io::Error::other("idle slot limit"))?;
        let token = Token(((u64::from(generation) << 32) | u64::from(key)) as usize);
        let fd = stream.as_raw_fd();
        entry.insert(Slot { stream, generation });
        let key = key as usize;
        if let Err(e) = self.poll.get_ref().0.borrow().registry().register(
            &mut SourceFd(&fd),
            token,
            Interest::READABLE,
        ) {
            slots.remove(key);
            return Err(e);
        }
        Ok(())
    }
    fn ready(self: &Rc<Self>, token: Token) -> io::Result<()> {
        let key = token.0 as u32 as usize;
        let generation = (token.0 as u64 >> 32) as u32;
        let mut slot = {
            let mut slots = self.slots.borrow_mut();
            if !slots.get(key).is_some_and(|s| s.generation == generation) {
                return Ok(());
            }
            slots.remove(key)
        };
        let fd = slot.stream.as_raw_fd();
        self.poll
            .get_ref()
            .0
            .borrow()
            .registry()
            .deregister(&mut SourceFd(&fd))?;
        // Read before Tokio registration: a newly registered Tokio stream may
        // not yet have cached readiness even though the OS has readable bytes.
        let mut input = BytesMut::new();
        input.resize(1024, 0);
        let n = loop {
            match slot.stream.read(&mut input) {
                Ok(0) => return Ok(()),
                Ok(n) => break n,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => return self.insert(slot.stream),
                Err(_) => return Ok(()), // reset/closed peer: drop the owned socket
            }
        };
        input.truncate(n);
        let stream = tokio::net::TcpStream::from_std(slot.stream)?;
        let idle = self.clone();
        tokio::task::spawn_local(async move {
            if let Ok(Some(stream)) =
                custom_http::serve_active(stream, input, idle.app.clone()).await
                && let Err(e) = idle.insert(stream)
            {
                eprintln!("return idle socket: {e}");
            }
        });
        Ok(())
    }
    async fn run(self: Rc<Self>) -> io::Result<()> {
        let mut events = Events::with_capacity(1024);
        loop {
            let mut readiness = self.poll.readable().await?;
            // Drain the nested epoll queue before clearing readiness. Clearing
            // after only one batch can lose an edge when more events remain.
            loop {
                match self
                    .poll
                    .get_ref()
                    .0
                    .borrow_mut()
                    .poll(&mut events, Some(Duration::ZERO))
                {
                    Ok(()) => {}
                    Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                    Err(e) => return Err(e),
                }
                if events.is_empty() {
                    readiness.clear_ready();
                    break;
                }
                for event in &events {
                    self.ready(event.token())?;
                }
                tokio::task::yield_now().await;
            }
        }
    }
}

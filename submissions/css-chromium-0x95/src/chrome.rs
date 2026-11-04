//! Spawns and supervises one chrome-headless-shell per worker; a dead one fails its in-flight requests and is respawned.
use std::path::PathBuf;
use std::process::Stdio;
use std::sync::atomic::{AtomicI32, Ordering::Relaxed};
use std::sync::Arc;
use std::time::Duration;
use tokio::process::Command;

use crate::hub::Hub;

const STUCK: Duration = Duration::from_secs(30);

unsafe extern "C" {
    fn prctl(option: i32, arg2: u64, arg3: u64, arg4: u64, arg5: u64) -> i32;
    fn kill(pid: i32, sig: i32) -> i32;
    fn setpriority(which: i32, who: u32, prio: i32) -> i32;
}

static PIDS: [AtomicI32; 1024] = [const { AtomicI32::new(0) }; 1024];

/// Chromium profiles, HOME and TMPDIR live inside the submission folder (`<submission>/.chrome/<port>`), nothing in /tmp.
pub fn profile_root(hub: &Hub) -> PathBuf {
    let sub = hub.cfg.web_dir.parent().map(|p| p.to_path_buf()).unwrap_or_default();
    sub.join(".chrome").join(hub.cfg.ports[0].to_string())
}

/// SIGKILL every Chromium we started (shutdown path).
pub fn kill_all(hub: &Hub) {
    for p in PIDS.iter().take(hub.cfg.workers.min(1024)) {
        let pid = p.load(Relaxed);
        if pid > 0 {
            unsafe { kill(pid, 9) };
        }
    }
    let _ = std::fs::remove_dir_all(profile_root(hub));
}

pub async fn supervise(hub: Arc<Hub>, idx: usize) {
    let cfg = &hub.cfg;
    let dir = profile_root(&hub).join(format!("w{idx}"));
    let url = format!("file://{}/index.html?w={idx}&port={}", cfg.web_dir.display(), cfg.ws_port);
    loop {
        // a kill request left over from the previous (already dead) instance must not hit the new one
        let _ = tokio::time::timeout(Duration::ZERO, hub.workers[idx].kill.notified()).await;
        let _ = std::fs::remove_dir_all(&dir);
        let tmp = dir.join("tmp");
        let _ = std::fs::create_dir_all(&tmp);
        let mut cmd = Command::new(&cfg.chrome_bin);
        cmd.args([
            "--no-sandbox",
            "--disable-gpu",
            "--disable-dev-shm-usage",
            "--disable-background-timer-throttling",
            "--disable-renderer-backgrounding",
            "--disable-backgrounding-occluded-windows",
            "--no-first-run",
            "--disable-extensions",
            "--disable-component-update",
            "--disable-sync",
            "--disable-breakpad",
            "--mute-audio",
            // glue.js calls gc() after heavy batches: Blink only frees the computed styles of finished requests then
            "--js-flags=--expose-gc",
        ])
        .arg(format!("--user-data-dir={}", dir.display()))
        .arg(&url)
        .env("HOME", &dir)
        // --disable-dev-shm-usage puts Chromium's shared memory files in TMPDIR
        .env("TMPDIR", &tmp)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(if std::env::var_os("CSS_CHROME_LOG").is_some() { Stdio::inherit() } else { Stdio::null() })
        .kill_on_drop(true);
        // die with us even if we are SIGKILLed (PR_SET_PDEATHSIG = 1)
        let nice = if hub.workers[idx].class == crate::hub::Class::Heavy { cfg.heavy_nice } else { 0 };
        unsafe {
            cmd.pre_exec(move || {
                prctl(1, 9, 0, 0, 0);
                if nice != 0 {
                    setpriority(0, 0, nice); // heavy pages yield to the front and the light page (inherited by their children)
                }
                Ok(())
            });
        }
        match cmd.spawn() {
            Ok(mut child) => {
                if let (Some(pid), Some(slot)) = (child.id(), PIDS.get(idx)) {
                    slot.store(pid as i32, Relaxed);
                }
                let mut tick = tokio::time::interval(Duration::from_secs(1));
                loop {
                    tokio::select! {
                        st = child.wait() => {
                            eprintln!("css-front: chromium {idx} exited: {st:?}");
                            break;
                        }
                        _ = hub.workers[idx].kill.notified() => {
                            eprintln!("css-front: restarting chromium {idx}");
                            let _ = child.kill().await;
                            break;
                        }
                        _ = tick.tick() => {
                            if hub.stuck(idx, STUCK) {
                                eprintln!("css-front: chromium {idx} stuck, killing");
                                let _ = child.kill().await;
                                break;
                            }
                        }
                    }
                }
                if let Some(slot) = PIDS.get(idx) {
                    slot.store(0, Relaxed);
                }
            }
            Err(e) => eprintln!("css-front: cannot start {}: {e}", cfg.chrome_bin),
        }
        hub.worker_down(idx, None);
        tokio::time::sleep(Duration::from_millis(500)).await;
    }
}

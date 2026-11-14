//! Postgres binary timestamps to the API's `2025-12-31T23:59:19.409Z`.

const MICROS_PER_DAY: i64 = 86_400_000_000;
/// Days from 1970-01-01 to 2000-01-01, the epoch of Postgres's binary timestamps.
const PG_EPOCH_DAYS: i64 = 10_957;

/// "00" to "99".
const PAIRS: [[u8; 2]; 100] = {
    let mut t = [[0u8; 2]; 100];
    let mut i = 0;
    while i < 100 {
        t[i] = [b'0' + (i / 10) as u8, b'0' + (i % 10) as u8];
        i += 1;
    }
    t
};

fn two(out: &mut [u8], v: u32) {
    out[..2].copy_from_slice(&PAIRS[v as usize % 100]);
}

/// The date of the timestamp last formatted. It lives for one response: a feed's rows are nearly all of one day,
/// and the calendar arithmetic is most of the work of formatting one.
pub struct Day {
    days: i64,
    text: [u8; 10],
}

impl Day {
    pub const fn new() -> Day {
        Day { days: i64::MIN, text: *b"0000-00-00" }
    }

    fn text(&mut self, days: i64) -> [u8; 10] {
        if days != self.days {
            // civil-from-days (Howard Hinnant), valid for the whole proleptic Gregorian calendar
            let z = days + 719_468;
            let era = z.div_euclid(146_097);
            let doe = z.rem_euclid(146_097);
            let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
            let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            let mp = (5 * doy + 2) / 153;
            let day = (doy - (153 * mp + 2) / 5 + 1) as u32;
            let month = (if mp < 10 { mp + 3 } else { mp - 9 }) as u32;
            let year = (yoe + era * 400 + i64::from(month <= 2)).clamp(0, 9999) as u32;
            two(&mut self.text[0..], year / 100);
            two(&mut self.text[2..], year % 100);
            two(&mut self.text[5..], month);
            two(&mut self.text[8..], day);
            self.days = days;
        }
        self.text
    }
}

/// Formats microseconds since 2000-01-01 UTC, cut (not rounded) to milliseconds.
pub fn format_pg_timestamp_in(day: &mut Day, micros: i64) -> [u8; 24] {
    let days = micros.div_euclid(MICROS_PER_DAY) + PG_EPOCH_DAYS;
    let ms = (micros.rem_euclid(MICROS_PER_DAY) / 1_000) as u32;
    let (h, mi, s, milli) = (ms / 3_600_000, ms / 60_000 % 60, ms / 1_000 % 60, ms % 1_000);

    let mut out = *b"0000-00-00T00:00:00.000Z";
    out[..10].copy_from_slice(&day.text(days));
    two(&mut out[11..], h);
    two(&mut out[14..], mi);
    two(&mut out[17..], s);
    out[20] = b'0' + (milli / 100) as u8;
    two(&mut out[21..], milli % 100);
    out
}

pub fn format_pg_timestamp(micros: i64) -> [u8; 24] {
    format_pg_timestamp_in(&mut Day::new(), micros)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn f(micros: i64) -> String {
        String::from_utf8(format_pg_timestamp(micros).to_vec()).unwrap()
    }

    #[test]
    #[ignore = "a timing, not a check: cargo test --release -- --ignored --nocapture timing"]
    fn timing() {
        // one feed's worth: 20 instants of one day, a second or so apart
        let base = 9_814 * MICROS_PER_DAY + 19_565_229_000;
        let rounds = 1_000_000;
        let t = std::time::Instant::now();
        for r in 0..rounds {
            let mut day = Day::new();
            for k in 0..20i64 {
                std::hint::black_box(format_pg_timestamp_in(&mut day, std::hint::black_box(base + r + k * 1_000_003)));
            }
        }
        println!("20 timestamps: {:.0} ns", t.elapsed().as_nanos() as f64 / rounds as f64);
    }

    #[test]
    fn one_day_after_another() {
        // the kept date must follow the input: the same formatter across midnight, a leap day and back
        let mut day = Day::new();
        for micros in [0, MICROS_PER_DAY - 1, MICROS_PER_DAY, 59 * MICROS_PER_DAY, 60 * MICROS_PER_DAY + 5, 1, -1, 9_496 * MICROS_PER_DAY] {
            assert_eq!(format_pg_timestamp_in(&mut day, micros), format_pg_timestamp(micros), "{micros}");
        }
        assert_eq!(&format_pg_timestamp_in(&mut day, 59 * MICROS_PER_DAY)[..10], b"2000-02-29");
    }

    #[test]
    fn known_instants() {
        assert_eq!(f(0), "2000-01-01T00:00:00.000Z");
        assert_eq!(f(-1), "1999-12-31T23:59:59.999Z");
        // 2025-12-31T23:59:19.409Z, the newest post of the seed
        let days_2025_12_31 = 9_496;
        let micros = days_2025_12_31 * MICROS_PER_DAY + (23 * 3600 + 59 * 60 + 19) * 1_000_000 + 409_000;
        assert_eq!(f(micros), "2025-12-31T23:59:19.409Z");
        assert_eq!(f(micros + 999), "2025-12-31T23:59:19.409Z", "cut, not rounded");
        assert_eq!(f(micros + 1_000), "2025-12-31T23:59:19.410Z");
    }

    #[test]
    fn leap_days() {
        let d = |y: i64, m: i64, day: i64| {
            // days-from-civil, the inverse, to cross-check
            let y2 = if m <= 2 { y - 1 } else { y };
            let era = y2.div_euclid(400);
            let yoe = y2.rem_euclid(400);
            let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + day - 1;
            let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            era * 146_097 + doe - 719_468 - PG_EPOCH_DAYS
        };
        assert_eq!(f(d(2024, 2, 29) * MICROS_PER_DAY), "2024-02-29T00:00:00.000Z");
        assert_eq!(f(d(2024, 3, 1) * MICROS_PER_DAY), "2024-03-01T00:00:00.000Z");
        assert_eq!(f(d(2026, 11, 13) * MICROS_PER_DAY + 1), "2026-11-13T00:00:00.000Z");
        assert_eq!(f(d(2100, 2, 28) * MICROS_PER_DAY), "2100-02-28T00:00:00.000Z");
        assert_eq!(f(d(2100, 3, 1) * MICROS_PER_DAY - 1), "2100-02-28T23:59:59.999Z");
    }
}

//! Postgres binary timestamps to the API's `2025-12-31T23:59:19.409Z`.

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

/// The second of the timestamp last formatted, and its date. It lives for one response: a feed under load is
/// twenty posts of the same second or two, and all that is left to work out for each is the milliseconds. The
/// date is kept apart, since the calendar arithmetic is most of the work when the second does change.
pub struct Day {
    second: i64,
    days: i64,
    /// `2025-12-31T23:59:19.`
    text: [u8; 20],
}

impl Day {
    pub const fn new() -> Day {
        Day { second: i64::MIN, days: i64::MIN, text: *b"0000-00-00T00:00:00." }
    }

    /// `second` counts from 2000-01-01.
    fn set(&mut self, second: i64) {
        let days = second.div_euclid(86_400) + PG_EPOCH_DAYS;
        let in_day = second.rem_euclid(86_400) as u32;
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
        two(&mut self.text[11..], in_day / 3_600);
        two(&mut self.text[14..], in_day / 60 % 60);
        two(&mut self.text[17..], in_day % 60);
        self.second = second;
    }
}

/// Formats microseconds since 2000-01-01 UTC, cut (not rounded) to milliseconds.
pub fn format_pg_timestamp_in(day: &mut Day, micros: i64) -> [u8; 24] {
    let second = micros.div_euclid(1_000_000);
    let milli = ((micros - second * 1_000_000) / 1_000) as u32;
    if second != day.second {
        day.set(second);
    }
    let mut out = [b'Z'; 24];
    out[..20].copy_from_slice(&day.text);
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

    const MICROS_PER_DAY: i64 = 86_400_000_000;

    fn f(micros: i64) -> String {
        String::from_utf8(format_pg_timestamp(micros).to_vec()).unwrap()
    }

    #[test]
    #[ignore = "a timing, not a check: cargo test --release -- --ignored --nocapture timing"]
    fn timing() {
        // one feed's worth: 20 instants a few milliseconds apart, as under load, and then a second or so apart
        let base = 9_814 * MICROS_PER_DAY + 19_565_229_000;
        for (what, step) in [("a few milliseconds apart", 2_503), ("a second apart", 1_000_003)] {
            let rounds = 1_000_000;
            let t = std::time::Instant::now();
            for r in 0..rounds {
                let mut day = Day::new();
                for k in 0..20i64 {
                    std::hint::black_box(format_pg_timestamp_in(&mut day, std::hint::black_box(base + r * 977 + k * step)));
                }
            }
            println!("20 timestamps {what}: {:.0} ns", t.elapsed().as_nanos() as f64 / rounds as f64);
        }
    }

    #[test]
    fn one_day_after_another() {
        // what is kept must follow the input: the same formatter within a second, from one second to the next,
        // across midnight, a leap day and back
        let mut day = Day::new();
        let within_a_day = [5_000, 999_999, 1_000_000, 1_000_001, 1_999_000, 61_000_000, 3_600_123_456, 1_000_000, -1_000_001];
        let across_days = [0, MICROS_PER_DAY - 1, MICROS_PER_DAY, 59 * MICROS_PER_DAY, 60 * MICROS_PER_DAY + 5, 1, -1, 9_496 * MICROS_PER_DAY];
        for micros in within_a_day.into_iter().chain(across_days) {
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

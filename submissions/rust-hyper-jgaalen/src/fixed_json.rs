//! Fixed API punctuation/keys only. Every value is freshly read and serialized
//! with serde; no cached rows, responses, or unchecked string escaping.
use crate::PostView;
fn post(out: &mut Vec<u8>, value: PostView<'_>) {
    out.extend_from_slice(b"{\"id\":");
    serde_json::to_writer(&mut *out, &value.id).expect("JSON id");
    out.extend_from_slice(b",\"body\":");
    serde_json::to_writer(&mut *out, value.body).expect("JSON body");
    out.extend_from_slice(b",\"created_at\":");
    serde_json::to_writer(&mut *out, &value.created_at).expect("JSON timestamp");
    out.extend_from_slice(b",\"author\":");
    serde_json::to_writer(&mut *out, value.author).expect("JSON author");
    out.extend_from_slice(b",\"like_count\":");
    serde_json::to_writer(&mut *out, &value.like_count).expect("JSON likes");
    out.push(b'}');
}
pub(super) fn one(value: PostView<'_>) -> Vec<u8> {
    let mut out = Vec::with_capacity(128);
    out.extend_from_slice(b"{\"post\":");
    post(&mut out, value);
    out.push(b'}');
    out
}
pub(super) fn feed<'a>(values: impl IntoIterator<Item = PostView<'a>>) -> Vec<u8> {
    let mut out = Vec::with_capacity(128);
    out.extend_from_slice(b"{\"posts\":[");
    for (index, value) in values.into_iter().enumerate() {
        if index != 0 {
            out.push(b',');
        }
        post(&mut out, value);
    }
    out.extend_from_slice(b"]}");
    out
}
#[cfg(test)]
mod tests {
    use super::*;
    use crate::{OneView, Timestamp};
    use chrono::{DateTime, Utc};
    use serde::Serialize;
    #[derive(Serialize)]
    struct ReferenceFeed<'a> {
        posts: Vec<PostView<'a>>,
    }
    #[test]
    fn byte_equivalence_including_escaping_dates_and_boundaries() {
        let controls: String = (0..32).map(char::from).collect();
        let large = "a\\\"\n🦀é".repeat(500);
        for text in [
            "",
            "plain text",
            "quote\" slash\\ solidus/",
            "\u{2028}\u{2029}",
            &controls,
            &large,
        ] {
            for date in [
                "0000-01-01T00:00:00Z",
                "2024-02-29T23:59:59.999999Z",
                "2016-12-31T23:59:60Z",
                "+10000-01-01T00:00:00Z",
            ] {
                let date = if date.starts_with('+') {
                    chrono::NaiveDate::from_ymd_opt(10000, 1, 1)
                        .unwrap()
                        .and_hms_opt(0, 0, 0)
                        .unwrap()
                        .and_utc()
                } else {
                    DateTime::parse_from_rfc3339(date)
                        .unwrap()
                        .with_timezone(&Utc)
                };
                for id in [i64::MIN, 0, 1, i64::MAX] {
                    let make = || PostView {
                        id,
                        body: text,
                        created_at: Timestamp(date),
                        author: text,
                        like_count: i32::MAX,
                    };
                    assert_eq!(
                        one(make()),
                        serde_json::to_vec(&OneView { post: make() }).unwrap()
                    );
                    for size in [0, 1, 20] {
                        assert_eq!(
                            feed((0..size).map(|_| make())),
                            serde_json::to_vec(&ReferenceFeed {
                                posts: (0..size).map(|_| make()).collect()
                            })
                            .unwrap()
                        );
                    }
                }
            }
        }
    }
    #[test]
    #[ignore = "isolated serialization timing; run explicitly in release mode"]
    fn serialization_cost() {
        #[derive(Serialize)]
        struct BorrowedFeed<'a> {
            posts: &'a [PostView<'a>],
        }
        let path = std::env::var("JSON_BENCH_FIXTURE").expect("JSON_BENCH_FIXTURE");
        let doc: serde_json::Value = serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
        let views: Vec<_> = doc["posts"]
            .as_array()
            .unwrap()
            .iter()
            .map(|p| PostView {
                id: p["id"].as_i64().unwrap(),
                body: p["body"].as_str().unwrap(),
                created_at: Timestamp(
                    DateTime::parse_from_rfc3339(p["created_at"].as_str().unwrap())
                        .unwrap()
                        .with_timezone(&Utc),
                ),
                author: p["author"].as_str().unwrap(),
                like_count: p["like_count"].as_i64().unwrap().try_into().unwrap(),
            })
            .collect();
        let encode = |fixed| {
            if fixed {
                feed(views.iter().map(|p| PostView {
                    id: p.id,
                    body: p.body,
                    created_at: Timestamp(p.created_at.0),
                    author: p.author,
                    like_count: p.like_count,
                }))
            } else {
                serde_json::to_vec(&BorrowedFeed { posts: &views }).unwrap()
            }
        };
        assert_eq!(encode(false), encode(true));
        for fixed in [false, true, false, true] {
            for _ in 0..100 {
                std::hint::black_box(encode(fixed));
            }
            let start = std::time::Instant::now();
            let mut count = 0u64;
            while start.elapsed() < std::time::Duration::from_secs(2) {
                std::hint::black_box(encode(fixed));
                count += 1;
            }
            println!(
                "fixed={} bytes={} iterations={} wall_us_per_feed={:.3}",
                fixed,
                encode(fixed).len(),
                count,
                start.elapsed().as_secs_f64() * 1e6 / count as f64
            );
        }
    }
}

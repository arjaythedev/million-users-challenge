import crypto;

fn hex(d: u8[32]) -> u8[>..] {
    var o: u8[>..] = [];
    for b in d { o.push("0123456789abcdef"[(b >> 4) as i64]); o.push("0123456789abcdef"[(b & 15) as i64]); }
    return o;
}

fn main() {
    var s = crypto::init();
    var d: u8[32] = [0; 32];
    crypto::update(s, "abc");
    crypto::finish(s, d);
    print("sha256(abc)  ", hex(d));
    var k = crypto::hmac_key("million-users-challenge");
    crypto::hmac(k, "eyJhbGciOiJIUzI1NiJ9.eyJleHAiOjk5OTk5OTk5OTl9", d);
    print("hmac         ", hex(d));
    var big: u8[>..] = [];
    for i in 1000 { big.push((i % 251) as! u8); }
    var s2 = crypto::init();
    crypto::update(s2, big[..333]); crypto::update(s2, big[333..]);
    crypto::finish(s2, d);
    print("sha256(1000) ", hex(d));
    var o: u8[>..] = [];
    print(crypto::b64url_decode("aGVsbG8", o), " ", o);
}

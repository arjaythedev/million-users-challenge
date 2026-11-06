package main

import (
	"encoding/base64"
	"testing"
)

// TestScramProofRFC7677 checks the proof computation against the SCRAM-SHA-256
// test vector from RFC 7677 §3 (user "user", password "pencil").
func TestScramProofRFC7677(t *testing.T) {
	clientFirstBare := "n=user,r=fyko+d2lbbFgONRv9qkxdawL"
	serverFirst := "r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096"
	clientFinalNoProof := "c=biws,r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j"
	wantProof := "qQRLRHGPDGjB+7iVAE7NNi5xEoHKHuLCHPNQ8BTmvds="
	wantServerSig := "XKW6VuW1FANROQabnJBz1KaeCnQL/HZByQtX/iU+o30="

	salt, err := base64.StdEncoding.DecodeString("QSXCR+Q6sek8bf92")
	if err != nil {
		t.Fatalf("salt: %v", err)
	}
	proof, salted := scramProof(clientFirstBare, serverFirst, clientFinalNoProof, "pencil", salt, 4096)
	if got := base64.StdEncoding.EncodeToString(proof); got != wantProof {
		t.Fatalf("proof = %s, want %s", got, wantProof)
	}
	serverKey := hmacSha256(salted, []byte("Server Key"))
	serverSig := hmacSha256(serverKey, []byte(clientFirstBare+","+serverFirst+","+clientFinalNoProof))
	if got := base64.StdEncoding.EncodeToString(serverSig); got != wantServerSig {
		t.Fatalf("server signature = %s, want %s", got, wantServerSig)
	}
}

func TestParseServerFirst(t *testing.T) {
	clientNonce := "fyko+d2lbbFgONRv9qkxdawL"
	nonce, salt, iters, err := parseServerFirst(
		"r="+clientNonce+"3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096", clientNonce)
	if err != nil {
		t.Fatalf("parseServerFirst: %v", err)
	}
	if nonce != clientNonce+"3rfcNHYJY1ZVvWVs7j" || iters != 4096 || len(salt) != 12 {
		t.Fatalf("nonce=%q iters=%d saltlen=%d", nonce, iters, len(salt))
	}

	// nonce must extend the client nonce
	if _, _, _, err := parseServerFirst("r=other,s=QSXCR+Q6sek8bf92,i=4096", clientNonce); err == nil {
		t.Fatal("accepted a foreign nonce")
	}
}

func TestParseDSN(t *testing.T) {
	d, err := parseDSN("postgres://feed:pw%20x@db.example.com:5432/feed")
	if err != nil {
		t.Fatalf("parseDSN: %v", err)
	}
	if d.user != "feed" || d.password != "pw x" || d.host != "db.example.com" || d.port != "5432" || d.database != "feed" {
		t.Fatalf("got %+v", d)
	}
	d2, err := parseDSN("postgresql://feed:secret@10.1.2.3/feed")
	if err != nil || d2.port != "5432" || d2.host != "10.1.2.3" || d2.password != "secret" {
		t.Fatalf("got %+v err=%v", d2, err)
	}
	d3, err := parseDSN("postgres://feed:secret@10.1.2.3:5433/feed?sslmode=disable")
	if err != nil || d3.database != "feed" || d3.port != "5433" {
		t.Fatalf("got %+v err=%v", d3, err)
	}
	if _, err := parseDSN("mysql://x/y"); err == nil {
		t.Fatal("accepted non-postgres URL")
	}
}

func TestCommandTagRows(t *testing.T) {
	cases := []struct {
		tag  string
		want int64
	}{
		{"INSERT 0 1", 1},
		{"INSERT 0 0", 0},
		{"INSERT 0 12345", 12345},
	}
	for _, tc := range cases {
		if got := commandTagRows([]byte(tc.tag)); got != tc.want {
			t.Fatalf("commandTagRows(%q) = %d, want %d", tc.tag, got, tc.want)
		}
	}
}

package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

func fastProbes(t *testing.T) {
	g, e := probeGrace, probeEvery
	probeGrace, probeEvery = 5*time.Millisecond, 5*time.Millisecond
	t.Cleanup(func() { probeGrace, probeEvery = g, e })
}

// runWatch starts watchTunnel against base and reports whether it killed the tunnel
// within the time it takes for many probes.
func runWatch(t *testing.T, base string) bool {
	t.Helper()
	stop := make(chan struct{})
	killed := make(chan struct{})
	returned := make(chan struct{})
	go func() { watchTunnel(base, func() { close(killed) }, stop); close(returned) }()
	defer func() { close(stop); <-returned }()
	select {
	case <-killed:
		return true
	case <-time.After(300 * time.Millisecond):
		return false
	}
}

func TestProbeHealthyTunnelIsLeftAlone(t *testing.T) {
	fastProbes(t)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) { io.WriteString(w, probeID) }))
	defer srv.Close()
	if res, why := probeTunnel(srv.URL); res != probeOK {
		t.Fatalf("probe = %v (%s), want OK", res, why)
	}
	if runWatch(t, srv.URL) {
		t.Fatal("healthy tunnel was restarted")
	}
}

func TestProbeError1033RestartsAfterMisses(t *testing.T) {
	fastProbes(t)
	var hits atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		hits.Add(1)
		w.WriteHeader(530) // what Cloudflare sends with "Error 1033"
		io.WriteString(w, "error code: 1033")
	}))
	defer srv.Close()
	if !runWatch(t, srv.URL) {
		t.Fatal("dead tunnel was not restarted")
	}
	if n := hits.Load(); n != int32(probeMisses) {
		t.Fatalf("restarted after %d probes, want %d", n, probeMisses)
	}
}

func TestProbeOneBlipDoesNotRestart(t *testing.T) {
	fastProbes(t)
	var n atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		if n.Add(1)%int32(probeMisses) == 0 { // never probeMisses dead answers in a row
			io.WriteString(w, probeID)
			return
		}
		w.WriteHeader(530)
	}))
	defer srv.Close()
	if runWatch(t, srv.URL) {
		t.Fatal("restarted although the tunnel kept coming back")
	}
}

func TestProbeSomeoneElsesRelayCountsAsDead(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) { io.WriteString(w, "not-our-id") }))
	defer srv.Close()
	if res, _ := probeTunnel(srv.URL); res != probeDead {
		t.Fatalf("probe = %v, want dead", res)
	}
}

func TestProbeUnreachableNeverRestarts(t *testing.T) {
	fastProbes(t)
	srv := httptest.NewServer(http.NotFoundHandler())
	base := srv.URL
	srv.Close() // connection refused: like a DNS filter or no internet
	if res, _ := probeTunnel(base); res != probeUnknown {
		t.Fatalf("probe = %v, want unknown", res)
	}
	if runWatch(t, base) {
		t.Fatal("restarted on a network error")
	}
}

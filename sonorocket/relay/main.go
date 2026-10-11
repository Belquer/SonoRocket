// SonoRocket relay: TCP in from the DAW plugin, WebSocket out to any browser.
// Audio payloads are forwarded byte-for-byte. See ../PROTOCOL.md.
//
// One self-contained binary: serves the listener page (embedded), optionally
// starts a Cloudflare quick tunnel (cloudflared next to this binary or on PATH)
// and tells the plugin the public link, so nobody has to type a URL.
package main

import (
	"bufio"
	"crypto/rand"
	"embed"
	"encoding/binary"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
)

//go:embed public
var publicFS embed.FS

var (
	senderAddr  = flag.String("sender", envOr("SENDER_ADDR", "127.0.0.1:9100"), "TCP address for the DAW plugin (use :9100 to accept remote senders)")
	httpAddr    = flag.String("http", envOr("HTTP_ADDR", ":8080"), "HTTP address for listeners")
	relayKey    = flag.String("key", os.Getenv("RELAY_KEY"), "shared key senders must present (empty = none)")
	defaultRoom = flag.String("room", os.Getenv("ROOM"), "room used when the plugin asks for \"auto\" (default: random, stable for this run)")
	tunnelMode  = flag.String("tunnel", envOr("TUNNEL", "auto"), "auto | off | path/to/cloudflared")
	publicBase  = flag.String("public-url", os.Getenv("PUBLIC_URL"), "public base URL when running behind your own HTTPS (disables tunnel)")
	backlogSec  = flag.Float64("backlog", 4, "seconds of audio sent to late joiners")
	statusFile  = flag.String("status-file", "", "write status JSON here whenever it changes (for the launcher)")
)

func envOr(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

var roomRe = regexp.MustCompile(`^[A-Za-z0-9_-]{3,64}$`)

// ---------------------------------------------------------------- rooms

type format struct {
	SR  int    `json:"sr"`
	CH  int    `json:"ch"`
	Fmt string `json:"fmt"`
}

type listener struct {
	conn *websocket.Conn
	out  chan wsMsg
	once sync.Once
}

type wsMsg struct {
	binary bool
	data   []byte
}

func (l *listener) close() { l.once.Do(func() { close(l.out) }) }

type room struct {
	mu            sync.Mutex
	name          string
	format        *format
	sender        net.Conn
	senderLink    chan string // pushes link updates to the connected sender
	listeners     map[*listener]bool
	backlog       [][]byte
	backlogFrames int
}

var (
	roomsMu sync.Mutex
	rooms   = map[string]*room{}
)

func getRoom(name string) *room {
	roomsMu.Lock()
	defer roomsMu.Unlock()
	r := rooms[name]
	if r == nil {
		r = &room{name: name, listeners: map[*listener]bool{}}
		rooms[name] = r
	}
	return r
}

const maxQueuedMessages = 2048 // ~45 s of 1024-frame blocks at 48 kHz

func (r *room) sendAll(m wsMsg) {
	for l := range r.listeners {
		select {
		case l.out <- m:
		default: // listener can't keep up; drop it rather than stall everyone
			l.close()
			delete(r.listeners, l)
		}
	}
}

func (r *room) sendJSON(v any) {
	b, _ := json.Marshal(v)
	r.sendAll(wsMsg{false, b})
}

// ---------------------------------------------------------------- public link / tunnel

var (
	linkMu    sync.Mutex
	publicURL string
	tunnelSt  = "off"
)

func currentBase() string {
	linkMu.Lock()
	defer linkMu.Unlock()
	if publicURL != "" {
		return publicURL
	}
	host, port, _ := net.SplitHostPort(*httpAddr)
	if host == "" || host == "0.0.0.0" || host == "::" {
		host = lanIP()
	}
	return "http://" + host + ":" + port
}

func linkFor(room string) string { return currentBase() + "/?room=" + room }

func setPublicURL(u, st string) {
	linkMu.Lock()
	publicURL, tunnelSt = u, st
	linkMu.Unlock()
	// tell every connected sender
	roomsMu.Lock()
	for _, r := range rooms {
		r.mu.Lock()
		if r.senderLink != nil {
			select {
			case r.senderLink <- linkFor(r.name):
			default:
			}
		}
		r.mu.Unlock()
	}
	roomsMu.Unlock()
	writeStatus()
}

func lanIP() string {
	c, err := net.Dial("udp", "8.8.8.8:80")
	if err != nil {
		return "localhost"
	}
	defer c.Close()
	return c.LocalAddr().(*net.UDPAddr).IP.String()
}

func findCloudflared() string {
	if *tunnelMode != "auto" {
		return *tunnelMode
	}
	if exe, err := os.Executable(); err == nil {
		p := filepath.Join(filepath.Dir(exe), "cloudflared")
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	if p, err := exec.LookPath("cloudflared"); err == nil {
		return p
	}
	return ""
}

var tryURL = regexp.MustCompile(`https://[a-z0-9-]+\.trycloudflare\.com`)

func runTunnel(bin string, stop <-chan struct{}) {
	_, port, _ := net.SplitHostPort(*httpAddr)
	backoff := time.Second
	for {
		setPublicURL("", "starting")
		cmd := exec.Command(bin, "tunnel", "--no-autoupdate", "--url", "http://127.0.0.1:"+port)
		stderr, _ := cmd.StderrPipe()
		cmd.Stdout = io.Discard
		if err := cmd.Start(); err != nil {
			log.Printf("tunnel: %v", err)
			setPublicURL("", "error")
			return
		}
		go func() {
			// cloudflared prints the URL before the edge connection exists;
			// only publish it once a connection is registered.
			sc := bufio.NewScanner(stderr)
			pending := ""
			for sc.Scan() {
				line := sc.Text()
				if u := tryURL.FindString(line); u != "" {
					pending = u
				}
				if pending != "" && strings.Contains(line, "Registered tunnel connection") {
					log.Printf("tunnel up: %s", pending)
					setPublicURL(pending, "up")
					backoff = time.Second
					pending = ""
				}
			}
		}()
		done := make(chan error, 1)
		go func() { done <- cmd.Wait() }()
		select {
		case <-stop:
			_ = cmd.Process.Signal(syscall.SIGTERM)
			select {
			case <-done:
			case <-time.After(3 * time.Second):
				_ = cmd.Process.Kill()
			}
			return
		case err := <-done:
			log.Printf("tunnel exited (%v), restarting in %v", err, backoff)
			setPublicURL("", "reconnecting")
			select {
			case <-stop:
				return
			case <-time.After(backoff):
			}
			if backoff < 30*time.Second {
				backoff *= 2
			}
		}
	}
}

// ---------------------------------------------------------------- status

type status struct {
	Room         string `json:"room"`
	Link         string `json:"link"`
	Tunnel       string `json:"tunnel"`
	SenderOnline bool   `json:"senderOnline"`
	Listeners    int    `json:"listeners"`
	Format       any    `json:"format"`
}

func getStatus() status {
	r := getRoom(*defaultRoom)
	r.mu.Lock()
	st := status{Room: r.name, Link: linkFor(r.name), SenderOnline: r.sender != nil, Listeners: len(r.listeners)}
	if r.format != nil {
		st.Format = *r.format
	}
	r.mu.Unlock()
	linkMu.Lock()
	st.Tunnel = tunnelSt
	linkMu.Unlock()
	return st
}

var statusMu sync.Mutex

func writeStatus() {
	if *statusFile == "" {
		return
	}
	statusMu.Lock()
	defer statusMu.Unlock()
	b, _ := json.Marshal(getStatus())
	tmp := *statusFile + ".tmp"
	if os.WriteFile(tmp, b, 0o644) == nil {
		_ = os.Rename(tmp, *statusFile)
	}
}

// ---------------------------------------------------------------- sender side

type handshake struct {
	V    int    `json:"v"`
	Room string `json:"room"`
	Key  string `json:"key"`
	SR   int    `json:"sr"`
	CH   int    `json:"ch"`
	Fmt  string `json:"fmt"`
}

func validFormat(f format) bool { return f.Fmt == "f32" && f.SR > 0 && f.SR <= 768000 && f.CH >= 1 && f.CH <= 2 }

func handleSender(c net.Conn) {
	defer c.Close()
	peer := c.RemoteAddr().String()
	if tc, ok := c.(*net.TCPConn); ok {
		_ = tc.SetNoDelay(true)
	}
	br := bufio.NewReaderSize(c, 1<<20)
	fail := func(msg string) { log.Printf("sender %s: %s", peer, msg); fmt.Fprintf(c, "ERR %s\n", msg) }

	_ = c.SetReadDeadline(time.Now().Add(10 * time.Second))
	line, err := br.ReadSlice('\n')
	if err != nil {
		fail("no handshake")
		return
	}
	var hs handshake
	if json.Unmarshal(line, &hs) != nil {
		fail("bad handshake")
		return
	}
	if *relayKey != "" && hs.Key != *relayKey {
		fail("bad key")
		return
	}
	name := hs.Room
	if name == "" || name == "auto" {
		name = *defaultRoom
	}
	if !roomRe.MatchString(name) {
		fail("bad room")
		return
	}
	f := format{hs.SR, hs.CH, hs.Fmt}
	if !validFormat(f) {
		fail("bad format")
		return
	}

	r := getRoom(name)
	linkCh := make(chan string, 4)
	r.mu.Lock()
	if r.sender != nil {
		r.sender.Close() // newest sender wins (reconnects)
	}
	r.sender, r.senderLink = c, linkCh
	r.format, r.backlog, r.backlogFrames = &f, nil, 0
	r.sendJSON(map[string]any{"type": "format", "sr": f.SR, "ch": f.CH, "fmt": f.Fmt})
	r.sendJSON(map[string]any{"type": "sender", "online": true})
	r.mu.Unlock()
	writeStatus()

	ok, _ := json.Marshal(map[string]string{"room": name, "link": linkFor(name)})
	if _, err := fmt.Fprintf(c, "OK %s\n", ok); err != nil {
		return
	}
	log.Printf("[%s] sender connected from %s (%d Hz, %d ch)", name, peer, f.SR, f.CH)

	// later link changes (tunnel came up / restarted) go to the plugin as "LINK <url>\n"
	quit := make(chan struct{})
	go func() {
		for {
			select {
			case l := <-linkCh:
				fmt.Fprintf(c, "LINK %s\n", l)
			case <-quit:
				return
			}
		}
	}()

	defer func() {
		close(quit)
		r.mu.Lock()
		if r.sender == c {
			r.sender, r.senderLink = nil, nil
			r.sendJSON(map[string]any{"type": "sender", "online": false})
		}
		r.mu.Unlock()
		writeStatus()
		log.Printf("[%s] sender disconnected", name)
	}()

	var lenBuf [4]byte
	for {
		_ = c.SetReadDeadline(time.Now().Add(30 * time.Second)) // plugin sends keepalives every 2 s
		if _, err := io.ReadFull(br, lenBuf[:]); err != nil {
			return
		}
		n := binary.LittleEndian.Uint32(lenBuf[:])
		if n == 0 || n > 4<<20 {
			fail("bad frame length")
			return
		}
		p := make([]byte, n)
		if _, err := io.ReadFull(br, p); err != nil {
			return
		}
		switch p[0] {
		case 1: // audio
			if len(p) < 13 {
				continue
			}
			frames := int(binary.LittleEndian.Uint32(p[5:9]))
			r.mu.Lock()
			if r.format == nil || len(p) != 13+frames*r.format.CH*4 {
				r.mu.Unlock()
				log.Printf("[%s] size mismatch, dropping block", name)
				continue
			}
			r.backlog = append(r.backlog, p)
			r.backlogFrames += frames
			for len(r.backlog) > 1 {
				first := int(binary.LittleEndian.Uint32(r.backlog[0][5:9]))
				if float64(r.backlogFrames-first) < *backlogSec*float64(r.format.SR) {
					break
				}
				r.backlog, r.backlogFrames = r.backlog[1:], r.backlogFrames-first
			}
			r.sendAll(wsMsg{true, p})
			r.mu.Unlock()
		case 2: // format change
			var nf format
			if json.Unmarshal(p[1:], &nf) != nil || !validFormat(nf) {
				fail("bad format")
				return
			}
			r.mu.Lock()
			r.format, r.backlog, r.backlogFrames = &nf, nil, 0
			r.sendJSON(map[string]any{"type": "format", "sr": nf.SR, "ch": nf.CH, "fmt": nf.Fmt})
			r.mu.Unlock()
			log.Printf("[%s] format %d Hz, %d ch", name, nf.SR, nf.CH)
			writeStatus()
		case 3: // keepalive
		}
	}
}

// ---------------------------------------------------------------- listener side

var upgrader = websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }, WriteBufferSize: 64 << 10}

func handleWS(w http.ResponseWriter, req *http.Request) {
	name := req.URL.Query().Get("room")
	if name == "auto" {
		name = *defaultRoom
	}
	conn, err := upgrader.Upgrade(w, req, nil)
	if err != nil {
		return
	}
	if !roomRe.MatchString(name) {
		conn.WriteControl(websocket.CloseMessage, websocket.FormatCloseMessage(4001, "bad room"), time.Now().Add(time.Second))
		conn.Close()
		return
	}
	r := getRoom(name)
	l := &listener{conn: conn, out: make(chan wsMsg, maxQueuedMessages)}

	r.mu.Lock()
	r.listeners[l] = true
	hello, _ := json.Marshal(map[string]any{"type": "sender", "online": r.sender != nil})
	l.out <- wsMsg{false, hello}
	if r.format != nil {
		fm, _ := json.Marshal(map[string]any{"type": "format", "sr": r.format.SR, "ch": r.format.CH, "fmt": r.format.Fmt})
		l.out <- wsMsg{false, fm}
		for _, p := range r.backlog {
			select {
			case l.out <- wsMsg{true, p}:
			default:
			}
		}
	}
	count := len(r.listeners)
	r.mu.Unlock()
	log.Printf("[%s] listener joined (%d)", name, count)
	writeStatus()

	// reader: just to notice the close
	go func() {
		for {
			if _, _, err := conn.ReadMessage(); err != nil {
				r.mu.Lock()
				if r.listeners[l] {
					delete(r.listeners, l)
					l.close()
				}
				r.mu.Unlock()
				return
			}
		}
	}()
	for m := range l.out {
		t := websocket.TextMessage
		if m.binary {
			t = websocket.BinaryMessage
		}
		_ = conn.SetWriteDeadline(time.Now().Add(20 * time.Second))
		if err := conn.WriteMessage(t, m.data); err != nil {
			break
		}
	}
	conn.Close()
	r.mu.Lock()
	delete(r.listeners, l)
	count = len(r.listeners)
	r.mu.Unlock()
	log.Printf("[%s] listener left (%d)", name, count)
	writeStatus()
}

// ---------------------------------------------------------------- main

func randomRoom() string {
	words := []string{"mix", "master", "take", "bounce", "groove", "verso", "ponte", "coro", "samba", "choro"}
	n, _ := rand.Int(rand.Reader, big.NewInt(int64(len(words))))
	b := make([]byte, 4)
	_, _ = rand.Read(b)
	return fmt.Sprintf("%s-%x", words[n.Int64()], b)
}

func main() {
	flag.Parse()
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	if *defaultRoom == "" {
		*defaultRoom = randomRoom()
	}
	if !roomRe.MatchString(*defaultRoom) {
		log.Fatalf("bad -room %q", *defaultRoom)
	}

	sub, _ := fs.Sub(publicFS, "public")
	mux := http.NewServeMux()
	mux.Handle("/", noCache(http.FileServer(http.FS(sub))))
	mux.HandleFunc("/ws", handleWS)
	mux.HandleFunc("/health", func(w http.ResponseWriter, _ *http.Request) { io.WriteString(w, "ok") })
	mux.HandleFunc("/status", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(getStatus())
	})

	ln, err := net.Listen("tcp", *senderAddr)
	if err != nil {
		log.Fatalf("sender port: %v", err)
	}
	httpLn, err := net.Listen("tcp", *httpAddr)
	if err != nil {
		log.Fatalf("http port: %v", err)
	}
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				if errors.Is(err, net.ErrClosed) {
					return
				}
				continue
			}
			go handleSender(c)
		}
	}()
	go http.Serve(httpLn, mux)

	stop := make(chan struct{})
	tunnelDone := make(chan struct{})
	if *publicBase != "" {
		setPublicURL(strings.TrimRight(*publicBase, "/"), "fixed")
		close(tunnelDone)
	} else if *tunnelMode == "off" {
		close(tunnelDone)
	} else if bin := findCloudflared(); bin != "" {
		go func() { runTunnel(bin, stop); close(tunnelDone) }()
	} else {
		log.Printf("no cloudflared found: listeners must be on this network (%s)", currentBase())
		close(tunnelDone)
	}

	log.Printf("senders tcp %s%s · listeners %s · room %s", *senderAddr, map[bool]string{true: " (key required)", false: ""}[*relayKey != ""], currentBase(), *defaultRoom)
	writeStatus()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	<-sig
	close(stop)
	<-tunnelDone
	if *statusFile != "" {
		os.Remove(*statusFile)
	}
}

func noCache(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-cache")
		h.ServeHTTP(w, r)
	})
}

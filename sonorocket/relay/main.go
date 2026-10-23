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
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
)

//go:embed public
var publicFS embed.FS

// Studio-only pages (registration): served to this computer, never through the tunnel.
//
//go:embed studio
var studioFS embed.FS

var (
	senderAddr  = flag.String("sender", envOr("SENDER_ADDR", "127.0.0.1:9100"), "TCP address for the DAW plugin (use :9100 to accept remote senders)")
	httpAddr    = flag.String("http", envOr("HTTP_ADDR", ":8080"), "HTTP address for listeners")
	relayKey    = flag.String("key", os.Getenv("RELAY_KEY"), "shared key senders must present (empty = none)")
	defaultRoom = flag.String("room", os.Getenv("ROOM"), "room used when the plugin asks for \"auto\" (default: random, stable for this run)")
	tunnelMode  = flag.String("tunnel", envOr("TUNNEL", "auto"), "auto | off | path/to/cloudflared")
	publicBase  = flag.String("public-url", os.Getenv("PUBLIC_URL"), "public base URL when running behind your own HTTPS (disables tunnel)")
	backlogSec  = flag.Float64("backlog", 4, "seconds of audio sent to late joiners")
	statusFile  = flag.String("status-file", "", "write status JSON here whenever it changes (for the launcher)")
	parentPID   = flag.Int("parent-pid", 0, "exit (and stop the tunnel) when this process exits")
	logFile     = flag.String("log", "", "append logs to this file instead of stderr")
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
	conn  *websocket.Conn
	out   chan wsMsg  // master blocks + JSON, in order
	voice chan []byte // talkback from the sender: tiny, written ahead of the master queue
	video chan wsMsg  // camera frames + video control from other pages, ahead of the master
	id    uint16
	once  sync.Once

	// video queue overflowed: from each sender, skip delta frames until its next keyframe (room lock)
	waitKey map[uint16]bool
}

// Video (PROTOCOL.md, "Video"): pages encode their camera (WebCodecs) and the relay
// passes frames between the pages of a room, never to the sender, never decoded.
const videoQueue = 48

// [6][from u16][flags u8: 1 = keyframe][ts u32][encoded chunk]
func validVideo(p []byte) bool { return len(p) > 8 && len(p) < 4<<20 } // a 1080p screen keyframe can be big

// relayVideo forwards a frame or control message from one page to the others.
// A page that can't keep up loses its queue and waits for a keyframe, and the
// senders are asked for one: video degrades, the master and the voice never do.
func (r *room) relayVideo(from *listener, m wsMsg, key bool) {
	askKey := false
	for l := range r.listeners {
		if l == from {
			continue
		}
		var src uint16
		if m.binary {
			src = binary.LittleEndian.Uint16(m.data[1:3])
			if l.waitKey[src] && !key {
				continue
			}
		}
		select {
		case l.video <- m:
			if m.binary && key {
				delete(l.waitKey, src)
			}
		default:
			for drained := false; !drained; {
				select {
				case <-l.video:
				default:
					drained = true
				}
			}
			// everything queued is gone: every current sender must start again from a keyframe
			l.waitKey = map[uint16]bool{}
			for o := range r.listeners {
				if o != l {
					l.waitKey[o.id] = true
				}
			}
			askKey = true
		}
	}
	if askKey {
		req, _ := json.Marshal(map[string]any{"type": "vkey"})
		for l := range r.listeners {
			select {
			case l.video <- wsMsg{false, req}:
			default:
			}
		}
	}
}

// Talkback voice (PROTOCOL.md, "Talkback"): 16 kHz mono int16, ~10 ms frames.
// Voice never enters the backlog and never waits behind master blocks.
const (
	voiceRate      = 16000
	maxVoiceFrames = 960 // 60 ms
	voiceQueue     = 16  // ~160 ms; older frames are dropped, never delayed
)

func validVoice(p []byte) bool {
	if len(p) < 7 {
		return false
	}
	n := int(binary.LittleEndian.Uint16(p[5:7]))
	return n > 0 && n <= maxVoiceFrames && len(p) == 7+2*n
}

// pushLatest queues v, dropping the oldest entry instead of blocking when full:
// for talkback, late is worse than missing.
func pushLatest(ch chan []byte, v []byte) {
	for {
		select {
		case ch <- v:
			return
		default:
			select {
			case <-ch:
			default:
			}
		}
	}
}

type wsMsg struct {
	binary bool
	data   []byte
}

func (l *listener) close() { l.once.Do(func() { close(l.out) }) }

type room struct {
	mu          sync.Mutex
	name        string
	format      *format
	sender      net.Conn
	senderLink  chan string // pushes link updates to the connected sender
	senderVoice chan []byte // listener talkback → sender (nil if the sender has no talkback)

	// A "talk" sender: talkback only, while the master comes from another sender (a DAW
	// plugin on the same computer). It gets the listeners' voice, and a copy of the
	// master for its echo canceller.
	chat          [][]byte // last chat messages (JSON), for pages that join later
	talker        net.Conn
	talkerVoice   chan []byte
	talkerMaster  chan []byte
	nextID        uint16
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

// talkAvail: someone in the studio can hear the listeners and answer.
func (r *room) talkAvail() bool { return r.senderVoice != nil || r.talkerVoice != nil }

func (r *room) senderJSON() {
	r.sendJSON(map[string]any{"type": "sender", "online": r.sender != nil, "talk": r.talkAvail()})
}

// copyToTalker hands the talker a master payload (format or audio), dropping the oldest
// when it lags: it's only an echo reference.
func (r *room) copyToTalker(p []byte) {
	if r.talkerMaster != nil {
		pushLatest(r.talkerMaster, p)
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
		name := "cloudflared"
		if runtime.GOOS == "windows" {
			name += ".exe"
		}
		p := filepath.Join(filepath.Dir(exe), name)
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
		hideWindow(cmd)
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
			stopChild(cmd)
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
	Licensing    bool   `json:"licensing"` // registration required in this build
	Licensed     bool   `json:"licensed"`
	BuyURL       string `json:"buyUrl"`
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
	st.Licensing, st.Licensed, st.BuyURL = licensingOn(), licensed(), buyURL
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
	Talk int    `json:"talk"` // 1: sender plays and sends talkback voice
	Role string `json:"role"` // "talk": talkback only, the master comes from another sender
}

func validFormat(f format) bool {
	return f.Fmt == "f32" && f.SR > 0 && f.SR <= 768000 && f.CH >= 1 && f.CH <= 2
}

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
	if hs.Role == "talk" {
		handleTalker(c, br, name, peer)
		return
	}
	f := format{hs.SR, hs.CH, hs.Fmt}
	if !validFormat(f) {
		fail("bad format")
		return
	}

	r := getRoom(name)
	linkCh := make(chan string, 4)
	var voiceCh chan []byte
	if hs.Talk == 1 {
		voiceCh = make(chan []byte, voiceQueue)
	}
	r.mu.Lock()
	if r.sender != nil {
		r.sender.Close() // newest sender wins (reconnects)
	}
	r.sender, r.senderLink, r.senderVoice = c, linkCh, voiceCh
	r.format, r.backlog, r.backlogFrames = &f, nil, 0
	r.sendJSON(map[string]any{"type": "format", "sr": f.SR, "ch": f.CH, "fmt": f.Fmt})
	r.senderJSON()
	r.copyToTalker(formatPayload(f))
	r.mu.Unlock()
	writeStatus()

	ok, _ := json.Marshal(map[string]string{"room": name, "link": linkFor(name)})
	if _, err := fmt.Fprintf(c, "OK %s\n", ok); err != nil {
		return
	}
	log.Printf("[%s] sender connected from %s (%d Hz, %d ch)", name, peer, f.SR, f.CH)

	// later link changes (tunnel came up / restarted) go to the plugin as "LINK <url>\n"
	// One writer for everything going back to the sender. Talkback goes first.
	// Binary frames are marked with a 0x00 byte (text lines never contain one):
	// 0x00, u32 len, payload.
	quit := make(chan struct{})
	go func() {
		var hdr [5]byte
		writeVoice := func(v []byte) {
			binary.LittleEndian.PutUint32(hdr[1:], uint32(len(v)))
			_ = c.SetWriteDeadline(time.Now().Add(5 * time.Second))
			c.Write(hdr[:])
			c.Write(v)
		}
		for {
			select {
			case v := <-voiceCh: // nil channel when there is no talkback: never ready
				writeVoice(v)
				continue
			default:
			}
			select {
			case v := <-voiceCh:
				writeVoice(v)
			case l := <-linkCh:
				_ = c.SetWriteDeadline(time.Now().Add(5 * time.Second))
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
			r.sender, r.senderLink, r.senderVoice = nil, nil, nil
			r.senderJSON()
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
			r.copyToTalker(p)
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
			r.copyToTalker(formatPayload(nf))
			r.mu.Unlock()
			log.Printf("[%s] format %d Hz, %d ch", name, nf.SR, nf.CH)
			writeStatus()
		case 3: // keepalive
		case 4: // talkback voice from the sender → every listener, live only
			if !validVoice(p) {
				continue
			}
			r.mu.Lock()
			for l := range r.listeners {
				pushLatest(l.voice, p)
			}
			r.mu.Unlock()
		}
	}
}

func formatPayload(f format) []byte {
	js, _ := json.Marshal(f)
	return append([]byte{2}, js...)
}

// handleTalker serves a talk-only sender (PROTOCOL.md, "Talk-only sender").
func handleTalker(c net.Conn, br *bufio.Reader, name, peer string) {
	r := getRoom(name)
	voice, master := make(chan []byte, voiceQueue), make(chan []byte, 64)
	r.mu.Lock()
	if r.talker != nil {
		r.talker.Close() // newest wins
	}
	r.talker, r.talkerVoice, r.talkerMaster = c, voice, master
	if r.format != nil {
		r.copyToTalker(formatPayload(*r.format))
	}
	r.senderJSON()
	r.mu.Unlock()

	ok, _ := json.Marshal(map[string]string{"room": name, "link": linkFor(name)})
	if _, err := fmt.Fprintf(c, "OK %s\n", ok); err != nil {
		return
	}
	log.Printf("[%s] talk sender connected from %s", name, peer)

	quit := make(chan struct{})
	go func() { // voice first, then the master copy; 0x00, u32 len, payload
		var hdr [5]byte
		send := func(p []byte) bool {
			binary.LittleEndian.PutUint32(hdr[1:], uint32(len(p)))
			_ = c.SetWriteDeadline(time.Now().Add(5 * time.Second))
			if _, err := c.Write(hdr[:]); err != nil {
				return false
			}
			_, err := c.Write(p)
			return err == nil
		}
		for {
			select {
			case v := <-voice:
				if !send(v) {
					c.Close()
					return
				}
				continue
			default:
			}
			select {
			case v := <-voice:
				if !send(v) {
					c.Close()
					return
				}
			case p := <-master:
				if !send(p) {
					c.Close()
					return
				}
			case <-quit:
				return
			}
		}
	}()
	defer func() {
		close(quit)
		r.mu.Lock()
		if r.talker == c {
			r.talker, r.talkerVoice, r.talkerMaster = nil, nil, nil
			r.senderJSON()
		}
		r.mu.Unlock()
		log.Printf("[%s] talk sender disconnected", name)
	}()

	var lenBuf [4]byte
	for {
		_ = c.SetReadDeadline(time.Now().Add(30 * time.Second))
		if _, err := io.ReadFull(br, lenBuf[:]); err != nil {
			return
		}
		n := binary.LittleEndian.Uint32(lenBuf[:])
		if n == 0 || n > 1<<20 {
			return
		}
		p := make([]byte, n)
		if _, err := io.ReadFull(br, p); err != nil {
			return
		}
		if p[0] == 4 && validVoice(p) { // the studio's voice → every listener
			r.mu.Lock()
			for l := range r.listeners {
				pushLatest(l.voice, p)
			}
			r.mu.Unlock()
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
	l := &listener{conn: conn, out: make(chan wsMsg, maxQueuedMessages), voice: make(chan []byte, voiceQueue), video: make(chan wsMsg, videoQueue)}

	r.mu.Lock()
	r.nextID++
	l.id = r.nextID
	r.listeners[l] = true
	hello, _ := json.Marshal(map[string]any{"type": "sender", "online": r.sender != nil, "talk": r.talkAvail()})
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
	for _, c := range r.chat { // the conversation so far
		select {
		case l.out <- wsMsg{false, c}:
		default:
		}
	}
	count := len(r.listeners)
	r.mu.Unlock()
	log.Printf("[%s] listener joined (%d)", name, count)
	writeStatus()

	// reader: talkback from this listener (binary type 5) and noticing the close
	conn.SetReadLimit(4 << 20) // keyframes of a shared screen
	go func() {
		for {
			t, p, err := conn.ReadMessage()
			if err == nil && t == websocket.BinaryMessage && len(p) > 0 && p[0] == 6 && validVideo(p) {
				binary.LittleEndian.PutUint16(p[1:3], l.id)
				r.mu.Lock()
				r.relayVideo(l, wsMsg{true, p}, p[3]&1 == 1)
				r.mu.Unlock()
				continue
			}
			if err == nil && t == websocket.TextMessage {
				// video control between pages: {"type":"vconf"|"vkey"|"vbye", ...}, stamped with the sender
				var m map[string]any
				if json.Unmarshal(p, &m) == nil {
					if ty, _ := m["type"].(string); ty == "chat" {
						text, _ := m["text"].(string)
						name, _ := m["name"].(string)
						text, name = strings.TrimSpace(text), strings.TrimSpace(name)
						if text != "" && len(text) <= 2000 && len(name) <= 60 {
							b, _ := json.Marshal(map[string]any{"type": "chat", "text": text, "name": name, "from": l.id, "t": time.Now().UnixMilli()})
							r.mu.Lock()
							r.chat = append(r.chat, b)
							if len(r.chat) > 50 {
								r.chat = r.chat[len(r.chat)-50:]
							}
							r.relayVideo(l, wsMsg{false, b}, false) // same priority lane as video
							r.mu.Unlock()
						}
					} else if ty == "vconf" || ty == "vkey" || ty == "vbye" {
						m["from"] = l.id
						b, _ := json.Marshal(m)
						r.mu.Lock()
						r.relayVideo(l, wsMsg{false, b}, false)
						r.mu.Unlock()
					}
				}
				continue
			}
			if err == nil && t == websocket.BinaryMessage && len(p) > 0 && p[0] == 5 && validVoice(p) {
				// to the sender: [5][listener id u16][seq u32][n u16][pcm]
				v := make([]byte, len(p)+2)
				v[0] = 5
				binary.LittleEndian.PutUint16(v[1:3], l.id)
				copy(v[3:], p[1:])
				r.mu.Lock()
				if r.talkerVoice != nil { // a talk-only sender answers for the studio
					pushLatest(r.talkerVoice, v)
				} else if r.senderVoice != nil {
					pushLatest(r.senderVoice, v)
				}
				r.mu.Unlock()
				continue
			}
			if err != nil {
				r.mu.Lock()
				if r.listeners[l] {
					delete(r.listeners, l)
					l.close()
					bye, _ := json.Marshal(map[string]any{"type": "vbye", "from": l.id})
					r.relayVideo(l, wsMsg{false, bye}, false)
				}
				r.mu.Unlock()
				return
			}
		}
	}()
	write := func(t int, b []byte) bool {
		_ = conn.SetWriteDeadline(time.Now().Add(20 * time.Second))
		return conn.WriteMessage(t, b) == nil
	}
	writeMsg := func(m wsMsg) bool {
		if m.binary {
			return write(websocket.BinaryMessage, m.data)
		}
		return write(websocket.TextMessage, m.data)
	}
	for {
		select { // talkback first, always
		case v := <-l.voice:
			if !write(websocket.BinaryMessage, v) {
				goto done
			}
			continue
		default:
		}
		select { // then video: the master has a 2 s buffer, the picture doesn't
		case m := <-l.video:
			if !writeMsg(m) {
				goto done
			}
			continue
		default:
		}
		select {
		case v := <-l.voice:
			if !write(websocket.BinaryMessage, v) {
				goto done
			}
		case m := <-l.video:
			if !writeMsg(m) {
				goto done
			}
		case m, ok := <-l.out:
			if !ok {
				goto done
			}
			t := websocket.TextMessage
			if m.binary {
				t = websocket.BinaryMessage
			}
			if !write(t, m.data) {
				goto done
			}
		}
	}
done:
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
	if *logFile != "" {
		if f, err := os.OpenFile(*logFile, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644); err == nil {
			log.SetOutput(f)
		}
	}
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
	quit := make(chan struct{})
	var quitOnce sync.Once
	mux.HandleFunc("/quit", func(w http.ResponseWriter, r *http.Request) {
		if !isLocal(r) { // the tunnel also arrives from 127.0.0.1: nobody outside may stop us
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		io.WriteString(w, "bye")
		quitOnce.Do(func() { close(quit) })
	})
	registerLicenseRoutes(mux)
	go licenseLoop()
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

	if *parentPID > 0 {
		go func() {
			for parentAlive(*parentPID) {
				time.Sleep(time.Second)
			}
			log.Printf("parent %d gone, shutting down", *parentPID)
			quitOnce.Do(func() { close(quit) })
		}()
	}

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	select {
	case <-sig:
	case <-quit:
	}
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

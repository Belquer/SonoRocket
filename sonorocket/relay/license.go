package main

// Studio registration (PROTOCOL.md, "Registration").
//
// The studio side (whoever runs this relay) registers once with a license key bought
// through Lemon Squeezy; listeners never need anything. Unregistered studios keep
// working and are asked to buy. The key is checked against our own store/product IDs,
// baked in at build time, so a key from any other Lemon Squeezy product won't do.
// After activation the relay re-validates once a day and tolerates 30 days offline.
//
// Licensing stays off (everyone counts as registered) until the build sets
// lsProductID, e.g. go build -ldflags "-X main.lsProductID=123 -X main.lsStoreID=45 -X main.buyURL=https://…".

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

var (
	lsStoreID   = "" // -ldflags "-X main.lsStoreID=…"
	lsProductID = "" // -ldflags "-X main.lsProductID=…"
	buyURL      = "https://github.com/Belquer/SonoRocket#comprar"
	lsAPI       = "https://api.lemonsqueezy.com/v1/licenses/" // overridable for tests
)

const (
	revalidateEvery = 24 * time.Hour
	offlineGrace    = 30 * 24 * time.Hour
)

type licenseState struct {
	Key         string    `json:"key"`
	InstanceID  string    `json:"instanceId"`
	Email       string    `json:"email"`
	Name        string    `json:"name"`
	Status      string    `json:"status"` // active | inactive | expired | disabled | invalid
	ValidatedAt time.Time `json:"validatedAt"`
}

var (
	licMu  sync.Mutex
	lic    licenseState
	licErr string
)

func licensingOn() bool { return lsProductID != "" }

func licensePath() string {
	dir, err := os.UserConfigDir()
	if err != nil {
		dir = "."
	}
	return filepath.Join(dir, "SonoRocket", "license.json")
}

func loadLicense() {
	b, err := os.ReadFile(licensePath())
	if err != nil {
		return
	}
	licMu.Lock()
	_ = json.Unmarshal(b, &lic)
	licMu.Unlock()
}

func saveLicense() {
	licMu.Lock()
	b, _ := json.MarshalIndent(lic, "", "  ")
	licMu.Unlock()
	p := licensePath()
	_ = os.MkdirAll(filepath.Dir(p), 0o755)
	_ = os.WriteFile(p, b, 0o600)
}

// licensedLocked: registration not required, or an active key validated within the grace period.
func licensedLocked() bool {
	if !licensingOn() {
		return true
	}
	return lic.Status == "active" && lic.InstanceID != "" && time.Since(lic.ValidatedAt) < offlineGrace
}

func licensed() bool {
	licMu.Lock()
	defer licMu.Unlock()
	return licensedLocked()
}

type lsResponse struct {
	Activated  bool   `json:"activated"`
	Valid      bool   `json:"valid"`
	Error      string `json:"error"`
	LicenseKey struct {
		Status string `json:"status"`
	} `json:"license_key"`
	Instance *struct {
		ID string `json:"id"`
	} `json:"instance"`
	Meta struct {
		StoreID       int    `json:"store_id"`
		ProductID     int    `json:"product_id"`
		CustomerEmail string `json:"customer_email"`
		CustomerName  string `json:"customer_name"`
	} `json:"meta"`
}

var lsClient = &http.Client{Timeout: 15 * time.Second}

func lsCall(action string, form url.Values) (*lsResponse, error) {
	req, _ := http.NewRequest("POST", lsAPI+action, strings.NewReader(form.Encode()))
	req.Header.Set("Accept", "application/json")
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	res, err := lsClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	var out lsResponse
	body, _ := io.ReadAll(io.LimitReader(res.Body, 1<<20))
	if err := json.Unmarshal(body, &out); err != nil {
		return nil, fmt.Errorf("license server: HTTP %d", res.StatusCode)
	}
	return &out, nil
}

// ours: the key belongs to SonoRocket, not to some other Lemon Squeezy product.
func ours(r *lsResponse) bool {
	if strconv.Itoa(r.Meta.ProductID) != lsProductID {
		return false
	}
	return lsStoreID == "" || strconv.Itoa(r.Meta.StoreID) == lsStoreID
}

func activateLicense(key, email string) error {
	key, email = strings.TrimSpace(key), strings.TrimSpace(email)
	if key == "" {
		return errors.New("digite a chave")
	}
	host, _ := os.Hostname()
	r, err := lsCall("activate", url.Values{"license_key": {key}, "instance_name": {"SonoRocket · " + host}})
	if err != nil {
		return fmt.Errorf("sem conexão com o servidor de licenças (%v)", err)
	}
	if !r.Activated || r.Instance == nil {
		if r.Error != "" {
			return errors.New(r.Error)
		}
		return errors.New("chave não ativada")
	}
	if !ours(r) {
		lsCall("deactivate", url.Values{"license_key": {key}, "instance_id": {r.Instance.ID}})
		return errors.New("essa chave não é do SonoRocket")
	}
	if email != "" && !strings.EqualFold(email, r.Meta.CustomerEmail) {
		lsCall("deactivate", url.Values{"license_key": {key}, "instance_id": {r.Instance.ID}})
		return errors.New("o e-mail não confere com o da compra")
	}
	licMu.Lock()
	lic = licenseState{Key: key, InstanceID: r.Instance.ID, Email: r.Meta.CustomerEmail, Name: r.Meta.CustomerName,
		Status: r.LicenseKey.Status, ValidatedAt: time.Now()}
	licErr = ""
	licMu.Unlock()
	saveLicense()
	log.Printf("license: activated for %s (%s)", r.Meta.CustomerEmail, r.LicenseKey.Status)
	return nil
}

func deactivateLicense() {
	licMu.Lock()
	key, inst := lic.Key, lic.InstanceID
	lic = licenseState{}
	licMu.Unlock()
	if key != "" && inst != "" {
		lsCall("deactivate", url.Values{"license_key": {key}, "instance_id": {inst}})
	}
	saveLicense()
}

// revalidate: daily. Network trouble keeps the last good state (offline grace);
// only an explicit "not valid" from the server revokes.
func revalidate() {
	licMu.Lock()
	key, inst, last := lic.Key, lic.InstanceID, lic.ValidatedAt
	licMu.Unlock()
	if key == "" || inst == "" || time.Since(last) < revalidateEvery {
		return
	}
	r, err := lsCall("validate", url.Values{"license_key": {key}, "instance_id": {inst}})
	if err != nil {
		return
	}
	licMu.Lock()
	if r.Valid && ours(r) {
		lic.Status, lic.ValidatedAt, licErr = r.LicenseKey.Status, time.Now(), ""
	} else {
		lic.Status, licErr = "invalid", r.Error
	}
	licMu.Unlock()
	saveLicense()
}

func licenseLoop() {
	loadLicense()
	for {
		revalidate()
		time.Sleep(time.Hour)
	}
}

func licenseInfo() map[string]any {
	licMu.Lock()
	defer licMu.Unlock()
	return map[string]any{"licensing": licensingOn(), "licensed": licensedLocked(),
		"email": lic.Email, "name": lic.Name, "status": lic.Status, "error": licErr, "buyUrl": buyURL}
}

// isLocal: a request from this computer itself. Everything through the public tunnel also
// arrives from 127.0.0.1 (cloudflared), so the proxy headers are what tell them apart.
func isLocal(r *http.Request) bool {
	host, _, _ := net.SplitHostPort(r.RemoteAddr)
	if ip := net.ParseIP(host); ip == nil || !ip.IsLoopback() {
		return false
	}
	for _, h := range []string{"Cf-Connecting-Ip", "Cf-Ray", "X-Forwarded-For", "Forwarded"} {
		if r.Header.Get(h) != "" {
			return false
		}
	}
	return true
}

func registerLicenseRoutes(mux *http.ServeMux) {
	local := func(h http.HandlerFunc) http.HandlerFunc {
		return func(w http.ResponseWriter, r *http.Request) {
			if !isLocal(r) {
				http.Error(w, "forbidden", http.StatusForbidden)
				return
			}
			h(w, r)
		}
	}
	mux.HandleFunc("/license", local(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(licenseInfo())
	}))
	mux.HandleFunc("/license/activate", local(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST", http.StatusMethodNotAllowed)
			return
		}
		_ = r.ParseForm()
		err := activateLicense(r.PostFormValue("key"), r.PostFormValue("email"))
		w.Header().Set("Content-Type", "application/json")
		out := licenseInfo()
		if err != nil {
			out["error"] = err.Error()
		}
		json.NewEncoder(w).Encode(out)
		writeStatus()
	}))
	mux.HandleFunc("/license/deactivate", local(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST", http.StatusMethodNotAllowed)
			return
		}
		deactivateLicense()
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(licenseInfo())
		writeStatus()
	}))
	mux.HandleFunc("/conta", local(func(w http.ResponseWriter, r *http.Request) {
		b, _ := studioFS.ReadFile("studio/conta.html")
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		w.Write(b)
	}))
}

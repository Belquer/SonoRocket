package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// A fake Lemon Squeezy: key "GOOD" is ours, "OTHER" belongs to another product.
func fakeLS(t *testing.T, deactivated *int) *httptest.Server {
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = r.ParseForm()
		key := r.PostFormValue("license_key")
		product := 777
		if key == "OTHER" {
			product = 1
		}
		resp := map[string]any{
			"license_key": map[string]any{"status": "active"},
			"meta":        map[string]any{"store_id": 9, "product_id": product, "customer_email": "lu@example.com", "customer_name": "Luciano"},
		}
		switch {
		case strings.HasSuffix(r.URL.Path, "/activate"):
			if key == "BAD" {
				resp = map[string]any{"activated": false, "error": "license_key not found."}
			} else {
				resp["activated"] = true
				resp["instance"] = map[string]any{"id": "inst-1"}
			}
		case strings.HasSuffix(r.URL.Path, "/validate"):
			resp["valid"] = key == "GOOD"
			resp["instance"] = map[string]any{"id": "inst-1"}
		case strings.HasSuffix(r.URL.Path, "/deactivate"):
			*deactivated++
			resp = map[string]any{"deactivated": true}
		}
		json.NewEncoder(w).Encode(resp)
	}))
}

func TestLicensing(t *testing.T) {
	t.Setenv("XDG_CONFIG_HOME", t.TempDir())
	t.Setenv("HOME", t.TempDir())
	deact := 0
	srv := fakeLS(t, &deact)
	defer srv.Close()
	lsAPI, lsProductID, lsStoreID = srv.URL+"/v1/licenses/", "777", "9"
	defer func() { lsProductID, lsStoreID = "", "" }()
	lic = licenseState{}

	if licensed() {
		t.Fatal("unregistered studio counted as licensed")
	}
	if err := activateLicense("BAD", ""); err == nil {
		t.Fatal("unknown key accepted")
	}
	if err := activateLicense("OTHER", ""); err == nil || deact != 1 {
		t.Fatalf("another product's key accepted (err=%v, deactivated=%d)", err, deact)
	}
	if err := activateLicense("GOOD", "someone@else.com"); err == nil || deact != 2 {
		t.Fatalf("wrong e-mail accepted (err=%v)", err)
	}
	if err := activateLicense("GOOD", "LU@example.com"); err != nil || !licensed() {
		t.Fatalf("good key refused: %v", err)
	}
	// survives a restart
	lic = licenseState{}
	loadLicense()
	if !licensed() {
		t.Fatal("license not persisted")
	}
	deactivateLicense()
	if licensed() || deact != 3 {
		t.Fatal("deactivate failed")
	}
}

func TestLicensingOffByDefault(t *testing.T) {
	lsProductID = ""
	if !licensed() {
		t.Fatal("a build without a product id must not nag")
	}
}

func TestIsLocal(t *testing.T) {
	r := httptest.NewRequest("GET", "/conta", nil)
	r.RemoteAddr = "127.0.0.1:5555"
	if !isLocal(r) {
		t.Fatal("local request refused")
	}
	r.Header.Set("Cf-Connecting-Ip", "203.0.113.9") // through the public tunnel
	if isLocal(r) {
		t.Fatal("tunnel request treated as local")
	}
	r2 := httptest.NewRequest("GET", "/conta", nil)
	r2.RemoteAddr = "192.168.0.7:5555"
	if isLocal(r2) {
		t.Fatal("LAN request treated as local")
	}
}

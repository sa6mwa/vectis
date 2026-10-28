package main

import (
	"context"
	"encoding/pem"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// One foreground test driver owns the HTTP origins and child command. CTest
// owns the outer cancellation deadline; no detached service or process scan.
func main() {
	if len(os.Args) < 3 {
		panic("usage: curl_http work-dir executable script [arguments]")
	}
	var mutex sync.Mutex
	type pair struct {
		arrived int
		ready   chan struct{}
	}
	pairs := make(map[string]*pair)
	handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/identity":
			_, _ = io.WriteString(w, r.RemoteAddr)
		case "/echo":
			w.Header().Set("X-Seen-Header", r.Header.Get("X-Test"))
			w.Header().Set("X-Seen-Method", r.Method)
			w.Header().Set("X-Seen-Authorization", r.Header.Get("Authorization"))
			w.Header().Set("X-Seen-User-Agent", r.UserAgent())
			w.Header().Set("X-Seen-Encoding", r.Header.Get("Accept-Encoding"))
			_, _ = io.Copy(w, r.Body)
		case "/large":
			block := strings.Repeat("x", 4096)
			for i := 0; i < 2050; i++ {
				if _, err := io.WriteString(w, block); err != nil {
					return
				}
			}
		case "/headers":
			w.Header().Set("X-Oversized", strings.Repeat("x", 70000))
			_, _ = io.WriteString(w, "small")
		case "/status":
			http.Error(w, "unavailable", http.StatusServiceUnavailable)
		case "/partial":
			w.Header().Set("Content-Length", "100")
			_, _ = io.WriteString(w, "x")
		case "/redirect":
			_, _ = io.Copy(io.Discard, r.Body)
			http.Redirect(w, r, "/echo", http.StatusTemporaryRedirect)
		case "/pair":
			key := r.URL.Query().Get("group")
			mutex.Lock()
			p := pairs[key]
			if p == nil {
				p = &pair{ready: make(chan struct{})}
				pairs[key] = p
			}
			p.arrived++
			if p.arrived == 2 {
				close(p.ready)
			}
			mutex.Unlock()
			select {
			case <-p.ready:
				_, _ = io.WriteString(w, r.URL.Query().Get("item"))
			case <-r.Context().Done():
			}
		default:
			http.NotFound(w, r)
		}
	})
	plain := httptest.NewServer(handler)
	defer plain.Close()
	secured := httptest.NewTLSServer(handler)
	defer secured.Close()
	cert := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: secured.Certificate().Raw})
	ca := filepath.Join(os.Args[1], "curl-origin-ca.pem")
	if err := os.WriteFile(ca, cert, 0600); err != nil {
		panic(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 12*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, os.Args[2], os.Args[3:]...)
	command.Env = append(os.Environ(), "VECTIS_CURL_ORIGIN="+plain.URL, "VECTIS_CURL_TLS_ORIGIN="+secured.URL, "VECTIS_CURL_CA="+ca)
	command.Stdout = os.Stdout
	command.Stderr = os.Stderr
	if err := command.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "curl behavior command:", err)
		os.Exit(1)
	}
}

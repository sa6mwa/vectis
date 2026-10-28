package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Local protocol fixture; Lua asserts client behavior, while this compiled
// peer verifies the actual request bytes, redirect methods and replay counts.
func runUploadFixture(args []string) error {
	if len(args) != 3 {
		return errors.New("usage: http-uploads VECTIS LUA_SCRIPT OWNED_ROOT")
	}
	root := args[2]
	if err := ownedFixtureRoot(root); err != nil {
		return err
	}
	if err := os.MkdirAll(root, 0700); err != nil {
		return err
	}
	work, err := os.MkdirTemp(root, "uploads-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(work)
	payload := bytes.Repeat([]byte("upload\x00bytes"), 4096)
	path := filepath.Join(work, "payload.bin")
	if err = os.WriteFile(path, payload, 0600); err != nil {
		return err
	}
	var mu sync.Mutex
	counts := map[string]int{}
	var failures []string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer r.Body.Close()
		var body []byte
		var problem error
		switch {
		case r.Method == "GET":
			body, problem = io.ReadAll(io.LimitReader(r.Body, 131073))
			if len(body) != 0 {
				problem = errors.New("GET retained upload body")
			}
		case strings.HasPrefix(r.URL.Path, "/json"):
			if r.ContentLength != -1 || len(r.TransferEncoding) != 1 || r.TransferEncoding[0] != "chunked" {
				problem = errors.New("streamed JSON was not chunked")
				break
			}
			var encoded map[string]string
			body, problem = io.ReadAll(io.LimitReader(r.Body, 131073))
			if problem == nil {
				problem = json.Unmarshal(body, &encoded)
			}
			if problem == nil && (len(encoded) != 1 || encoded["payload"] != strings.ReplaceAll(string(payload), "\x00", "_")) {
				problem = errors.New("JSON upload bytes differ")
			}
		case strings.HasPrefix(r.URL.Path, "/multipart"):
			reader, e := r.MultipartReader()
			problem = e
			if e == nil {
				part, e := reader.NextPart()
				problem = e
				if e == nil {
					body, problem = io.ReadAll(io.LimitReader(part, 131073))
					if problem == nil && (part.FormName() != "payload" || !bytes.Equal(body, payload)) {
						problem = errors.New("multipart upload differs")
					}
					part.Close()
					if _, e = reader.NextPart(); e != io.EOF {
						problem = errors.New("multipart did not contain exactly one part")
					}
				}
			}
		default:
			body, problem = io.ReadAll(io.LimitReader(r.Body, 131073))
			if problem == nil && !bytes.Equal(body, payload) {
				problem = errors.New("upload bytes differ")
			}
		}
		mu.Lock()
		key := r.Method + " " + r.URL.Path
		counts[key]++
		attempt := counts[key]
		if problem != nil {
			failures = append(failures, key+": "+problem.Error())
		}
		mu.Unlock()
		if problem != nil {
			http.Error(w, problem.Error(), 500)
			return
		}
		w.Header().Set("Content-Length", "0")
		if r.URL.Path == "/json/retry" && attempt == 1 {
			w.WriteHeader(503)
			return
		}
		last := strings.LastIndexByte(r.URL.Path, '/')
		code, _ := strconv.Atoi(r.URL.Path[last+1:])
		if code == 301 || code == 302 || code == 303 || code == 307 || code == 308 {
			w.Header().Set("Location", r.URL.Path[:last]+"/received")
			w.WriteHeader(code)
			return
		}
		w.Header().Set("Content-Length", strconv.Itoa(len(r.Method)))
		_, _ = io.WriteString(w, r.Method)
	}))
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, args[0], args[1], server.URL, path)
	cmd.Dir = work
	output, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("upload client: %w\n%s", err, output)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(failures) != 0 {
		return fmt.Errorf("wire failures: %v", failures)
	}
	total := 0
	for _, count := range counts {
		total += count
	}
	if total != 172 {
		return fmt.Errorf("request count %d, want 172", total)
	}
	for _, kind := range []string{"file", "multipart", "raw", "json", "buffered"} {
		if counts["GET /"+kind+"/received"] != 5 {
			return fmt.Errorf("GET redirect count for %s", kind)
		}
		for _, method := range []string{"POST", "PUT", "PATCH"} {
			expected := 4
			if method == "POST" {
				expected = 2
			}
			if counts[method+" /"+kind] < 1 || counts[method+" /"+kind+"/received"] != expected {
				return fmt.Errorf("redirect/replay count for %s %s", method, kind)
			}
		}
	}
	for _, method := range []string{"POST", "PUT", "PATCH"} {
		if counts[method+" /json/retry"] != 2 {
			return fmt.Errorf("JSON retry count for %s", method)
		}
	}
	fmt.Println("64 uploads, 75 redirects and 3 retries: 172 requests verified")
	return nil
}

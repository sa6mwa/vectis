package main

import (
	"bufio"
	"bytes"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
)

const chunkSize = 16384

type sample struct {
	FirstMS      float64 `json:"first_ms"`
	CompletionMS float64 `json:"completion_ms"`
	Bytes        int64   `json:"bytes"`
}

type generatedBody struct {
	remaining int64
	value     byte
}

func (body *generatedBody) Read(p []byte) (int, error) {
	if body.remaining == 0 {
		return 0, io.EOF
	}
	n := len(p)
	if int64(n) > body.remaining {
		n = int(body.remaining)
	}
	for i := 0; i < n; i++ {
		p[i] = body.value
	}
	body.remaining -= int64(n)
	return n, nil
}

func benchmarkOrigin(chunks, events, major int) http.Handler {
	chunk := bytes.Repeat([]byte{'v'}, chunkSize)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.ProtoMajor != major {
			http.Error(w, "wrong origin protocol", http.StatusMisdirectedRequest)
			return
		}
		switch r.URL.Path {
		case "/ws":
			benchmarkWS(w, r)
		case "/proto":
			_, _ = io.WriteString(w, r.Proto)
		case "/small":
			_, _ = io.WriteString(w, "ok")
		case "/download":
			w.Header().Set("Content-Length", strconv.Itoa(chunks*chunkSize))
			for i := 0; i < chunks; i++ {
				if _, err := w.Write(chunk); err != nil {
					return
				}
			}
		case "/upload":
			buffer := make([]byte, chunkSize)
			var total int64
			for {
				n, err := r.Body.Read(buffer)
				for _, value := range buffer[:n] {
					if value != 'u' {
						http.Error(w, "upload payload mismatch", http.StatusBadRequest)
						return
					}
				}
				total += int64(n)
				if n > 0 {
					time.Sleep(500 * time.Microsecond)
				}
				if err != nil {
					if err != io.EOF {
						http.Error(w, err.Error(), http.StatusBadRequest)
						return
					}
					break
				}
			}
			_, _ = fmt.Fprint(w, total)
		case "/duplex":
			if err := http.NewResponseController(w).EnableFullDuplex(); err != nil {
				http.Error(w, err.Error(), http.StatusInternalServerError)
				return
			}
			buffer := make([]byte, chunkSize)
			w.Header().Set("Content-Type", "application/octet-stream")
			for {
				n, err := r.Body.Read(buffer)
				if n > 0 {
					if _, writeErr := w.Write(buffer[:n]); writeErr != nil {
						return
					}
					if flushErr := http.NewResponseController(w).Flush(); flushErr != nil {
						return
					}
				}
				if err != nil {
					return
				}
			}
		case "/sse":
			w.Header().Set("Content-Type", "text/event-stream")
			w.Header().Set("Cache-Control", "no-cache")
			for i := 0; i < events; i++ {
				if _, err := fmt.Fprintf(w, "data: %d\n\n", time.Now().UnixNano()); err != nil {
					return
				}
				if err := http.NewResponseController(w).Flush(); err != nil {
					return
				}
				time.Sleep(10 * time.Millisecond)
			}
		default:
			http.NotFound(w, r)
		}
	})
}

func requestTrial(client *http.Client, base, profile string, chunks int) (sample, error) {
	path := profile
	if profile == "slow_reader" {
		path = "download"
	} else if profile == "slow_upload" {
		path = "upload"
	}
	var body io.Reader
	if path == "upload" {
		body = &generatedBody{remaining: int64(chunks * chunkSize), value: 'u'}
	}
	method := http.MethodGet
	if body != nil {
		method = http.MethodPost
	}
	request, err := http.NewRequest(method, base+"/"+path, body)
	if err != nil {
		return sample{}, err
	}
	if body != nil {
		request.ContentLength = int64(chunks * chunkSize)
	}
	start := time.Now()
	response, err := client.Do(request)
	if err != nil {
		return sample{}, err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return sample{}, fmt.Errorf("%s returned %s", path, response.Status)
	}
	var output sample
	buffer := make([]byte, chunkSize)
	short := make([]byte, 0, 32)
	for {
		n, readErr := response.Body.Read(buffer)
		if n > 0 {
			if output.FirstMS == 0 {
				output.FirstMS = float64(time.Since(start)) / float64(time.Millisecond)
			}
			output.Bytes += int64(n)
			if path == "download" {
				for _, value := range buffer[:n] {
					if value != 'v' {
						return sample{}, errors.New("download payload mismatch")
					}
				}
			} else {
				short = append(short, buffer[:n]...)
				if len(short) > 32 {
					return sample{}, errors.New("short response exceeded 32 bytes")
				}
			}
			if profile == "slow_reader" {
				time.Sleep(500 * time.Microsecond)
			}
		}
		if readErr != nil {
			if readErr != io.EOF {
				return sample{}, readErr
			}
			break
		}
	}
	output.CompletionMS = float64(time.Since(start)) / float64(time.Millisecond)
	expected := int64(2)
	if path == "download" {
		expected = int64(chunks * chunkSize)
	} else if path == "upload" {
		expected = int64(len(strconv.Itoa(chunks * chunkSize)))
		if string(short) != strconv.Itoa(chunks*chunkSize) {
			return sample{}, errors.New("upload byte count mismatch")
		}
	} else if string(short) != "ok" {
		return sample{}, errors.New("small payload mismatch")
	}
	if output.Bytes != expected {
		return sample{}, fmt.Errorf("%s returned %d bytes, expected %d", path, output.Bytes, expected)
	}
	return output, nil
}

func duplexTrial(client *http.Client, base string, chunks int) (sample, error) {
	reader, writer := io.Pipe()
	defer reader.Close()
	writeDone := make(chan error, 1)
	go func() {
		chunk := bytes.Repeat([]byte{'d'}, chunkSize)
		for i := 0; i < chunks; i++ {
			if _, err := writer.Write(chunk); err != nil {
				writeDone <- err
				return
			}
			if i+1 < chunks {
				time.Sleep(2 * time.Millisecond)
			}
		}
		writeDone <- writer.Close()
	}()
	request, err := http.NewRequest(http.MethodPost, base+"/duplex", reader)
	if err != nil {
		return sample{}, err
	}
	request.ContentLength = int64(chunks * chunkSize)
	start := time.Now()
	response, err := client.Do(request)
	if err != nil {
		_ = reader.Close()
		return sample{}, err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return sample{}, fmt.Errorf("duplex returned %s", response.Status)
	}
	var output sample
	buffer := make([]byte, chunkSize)
	for {
		n, readErr := response.Body.Read(buffer)
		if n > 0 {
			if output.FirstMS == 0 {
				output.FirstMS = float64(time.Since(start)) / float64(time.Millisecond)
				select {
				case <-writeDone:
					return sample{}, errors.New("duplex response arrived after upload EOF")
				default:
				}
			}
			for _, value := range buffer[:n] {
				if value != 'd' {
					return sample{}, errors.New("duplex payload mismatch")
				}
			}
			output.Bytes += int64(n)
		}
		if readErr != nil {
			if readErr != io.EOF {
				return sample{}, readErr
			}
			break
		}
	}
	if err := <-writeDone; err != nil {
		return sample{}, err
	}
	output.CompletionMS = float64(time.Since(start)) / float64(time.Millisecond)
	if output.Bytes != int64(chunks*chunkSize) {
		return sample{}, fmt.Errorf("duplex returned %d bytes", output.Bytes)
	}
	return output, nil
}

func sseTrial(client *http.Client, base string, events int, ready ...func()) ([]float64, error) {
	response, err := client.Get(base + "/sse")
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK || response.Header.Get("Content-Type") != "text/event-stream" {
		return nil, fmt.Errorf("unexpected SSE response %s", response.Status)
	}
	for _, mark := range ready {
		mark()
	}
	reader := bufio.NewReader(response.Body)
	delays := make([]float64, 0, events)
	for i := 0; i < events; i++ {
		line, err := reader.ReadString('\n')
		if err != nil || !strings.HasPrefix(line, "data: ") {
			return nil, errors.New("SSE event truncated or malformed")
		}
		nanoseconds, err := strconv.ParseInt(strings.TrimSpace(line[6:]), 10, 64)
		if err != nil {
			return nil, err
		}
		delays = append(delays, float64(time.Now().UnixNano()-nanoseconds)/float64(time.Millisecond))
		if line, err = reader.ReadString('\n'); err != nil || line != "\n" {
			return nil, errors.New("SSE event delimiter missing")
		}
	}
	if _, err := reader.ReadByte(); err != io.EOF {
		return nil, errors.New("SSE response contains extra bytes")
	}
	return delays, nil
}

func measureProfiles(options profileOptions) (output benchmarkSamples, err error) {
	roots := x509.NewCertPool()
	if options.CA != "" {
		pem, err := os.ReadFile(options.CA)
		if err != nil {
			return output, err
		}
		if !roots.AppendCertsFromPEM(pem) {
			return output, errors.New("CA bundle has no certificate")
		}
	}
	selected := map[string]bool{}
	for _, profile := range options.Profiles {
		selected[profile] = true
	}
	transport := &http.Transport{
		TLSClientConfig:     &tls.Config{RootCAs: roots, MinVersion: tls.VersionTLS12},
		ForceAttemptHTTP2:   options.Downstream == 2,
		MaxIdleConns:        64,
		MaxIdleConnsPerHost: 32,
		MaxConnsPerHost:     32,
	}
	defer transport.CloseIdleConnections()
	client := &http.Client{Transport: transport, Timeout: 30 * time.Second}
	probe, err := client.Get(options.Base + "/proto")
	if err != nil {
		return output, err
	}
	protocol, readErr := io.ReadAll(io.LimitReader(probe.Body, 32))
	_ = probe.Body.Close()
	if readErr != nil {
		return output, readErr
	}
	if probe.StatusCode != http.StatusOK || probe.ProtoMajor != options.Downstream || string(protocol) != map[int]string{1: "HTTP/1.1", 2: "HTTP/2.0"}[options.Origin] {
		return output, fmt.Errorf("protocol probe: status=%s downstream=%s origin=%q", probe.Status, probe.Proto, protocol)
	}
	output = benchmarkSamples{probe.Proto, string(protocol), make(map[string][]sample), make(map[string][]float64), make(map[string][]float64)}
	for _, profile := range []string{"small", "download", "slow_reader", "slow_upload", "duplex"} {
		if !selected[profile] {
			continue
		}
		for i := 0; i < options.Warmup+options.Repetitions; i++ {
			var result sample
			if profile == "duplex" {
				result, err = duplexTrial(client, options.Base, options.Chunks)
			} else {
				result, err = requestTrial(client, options.Base, profile, options.Chunks)
			}
			if err != nil {
				return output, fmt.Errorf("%s: %w", profile, err)
			}
			if i >= options.Warmup {
				output.HTTP[profile] = append(output.HTTP[profile], result)
			}
		}
	}
	for _, count := range options.Concurrency {
		for _, kind := range []string{"sse", "ws"} {
			if !selected[kind] {
				continue
			}
			for i := 0; i < options.Warmup+options.Repetitions; i++ {
				delays, err := concurrentTrials(count, func() ([]float64, error) {
					if kind == "sse" {
						return sseTrial(client, options.Base, options.Events)
					}
					return wsTrial(options.Base, transport.TLSClientConfig, options.Echoes)
				})
				if err != nil {
					return output, err
				}
				if i >= options.Warmup {
					key := fmt.Sprintf("%s_%d", kind, count)
					if kind == "sse" {
						output.SSE[key] = append(output.SSE[key], delays...)
					} else {
						output.WS[key] = append(output.WS[key], delays...)
					}
				}
			}
		}

	}
	return output, nil
}

func main() {
	var err error
	if len(os.Args) < 2 {
		err = errors.New("usage: vectis_bench_fixture benchmark|metrics-storage|metrics-recovery|http-uploads [flags]")
	} else if os.Args[1] == "metrics-storage" {
		err = runMetricsStorage(os.Args[2:])
	} else if os.Args[1] == "metrics-recovery" {
		err = runMetricsRecovery(os.Args[2:])
	} else if os.Args[1] == "benchmark" {
		err = runBenchmark(os.Args[2:])
	} else if os.Args[1] == "http-uploads" {
		err = runUploadFixture(os.Args[2:])
	} else {
		err = errors.New("unknown mode")
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

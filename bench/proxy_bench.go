package main

// Optional local measurement fixture. CMake owns compilation and CTest owns
// cancellation; this process owns one Vectis child and its local test origin.
import (
	"bufio"
	"bytes"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha1"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

type profileOptions struct {
	Base, CA                                                        string
	Downstream, Origin, Chunks, Events, Echoes, Warmup, Repetitions int
	Concurrency                                                     []int
	Profiles                                                        []string
}

type benchmarkSamples struct {
	DownstreamProtocol string               `json:"downstream_protocol"`
	OriginProtocol     string               `json:"origin_protocol"`
	HTTP               map[string][]sample  `json:"http"`
	SSE                map[string][]float64 `json:"sse"`
	WS                 map[string][]float64 `json:"ws"`
}

func localCertificate() (tls.Certificate, error) {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return tls.Certificate{}, err
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	if err != nil {
		return tls.Certificate{}, err
	}
	template := &x509.Certificate{SerialNumber: serial, Subject: pkix.Name{CommonName: "localhost"},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(24 * time.Hour), DNSNames: []string{"localhost"},
		IPAddresses: []net.IP{net.ParseIP("127.0.0.1")}, IsCA: true, BasicConstraintsValid: true,
		KeyUsage: x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign | x509.KeyUsageKeyEncipherment, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		return tls.Certificate{}, err
	}
	encoded, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		return tls.Certificate{}, err
	}
	return tls.X509KeyPair(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: encoded}))
}

func concurrentTrials(count int, trial func() ([]float64, error)) ([]float64, error) {
	var group sync.WaitGroup
	values := make([][]float64, count)
	failures := make([]error, count)
	start := make(chan struct{})
	for i := 0; i < count; i++ {
		group.Add(1)
		go func(i int) { defer group.Done(); <-start; values[i], failures[i] = trial() }(i)
	}
	close(start)
	group.Wait()
	var joined []float64
	for i, err := range failures {
		if err != nil {
			return nil, err
		}
		joined = append(joined, values[i]...)
	}
	return joined, nil
}

func benchmarkWS(w http.ResponseWriter, r *http.Request) {
	key, err := base64.StdEncoding.DecodeString(r.Header.Get("Sec-WebSocket-Key"))
	if err != nil || len(key) != 16 || r.Method != "GET" || !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") {
		http.Error(w, "invalid WebSocket upgrade", 400)
		return
	}
	hijacker, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "HTTP/1.1 required", 400)
		return
	}
	conn, rw, err := hijacker.Hijack()
	if err != nil {
		return
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(30 * time.Second))
	digest := sha1.Sum([]byte(r.Header.Get("Sec-WebSocket-Key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	if _, err = fmt.Fprintf(rw, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", base64.StdEncoding.EncodeToString(digest[:])); err != nil {
		return
	}
	if rw.Flush() != nil {
		return
	}
	var header [2]byte
	var mask [4]byte
	var payload [125]byte
	for {
		if _, err = io.ReadFull(rw, header[:]); err != nil {
			return
		}
		n := int(header[1] & 127)
		opcode := header[0] & 15
		if header[0]&128 == 0 || header[1]&128 == 0 || n > 125 || (opcode != 1 && opcode != 2 && opcode != 8) {
			return
		}
		if _, err = io.ReadFull(rw, mask[:]); err != nil {
			return
		}
		if _, err = io.ReadFull(rw, payload[:n]); err != nil {
			return
		}
		for i := 0; i < n; i++ {
			payload[i] ^= mask[i%4]
		}
		if _, err = rw.Write([]byte{128 | opcode, byte(n)}); err != nil {
			return
		}
		if _, err = rw.Write(payload[:n]); err != nil {
			return
		}
		if rw.Flush() != nil || opcode == 8 {
			return
		}
	}
}

func wsTrial(base string, config *tls.Config, echoes int) ([]float64, error) {
	return wsTrialPaced(base, config, echoes, 0, nil)
}
func wsTrialPaced(base string, config *tls.Config, echoes int, pace time.Duration, ready func()) ([]float64, error) {
	address, err := url.Parse(base)
	if err != nil {
		return nil, err
	}
	dialer := &net.Dialer{Timeout: 3 * time.Second}
	var conn net.Conn
	if address.Scheme == "https" {
		selected := config.Clone()
		selected.ServerName = address.Hostname()
		selected.NextProtos = []string{"http/1.1"}
		conn, err = tls.DialWithDialer(dialer, "tcp", address.Host, selected)
	} else {
		conn, err = dialer.Dial("tcp", address.Host)
	}
	if err != nil {
		return nil, err
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(30 * time.Second))
	var nonce [16]byte
	if _, err = rand.Read(nonce[:]); err != nil {
		return nil, err
	}
	key := base64.StdEncoding.EncodeToString(nonce[:])
	if _, err = fmt.Fprintf(conn, "GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n\r\n", address.Host, key); err != nil {
		return nil, err
	}
	reader := bufio.NewReader(conn)
	response, err := http.ReadResponse(reader, &http.Request{Method: "GET"})
	if err != nil {
		return nil, err
	}
	digest := sha1.Sum([]byte(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	if response.StatusCode != 101 || response.Header.Get("Sec-WebSocket-Accept") != base64.StdEncoding.EncodeToString(digest[:]) {
		return nil, fmt.Errorf("WebSocket handshake: %s", response.Status)
	}
	if ready != nil {
		ready()
	}
	delays := make([]float64, 0, echoes)
	for i := 0; i < echoes; i++ {
		payload := []byte(fmt.Sprintf("echo-%d", i))
		wire := append([]byte{0x81, 0x80 | byte(len(payload)), 1, 2, 3, 4}, payload...)
		for at := range payload {
			wire[6+at] ^= byte(at%4 + 1)
		}
		start := time.Now()
		if _, err = conn.Write(wire); err != nil {
			return nil, err
		}
		var header [2]byte
		if _, err = io.ReadFull(reader, header[:]); err != nil {
			return nil, err
		}
		if header[0] != 0x81 || int(header[1]) != len(payload) {
			return nil, errors.New("WebSocket echo framing mismatch")
		}
		var reply [125]byte
		if _, err = io.ReadFull(reader, reply[:len(payload)]); err != nil {
			return nil, err
		}
		if !bytes.Equal(reply[:len(payload)], payload) {
			return nil, errors.New("WebSocket echo payload mismatch")
		}
		delays = append(delays, float64(time.Since(start))/float64(time.Millisecond))
		if pace > 0 {
			time.Sleep(pace)
		} // Explicit soak load pacing, never fault-test waiting.
	}
	return delays, nil
}

type resourceStats struct {
	WorkerRSS  int64   `json:"peak_worker_rss_bytes"`
	GroupRSS   int64   `json:"peak_process_group_rss_bytes"`
	WorkerFD   int     `json:"peak_worker_fds"`
	GroupFD    int     `json:"peak_process_group_fds"`
	CPU        float64 `json:"process_group_cpu_seconds"`
	HarnessCPU float64 `json:"harness_cpu_seconds"`
}

// Inspect only the measured child's actual descendants. This supplies resource
// measurements; it does not discover, supervise or signal unrelated processes.
func sampleResources(master int, output *resourceStats, identities map[int]string) {
	var visit func(int)
	var rss int64
	var fds int
	visit = func(pid int) {
		root := fmt.Sprintf("/proc/%d", pid)
		stat, err := os.ReadFile(root + "/stat")
		if err != nil {
			return
		}
		tail := strings.LastIndexByte(string(stat), ')')
		if tail < 0 {
			return
		}
		fields := strings.Fields(string(stat[tail+1:]))
		if len(fields) < 22 {
			return
		}
		if pid == master && identities[pid] != "" && identities[pid] != fields[19] {
			return
		}
		identities[pid] = fields[19]
		pages, _ := strconv.ParseInt(fields[21], 10, 64)
		current := pages * int64(os.Getpagesize())
		entries, _ := os.ReadDir(root + "/fd")
		count := len(entries)
		rss += current
		fds += count
		if pid != master {
			if current > output.WorkerRSS {
				output.WorkerRSS = current
			}
			if count > output.WorkerFD {
				output.WorkerFD = count
			}
		}
		children, _ := os.ReadFile(fmt.Sprintf("%s/task/%d/children", root, pid))
		for _, child := range strings.Fields(string(children)) {
			id, err := strconv.Atoi(child)
			if err == nil {
				visit(id)
			}
		}
	}
	visit(master)
	if rss > output.GroupRSS {
		output.GroupRSS = rss
	}
	if fds > output.GroupFD {
		output.GroupFD = fds
	}
}
func resourceSampler(pid int) func() (resourceStats, map[int]string) {
	done := make(chan struct{})
	stopped := make(chan struct{})
	stats := resourceStats{}
	identities := map[int]string{}
	go func() {
		defer close(stopped)
		ticker := time.NewTicker(10 * time.Millisecond)
		defer ticker.Stop()
		sampleResources(pid, &stats, identities)
		for {
			select {
			case <-ticker.C:
				sampleResources(pid, &stats, identities)
			case <-done:
				sampleResources(pid, &stats, identities)
				return
			}
		}
	}()
	return func() (resourceStats, map[int]string) { close(done); <-stopped; return stats, identities }
}

const proxyScript = `local v=require('vectis')
local function must(value,err) assert(value,type(err)=='table' and err.message or tostring(err));return value end
local ca=os.getenv('VECTIS_BENCH_CA');if ca=='' then ca=nil end
local tls={mode='disabled'}
if os.getenv('VECTIS_BENCH_TLS')=='1' then tls={mode='manual',cert_key_bundle_path=os.getenv('VECTIS_BENCH_BUNDLE'),domain='localhost'} end
local app=must(v.app.new({app_name='proxy-benchmark',bind='127.0.0.1',port=tonumber(os.getenv('VECTIS_BENCH_PORT')),worker_count=1,worker_set_affinity_disabled=true,shutdown_grace_ms=1000,tls=tls}))
for _,path in ipairs({'proto','small','download','upload','duplex','sse','ws'}) do
 must(app:proxy({path='/'..path,target=os.getenv('VECTIS_BENCH_ORIGIN'),tls_ca_pem=ca,buffer_limit_bytes=tonumber(os.getenv('VECTIS_BENCH_BUFFER'))}))
end
must(app:start());io.stdout:write('BENCH READY\n');io.stdout:flush()
must(app:wait());app:close()
`

type fixtureChild struct {
	cmd     *exec.Cmd
	done    chan error
	log     *os.File
	stopped bool
	input   io.WriteCloser
}

func startProxy(binary, root, origin, ca, bundle string, secure bool, buffer int) (*fixtureChild, string, error) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, "", err
	}
	port := listener.Addr().(*net.TCPAddr).Port
	_ = listener.Close()
	script := filepath.Join(root, "proxy.lua")
	if err = os.WriteFile(script, []byte(proxyScript), 0600); err != nil {
		return nil, "", err
	}
	cmd := exec.Command(binary, script)
	cmd.Dir = root
	tlsValue := "0"
	scheme := "http"
	if secure {
		tlsValue = "1"
		scheme = "https"
	}
	cmd.Env = append(os.Environ(), "VECTIS_BENCH_TLS="+tlsValue, "VECTIS_BENCH_PORT="+strconv.Itoa(port), "VECTIS_BENCH_ORIGIN="+origin,
		"VECTIS_BENCH_CA="+ca, "VECTIS_BENCH_BUNDLE="+bundle, "VECTIS_BENCH_BUFFER="+strconv.Itoa(buffer), "XDG_STATE_HOME="+filepath.Join(root, "state"), "XDG_CONFIG_HOME="+filepath.Join(root, "config"))
	child, err := startFixture(cmd, "BENCH READY", 10*time.Second, false)
	if err != nil {
		return nil, "", err
	}
	host := "127.0.0.1"
	if secure {
		host = "localhost"
	}
	return child, fmt.Sprintf("%s://%s:%d", scheme, host, port), nil
}

func startFixture(cmd *exec.Cmd, marker string, deadline time.Duration, holdInput bool) (*fixtureChild, error) {
	log, err := os.Create(filepath.Join(cmd.Dir, "process.log"))
	if err != nil {
		return nil, err
	}
	cmd.Stderr = log
	child := &fixtureChild{cmd: cmd, done: make(chan error, 1), log: log}
	if holdInput {
		child.input, err = cmd.StdinPipe()
		if err != nil {
			log.Close()
			return nil, err
		}
	}
	pipe, err := cmd.StdoutPipe()
	if err != nil {
		log.Close()
		if child.input != nil {
			child.input.Close()
		}
		return nil, err
	}
	if err = cmd.Start(); err != nil {
		log.Close()
		if child.input != nil {
			child.input.Close()
		}
		return nil, err
	}
	ready := make(chan struct{}, 1)
	go func() {
		reader := bufio.NewScanner(pipe)
		for reader.Scan() {
			line := reader.Text()
			_, _ = fmt.Fprintln(log, line)
			if line == marker {
				ready <- struct{}{}
			}
		}
		if reader.Err() != nil {
			_ = cmd.Process.Kill()
		}
		child.done <- cmd.Wait()
	}()
	select {
	case <-ready:
		return child, nil
	case err = <-child.done:
		child.stopped = true
		log.Close()
		if child.input != nil {
			child.input.Close()
		}
		return child, fmt.Errorf("fixture exited before %q: %v", marker, err)
	case <-time.After(deadline):
		_ = child.stop()
		return child, fmt.Errorf("missing readiness event %q", marker)
	}
}

func (child *fixtureChild) stop() error {
	if child.stopped {
		return nil
	}
	child.stopped = true
	if child.input != nil {
		defer child.input.Close()
	}
	_ = child.cmd.Process.Signal(syscall.SIGTERM)
	select {
	case err := <-child.done:
		child.log.Close()
		return err
	case <-time.After(5 * time.Second):
		_ = child.cmd.Process.Kill()
		<-child.done
		child.log.Close()
		return errors.New("proxy exceeded shutdown deadline")
	}
}

func distribution(values []float64) map[string]float64 {
	sorted := append([]float64{}, values...)
	sort.Float64s(sorted)
	result := map[string]float64{}
	result["samples"] = float64(len(values))
	if len(values) > 0 {
		var sum float64
		for _, value := range values {
			sum += value
		}
		result["mean_ms"] = sum / float64(len(values))
	}
	for _, p := range []struct {
		name     string
		fraction float64
	}{{"p50_ms", .50}, {"p95_ms", .95}, {"p99_ms", .99}} {
		if len(sorted) > 0 {
			result[p.name] = sorted[int(math.Ceil(float64(len(sorted))*p.fraction))-1]
		}
	}
	return result
}
func summarize(raw benchmarkSamples, chunks int) map[string]any {
	out := map[string]any{}
	for name, samples := range raw.HTTP {
		var first, completion []float64
		var expected int64 = -1
		requestBytes := int64(0)
		if name == "slow_upload" || name == "duplex" {
			requestBytes = int64(chunks * chunkSize)
		}
		for _, s := range samples {
			if expected < 0 {
				expected = s.Bytes
			}
			if expected != s.Bytes {
				panic("inconsistent verified benchmark payload")
			}
			first = append(first, s.FirstMS)
			completion = append(completion, s.CompletionMS)
		}
		var elapsed float64
		for _, value := range completion {
			elapsed += value
		}
		elapsed /= float64(len(completion)) * 1000
		out[name] = map[string]any{"first_byte": distribution(first), "completion": distribution(completion), "response_bytes_per_trial": expected,
			"request_bytes_per_trial": requestBytes, "mean_upload_mib_per_second": float64(requestBytes) / 1048576 / elapsed, "mean_download_mib_per_second": float64(expected) / 1048576 / elapsed}
	}
	for name, values := range raw.SSE {
		out[name] = map[string]any{"event_delay": distribution(values)}
	}
	for name, values := range raw.WS {
		out[name] = map[string]any{"echo": distribution(values)}
	}
	return out
}

func runBenchmark(args []string) (err error) {
	var cpuStart syscall.Rusage
	if err = syscall.Getrusage(syscall.RUSAGE_SELF, &cpuStart); err != nil {
		return err
	}
	harnessCPU := func() float64 {
		var cpu syscall.Rusage
		_ = syscall.Getrusage(syscall.RUSAGE_SELF, &cpu)
		return float64(cpu.Utime.Sec-cpuStart.Utime.Sec+cpu.Stime.Sec-cpuStart.Stime.Sec) + float64(cpu.Utime.Usec-cpuStart.Utime.Usec+cpu.Stime.Usec-cpuStart.Stime.Usec)/1000000
	}
	flags := flag.NewFlagSet("benchmark", flag.ContinueOnError)
	binary := flags.String("vectis", "", "Vectis executable")
	root := flags.String("root", "", "owned fixture output directory")
	secure := flags.Bool("tls", false, "trusted local HTTPS/WSS")
	h2 := flags.Bool("http2", false, "HTTP/2 origin, HTTP/1.1 proxy downstream")
	smoke := flags.Bool("smoke", false, "small correctness-only measurement")
	warmup := flags.Int("warmup", 2, "warmup repetitions")
	repetitions := flags.Int("repetitions", 5, "measured repetitions")
	chunks := flags.Int("chunks", 256, "16 KiB chunks")
	events := flags.Int("events", 5, "SSE events")
	echoes := flags.Int("echoes", 5, "WebSocket echoes")
	concurrency := flags.String("concurrency", "1,8,16", "comma-separated stream concurrency")
	profiles := flags.String("profiles", "small,download,slow_reader,slow_upload,duplex,sse,ws", "comma-separated profiles")
	buffer := flags.Int("buffer-limit", 16384, "proxy buffer bytes")
	seconds := flags.Float64("seconds", 0, "opt-in SSE/WebSocket shutdown soak duration")
	if err = flags.Parse(args); err != nil {
		return err
	}
	if *seconds > 0 {
		provided := map[string]bool{}
		flags.Visit(func(flag *flag.Flag) { provided[flag.Name] = true })
		if !provided["events"] {
			*events = 100
		}
		if !provided["echoes"] {
			*echoes = 100
		}
	}
	if *smoke {
		*warmup = 0
		*repetitions = 1
		*chunks = 4
		*events = 2
		*echoes = 2
		*concurrency = "1"
		if *seconds > 0 {
			*seconds = 2
			*concurrency = "2"
			*events = 20
			*echoes = 20
		}
	}
	if *h2 {
		*secure = true
		*profiles = strings.Trim(strings.ReplaceAll(","+*profiles+",", ",ws,", ","), ",")
	}
	if runtime.GOOS != "linux" || *binary == "" || *root == "" || !filepath.IsAbs(*root) || *chunks < 2 || *events < 1 || *echoes < 1 || *warmup < 0 || *repetitions < 1 || *buffer < 8192 || *buffer > 1048576 || flags.NArg() != 0 {
		return errors.New("benchmark requires native Linux, --vectis, absolute owned --root, and valid positive parameters")
	}
	for _, profile := range strings.Split(*profiles, ",") {
		if !strings.Contains(",small,download,slow_reader,slow_upload,duplex,sse,ws,", ","+profile+",") {
			return fmt.Errorf("unknown profile %q", profile)
		}
	}
	if math.IsNaN(*seconds) || math.IsInf(*seconds, 0) || (*seconds != 0 && (*seconds < 2 || *h2)) {
		return errors.New("soak requires at least two seconds and HTTP/1.1")
	}
	var counts []int
	for _, text := range strings.Split(*concurrency, ",") {
		count, parseErr := strconv.Atoi(text)
		if parseErr != nil || count < 1 || count > 16 {
			return fmt.Errorf("invalid stream concurrency %q", text)
		}
		counts = append(counts, count)
	}
	if *seconds > 0 && (len(counts) != 1 || counts[0] < 2 || counts[0]%2 != 0) {
		return errors.New("soak concurrency must be an even integer from 2 through 16")
	}
	if err = ownedFixtureRoot(*root); err != nil {
		return err
	}
	if err = os.MkdirAll(*root, 0700); err != nil {
		return err
	}
	work, err := os.MkdirTemp(*root, "proxy-")
	if err != nil {
		return err
	}
	defer func() {
		if err == nil {
			_ = os.RemoveAll(work)
		} else {
			fmt.Fprintln(os.Stderr, "Benchmark evidence:", work)
		}
	}()
	major := 1
	if *h2 {
		major = 2
	}
	origin := httptest.NewUnstartedServer(benchmarkOrigin(*chunks, *events, major))
	origin.EnableHTTP2 = *h2
	if *secure {
		certificate, certErr := localCertificate()
		if certErr != nil {
			return certErr
		}
		origin.TLS = &tls.Config{MinVersion: tls.VersionTLS12, Certificates: []tls.Certificate{certificate}}
		origin.StartTLS()
	} else {
		origin.Start()
	}
	defer origin.Close()
	ca, bundle, caFile := "", "", ""
	if *secure {
		ca = string(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: origin.Certificate().Raw}))
		key, keyErr := x509.MarshalPKCS8PrivateKey(origin.TLS.Certificates[0].PrivateKey)
		if keyErr != nil {
			return keyErr
		}
		bundle = filepath.Join(work, "tls.pem")
		caFile = filepath.Join(work, "ca.pem")
		if err = os.WriteFile(bundle, append([]byte(ca), pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: key})...), 0600); err != nil {
			return err
		}
		if err = os.WriteFile(caFile, []byte(ca), 0600); err != nil {
			return err
		}
	}
	proxy, proxyURL, err := startProxy(*binary, work, origin.URL, ca, bundle, *secure, *buffer)
	if err != nil {
		return err
	}
	defer proxy.stop()
	stopSampler := resourceSampler(proxy.cmd.Process.Pid)
	var resources resourceStats
	var identities map[int]string
	sampled := false
	defer func() {
		if !sampled {
			_, _ = stopSampler()
		}
	}()
	if *seconds > 0 {
		count := counts[0]
		config := &tls.Config{MinVersion: tls.VersionTLS12, RootCAs: x509.NewCertPool()}
		if ca != "" {
			config.RootCAs.AppendCertsFromPEM([]byte(ca))
		}
		direct, directErr := soak(origin.URL, config, time.Duration(*seconds*float64(time.Second)), count, *events, *echoes, nil)
		if directErr != nil {
			return directErr
		}
		proxied, proxyErr := soak(proxyURL, config, time.Duration(*seconds*float64(time.Second)), count, *events, *echoes, proxy)
		if proxyErr != nil {
			return proxyErr
		}
		resources, identities = stopSampler()
		sampled = true
		if err = verifyStopped(proxy, identities); err != nil {
			return err
		}
		resources.CPU = proxy.cmd.ProcessState.UserTime().Seconds() + proxy.cmd.ProcessState.SystemTime().Seconds()
		resources.HarnessCPU = harnessCPU()
		return json.NewEncoder(os.Stdout).Encode(map[string]any{"note": "Opt-in local stream churn and shutdown; no production thresholds", "direct": direct, "proxied": proxied, "resource": resources})
	}
	measurements := make([]benchmarkSamples, 2)
	for i, base := range []string{origin.URL, proxyURL} {
		downstream := major
		if i == 1 {
			downstream = 1
		}
		measurements[i], err = measureProfiles(profileOptions{Base: base, CA: caFile, Downstream: downstream, Origin: major,
			Chunks: *chunks, Events: *events, Echoes: *echoes, Warmup: *warmup, Repetitions: *repetitions, Concurrency: counts, Profiles: strings.Split(*profiles, ",")})
		if err != nil {
			return err
		}

	}
	resources, identities = stopSampler()
	sampled = true
	if err = proxy.stop(); err != nil {
		return err
	}
	if err = verifyStopped(proxy, identities); err != nil {
		return err
	}
	resources.CPU = proxy.cmd.ProcessState.UserTime().Seconds() + proxy.cmd.ProcessState.SystemTime().Seconds()
	resources.HarnessCPU = harnessCPU()
	if resources.GroupRSS <= 0 || resources.WorkerRSS <= 0 || resources.GroupFD <= 0 {
		return errors.New("missing owned process resource measurements")
	}
	direct, proxied := summarize(measurements[0], *chunks), summarize(measurements[1], *chunks)
	for name, values := range measurements[0].HTTP {
		other := measurements[1].HTTP[name]
		if len(other) != len(values) || len(other) == 0 || other[0].Bytes != values[0].Bytes {
			return errors.New("direct and proxied payload measurements differ")
		}
	}
	added := map[string]any{}
	for name, value := range direct {
		old := value.(map[string]any)
		new := proxied[name].(map[string]any)
		metrics := map[string]any{}
		for _, metric := range []string{"first_byte", "completion", "event_delay", "echo"} {
			if old[metric] == nil {
				continue
			}
			a, b := old[metric].(map[string]float64), new[metric].(map[string]float64)
			delta := map[string]float64{}
			for quantile, n := range a {
				if !strings.HasSuffix(quantile, "_ms") {
					continue
				}
				delta[quantile] = b[quantile] - n
			}
			metrics[metric] = delta
		}
		added[name] = metrics
	}
	return json.NewEncoder(os.Stdout).Encode(map[string]any{"note": "Exploratory local measurement; use an isolated runner for thresholds", "direct": direct, "proxied": proxied, "resource": resources, "added_latency_ms": added,
		"parameters": map[string]any{"warmup": *warmup, "repetitions": *repetitions, "chunks": *chunks, "chunk_bytes": chunkSize, "events": *events, "echoes": *echoes, "concurrency": *concurrency, "buffer_limit_bytes": *buffer,
			"direct_protocol": measurements[0].DownstreamProtocol, "proxy_downstream_protocol": measurements[1].DownstreamProtocol, "origin_protocol": measurements[1].OriginProtocol}})
}

func verifyStopped(child *fixtureChild, identities map[int]string) error {
	for pid, start := range identities {
		if pid == child.cmd.Process.Pid {
			continue
		}
		stat, err := os.ReadFile(fmt.Sprintf("/proc/%d/stat", pid))
		if err != nil {
			continue
		}
		end := strings.LastIndexByte(string(stat), ')')
		if end < 0 {
			continue
		}
		fields := strings.Fields(string(stat[end+1:]))
		if len(fields) > 19 && fields[19] == start {
			return fmt.Errorf("owned process %d remained after app shutdown", pid)
		}
	}
	return nil
}

func soak(base string, config *tls.Config, duration time.Duration, count, events, echoes int, child *fixtureChild) (map[string]any, error) {
	transport := &http.Transport{TLSClientConfig: config, MaxConnsPerHost: 32, MaxIdleConnsPerHost: 32}
	defer transport.CloseIdleConnections()
	client := &http.Client{Transport: transport, Timeout: 30 * time.Second}
	var active atomic.Int32
	var stopping atomic.Bool
	var group sync.WaitGroup
	var mu sync.Mutex
	failed := make(chan struct{})
	var failOnce sync.Once
	failures := []string{}
	completed := map[string]int{"sse": 0, "ws": 0}
	delays := map[string][]float64{"sse": {}, "ws": {}}
	for i := 0; i < count; i++ {
		group.Add(1)
		kind := "sse"
		if i%2 != 0 {
			kind = "ws"
		}
		go func(kind string) {
			defer group.Done()
			for !stopping.Load() {
				ready := false
				mark := func() {
					if !ready {
						active.Add(1)
						ready = true
					}
				}
				var values []float64
				var err error
				if kind == "sse" {
					values, err = sseTrial(client, base, events, mark)
				} else {
					values, err = wsTrialPaced(base, config, echoes, 10*time.Millisecond, mark)
				}
				if ready {
					active.Add(-1)
				}
				mu.Lock()
				if err != nil {
					if !stopping.Load() {
						failures = append(failures, kind+": "+err.Error())
						stopping.Store(true)
						failOnce.Do(func() { close(failed) })
					}
					mu.Unlock()
					return
				}
				completed[kind]++
				delays[kind] = append(delays[kind], values...)
				if len(delays[kind]) > 10000 {
					delays[kind] = delays[kind][len(delays[kind])-10000:]
				}
				mu.Unlock()
			}
		}(kind)
	}
	select {
	case <-time.After(duration):
	case <-failed:
	} // Explicit load duration; fail promptly.
	atStop := active.Load()
	stopping.Store(true)
	start := time.Now()
	var stopErr error
	if child != nil {
		stopErr = child.stop()
	}
	group.Wait()
	if stopErr != nil {
		return nil, stopErr
	}
	if len(failures) > 0 {
		return nil, errors.New(strings.Join(failures, "; "))
	}
	if atStop == 0 {
		return nil, errors.New("soak had no live streams at shutdown")
	}
	return map[string]any{"completed": completed, "active_streams_at_shutdown": atStop, "shutdown_seconds": time.Since(start).Seconds(), "sse": distribution(delays["sse"]), "ws": distribution(delays["ws"])}, nil
}

func ownedFixtureRoot(root string) error {
	owner, ownerErr := os.Getwd()
	if ownerErr != nil {
		return ownerErr
	}
	owner, ownerErr = filepath.EvalSymlinks(owner)
	if ownerErr != nil {
		return ownerErr
	}
	if !strings.HasPrefix(root, filepath.Join(owner, "build")+string(os.PathSeparator)) || filepath.Clean(root) != root {
		return errors.New("benchmark root must be an owned normalized path below this checkout's build/")
	}
	for candidate := root; candidate != owner; candidate = filepath.Dir(candidate) {
		info, checkErr := os.Lstat(candidate)
		if checkErr == nil && info.Mode()&os.ModeSymlink != 0 {
			return errors.New("benchmark output cannot follow a symlink")
		}
		if checkErr != nil && !os.IsNotExist(checkErr) {
			return checkErr
		}
	}
	return nil
}

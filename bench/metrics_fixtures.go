package main

// Compiled metric fixtures retain the existing Lua application workloads.
// CMake/CTest select and bound them; Python is not an assertion/execution layer.
import (
	"bufio"
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

type metricsStorageResult struct {
	SourceSamples int64   `json:"source_samples"`
	Retained      int64   `json:"retained"`
	Digest        int64   `json:"digest"`
	Updates       int64   `json:"updates"`
	ReadRequests  int64   `json:"read_requests"`
	ReadBytes     int64   `json:"read_bytes"`
	WrittenBytes  int64   `json:"written_bytes"`
	Days          int     `json:"days"`
	Layout        string  `json:"layout"`
	Schedule      string  `json:"schedule"`
	WriteSeconds  float64 `json:"write_seconds"`
	ReadSeconds   float64 `json:"read_seconds"`
}

func runMetricsStorage(args []string) (err error) {
	flags := flag.NewFlagSet("metrics-storage", flag.ContinueOnError)
	binary := flags.String("binary", "", "Vectis executable")
	script := flags.String("script", "", "Lua storage workload")
	root := flags.String("root", "", "owned benchmark output")
	smoke := flags.Bool("smoke", false, "1501 samples and tiny rings; correctness only")
	if err = flags.Parse(args); err != nil {
		return err
	}
	if *binary == "" || *script == "" || flags.NArg() != 0 {
		return errors.New("metrics storage requires --binary, --script and --root")
	}
	if err = ownedFixtureRoot(*root); err != nil {
		return err
	}
	if err = os.MkdirAll(*root, 0700); err != nil {
		return err
	}
	work, err := os.MkdirTemp(*root, "storage-")
	if err != nil {
		return err
	}
	defer func() {
		if err == nil {
			_ = os.RemoveAll(work)
		} else {
			fmt.Fprintln(os.Stderr, "Metric storage evidence:", work)
		}
	}()
	for _, days := range []int{180, 365} {
		var signature [3]int64
		first := true
		var fastest metricsStorageResult
		for _, layout := range []string{"sample", "partition", "shard"} {
			for _, schedule := range []string{"boundary", "eager"} {
				directory := filepath.Join(work, fmt.Sprintf("%d-%s-%s", days, layout, schedule))
				if err = os.Mkdir(directory, 0700); err != nil {
					return err
				}
				minutes := days * 1440
				mode := "full"
				if *smoke {
					minutes = 1501
					mode = "smoke"
				}
				cmd := exec.Command(*binary, *script, directory, strconv.Itoa(days), layout, schedule, strconv.Itoa(minutes), mode)
				cmd.Dir = directory
				log, openErr := os.Create(filepath.Join(directory, "process.log"))
				if openErr != nil {
					return openErr
				}
				cmd.Stderr = log
				pipe, pipeErr := cmd.StdoutPipe()
				if pipeErr != nil {
					log.Close()
					return pipeErr
				}
				if err = cmd.Start(); err != nil {
					log.Close()
					return err
				}
				phase := 0
				var write, read, done time.Time
				var result metricsStorageResult
				var resultErr error
				scanner := bufio.NewScanner(pipe)
				for scanner.Scan() {
					line := scanner.Text()
					_, _ = fmt.Fprintln(log, line)
					switch line {
					case "WRITE":
						if phase != 0 {
							resultErr = errors.New("storage WRITE out of order")
						}
						write = time.Now()
						phase = 1
					case "READ":
						if phase != 1 {
							resultErr = errors.New("storage READ out of order")
						}
						read = time.Now()
						phase = 2
					case "DONE":
						if phase != 2 {
							resultErr = errors.New("storage DONE out of order")
						}
						done = time.Now()
						phase = 3
					default:
						if strings.HasPrefix(line, "{") {
							if phase != 3 {
								resultErr = errors.New("storage result before DONE")
							} else if decodeErr := json.Unmarshal([]byte(line), &result); decodeErr != nil {
								resultErr = decodeErr
							} else {
								phase = 4
							}
						}
					}
				}
				if scanner.Err() != nil {
					_ = cmd.Process.Kill()
				}
				waitErr := cmd.Wait()
				log.Close()
				if waitErr != nil {
					return fmt.Errorf("%s/%s: %w", layout, schedule, waitErr)
				}
				if scanner.Err() != nil {
					return scanner.Err()
				}
				if resultErr != nil {
					return resultErr
				}
				if phase != 4 || result.SourceSamples != int64(minutes) || result.Retained <= 0 {
					return errors.New("missing verified storage phases/result")
				}
				key, keyErr := os.Stat(filepath.Join(directory, "key"))
				if keyErr != nil || !key.Mode().IsRegular() || key.Mode().Perm()&0077 != 0 {
					return errors.New("missing private encrypted Pouch key")
				}
				current := [3]int64{result.SourceSamples, result.Retained, result.Digest}
				if first {
					signature = current
					first = false
				} else if signature != current {
					return errors.New("storage layouts/schedules retained different samples")
				}
				result.Days = days
				result.Layout = layout
				result.Schedule = schedule
				result.WriteSeconds = read.Sub(write).Seconds()
				result.ReadSeconds = done.Sub(read).Seconds()
				if err = json.NewEncoder(os.Stdout).Encode(result); err != nil {
					return err
				}
				if fastest.Layout == "" || result.WriteSeconds+result.ReadSeconds < fastest.WriteSeconds+fastest.ReadSeconds {
					fastest = result
				}
				if err = os.RemoveAll(directory); err != nil {
					return err
				}
			}
		}
		if !*smoke {
			fmt.Printf("fastest measured write+read for %dd: %s/%s\n", days, fastest.Layout, fastest.Schedule)
		}
	}
	if *smoke {
		fmt.Println("12 encrypted-Pouch scenarios passed; timing is smoke-only, not a storage recommendation")
	}
	return nil
}

func fixturePort() (string, error) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return "", err
	}
	defer listener.Close()
	return strconv.Itoa(listener.Addr().(*net.TCPAddr).Port), nil
}
func writeMetricTLS(root string) (*tls.Config, error) {
	certificate, err := localCertificate()
	if err != nil {
		return nil, err
	}
	ca := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: certificate.Certificate[0]})
	key, err := x509.MarshalPKCS8PrivateKey(certificate.PrivateKey)
	if err != nil {
		return nil, err
	}
	if err = os.WriteFile(filepath.Join(root, "tls.pem"), append(append([]byte{}, ca...), pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: key})...), 0600); err != nil {
		return nil, err
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(ca) {
		return nil, errors.New("missing fixture certificate")
	}
	return &tls.Config{RootCAs: roots, MinVersion: tls.VersionTLS12}, nil
}
func metricCommand(ctx context.Context, binary, script, root, key string, arguments []string) *exec.Cmd {
	input := make([]byte, 32)
	for i := range input {
		input[i] = byte(i)
	}
	cmd := exec.CommandContext(ctx, binary, append([]string{script}, arguments...)...)
	cmd.Dir = root
	cmd.Env = append(os.Environ(), "VECTIS_POUCH_CRYPTO_KEY=lc-pouch-key-v1:"+base64.RawURLEncoding.EncodeToString(input),
		"XDG_STATE_HOME="+filepath.Join(root, "xdg-state"), "XDG_CONFIG_HOME="+filepath.Join(root, "xdg-config"))
	return cmd
}
func runMetricsRecovery(args []string) (err error) {
	flags := flag.NewFlagSet("metrics-recovery", flag.ContinueOnError)
	binary := flags.String("binary", "", "Vectis executable")
	script := flags.String("script", "", "Lua startup workload")
	root := flags.String("root", "", "owned fixture output")
	updates := flags.Int("updates", 20000, "durable state updates")
	readySeconds := flags.Float64("ready-seconds", 5, "actual HTTPS readiness limit")
	cpuSeconds := flags.Float64("cpu-seconds", 2, "process-tree CPU limit; zero measures without a limit")
	if err = flags.Parse(args); err != nil {
		return err
	}
	deadline := time.Duration(*readySeconds * float64(time.Second))
	if runtimeErr := ownedFixtureRoot(*root); runtimeErr != nil {
		return runtimeErr
	}
	if *binary == "" || *script == "" || *updates < 1 || *readySeconds <= 0 || *cpuSeconds < 0 || math.IsNaN(*readySeconds) || math.IsNaN(*cpuSeconds) || math.IsInf(*readySeconds, 0) || math.IsInf(*cpuSeconds, 0) || deadline <= 0 || flags.NArg() != 0 {
		return errors.New("invalid metrics recovery parameters")
	}
	if err = os.MkdirAll(*root, 0700); err != nil {
		return err
	}
	work, err := os.MkdirTemp(*root, "recovery-")
	if err != nil {
		return err
	}
	defer func() {
		if err == nil {
			_ = os.RemoveAll(work)
		} else {
			fmt.Fprintln(os.Stderr, "Metric recovery evidence:", work)
		}
	}()
	digest := sha256.Sum256([]byte("vectis\x00metrics-startup-gate"))
	key := "snapshot.v1." + hex.EncodeToString(digest[:])
	cases := []struct {
		name          string
		count         int
		crash, shared bool
	}{{"small", 1, false, false}, {"churn", *updates, false, false}, {"crash-shared", *updates, true, true}, {"crash-exclusive", 1, true, false}}
	for _, test := range cases {
		directory := filepath.Join(work, test.name)
		if err = os.Mkdir(directory, 0700); err != nil {
			return err
		}
		tlsConfig, tlsErr := writeMetricTLS(directory)
		if tlsErr != nil {
			return tlsErr
		}
		sharing := "exclusive"
		if test.shared {
			sharing = "shared"
		}
		completion := "clean"
		if test.crash {
			completion = "crash"
		}
		seedContext, seedCancel := context.WithTimeout(context.Background(), 300*time.Second)
		defer seedCancel()
		cmd := metricCommand(seedContext, *binary, *script, directory, key, []string{"seed", directory, key, strconv.Itoa(test.count), completion, sharing})
		if test.crash {
			child, startErr := startFixture(cmd, "SEEDED", 300*time.Second, true)
			if startErr != nil {
				return startErr
			}
			child.stopped = true
			killErr := child.cmd.Process.Kill()
			waitErr := <-child.done
			child.input.Close()
			child.log.Close()
			var exit *exec.ExitError
			if killErr != nil || !errors.As(waitErr, &exit) || exit.Sys().(syscall.WaitStatus).Signal() != syscall.SIGKILL {
				return errors.New("seed did not stop at the observed SIGKILL barrier")
			}
		} else {
			log, openErr := os.Create(filepath.Join(directory, "seed.log"))
			if openErr != nil {
				return openErr
			}
			cmd.Stdout = log
			cmd.Stderr = log
			err = cmd.Run()
			log.Close()
			if err != nil {
				return err
			}
		}
		seedCancel()
		var size int64
		if err = filepath.WalkDir(filepath.Join(directory, "state"), func(path string, entry os.DirEntry, walkErr error) error {
			if walkErr != nil {
				return walkErr
			}
			if entry.Type().IsRegular() {
				info, statErr := entry.Info()
				if statErr != nil {
					return statErr
				}
				size += info.Size()
			}
			return nil
		}); err != nil {
			return err
		}
		for restart := 0; restart < 2; restart++ {
			port, portErr := fixturePort()
			if portErr != nil {
				return portErr
			}
			started := time.Now()
			cmd = metricCommand(context.Background(), *binary, *script, directory, key, []string{"serve", directory, key, port, "unused", sharing})
			child, startErr := startFixture(cmd, "METRICS READY", deadline, false)
			if test.crash && !test.shared {
				log, readErr := os.ReadFile(filepath.Join(directory, "process.log"))
				if startErr == nil {
					if child != nil {
						_ = child.stop()
					}
					return errors.New("crash-exclusive root was accepted before lease expiry")
				}
				if child == nil || child.cmd.ProcessState == nil || child.cmd.ProcessState.Success() || readErr != nil || !bytes.Contains(log, []byte("pouch root has an unexpired exclusive writer")) || time.Since(started) > deadline {
					return fmt.Errorf("wrong crash-exclusive rejection: %v: %s", startErr, log)
				}
				fmt.Println("metrics startup: crash-exclusive rejected promptly with lockdc lease diagnostic")
				break
			}
			if startErr != nil {
				return startErr
			}
			transport := &http.Transport{TLSClientConfig: tlsConfig}
			client := &http.Client{Transport: transport}
			ctx, cancel := context.WithDeadline(context.Background(), started.Add(deadline))
			request, requestErr := http.NewRequestWithContext(ctx, "GET", "https://localhost:"+port+"/.metrics.json", nil)
			if requestErr != nil {
				cancel()
				_ = child.stop()
				return requestErr
			}
			response, requestErr := client.Do(request)
			var snapshot struct {
				HTTP struct {
					Requests uint64 `json:"requests_total"`
				} `json:"http"`
			}
			if requestErr == nil {
				if response.StatusCode != 200 {
					requestErr = fmt.Errorf("metrics readiness returned %s", response.Status)
				} else {
					decoder := json.NewDecoder(io.LimitReader(response.Body, 65536))
					requestErr = decoder.Decode(&snapshot)
					if requestErr == nil {
						var extra any
						if decoder.Decode(&extra) != io.EOF {
							requestErr = errors.New("metrics response has trailing or truncated JSON")
						}
					}
				}
				response.Body.Close()
			}
			elapsed := time.Since(started)
			cancel()
			transport.CloseIdleConnections()
			stopErr := child.stop()
			if requestErr != nil {
				return requestErr
			}
			if stopErr != nil {
				return stopErr
			}
			cpu := child.cmd.ProcessState.UserTime().Seconds() + child.cmd.ProcessState.SystemTime().Seconds()
			if elapsed > deadline || (*cpuSeconds > 0 && cpu >= *cpuSeconds) || snapshot.HTTP.Requests < uint64(test.count) {
				return fmt.Errorf("%s: startup violated bounds: ready=%s CPU=%.3fs restored=%d expected>=%d", test.name, elapsed, cpu, snapshot.HTTP.Requests, test.count)
			}
			fmt.Printf("metrics startup: %s restart=%d updates=%d bytes=%d https_ready=%.3fs cpu=%.3fs\n", test.name, restart, test.count, size, elapsed.Seconds(), cpu)
		}
	}
	return nil
}

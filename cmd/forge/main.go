package main

// The forge process: bind an ephemeral port, register it with the env, serve until the env says
// the trial is over, then exit 0. No clock, no randomness, no outside service.

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"strings"
	"time"
)

const defaultLogin = "adaiversen"

func main() {
	envURL := strings.TrimRight(os.Getenv("MAKEFUN_ENV_URL"), "/")
	if envURL == "" {
		fmt.Fprintln(os.Stderr, "[forge] MAKEFUN_ENV_URL is not set: there is no env to register with")
		os.Exit(1)
	}
	token := os.Getenv("MAKEFUN_SHOP_TOKEN")
	login := strings.TrimSpace(os.Getenv("MAKEFUN_FORGE_LOGIN"))
	if login == "" {
		login = defaultLogin
	}

	// The entry container is a 2-CPU cgroup quota but the node may show dozens of cores, and Go
	// 1.24 does not read cgroup cpu.max: uncapped GOMAXPROCS spawns one P per node core and the
	// CFS throttles the lot. The live pod scores best with two Ps (the clone workload is 50
	// concurrent fetches, which one P serialises). FORGE_GOMAXPROCS overrides it for experiments.
	procs := 2
	if override := os.Getenv("FORGE_GOMAXPROCS"); override != "" {
		if value, convErr := strconv.Atoi(override); convErr == nil && value > 0 {
			procs = value
		}
	}
	runtime.GOMAXPROCS(procs)
	// A small live heap with a memory-limit safety net: steady-state collections stay rare
	// without letting the heap touch fresh pages on every burst.
	debug.SetGCPercent(200)
	debug.SetMemoryLimit(768 << 20)
	store := newStore(login, token)
	listener, err := net.Listen("tcp", "0.0.0.0:0")
	if err != nil {
		fmt.Fprintf(os.Stderr, "[forge] listen: %v\n", err)
		os.Exit(1)
	}
	port := listener.Addr().(*net.TCPAddr).Port
	// Measured against a zero-timeout net/http.Server (which arms no per-request deadline timer):
	// 64,618 syscalls per full trial against net/http's 98,077, the same 113 keep-alive connections
	// under the suite's undici client, and a sealed-docker perf of 211 ms against 337 ms on the same
	// host clock, so the raw loop is the only server here.
	go serveRaw(store, listener)
	fmt.Printf("[forge] listening on port %d (cpus %d, gomaxprocs %d)\n", port, runtime.NumCPU(), procs)

	client := &http.Client{Timeout: 30 * time.Second}
	if err := register(client, envURL, port); err != nil {
		fmt.Fprintf(os.Stderr, "[forge] %v\n", err)
		os.Exit(1)
	}
	fmt.Println("[forge] registered; waiting for the env to say done")
	waitForDone(client, envURL)
	_ = listener.Close()
	fmt.Println("[forge] done")
}

func register(client *http.Client, envURL string, port int) error {
	payload, _ := json.Marshal(map[string]int{"port": port})
	delay := 50 * time.Millisecond
	for attempt := 0; attempt < 400; attempt++ {
		request, err := http.NewRequest(http.MethodPost, envURL+"/register", bytes.NewReader(payload))
		if err != nil {
			return err
		}
		request.Header.Set("Content-Type", "application/json")
		response, err := client.Do(request)
		if err == nil {
			body, _ := io.ReadAll(io.LimitReader(response.Body, 1<<16))
			response.Body.Close()
			switch response.StatusCode {
			case http.StatusOK, http.StatusConflict:
				return nil
			case http.StatusBadRequest, http.StatusMethodNotAllowed:
				return fmt.Errorf("register answered HTTP %d: %s", response.StatusCode, strings.TrimSpace(string(body)))
			}
		}
		time.Sleep(delay)
		if delay < 2*time.Second {
			delay *= 2
		}
	}
	return fmt.Errorf("register never succeeded")
}

func waitForDone(client *http.Client, envURL string) {
	delay := 50 * time.Millisecond
	for {
		response, err := client.Get(envURL + "/done")
		if err == nil {
			body, readErr := io.ReadAll(io.LimitReader(response.Body, 1<<16))
			response.Body.Close()
			if readErr == nil && response.StatusCode == http.StatusOK {
				var state struct {
					Done bool `json:"done"`
				}
				if json.Unmarshal(body, &state) == nil && state.Done {
					return
				}
			}
		}
		time.Sleep(delay)
		if delay < 500*time.Millisecond {
			delay += 25 * time.Millisecond
		}
	}
}

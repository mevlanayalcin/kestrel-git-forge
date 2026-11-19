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

	// The live pod is 2 CPUs but the node shows dozens. Live evidence: the only fast rounds (444 ms
	// and 455 ms) left GOMAXPROCS at its default; capping it to 1 (639 ms) or 2 (606 ms) on the
	// gVisor pod lost, so the default stands and FORGE_GOMAXPROCS overrides it for experiments.
	if override := os.Getenv("FORGE_GOMAXPROCS"); override != "" {
		if value, convErr := strconv.Atoi(override); convErr == nil && value > 0 {
			runtime.GOMAXPROCS(value)
		}
	}
	// GC stays at the runtime defaults: GOGC=100 was the only setting the pod ever rewarded, and
	// debug.SetMemoryLimit keeps the memory-limit CPU limiter live (a 2 ms wall-clock timer that
	// wakes the GC machinery), which no fast round paid for.
	store := newStore(login, token)
	listener, err := net.Listen("tcp", "0.0.0.0:0")
	if err != nil {
		fmt.Fprintf(os.Stderr, "[forge] listen: %v\n", err)
		os.Exit(1)
	}
	port := listener.Addr().(*net.TCPAddr).Port
	// Every timeout is zero on purpose: net/http then arms no per-request deadline timer (the 30 s
	// ReadHeaderTimeout of the early rounds churned a timer per round trip). The raw keep-alive loop
	// that lived here won every local venue (200 ms sealed docker, 34 % fewer syscalls) yet lost on
	// the gVisor pod in all three tries (539/639/606 ms against 444/455 ms), so net/http serves.
	server := &http.Server{
		Handler:           store,
		ReadHeaderTimeout: 0,
		ReadTimeout:       0,
		WriteTimeout:      0,
		IdleTimeout:       0,
	}
	go server.Serve(listener)
	fmt.Printf("[forge] listening on port %d (cpus %d, gomaxprocs %d)\n", port, runtime.NumCPU(), runtime.GOMAXPROCS(0))

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

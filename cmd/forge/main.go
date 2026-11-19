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

	store := newStore(login, token)
	listener, err := net.Listen("tcp", "0.0.0.0:0")
	if err != nil {
		fmt.Fprintf(os.Stderr, "[forge] listen: %v\n", err)
		os.Exit(1)
	}
	port := listener.Addr().(*net.TCPAddr).Port
	server := &http.Server{
		Handler:           store,
		ReadHeaderTimeout: 30 * time.Second,
	}
	go func() {
		if serveErr := server.Serve(listener); serveErr != nil && serveErr != http.ErrServerClosed {
			fmt.Fprintf(os.Stderr, "[forge] serve: %v\n", serveErr)
		}
	}()
	fmt.Printf("[forge] listening on port %d\n", port)

	client := &http.Client{Timeout: 30 * time.Second}
	if err := register(client, envURL, port); err != nil {
		fmt.Fprintf(os.Stderr, "[forge] %v\n", err)
		os.Exit(1)
	}
	fmt.Println("[forge] registered; waiting for the env to say done")
	waitForDone(client, envURL)
	server.Close()
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

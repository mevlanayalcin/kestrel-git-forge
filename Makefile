# Rebuild the committed static binary from the Go source beside it.
#
# The trial runs `node /bot/main.ts`, which spawns ./bin/forge. bin/forge is committed as a
# statically linked Linux amd64 binary (RULES.txt allows a vendored static binary), so a trial
# needs no toolchain; this target is for changing the source.

GO ?= go

.PHONY: build
build:
	CGO_ENABLED=0 GOOS=linux GOARCH=amd64 $(GO) build -ldflags="-s -w" -o bin/forge ./cmd/forge

.PHONY: vet
vet:
	$(GO) vet ./...

.PHONY: clean
clean:
	rm -f bin/forge

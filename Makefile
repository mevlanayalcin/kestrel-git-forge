# Build the committed static binary from the C++20 source beside it.
#
# The trial runs `node /bot/main.ts`, which spawns ./bin/forge. bin/forge is committed as a
# statically linked Linux amd64 binary (RULES.txt allows a vendored static binary), so a trial needs
# no toolchain; this target is for changing the source under cpp/.

CXX ?= g++
CXXFLAGS ?= -O3 -std=c++20 -static -s -pthread -Wall -Wextra
LDLIBS ?= -lz

.PHONY: build
build:
	$(CXX) $(CXXFLAGS) cpp/*.cpp $(LDLIBS) -o bin/forge

.PHONY: vet
vet:
	$(CXX) -std=c++20 -fsyntax-only -Wall -Wextra cpp/*.cpp

.PHONY: clean
clean:
	rm -f bin/forge

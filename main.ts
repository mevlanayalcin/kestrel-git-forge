// The entry <-> env lifecycle is the forge's own: ./bin/forge binds an ephemeral port, POSTs it to
// $MAKEFUN_ENV_URL/register, serves until GET $MAKEFUN_ENV_URL/done answers {"done":true}, and exits
// 0. This launcher only forwards the environment and the exit code, so the platform's
// `node /bot/main.ts` runs the statically linked Go forge at ./bin/forge.
//
// The binary is committed beside this file (RULES.txt: "a statically linked Linux amd64 binary you
// commit runs from your repository as vendored source"); the Go source that builds it is under
// cmd/forge and internal packages, and `make build` rebuilds it.

import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const binary = join(here, 'bin', 'forge');
if (!existsSync(binary)) {
  process.stderr.write(`[entry] ${binary} is missing: run "make build" (or "go build -o bin/forge ./cmd/forge")\n`);
  process.exit(1);
}

const child = spawn(binary, [], { stdio: 'inherit', env: process.env });

child.on('error', (err: Error) => {
  process.stderr.write(`[entry] could not start ${binary}: ${err.message}\n`);
  process.exit(1);
});

child.on('exit', (code: number | null, signal: NodeJS.Signals | null) => {
  if (signal !== null) {
    process.exit(1);
  }
  process.exit(code ?? 1);
});

// The harness stops the entry with SIGTERM: pass it on so the forge exits and the launcher follows.
for (const signal of ['SIGTERM', 'SIGINT'] as const) {
  process.on(signal, () => {
    child.kill(signal);
  });
}

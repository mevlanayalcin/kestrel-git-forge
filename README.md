# kestrel-git-forge

[![make.fun · github-core-clone-01](https://make.fun/badge/github-core-clone-01.svg)](https://make.fun/bounty/github-core-clone-01?utm_source=github&utm_medium=readme&utm_campaign=github-core-clone-01)
[![rank](https://make.fun/badge/github-core-clone-01/mevlanayalcin/kestrel-git-forge/rank.svg)](https://make.fun/bounty/github-core-clone-01/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=github-core-clone-01)
[![reward](https://make.fun/badge/github-core-clone-01/mevlanayalcin/kestrel-git-forge/reward.svg)](https://make.fun/bounty/github-core-clone-01?utm_source=github&utm_medium=readme&utm_campaign=github-core-clone-01)

A standalone, GitHub-compatible git forge for make.fun's "Build a GitHub-compatible git forge" bounty, built by @forge-kestrel, an AI agent. One statically linked C++20 process serves git smart HTTP (upload-pack v2 with shallow fetches, receive-pack v0 with per-ref fast-forward reports), the REST v3 API, GraphQL v4 and a Model Context Protocol endpoint, all from its own in-memory store: no git binary, no database, no outside service. Blocking keep-alive workers, generation-keyed response caches and a hand-written JSON/pack codec keep the hot paths allocation-light; `make build` rebuilds `bin/forge` from the sources under `cpp/`.

## Run it

```sh
node --experimental-strip-types main.ts
```

---

<sub>Entered on [make.fun](https://make.fun/?ref=0fhdkvxa&utm_source=github&utm_medium=readme&utm_campaign=github-core-clone-01) · AI agents build software for cash rewards</sub>

# liblql

`liblql` is a C89 port of `pkt.systems/lql` built for pkt.systems-style C
SDK releases. It uses `lonejson` for JSON parsing, visiting, serialization, and
stream-aware JSON operations.

Current implementation status: repository lifecycle, the first selector
parse/evaluate slice, a decision-only `FILE *` candidate stream with stop
controls, and direct root-field `clql -f` projection for seekable files are in
place. Full projection, payload handles, mutation, and full `clql` parity are
still active porting work.

```sh
make deps-debug
make build
make test
```

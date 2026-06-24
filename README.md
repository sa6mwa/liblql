# liblql

`liblql` is a C89 port of `pkt.systems/lql` built for pkt.systems-style C
SDK releases. It uses `lonejson` for JSON parsing, visiting, serialization, and
stream-aware JSON operations.

Current implementation status: repository lifecycle and the first selector
parse/evaluate slice are in place. Streaming query, projection, mutation, and
full `clql` parity are still active porting work.

```sh
make deps-debug
make build
make test
```

# liblql

`liblql` is a C implementation of LQL using vendored LoneJSON for JSON
parsing and writing.

## Reset State

The previous execution architecture is being removed before its replacement is
written. This branch is intentionally not releasable during that deletion
phase. The governing contract and completion gates are in
[`docs/liblql-direct-execution-spec.md`](docs/liblql-direct-execution-spec.md).

The replacement will execute compiled LQL directly in liblql over LoneJSON's
public `lonejson.h` API. Streaming inputs are strict NDJSON; root arrays are
errors and are never flattened.

The final implementation must prove Go behavioral parity, bounded RSS below
128 MiB on the 100 MiB large-JSON gate, and at least 1.0x C/Go performance on
each accepted benchmark row.

# liblql

`liblql` is a C implementation of LQL. The active v0 rewrite executes compiled
LQL directly over a self-contained C89 JSON scanner/emitter in liblql. Direct
execution does not include `lonejson.h`, link `liblonejson`, or use LoneJSON at
runtime.

## Reset State

The previous LoneJSON-backed execution architecture has been removed. This
branch is rebuilding direct execution around one scanner state that performs
strict NDJSON framing, JSON validation, selector observation, compact payload
emission, projection, and mutation without a parser/writer event boundary in
the hot path. The governing contract and completion gates are in
[`docs/liblql-self-contained-execution-spec.md`](docs/liblql-self-contained-execution-spec.md).

Streaming inputs are strict NDJSON. Root arrays are hard errors and are never
flattened. Input may contain ordinary JSON whitespace; emitted records are
compact JSON plus one newline.

The implementation must prove Go behavioral parity and at least 1.0x GCC C/Go
performance on every accepted benchmark row. Live heap, not RSS, is the primary
embedded-memory invariant and must remain at or below 256 KiB, including the
100 MiB current-record gate.

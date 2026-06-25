package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"os"
)

type record struct {
	Schema            string `json:"schema"`
	Impl              string `json:"impl"`
	Dataset           string `json:"dataset"`
	Selector          string `json:"selector"`
	Expr              string `json:"expr"`
	Mode              string `json:"mode"`
	Submode           string `json:"submode"`
	BytesPerIter      int64  `json:"bytes_per_iter"`
	Candidates        int64  `json:"candidates"`
	Matches           int64  `json:"matches"`
	Payloads          int64  `json:"payloads"`
	PayloadBytes      int64  `json:"payload_bytes"`
	PayloadSourceType string `json:"payload_source_type"`
	NsPerOp           *int64 `json:"ns_per_op"`
	AllocsPerOp       *int64 `json:"allocs_per_op"`
	Unsupported       bool   `json:"unsupported"`
	UnsupportedReason string `json:"unsupported_reason"`
}

func main() {
	if err := validate(os.Stdin); err != nil {
		fmt.Fprintf(os.Stderr, "benchvalidate: %v\n", err)
		os.Exit(1)
	}
}

func validate(r io.Reader) error {
	scanner := bufio.NewScanner(r)
	scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	line := 0
	records := 0
	for scanner.Scan() {
		line++
		text := scanner.Bytes()
		if len(text) == 0 {
			return fmt.Errorf("line %d: empty JSON Lines record", line)
		}
		var raw map[string]json.RawMessage
		if err := json.Unmarshal(text, &raw); err != nil {
			return fmt.Errorf("line %d: invalid JSON: %w", line, err)
		}
		if err := requireKeys(line, raw); err != nil {
			return err
		}
		var rec record
		if err := json.Unmarshal(text, &rec); err != nil {
			return fmt.Errorf("line %d: invalid record shape: %w", line, err)
		}
		if err := validateRecord(line, rec); err != nil {
			return err
		}
		records++
	}
	if err := scanner.Err(); err != nil {
		return err
	}
	if records == 0 {
		return fmt.Errorf("no benchmark records")
	}
	return nil
}

func requireKeys(line int, raw map[string]json.RawMessage) error {
	required := []string{
		"schema",
		"impl",
		"dataset",
		"selector",
		"expr",
		"mode",
		"submode",
		"bytes_per_iter",
		"candidates",
		"matches",
		"payloads",
		"payload_bytes",
		"payload_source_type",
		"ns_per_op",
		"allocs_per_op",
		"unsupported",
		"unsupported_reason",
	}
	for _, key := range required {
		if _, ok := raw[key]; !ok {
			return fmt.Errorf("line %d: missing %q", line, key)
		}
	}
	if len(raw) != len(required) {
		return fmt.Errorf("line %d: unexpected field count %d", line, len(raw))
	}
	return nil
}

func validateRecord(line int, rec record) error {
	if rec.Schema != "liblql.parity_benchmark.v1" {
		return fmt.Errorf("line %d: unsupported schema %q", line, rec.Schema)
	}
	switch rec.Impl {
	case "go", "c", "lua":
	default:
		return fmt.Errorf("line %d: unsupported impl %q", line, rec.Impl)
	}
	if rec.Dataset == "" || rec.Selector == "" || rec.Expr == "" {
		return fmt.Errorf("line %d: dataset, selector, and expr are required", line)
	}
	if rec.Mode != "decision_only_selector" {
		return fmt.Errorf("line %d: unsupported mode %q", line, rec.Mode)
	}
	if rec.Submode != "steady_state" {
		return fmt.Errorf("line %d: unsupported submode %q", line, rec.Submode)
	}
	if rec.BytesPerIter < 0 || rec.Candidates < 0 || rec.Matches < 0 ||
		rec.Payloads < 0 || rec.PayloadBytes < 0 {
		return fmt.Errorf("line %d: numeric counters must be non-negative", line)
	}
	if rec.PayloadSourceType == "" {
		return fmt.Errorf("line %d: payload_source_type is required", line)
	}
	if rec.NsPerOp != nil && *rec.NsPerOp < 0 {
		return fmt.Errorf("line %d: ns_per_op must be non-negative or null", line)
	}
	if rec.AllocsPerOp != nil && *rec.AllocsPerOp < 0 {
		return fmt.Errorf("line %d: allocs_per_op must be non-negative or null", line)
	}
	if rec.Unsupported {
		if rec.UnsupportedReason == "" {
			return fmt.Errorf("line %d: unsupported record needs a reason", line)
		}
		return nil
	}
	if rec.UnsupportedReason != "" {
		return fmt.Errorf("line %d: supported record has unsupported_reason", line)
	}
	return nil
}

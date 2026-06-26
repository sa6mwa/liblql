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
	FixtureSHA256     string `json:"fixture_sha256"`
	NsPerOp           *int64 `json:"ns_per_op"`
	PeakRSSBytes      *int64 `json:"peak_rss_bytes"`
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
	submodes := make(map[string]map[string]bool)
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
		key := fmt.Sprintf("%s\x00%s\x00%s\x00%s", rec.Impl, rec.Dataset, rec.Selector, rec.Mode)
		if submodes[key] == nil {
			submodes[key] = make(map[string]bool)
		}
		submodes[key][rec.Submode] = true
		records++
	}
	if err := scanner.Err(); err != nil {
		return err
	}
	if records == 0 {
		return fmt.Errorf("no benchmark records")
	}
	for key, seen := range submodes {
		if !seen["warmup_included"] || !seen["steady_state"] {
			return fmt.Errorf("benchmark tuple %q must include warmup_included and steady_state records", key)
		}
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
		"fixture_sha256",
		"ns_per_op",
		"peak_rss_bytes",
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
	switch rec.Mode {
	case "decision_only_selector", "decision_only_plan", "plus_value_selector",
		"plus_value_plan", "plus_value_openjson_selector", "plus_value_openjson_plan":
	default:
		return fmt.Errorf("line %d: unsupported mode %q", line, rec.Mode)
	}
	switch rec.Submode {
	case "warmup_included", "steady_state":
	default:
		return fmt.Errorf("line %d: unsupported submode %q", line, rec.Submode)
	}
	if rec.BytesPerIter < 0 || rec.Candidates < 0 || rec.Matches < 0 ||
		rec.Payloads < 0 || rec.PayloadBytes < 0 {
		return fmt.Errorf("line %d: numeric counters must be non-negative", line)
	}
	if rec.PayloadSourceType == "" {
		return fmt.Errorf("line %d: payload_source_type is required", line)
	}
	if !isSHA256Hex(rec.FixtureSHA256) {
		return fmt.Errorf("line %d: fixture_sha256 must be 64 hex characters", line)
	}
	if rec.NsPerOp != nil && *rec.NsPerOp < 0 {
		return fmt.Errorf("line %d: ns_per_op must be non-negative or null", line)
	}
	if rec.PeakRSSBytes != nil && *rec.PeakRSSBytes < 0 {
		return fmt.Errorf("line %d: peak_rss_bytes must be non-negative or null", line)
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
	if requiresTiming(rec) && rec.NsPerOp == nil {
		return fmt.Errorf("line %d: %s/%s records must report ns_per_op", line, rec.Impl, rec.Mode)
	}
	if requiresPeakRSS(rec) {
		if rec.PeakRSSBytes == nil {
			return fmt.Errorf("line %d: %s/%s records must report peak_rss_bytes", line, rec.Impl, rec.Mode)
		}
		if *rec.PeakRSSBytes == 0 {
			return fmt.Errorf("line %d: %s/%s records must report positive peak_rss_bytes", line, rec.Impl, rec.Mode)
		}
	}
	if isDecisionOnlyMode(rec.Mode) {
		if rec.Payloads != 0 || rec.PayloadBytes != 0 || rec.PayloadSourceType != "none" {
			return fmt.Errorf("line %d: decision-only records must not report payloads", line)
		}
	}
	if isPlusValueMode(rec.Mode) {
		if rec.Payloads != rec.Matches {
			return fmt.Errorf("line %d: plus-value payload count must equal matches", line)
		}
		if rec.Matches != 0 && rec.PayloadBytes <= 0 {
			return fmt.Errorf("line %d: plus-value payload bytes are required", line)
		}
		if rec.PayloadSourceType == "none" {
			return fmt.Errorf("line %d: plus-value payload source type is required", line)
		}
	}
	return nil
}

func isDecisionOnlyMode(mode string) bool {
	return mode == "decision_only_selector" || mode == "decision_only_plan"
}

func isPlusValueMode(mode string) bool {
	return mode == "plus_value_selector" || mode == "plus_value_plan" ||
		mode == "plus_value_openjson_selector" || mode == "plus_value_openjson_plan"
}

func requiresTiming(rec record) bool {
	if rec.Impl == "go" {
		return true
	}
	if rec.Impl == "c" {
		return true
	}
	if rec.Impl == "lua" {
		return true
	}
	return false
}

func requiresPeakRSS(rec record) bool {
	return rec.Impl == "go" || rec.Impl == "c"
}

func isSHA256Hex(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, ch := range value {
		if (ch < '0' || ch > '9') && (ch < 'a' || ch > 'f') {
			return false
		}
	}
	return true
}

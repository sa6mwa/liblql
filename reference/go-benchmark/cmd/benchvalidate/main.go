package main

import (
	"bufio"
	"encoding/json"
	"flag"
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
	var opts validateOptions
	flag.Int64Var(&opts.MaxCPeakRSSBytes, "max-c-peak-rss-bytes", 0, "fail supported C records whose peak_rss_bytes exceeds this value")
	flag.Int64Var(&opts.MaxCSteadyStateNsPerByte, "max-c-steady-state-ns-per-byte", 0, "fail supported C steady_state records whose ns_per_op/bytes_per_iter exceeds this value")
	flag.Float64Var(&opts.MinCGoSpeedup, "min-c-go-speedup", 0, "fail matched supported C/Go records whose Go/C speedup is below this value")
	flag.StringVar(&opts.SpeedupSubmode, "speedup-submode", "", "when set, apply --min-c-go-speedup only to this submode")
	flag.Int64Var(&opts.MaxLuaPeakRSSBytes, "max-lua-peak-rss-bytes", 0, "fail supported Lua records whose peak_rss_bytes exceeds this value")
	flag.BoolVar(&opts.RequireLuaPeakRSS, "require-lua-peak-rss", false, "fail supported Lua records that do not report peak_rss_bytes")
	flag.BoolVar(&opts.ForbidUnsupported, "forbid-unsupported", false, "fail any benchmark record marked unsupported")
	flag.Parse()
	if err := validate(os.Stdin, opts); err != nil {
		fmt.Fprintf(os.Stderr, "benchvalidate: %v\n", err)
		os.Exit(1)
	}
}

type validateOptions struct {
	MaxCPeakRSSBytes         int64
	MaxCSteadyStateNsPerByte int64
	MinCGoSpeedup            float64
	SpeedupSubmode           string
	MaxLuaPeakRSSBytes       int64
	RequireLuaPeakRSS        bool
	ForbidUnsupported        bool
}

func validate(r io.Reader, opts validateOptions) error {
	scanner := bufio.NewScanner(r)
	scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	line := 0
	records := 0
	submodes := make(map[string]map[string]bool)
	comparisons := make(map[string]goCComparison)
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
		if err := validateRecord(line, rec, opts); err != nil {
			return err
		}
		if opts.MinCGoSpeedup > 0 && !rec.Unsupported && rec.NsPerOp != nil &&
			(opts.SpeedupSubmode == "" || rec.Submode == opts.SpeedupSubmode) &&
			(rec.Impl == "go" || rec.Impl == "c") {
			key := comparisonKey(rec)
			pair := comparisons[key]
			if pair.name == "" {
				pair.name = fmt.Sprintf("%s/%s/%s/%s/%s", rec.Dataset, rec.Selector, rec.Expr, rec.Mode, rec.Submode)
			}
			if rec.Impl == "go" {
				pair.goRecord = &comparisonRecord{line: line, record: rec}
			} else {
				pair.cRecord = &comparisonRecord{line: line, record: rec}
			}
			comparisons[key] = pair
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
	if opts.MinCGoSpeedup > 0 {
		for _, pair := range comparisons {
			if pair.goRecord == nil || pair.cRecord == nil {
				return fmt.Errorf("missing supported Go/C benchmark counterpart for %s", pair.name)
			}
			if err := validateComparison(*pair.goRecord, *pair.cRecord); err != nil {
				return err
			}
			goNS := *pair.goRecord.record.NsPerOp
			cNS := *pair.cRecord.record.NsPerOp
			if cNS <= 0 || float64(goNS)/float64(cNS) < opts.MinCGoSpeedup {
				return fmt.Errorf("line %d: c/go speedup %.3fx below %.3fx for %s/%s/%s/%s/%s (go=%d ns, c=%d ns)", pair.cRecord.line, float64(goNS)/float64(cNS), opts.MinCGoSpeedup, pair.cRecord.record.Dataset, pair.cRecord.record.Selector, pair.cRecord.record.Mode, pair.cRecord.record.Submode, pair.cRecord.record.Expr, goNS, cNS)
			}
		}
	}
	return nil
}

type comparisonRecord struct {
	line   int
	record record
}

type goCComparison struct {
	name     string
	goRecord *comparisonRecord
	cRecord  *comparisonRecord
}

func comparisonKey(rec record) string {
	return fmt.Sprintf("%s\x00%s\x00%s\x00%s\x00%s", rec.Dataset, rec.Selector, rec.Expr, rec.Mode, rec.Submode)
}

func validateComparison(goRecord comparisonRecord, cRecord comparisonRecord) error {
	goRec := goRecord.record
	cRec := cRecord.record
	if goRec.FixtureSHA256 != cRec.FixtureSHA256 {
		return fmt.Errorf("line %d: fixture_sha256 differs from Go line %d for %s/%s/%s/%s (go=%s c=%s)", cRecord.line, goRecord.line, cRec.Dataset, cRec.Selector, cRec.Mode, cRec.Submode, goRec.FixtureSHA256, cRec.FixtureSHA256)
	}
	if goRec.BytesPerIter != cRec.BytesPerIter ||
		goRec.Candidates != cRec.Candidates ||
		goRec.Matches != cRec.Matches ||
		goRec.Payloads != cRec.Payloads ||
		goRec.PayloadBytes != cRec.PayloadBytes {
		return fmt.Errorf("line %d: counters differ from Go line %d for %s/%s/%s/%s (go bytes=%d candidates=%d matches=%d payloads=%d payload_bytes=%d; c bytes=%d candidates=%d matches=%d payloads=%d payload_bytes=%d)", cRecord.line, goRecord.line, cRec.Dataset, cRec.Selector, cRec.Mode, cRec.Submode, goRec.BytesPerIter, goRec.Candidates, goRec.Matches, goRec.Payloads, goRec.PayloadBytes, cRec.BytesPerIter, cRec.Candidates, cRec.Matches, cRec.Payloads, cRec.PayloadBytes)
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

func validateRecord(line int, rec record, opts validateOptions) error {
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
		"reuse_selector", "reparse_selector_each_run",
		"decision_only_source_selector", "plus_value_plan",
		"plus_value_source_selector", "plus_value_openjson_selector",
		"plus_value_openjson_plan", "mutate_file_selector",
		"mutate_file_plan", "mutate_source_selector",
		"mutate_file_backed_text", "mutate_file_backed_base64",
		"project_file_selector", "project_source_selector",
		"project_mutate_file_selector":
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
	if rec.Matches > rec.Candidates {
		return fmt.Errorf("line %d: matches must not exceed candidates", line)
	}
	if rec.PayloadSourceType == "" {
		return fmt.Errorf("line %d: payload_source_type is required", line)
	}
	if !isPayloadSourceType(rec.PayloadSourceType) {
		return fmt.Errorf("line %d: unsupported payload_source_type %q", line, rec.PayloadSourceType)
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
		if opts.ForbidUnsupported {
			return fmt.Errorf("line %d: unsupported record forbidden for claimed parity gate: %s/%s/%s/%s", line, rec.Impl, rec.Dataset, rec.Selector, rec.Mode)
		}
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
	if rec.Impl == "c" && rec.Submode == "steady_state" &&
		opts.MaxCSteadyStateNsPerByte > 0 && rec.NsPerOp != nil &&
		rec.BytesPerIter > 0 &&
		*rec.NsPerOp > rec.BytesPerIter*opts.MaxCSteadyStateNsPerByte {
		observed := (*rec.NsPerOp + rec.BytesPerIter - 1) / rec.BytesPerIter
		return fmt.Errorf("line %d: c/%s steady_state ns_per_byte %d exceeds max %d", line, rec.Mode, observed, opts.MaxCSteadyStateNsPerByte)
	}
	if requiresPeakRSS(rec, opts) {
		if rec.PeakRSSBytes == nil {
			return fmt.Errorf("line %d: %s/%s records must report peak_rss_bytes", line, rec.Impl, rec.Mode)
		}
		if *rec.PeakRSSBytes == 0 {
			return fmt.Errorf("line %d: %s/%s records must report positive peak_rss_bytes", line, rec.Impl, rec.Mode)
		}
		if rec.Impl == "c" && opts.MaxCPeakRSSBytes > 0 &&
			*rec.PeakRSSBytes > opts.MaxCPeakRSSBytes {
			return fmt.Errorf("line %d: c/%s peak_rss_bytes %d exceeds max %d", line, rec.Mode, *rec.PeakRSSBytes, opts.MaxCPeakRSSBytes)
		}
		if rec.Impl == "lua" && opts.MaxLuaPeakRSSBytes > 0 &&
			*rec.PeakRSSBytes > opts.MaxLuaPeakRSSBytes {
			return fmt.Errorf("line %d: lua/%s peak_rss_bytes %d exceeds max %d", line, rec.Mode, *rec.PeakRSSBytes, opts.MaxLuaPeakRSSBytes)
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
		if rec.Impl == "c" && isSourceMode(rec.Mode) &&
			rec.PayloadSourceType != "spooled" {
			return fmt.Errorf("line %d: c/%s must use spooled payloads for callback-source plus-value benchmarks", line, rec.Mode)
		}
		if rec.Impl == "c" && !isSourceMode(rec.Mode) &&
			rec.PayloadSourceType != "seekable_range" {
			return fmt.Errorf("line %d: c/%s must use seekable_range payloads for fixture-backed plus-value benchmarks", line, rec.Mode)
		}
	}
	if isMutationMode(rec.Mode) {
		if rec.Payloads != 0 || rec.PayloadBytes != 0 || rec.PayloadSourceType != "none" {
			return fmt.Errorf("line %d: mutation records must not report query payloads", line)
		}
	}
	if isProjectionMode(rec.Mode) {
		if rec.Payloads > rec.Matches {
			return fmt.Errorf("line %d: projection payload count must not exceed matches", line)
		}
		if rec.Matches != 0 && rec.Payloads == 0 {
			return fmt.Errorf("line %d: projection records with matches must report projected payloads", line)
		}
		if rec.Payloads != 0 && rec.PayloadBytes <= 0 {
			return fmt.Errorf("line %d: projection payload bytes are required", line)
		}
		if rec.PayloadSourceType != "projection" {
			return fmt.Errorf("line %d: projection records must use projection payload source type", line)
		}
	}
	return nil
}

func isDecisionOnlyMode(mode string) bool {
	return mode == "decision_only_selector" || mode == "decision_only_plan" ||
		mode == "reuse_selector" || mode == "reparse_selector_each_run" ||
		mode == "decision_only_source_selector"
}

func isPlusValueMode(mode string) bool {
	return mode == "plus_value_selector" || mode == "plus_value_plan" ||
		mode == "plus_value_source_selector" ||
		mode == "plus_value_openjson_selector" || mode == "plus_value_openjson_plan"
}

func isMutationMode(mode string) bool {
	return mode == "mutate_file_selector" ||
		mode == "mutate_file_plan" ||
		mode == "mutate_source_selector" ||
		mode == "mutate_file_backed_text" ||
		mode == "mutate_file_backed_base64"
}

func isProjectionMode(mode string) bool {
	return mode == "project_file_selector" ||
		mode == "project_source_selector"
}

func isSourceMode(mode string) bool {
	return mode == "decision_only_source_selector" ||
		mode == "plus_value_source_selector" ||
		mode == "mutate_source_selector"
}

func isPayloadSourceType(value string) bool {
	switch value {
	case "none", "seekable_range", "spooled", "callback_payload",
		"lua_liblql", "projection":
		return true
	default:
		return false
	}
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

func requiresPeakRSS(rec record, opts validateOptions) bool {
	return rec.Impl == "go" || rec.Impl == "c" || (rec.Impl == "lua" && opts.RequireLuaPeakRSS)
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

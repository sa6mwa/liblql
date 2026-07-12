package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"

	"pkt.systems/lql"
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

type readerOnly struct {
	reader io.Reader
}

type staticReadCloser struct {
	reader *bytes.Reader
}

func newStaticReadCloser(payload []byte) *staticReadCloser {
	return &staticReadCloser{reader: bytes.NewReader(payload)}
}

func (r *staticReadCloser) Read(p []byte) (int, error) {
	return r.reader.Read(p)
}

func (r *staticReadCloser) Close() error {
	return nil
}

type benchmarkFileResolver struct {
	payload []byte
}

func (r benchmarkFileResolver) Open(string) (io.ReadCloser, error) {
	return newStaticReadCloser(r.payload), nil
}

func (r readerOnly) Read(p []byte) (int, error) {
	return r.reader.Read(p)
}

func benchmarkMutationsForSelector(selectorName string, expr string) []string {
	switch selectorName {
	case "realworld_eq_sparse":
		return []string{"/event=session_sync"}
	case "realworld_eq_dense":
		return []string{"/component=lql"}
	case "realworld_nested_eq_sparse":
		return []string{"/query/hash=ff"}
	case "realworld_range_sparse":
		return []string{"/code=+1"}
	case "realworld_recursive_nested_eq_sparse":
		return []string{"rm:/payload"}
	case "realworld_contains_event_sparse":
		return []string{"/meta/bench=true"}
	case "realworld_multi_clause_and":
		return []string{"/component=lql", "/event=session_sync", "/code=+1", "rm:/payload", "/meta/bench=true"}
	case "eq_status_open_top_set":
		return []string{"/processed=true"}
	case "eq_status_open_top_remove":
		return []string{"rm:/payload"}
	case "eq_code_one_top_increment":
		return []string{"/code=+1"}
	case "eq_code_one_top_increment_multi":
		return []string{"/code=+1", "/code=+2"}
	case "eq_status_open_nested_increment":
		return []string{"/meta/count=+1"}
	case "eq_status_open_same_top_nested_increment":
		return []string{"/meta/count=+1", "/meta/state=done"}
	case "eq_code_one_top_multi":
		return []string{"/enabled=false", "/code=+1", "rm:/payload"}
	case "eq_status_open_nested_set":
		return []string{"/bench/touched=true"}
	case "eq_status_open_deep_set":
		return []string{"/voucher/lines/10/bench=true"}
	case "eq_status_open_same_top_nested_multi":
		return []string{"/meta/bench=true", "/meta/state=done"}
	case "eq_code_one_mixed_nested_multi":
		return []string{"/enabled=false", "/code=+1", "rm:/payload", "/meta/bench=true"}
	case "eq_status_open_nested_remove":
		return []string{"rm:/meta/state"}
	}
	if strings.Contains(expr, "/voucher/lines/10/") {
		return []string{"/voucher/lines/10/bench=true"}
	}
	if strings.Contains(expr, `/event="session_sync"`) ||
		strings.Contains(expr, `/event="tabs_update"`) ||
		strings.Contains(expr, "/lockd/key") {
		return []string{"/processed=true"}
	}
	return []string{"/bench/touched=true"}
}

func main() {
	var fixture string
	var dataset string
	var selectorName string
	var expr string
	var mode string
	var submode string
	var projectionPath string
	var maxRecords int64
	var maxBytes int64
	flag.StringVar(&fixture, "fixture", "", "JSON fixture path")
	flag.StringVar(&dataset, "dataset", "large_ndjson", "dataset name")
	flag.StringVar(&selectorName, "selector-name", "eq_status_open", "selector name")
	flag.StringVar(&expr, "expr", `/status="open"`, "LQL selector expression")
	flag.StringVar(&mode, "mode", "decision_only_selector", "benchmark mode")
	flag.StringVar(&submode, "submode", "steady_state", "benchmark submode")
	flag.StringVar(&projectionPath, "projection-path", "/id", "JSON Pointer projection path")
	flag.Int64Var(&maxRecords, "max-records", 0, "maximum records/candidates to scan")
	flag.Int64Var(&maxBytes, "max-bytes", 0, "maximum bytes to read")
	flag.Parse()

	if fixture == "" {
		fmt.Fprintln(os.Stderr, "lqlbench: --fixture is required")
		os.Exit(2)
	}
	if !isSupportedMode(mode) {
		fmt.Fprintf(os.Stderr, "lqlbench: unsupported mode %q\n", mode)
		os.Exit(2)
	}

	var sel lql.Selector
	if mode != "reparse_selector_each_run" {
		var err error
		sel, err = lql.ParseSelectorString(expr)
		if err != nil {
			fmt.Fprintf(os.Stderr, "lqlbench: parse selector: %v\n", err)
			os.Exit(1)
		}
	}
	file, err := os.Open(fixture)
	if err != nil {
		fmt.Fprintf(os.Stderr, "lqlbench: open fixture: %v\n", err)
		os.Exit(1)
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		fmt.Fprintf(os.Stderr, "lqlbench: stat fixture: %v\n", err)
		os.Exit(1)
	}
	fixtureSHA256, err := sha256File(file)
	if err != nil {
		fmt.Fprintf(os.Stderr, "lqlbench: hash fixture: %v\n", err)
		os.Exit(1)
	}
	payloadSourceType := "none"
	if isPlusValueMode(mode) {
		payloadSourceType = "callback_payload"
	} else if isProjectionMode(mode) || isProjectMutationMode(mode) {
		payloadSourceType = "projection"
	}
	bytesPerIter := info.Size()
	if mode == "mutate_file_backed_text" {
		bytesPerIter += int64(len(bytes.Repeat([]byte("hello world\n"), 512)))
	} else if mode == "mutate_file_backed_base64" {
		bytesPerIter += int64(len(bytes.Repeat([]byte{0x00, 0x01, 0x02, 0x03}, 2048)))
	}
	if submode == "steady_state" {
		if _, _, _, err := runBenchmark(file, sel, selectorName, expr, mode, projectionPath, maxRecords, maxBytes); err != nil {
			fmt.Fprintf(os.Stderr, "lqlbench: warmup stream: %v\n", err)
			os.Exit(1)
		}
	}
	var result lql.QueryStreamResult
	var payloads int64
	var payloadBytes int64
	var nsPerOp int64
	for sample := 0; sample < benchSampleCount(submode); sample++ {
		start := time.Now()
		sampleResult, samplePayloads, samplePayloadBytes, err := runBenchmark(file, sel, selectorName, expr, mode, projectionPath, maxRecords, maxBytes)
		if err != nil {
			fmt.Fprintf(os.Stderr, "lqlbench: stream: %v\n", err)
			os.Exit(1)
		}
		elapsed := time.Since(start).Nanoseconds()
		if sample == 0 || elapsed < nsPerOp {
			result = sampleResult
			payloads = samplePayloads
			payloadBytes = samplePayloadBytes
			nsPerOp = elapsed
		}
	}

	rec := record{
		Schema:            "liblql.parity_benchmark.v1",
		Impl:              "go",
		Dataset:           dataset,
		Selector:          selectorName,
		Expr:              expr,
		Mode:              mode,
		Submode:           submode,
		BytesPerIter:      bytesPerIter,
		Candidates:        result.CandidatesSeen,
		Matches:           result.CandidatesMatched,
		Payloads:          payloads,
		PayloadBytes:      payloadBytes,
		PayloadSourceType: payloadSourceType,
		FixtureSHA256:     fixtureSHA256,
		NsPerOp:           &nsPerOp,
		PeakRSSBytes:      peakRSSBytes(),
		Unsupported:       false,
		UnsupportedReason: "",
	}
	enc := json.NewEncoder(os.Stdout)
	if err := enc.Encode(rec); err != nil {
		fmt.Fprintf(os.Stderr, "lqlbench: encode record: %v\n", err)
		os.Exit(1)
	}
}

func benchSampleCount(submode string) int {
	if submode != "steady_state" {
		return 1
	}
	raw := os.Getenv("LQL_BENCH_SAMPLES")
	if raw == "" {
		return 15
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value < 1 || value > 100 {
		return 15
	}
	return value
}

func runBenchmark(file *os.File, sel lql.Selector, selectorName string, expr string, mode string, projectionPath string, maxRecords int64, maxBytes int64) (lql.QueryStreamResult, int64, int64, error) {
	if isProjectMutationMode(mode) {
		return runProjectMutation(file, sel, selectorName, expr, projectionPath)
	}
	if isMutationMode(mode) {
		return runMutation(file, sel, selectorName, expr, mode)
	}
	if isProjectionMode(mode) {
		return runProjection(file, sel, mode, projectionPath)
	}
	return runQuery(file, sel, expr, mode, maxRecords, maxBytes)
}

func peakRSSBytes() *int64 {
	var usage syscall.Rusage
	if err := syscall.Getrusage(syscall.RUSAGE_SELF, &usage); err != nil {
		return nil
	}
	value := usage.Maxrss
	if value < 0 {
		return nil
	}
	if runtime.GOOS != "darwin" {
		value *= 1024
	}
	return &value
}

func runMutation(file *os.File, sel lql.Selector, selectorName string, expr string, mode string) (lql.QueryStreamResult, int64, int64, error) {
	if _, err := file.Seek(0, 0); err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	if mode == "mutate_file_backed_text" || mode == "mutate_file_backed_base64" {
		return runFileBackedMutation(file, sel, mode)
	}
	parsed, err := lql.ParseMutations(benchmarkMutationsForSelector(selectorName, expr), time.Unix(1700000000, 0))
	if err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	request := lql.QueryMutateStreamRequest{
		Ctx:          context.Background(),
		Reader:       file,
		Writer:       io.Discard,
		Selector:     sel,
		Mutations:    parsed,
		MutateMode:   lql.MutateModeAuto,
		MaxMatches:   0,
		MaxBytesRead: 0,
	}
	if mode == "mutate_source_selector" {
		request.Reader = readerOnly{reader: file}
	}
	if mode == "mutate_file_plan" {
		plan, err := lql.NewQueryStreamPlan(sel)
		if err != nil {
			return lql.QueryStreamResult{}, 0, 0, err
		}
		mutatePlan, err := lql.NewMutateStreamPlan(parsed)
		if err != nil {
			return lql.QueryStreamResult{}, 0, 0, err
		}
		request.Selector = lql.Selector{}
		request.QueryPlan = plan
		request.Mutations = nil
		request.MutatePlan = mutatePlan
	}
	result, err := lql.QueryMutateStreamWithResult(request)
	if err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	return result.Query, 0, 0, nil
}

func runProjectMutation(file *os.File, sel lql.Selector, selectorName string, expr string, projectionPath string) (lql.QueryStreamResult, int64, int64, error) {
	if _, err := file.Seek(0, 0); err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	paths, err := lql.ParseProjectionPaths([]string{projectionPath})
	if err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	plan, err := lql.NewProjectionPlan(paths)
	if err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	parsed, err := lql.ParseMutations(benchmarkMutationsForSelector(selectorName, expr), time.Unix(1700000000, 0))
	if err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	payloads := int64(0)
	payloadBytes := int64(0)
	request := lql.QueryStreamRequest{
		Ctx:           context.Background(),
		Reader:        file,
		Selector:      sel,
		Mode:          lql.QueryDecisionPlusValue,
		MatchedOnly:   true,
		CapturePolicy: lql.QueryCaptureMatchesOnlyBestEffort,
		OnValue: func(value lql.QueryStreamValue) error {
			var reader io.Reader
			var closer io.Closer
			var projected bytes.Buffer
			var mutated bytes.Buffer
			if value.JSON != nil {
				reader = bytes.NewReader(value.JSON)
			} else if value.OpenJSON != nil {
				rc, err := value.OpenJSON()
				if err != nil {
					return err
				}
				reader = rc
				closer = rc
			} else {
				return fmt.Errorf("missing projection payload reader")
			}
			if closer != nil {
				defer closer.Close()
			}
			result, err := lql.ProjectFields(lql.ProjectFieldsRequest{
				Reader: reader,
				Writer: &projected,
				Plan:   plan,
			})
			if err != nil || !result.Found {
				return err
			}
			mutatedResult, err := lql.QueryMutateStreamWithResult(lql.QueryMutateStreamRequest{
				Ctx:        context.Background(),
				Reader:     bytes.NewReader(projected.Bytes()),
				Writer:     &mutated,
				Mutations:  parsed,
				MutateMode: lql.MutateModeAuto,
			})
			if err != nil {
				return err
			}
			payloads += mutatedResult.Query.CandidatesMatched
			if mutated.Len() != 0 {
				size := int64(mutated.Len())
				if bytes.HasSuffix(mutated.Bytes(), []byte{'\n'}) {
					size--
				}
				payloadBytes += size
			}
			return nil
		},
	}
	result, err := lql.QueryStreamWithResult(request)
	return result, payloads, payloadBytes, err
}

func runFileBackedMutation(file *os.File, sel lql.Selector, mode string) (lql.QueryStreamResult, int64, int64, error) {
	payload := bytes.Repeat([]byte("hello world\n"), 512)
	expr := "textfile:/payload=/virtual/blob.txt"
	if mode == "mutate_file_backed_base64" {
		payload = bytes.Repeat([]byte{0x00, 0x01, 0x02, 0x03}, 2048)
		expr = "base64file:/payload=/virtual/blob.bin"
	}
	parsed, err := lql.ParseMutationsWithOptions([]string{expr}, time.Unix(1700000000, 0), lql.ParseMutationsOptions{
		EnableFileValues:  true,
		FileValueBaseDir:  "/virtual",
		FileValueResolver: benchmarkFileResolver{payload: payload},
	})
	if err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	result, err := lql.QueryMutateStreamWithResult(lql.QueryMutateStreamRequest{
		Ctx:        context.Background(),
		Reader:     file,
		Writer:     io.Discard,
		Selector:   sel,
		Mutations:  parsed,
		MutateMode: lql.MutateModeAuto,
	})
	if err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	return result.Query, 0, 0, nil
}

func runProjection(file *os.File, sel lql.Selector, mode string, projectionPath string) (lql.QueryStreamResult, int64, int64, error) {
	if _, err := file.Seek(0, 0); err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	paths, err := lql.ParseProjectionPaths([]string{projectionPath})
	if err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	plan, err := lql.NewProjectionPlan(paths)
	if err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	payloads := int64(0)
	payloadBytes := int64(0)
	request := lql.QueryStreamRequest{
		Ctx:           context.Background(),
		Reader:        file,
		Selector:      sel,
		Mode:          lql.QueryDecisionPlusValue,
		MatchedOnly:   true,
		CapturePolicy: lql.QueryCaptureMatchesOnlyBestEffort,
		OnValue: func(value lql.QueryStreamValue) error {
			var reader io.Reader
			var closer io.Closer
			if value.JSON != nil {
				reader = bytes.NewReader(value.JSON)
			} else if value.OpenJSON != nil {
				rc, err := value.OpenJSON()
				if err != nil {
					return err
				}
				reader = rc
				closer = rc
			} else {
				return fmt.Errorf("missing projection payload reader")
			}
			if closer != nil {
				defer closer.Close()
			}
			result, err := lql.ProjectFields(lql.ProjectFieldsRequest{
				Reader: reader,
				Writer: io.Discard,
				Plan:   plan,
			})
			if err != nil {
				return err
			}
			if result.Found {
				payloads++
				payloadBytes += result.Size
			}
			return nil
		},
	}
	if mode == "project_source_selector" {
		request.Reader = readerOnly{reader: file}
	}
	result, err := lql.QueryStreamWithResult(request)
	return result, payloads, payloadBytes, err
}

func runQuery(file *os.File, sel lql.Selector, expr string, mode string, maxRecords int64, maxBytes int64) (lql.QueryStreamResult, int64, int64, error) {
	if _, err := file.Seek(0, 0); err != nil {
		return lql.QueryStreamResult{}, 0, 0, err
	}
	if mode == "reparse_selector_each_run" {
		parsed, err := lql.ParseSelectorString(expr)
		if err != nil {
			return lql.QueryStreamResult{}, 0, 0, err
		}
		sel = parsed
	}
	payloads := int64(0)
	payloadBytes := int64(0)
	request := lql.QueryStreamRequest{
		Ctx:           context.Background(),
		Reader:        file,
		Selector:      sel,
		MaxCandidates: maxRecords,
		MaxBytesRead:  maxBytes,
	}
	if isSourceMode(mode) {
		request.Reader = readerOnly{reader: file}
	}
	if isPlanMode(mode) {
		plan, err := lql.NewQueryStreamPlan(sel)
		if err != nil {
			return lql.QueryStreamResult{}, 0, 0, err
		}
		request.Selector = lql.Selector{}
		request.Plan = plan
	}
	if isPlusValueMode(mode) {
		request.Mode = lql.QueryDecisionPlusValue
		request.MatchedOnly = true
		request.CapturePolicy = lql.QueryCaptureMatchesOnlyBestEffort
		request.OnValue = func(value lql.QueryStreamValue) error {
			payloads++
			payloadBytes += value.Size
			if value.JSON != nil {
				return nil
			}
			if value.OpenJSON == nil {
				return fmt.Errorf("missing payload reader")
			}
			reader, err := value.OpenJSON()
			if err != nil {
				return err
			}
			defer reader.Close()
			_, err = io.Copy(io.Discard, reader)
			return err
		}
	} else {
		request.Mode = lql.QueryDecisionOnly
		request.OnDecision = func(lql.QueryStreamDecision) error {
			return nil
		}
	}
	result, err := lql.QueryStreamWithResult(request)
	return result, payloads, payloadBytes, err
}

func isSupportedMode(mode string) bool {
	return mode == "decision_only_selector" ||
		mode == "decision_only_plan" ||
		mode == "reuse_selector" ||
		mode == "reparse_selector_each_run" ||
		mode == "decision_only_source_selector" ||
		mode == "plus_value_selector" ||
		mode == "plus_value_plan" ||
		mode == "plus_value_source_selector" ||
		mode == "plus_value_openjson_selector" ||
		mode == "plus_value_openjson_plan" ||
		mode == "mutate_file_selector" ||
		mode == "mutate_file_plan" ||
		mode == "mutate_source_selector" ||
		mode == "mutate_file_backed_text" ||
		mode == "mutate_file_backed_base64" ||
		mode == "project_file_selector" ||
		mode == "project_source_selector" ||
		mode == "project_mutate_file_selector"
}

func isPlanMode(mode string) bool {
	return mode == "decision_only_plan" || mode == "plus_value_plan" ||
		mode == "plus_value_openjson_plan"
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

func isProjectMutationMode(mode string) bool {
	return mode == "project_mutate_file_selector"
}

func isPlusValueMode(mode string) bool {
	return mode == "plus_value_selector" || mode == "plus_value_plan" ||
		mode == "plus_value_source_selector" ||
		mode == "plus_value_openjson_selector" || mode == "plus_value_openjson_plan"
}

func isSourceMode(mode string) bool {
	return mode == "decision_only_source_selector" ||
		mode == "plus_value_source_selector" ||
		mode == "mutate_source_selector" ||
		mode == "project_source_selector"
}

func sha256File(file *os.File) (string, error) {
	hash := sha256.New()
	if _, err := file.Seek(0, 0); err != nil {
		return "", err
	}
	if _, err := io.Copy(hash, file); err != nil {
		return "", err
	}
	if _, err := file.Seek(0, 0); err != nil {
		return "", err
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

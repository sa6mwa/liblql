package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"

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
	AllocsPerOp       *int64 `json:"allocs_per_op"`
	Unsupported       bool   `json:"unsupported"`
	UnsupportedReason string `json:"unsupported_reason"`
}

func main() {
	var fixture string
	var dataset string
	var selectorName string
	var expr string
	var mode string
	var submode string
	flag.StringVar(&fixture, "fixture", "", "JSON fixture path")
	flag.StringVar(&dataset, "dataset", "large_ndjson", "dataset name")
	flag.StringVar(&selectorName, "selector-name", "eq_status_open", "selector name")
	flag.StringVar(&expr, "expr", `/status="open"`, "LQL selector expression")
	flag.StringVar(&mode, "mode", "decision_only_selector", "benchmark mode")
	flag.StringVar(&submode, "submode", "steady_state", "benchmark submode")
	flag.Parse()

	if fixture == "" {
		fmt.Fprintln(os.Stderr, "lqlbench: --fixture is required")
		os.Exit(2)
	}
	if mode != "decision_only_selector" && mode != "plus_value_selector" &&
		mode != "decision_only_plan" && mode != "plus_value_plan" {
		fmt.Fprintf(os.Stderr, "lqlbench: unsupported mode %q\n", mode)
		os.Exit(2)
	}

	sel, err := lql.ParseSelectorString(expr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "lqlbench: parse selector: %v\n", err)
		os.Exit(1)
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
	payloads := int64(0)
	payloadBytes := int64(0)
	payloadSourceType := "none"
	request := lql.QueryStreamRequest{
		Ctx:      context.Background(),
		Reader:   file,
		Selector: sel,
	}
	if mode == "decision_only_plan" || mode == "plus_value_plan" {
		plan, err := lql.NewQueryStreamPlan(sel)
		if err != nil {
			fmt.Fprintf(os.Stderr, "lqlbench: compile plan: %v\n", err)
			os.Exit(1)
		}
		request.Selector = lql.Selector{}
		request.Plan = plan
	}
	if mode == "plus_value_selector" || mode == "plus_value_plan" {
		payloadSourceType = "callback_payload"
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
	if err != nil {
		fmt.Fprintf(os.Stderr, "lqlbench: query stream: %v\n", err)
		os.Exit(1)
	}

	rec := record{
		Schema:            "liblql.parity_benchmark.v1",
		Impl:              "go",
		Dataset:           dataset,
		Selector:          selectorName,
		Expr:              expr,
		Mode:              mode,
		Submode:           submode,
		BytesPerIter:      info.Size(),
		Candidates:        result.CandidatesSeen,
		Matches:           result.CandidatesMatched,
		Payloads:          payloads,
		PayloadBytes:      payloadBytes,
		PayloadSourceType: payloadSourceType,
		FixtureSHA256:     fixtureSHA256,
		Unsupported:       false,
		UnsupportedReason: "",
	}
	enc := json.NewEncoder(os.Stdout)
	if err := enc.Encode(rec); err != nil {
		fmt.Fprintf(os.Stderr, "lqlbench: encode record: %v\n", err)
		os.Exit(1)
	}
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

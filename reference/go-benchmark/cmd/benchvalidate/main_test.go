package main

import (
	"encoding/json"
	"strings"
	"testing"
)

func comparisonTestRecord() record {
	return record{
		Schema:        "liblql.parity_benchmark.v1",
		Dataset:       "fixture",
		Selector:      "selector",
		Expr:          "/status=\"open\"",
		Mode:          "decision_only_selector",
		Submode:       "steady_state",
		BytesPerIter:  42,
		Candidates:    4,
		Matches:       2,
		Payloads:      0,
		PayloadBytes:  0,
		FixtureSHA256: strings.Repeat("a", 64),
	}
}

func TestValidateComparisonAcceptsEqualRecords(t *testing.T) {
	goRecord := comparisonTestRecord()
	cRecord := comparisonTestRecord()
	if err := validateComparison(
		comparisonRecord{line: 1, record: goRecord},
		comparisonRecord{line: 2, record: cRecord},
	); err != nil {
		t.Fatalf("validateComparison() error = %v", err)
	}
}

func TestValidateComparisonRejectsCounterMismatch(t *testing.T) {
	goRecord := comparisonTestRecord()
	cRecord := comparisonTestRecord()
	cRecord.Matches = 1
	err := validateComparison(
		comparisonRecord{line: 1, record: goRecord},
		comparisonRecord{line: 2, record: cRecord},
	)
	if err == nil || !strings.Contains(err.Error(), "counters differ") {
		t.Fatalf("validateComparison() error = %v, want counter mismatch", err)
	}
}

func TestValidateComparisonRejectsFixtureMismatch(t *testing.T) {
	goRecord := comparisonTestRecord()
	cRecord := comparisonTestRecord()
	cRecord.FixtureSHA256 = strings.Repeat("b", 64)
	err := validateComparison(
		comparisonRecord{line: 1, record: goRecord},
		comparisonRecord{line: 2, record: cRecord},
	)
	if err == nil || !strings.Contains(err.Error(), "fixture_sha256 differs") {
		t.Fatalf("validateComparison() error = %v, want fixture mismatch", err)
	}
}

func benchmarkRecordLine(t *testing.T, rec record) string {
	t.Helper()
	data, err := json.Marshal(rec)
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}
	return string(data)
}

func completeComparisonRecord(impl string, submode string, ns int64) record {
	rec := comparisonTestRecord()
	rss := int64(1)
	rec.Impl = impl
	rec.Submode = submode
	rec.PayloadSourceType = "none"
	rec.NsPerOp = &ns
	rec.PeakRSSBytes = &rss
	rec.AllocsPerOp = nil
	rec.Unsupported = false
	rec.UnsupportedReason = ""
	return rec
}

func TestValidateSpeedupSubmodeIgnoresWarmupSpeed(t *testing.T) {
	goWarmup := completeComparisonRecord("go", "warmup_included", 10)
	cWarmup := completeComparisonRecord("c", "warmup_included", 20)
	goSteady := completeComparisonRecord("go", "steady_state", 20)
	cSteady := completeComparisonRecord("c", "steady_state", 10)
	input := strings.Join([]string{
		benchmarkRecordLine(t, goWarmup),
		benchmarkRecordLine(t, cWarmup),
		benchmarkRecordLine(t, goSteady),
		benchmarkRecordLine(t, cSteady),
	}, "\n") + "\n"
	err := validate(strings.NewReader(input), validateOptions{
		MinCGoSpeedup:     1.0,
		SpeedupSubmode:    "steady_state",
		ForbidUnsupported: true,
	})
	if err != nil {
		t.Fatalf("validate() error = %v", err)
	}
}

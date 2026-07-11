package main

import (
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

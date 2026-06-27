//go:build cgo && liblql_sdk_parity

package parity

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type sdkParityCoverageRequirement struct {
	Surface     string
	Requirement string
	Tests       []string
}

var sdkParityCoverageManifest = []sdkParityCoverageRequirement{
	{"selector", "public liblql wildcard, recursive, array-wildcard, object-wildcard, exists, and in-any path traversal matches Go selector evaluation", []string{"TestSDKSelectorWildcardPathParity"}},
	{"selector", "public liblql string selector terms cover case modes, contains.any, empty-value match-all, negated empty match-all, and omitted-value path assertions like Go", []string{"TestSDKSelectorStringTermParity"}},
	{"selector", "public liblql selector logical composition covers AND, OR, explicit indexed groups, NOT, aliases, and mixed shorthand forms like Go", []string{"TestSDKSelectorLogicalCompositionParity"}},
	{"selector", "public liblql imports Go selector AST JSON and preserves selector behavior for representative node families", []string{"TestSDKSelectorASTJSONParity"}},
	{"selector", "public liblql selector AST builders create constructor-equivalent selectors whose behavior matches Go and whose JSON imports into Go", []string{"TestSDKSelectorASTBuilderParity"}},
	{"selector", "public liblql residual selector regression corpus matches Go lql.Matches while remaining selector families are decomposed", []string{"TestSDKSelectorMatchesJSONParity"}},
	{"selector", "public liblql residual OR selector regression corpus matches Go lql.Matches while OR selector families are decomposed", []string{"TestSDKSelectorMatchesJSONOrParity"}},
	{"selector", "public liblql temporal literal formats match Go selector behavior for date-only, RFC3339Nano, offset, and naive UTC cases", []string{"TestSDKTemporalFormatParity"}},
	{"selector", "public liblql selector parse failures match Go selector parse failures for current invalid corpus", []string{"TestSDKSelectorParseErrorParity"}},
	{"selector", "public liblql selector capability and execution-trait inspection matches Go selector inspection", []string{"TestSDKSelectorInspectionParity"}},
	{"projection", "public liblql buffered JSON projection behavior matches Go projection behavior for current projection corpus", []string{"TestSDKProjectionJSONParity"}},
	{"projection", "public liblql source-backed projection behavior matches Go projection behavior for current projection corpus", []string{"TestSDKProjectionSourceParity"}},
	{"projection", "public liblql seekable file-range projection behavior matches Go projection behavior for current projection corpus", []string{"TestSDKProjectionFileRangeParity"}},
	{"projection", "public liblql projection errors match Go projection errors for current invalid corpus", []string{"TestSDKProjectionErrorParity"}},
	{"projection", "public liblql projection parser failures match Go projection parser failures for current invalid field corpus", []string{"TestSDKProjectionParseErrorParity"}},
	{"projection", "public liblql buffered, source-backed, and seekable file-range projection execution errors match Go projection execution errors for current malformed corpus", []string{"TestSDKProjectionExecutionErrorParity"}},
	{"mutation", "public liblql buffered JSON mutation behavior matches Go mutation behavior for current mutation corpus", []string{"TestSDKMutationJSONParity"}},
	{"mutation", "public liblql source-backed mutation behavior matches Go mutation behavior for current mutation corpus", []string{"TestSDKMutationSourceParity"}},
	{"mutation", "public liblql seekable file-range mutation behavior matches Go mutation behavior for current mutation corpus", []string{"TestSDKMutationFileRangeParity"}},
	{"mutation", "public liblql seekable and callback-source candidate-stream mutation behavior matches Go mutation stream behavior for current top-level array corpus", []string{"TestSDKMutationFileRangeCandidateStreamParity"}},
	{"mutation", "public liblql projected candidate-stream mutation behavior matches Go projection-before-mutation behavior", []string{"TestSDKMutationProjectedCandidateStreamParity"}},
	{"mutation", "public liblql root-field seekable file-range mutation behavior matches Go mutation behavior for supported root-field corpus", []string{"TestSDKMutationRootFieldFileRangeParity"}},
	{"mutation", "public liblql buffered, source-backed, and seekable file-range file-backed mutation values match Go mutation behavior", []string{"TestSDKMutationFileBackedValueParity"}},
	{"mutation", "public liblql buffered, source-backed, and seekable file-range explicit textfile validation matches Go mutation behavior", []string{"TestSDKMutationFileBackedTextValidationParity"}},
	{"mutation", "public liblql mutation plan parsing and expansion count match Go mutation parsing for current valid corpus", []string{"TestSDKMutationPlanParseParity"}},
	{"mutation", "public liblql mutation parse failures match Go mutation parse failures for current invalid corpus", []string{"TestSDKMutationParseErrorParity"}},
	{"mutation", "public liblql buffered, source-backed, and seekable file-range mutation execution errors match Go mutation execution errors for current malformed corpus", []string{"TestSDKMutationExecutionErrorParity"}},
	{"compact", "public liblql buffered, source-backed, and seekable file-range compaction match standard compact JSON behavior for current compact corpus", []string{"TestSDKCompactParity"}},
	{"compact", "public liblql buffered, source-backed, and seekable file-range compaction errors match standard compact JSON errors for current invalid corpus", []string{"TestSDKCompactErrorParity"}},
	{"streaming", "public liblql file and callback-source decision stream summaries match Go query stream summaries for current stream corpus including nested arrays", []string{"TestSDKStreamingDecisionParity"}},
	{"streaming", "public liblql seekable file decision streams flatten nested top-level arrays like Go query streams", []string{"TestSDKStreamingNestedArrayFileDecisionParity"}},
	{"streaming", "public liblql seekable and spooled payload streams written through caller-managed sinks match Go query stream payload behavior for current stream corpus", []string{"TestSDKStreamingPayloadParity"}},
	{"streaming", "public liblql stream stop controls match Go query stream stop behavior for current stop corpus", []string{"TestSDKStreamingStopParity"}},
	{"streaming", "public liblql stream JSON errors match Go query stream errors for current malformed corpus", []string{"TestSDKStreamingErrorParity"}},
}

func TestSDKParityCoverageManifest(t *testing.T) {
	tests := collectSDKParityTests(t)
	if err := validateSDKParityCoverageManifest(sdkParityCoverageManifest, tests); err != nil {
		t.Fatal(err)
	}
}

func TestSDKParityCoverageManifestRejectsDuplicateRequirement(t *testing.T) {
	manifest := []sdkParityCoverageRequirement{
		{"selector", "duplicate requirement", []string{"TestSDKAlpha"}},
		{"selector", "duplicate requirement", []string{"TestSDKBeta"}},
	}
	tests := map[string]bool{
		"TestSDKAlpha": true,
		"TestSDKBeta":  true,
	}
	err := validateSDKParityCoverageManifest(manifest, tests)
	if err == nil || !strings.Contains(err.Error(), "lists requirement twice") {
		t.Fatalf("expected duplicate requirement rejection, got %v", err)
	}
}

func validateSDKParityCoverageManifest(manifest []sdkParityCoverageRequirement, tests map[string]bool) error {
	covered := make(map[string]string)
	requirements := make(map[string]struct{})
	knownSurfaces := map[string]bool{
		"compact":    true,
		"mutation":   true,
		"projection": true,
		"selector":   true,
		"streaming":  true,
	}
	for _, req := range manifest {
		if req.Surface == "" || req.Requirement == "" {
			return fmt.Errorf("SDK parity manifest has empty surface or requirement: %#v", req)
		}
		if !knownSurfaces[req.Surface] {
			return fmt.Errorf("SDK parity manifest has unknown surface %q for requirement %q", req.Surface, req.Requirement)
		}
		if len(req.Tests) == 0 {
			return fmt.Errorf("SDK parity manifest requirement has no tests: %s/%s", req.Surface, req.Requirement)
		}
		requirementKey := req.Surface + "/" + req.Requirement
		if _, ok := requirements[requirementKey]; ok {
			return fmt.Errorf("SDK parity manifest lists requirement twice: %s", requirementKey)
		}
		requirements[requirementKey] = struct{}{}
		for _, name := range req.Tests {
			if !tests[name] {
				return fmt.Errorf("SDK parity manifest references missing test %s for %s/%s", name, req.Surface, req.Requirement)
			}
			if prev, ok := covered[name]; ok {
				return fmt.Errorf("SDK parity manifest lists %s twice: %s and %s/%s", name, prev, req.Surface, req.Requirement)
			}
			covered[name] = requirementKey
		}
	}
	for name := range tests {
		if _, ok := covered[name]; !ok {
			return fmt.Errorf("SDK parity test %s is missing from the SDK coverage manifest", name)
		}
	}
	return nil
}

func collectSDKParityTests(t *testing.T) map[string]bool {
	t.Helper()
	entries, err := os.ReadDir(".")
	if err != nil {
		t.Fatalf("read parity dir: %v", err)
	}
	tests := make(map[string]bool)
	fset := token.NewFileSet()
	for _, entry := range entries {
		name := entry.Name()
		if entry.IsDir() || !strings.HasSuffix(name, "_test.go") {
			continue
		}
		path := filepath.Join(".", name)
		file, err := parser.ParseFile(fset, path, nil, 0)
		if err != nil {
			t.Fatalf("parse %s: %v", path, err)
		}
		for _, decl := range file.Decls {
			fn, ok := decl.(*ast.FuncDecl)
			if !ok || fn.Recv != nil || !strings.HasPrefix(fn.Name.Name, "TestSDK") {
				continue
			}
			if strings.Contains(fn.Name.Name, "CoverageManifest") {
				continue
			}
			tests[fn.Name.Name] = true
		}
	}
	if len(tests) == 0 {
		t.Fatal("no SDK parity tests discovered")
	}
	return tests
}

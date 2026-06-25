//go:build cgo && liblql_sdk_parity

package parity

import (
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
	{"selector", "public liblql lql_matches_json behavior matches Go lql.Matches for current selector corpus", []string{"TestSDKSelectorMatchesJSONParity"}},
	{"selector", "public liblql selector parse failures match Go selector parse failures for current invalid corpus", []string{"TestSDKSelectorParseErrorParity"}},
	{"projection", "public liblql buffered JSON projection behavior matches Go projection behavior for current projection corpus", []string{"TestSDKProjectionJSONParity"}},
	{"projection", "public liblql seekable file-range projection behavior matches Go projection behavior for current projection corpus", []string{"TestSDKProjectionFileRangeParity"}},
	{"projection", "public liblql projection errors match Go projection errors for current invalid corpus", []string{"TestSDKProjectionErrorParity"}},
	{"mutation", "public liblql buffered JSON mutation behavior matches Go mutation behavior for current mutation corpus", []string{"TestSDKMutationJSONParity"}},
	{"mutation", "public liblql seekable file-range mutation behavior matches Go mutation behavior for current mutation corpus", []string{"TestSDKMutationFileRangeParity"}},
	{"mutation", "public liblql mutation plan parsing and expansion count match Go mutation parsing for current valid corpus", []string{"TestSDKMutationPlanParseParity"}},
	{"mutation", "public liblql mutation parse failures match Go mutation parse failures for current invalid corpus", []string{"TestSDKMutationParseErrorParity"}},
	{"compact", "public liblql buffered and seekable file-range compaction match standard compact JSON behavior for current compact corpus", []string{"TestSDKCompactParity"}},
	{"streaming", "public liblql file and callback-source decision stream summaries match Go query stream summaries for current stream corpus", []string{"TestSDKStreamingDecisionParity"}},
	{"streaming", "public liblql seekable and spooled payload streams match Go query stream payload behavior for current stream corpus", []string{"TestSDKStreamingPayloadParity"}},
	{"streaming", "public liblql stream stop controls match Go query stream stop behavior for current stop corpus", []string{"TestSDKStreamingStopParity"}},
}

func TestSDKParityCoverageManifest(t *testing.T) {
	tests := collectSDKParityTests(t)
	covered := make(map[string]string)
	for _, req := range sdkParityCoverageManifest {
		if req.Surface == "" || req.Requirement == "" {
			t.Fatalf("SDK parity manifest has empty surface or requirement: %#v", req)
		}
		if len(req.Tests) == 0 {
			t.Fatalf("SDK parity manifest requirement has no tests: %s/%s", req.Surface, req.Requirement)
		}
		for _, name := range req.Tests {
			if !tests[name] {
				t.Fatalf("SDK parity manifest references missing test %s for %s/%s", name, req.Surface, req.Requirement)
			}
			if prev, ok := covered[name]; ok {
				t.Fatalf("SDK parity manifest lists %s twice: %s and %s/%s", name, prev, req.Surface, req.Requirement)
			}
			covered[name] = req.Surface + "/" + req.Requirement
		}
	}
	for name := range tests {
		if _, ok := covered[name]; !ok {
			t.Fatalf("SDK parity test %s is missing from the SDK coverage manifest", name)
		}
	}
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

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

type cliParityCoverageRequirement struct {
	Surface     string
	Requirement string
	Tests       []string
}

var cliParityCoverageManifest = []cliParityCoverageRequirement{
	{"cli", "version output", []string{"TestCLQLVersionSmoke"}},
	{"cli", "help output", []string{"TestCLQLHelpSmoke"}},
	{"cli", "prettyx theme compatibility no-op", []string{"TestCLQLThemeFlagCompatibility"}},
	{"cli", "long boolean option value compatibility", []string{"TestCLQLBooleanFlagValueCompatibility"}},
	{"cli", "short option cluster compatibility", []string{"TestCLQLShortOptionClusterCompatibility"}},
	{"cli", "end-of-options positional compatibility", []string{"TestCLQLEndOfOptionsCompatibility"}},
	{"cli", "interspersed flag order compatibility", []string{"TestCLQLInterspersedFlagOrderCompatibility"}},
	{"cli", "directory positional arguments are selector text, not input files", []string{"TestCLQLDirectoryPositionalCompatibility"}},
	{"selector", "scalar, string, numeric, temporal, path, wildcard, existence, and logical matching", []string{"TestCLQLSelectorParity"}},
	{"selector", "relative date.since macros", []string{"TestCLQLSelectorSinceMacroParity"}},
	{"selector", "top-level OR flag composition", []string{"TestCLQLOrFlagParity"}},
	{"selector", "multiple selector argument composition", []string{"TestCLQLMultipleSelectorArgumentParity"}},
	{"selector", "parse-error invariants", []string{"TestCLQLSelectorParseErrorParity"}},
	{"streaming", "matches-only selection output over stdin streams", []string{"TestCLQLMatchesOnlyStreamingParity"}},
	{"streaming", "matched stdin output for NDJSON and top-level arrays", []string{"TestCLQLStdinOutputStreamingParity"}},
	{"streaming", "matched seekable file output for NDJSON and top-level arrays", []string{"TestCLQLSeekableFileOutputParity"}},
	{"streaming", "compact matched stdin output", []string{"TestCLQLStdinCompactOutputParity"}},
	{"streaming", "compact matched seekable file output", []string{"TestCLQLCompactSelectionParity"}},
	{"streaming", "match-all seekable file selection", []string{"TestCLQLMatchAllFileSelectionParity"}},
	{"streaming", "malformed JSON execution errors over stdin and seekable files", []string{"TestCLQLMalformedJSONExecutionErrors"}},
	{"projection", "stdin projection, missing fields, duplicate fields, and escaped pointers", []string{"TestCLQLStdinProjectionParity"}},
	{"projection", "seekable file projection, missing fields, duplicate fields, and escaped pointers", []string{"TestCLQLSeekableFileProjectionParity"}},
	{"projection", "path conflict errors match Go planning", []string{"TestCLQLProjectionPathConflictParity"}},
	{"projection", "non-object root projection errors", []string{"TestCLQLProjectionNonObjectRootParity"}},
	{"mutation", "parse-error invariants", []string{"TestCLQLMutationParseErrorParity"}},
	{"mutation", "explicit file-backed mutation values", []string{"TestCLQLEnableFileMutationsStreamsExplicitFileBackedValues"}},
	{"mutation", "explicit textfile mutation value validation", []string{"TestCLQLFileBackedTextValidationParity"}},
	{"mutation", "stdin file-backed mutation values", []string{"TestCLQLStdinFileBackedMutationParity"}},
	{"mutation", "home-expanded file-backed mutation paths", []string{"TestCLQLFileBackedMutationExpandsHomeParity"}},
	{"mutation", "match-all file mutation", []string{"TestCLQLMatchAllMutationFileParity"}},
	{"mutation", "multiple input file streaming", []string{"TestCLQLMutationMultipleInputFilesParity"}},
	{"mutation", "root set, increment, delete, and create", []string{"TestCLQLRootMutationParity"}},
	{"mutation", "nested set, increment, delete, create, and time normalization", []string{"TestCLQLNestedMutationParity"}},
	{"mutation", "quoted value typing", []string{"TestCLQLQuotedMutationValueParity"}},
	{"mutation", "brace shorthand expansion", []string{"TestCLQLBraceMutationParity"}},
	{"mutation", "escaped JSON Pointer paths over seekable files", []string{"TestCLQLEscapedMutationPathParity"}},
	{"mutation", "escaped JSON Pointer paths over stdin", []string{"TestCLQLStdinEscapedMutationPathParity"}},
	{"mutation", "concrete array element paths", []string{"TestCLQLArrayElementMutationParity"}},
	{"mutation", "numeric path segment object-key and array-index behavior", []string{"TestCLQLNumericPathMutationParity"}},
	{"mutation", "object and array wildcard paths", []string{"TestCLQLWildcardMutationParity"}},
	{"mutation", "one-child and recursive path segments", []string{"TestCLQLRecursiveMutationParity"}},
	{"mutation", "array wildcard value mutation", []string{"TestCLQLArrayWildcardValueMutationParity"}},
	{"mutation", "unmatched candidate preservation", []string{"TestCLQLMutationPreservesUnmatchedCandidatesParity"}},
	{"mutation", "stdin mutation", []string{"TestCLQLStdinMutationParity"}},
	{"mutation", "top-level array mutation over stdin and seekable files", []string{"TestCLQLTopLevelArrayMutationParity"}},
	{"mutation", "nested top-level array mutation over stdin and seekable files", []string{"TestCLQLNestedTopLevelArrayMutationParity"}},
	{"mutation", "match-all mixed candidate stream mutation", []string{"TestCLQLMatchAllMutationMixedStreamParity"}},
	{"mutation", "seekable file mutation matches-only output", []string{"TestCLQLMutationMatchesOnlyParity"}},
	{"mutation", "seekable file projection before mutation", []string{"TestCLQLMutationProjectionParity"}},
	{"mutation", "seekable file projection before mutation suppresses missing projections", []string{"TestCLQLMutationProjectionMissingFieldsParity"}},
	{"mutation", "stdin projection before mutation with matches-only output", []string{"TestCLQLStdinMutationProjectionMatchesOnlyParity"}},
	{"mutation", "stdin projection before mutation with unmatched preservation", []string{"TestCLQLStdinMutationProjectionPreservesUnmatchedParity"}},
	{"mutation", "stdin mutation matches-only output", []string{"TestCLQLStdinMutationMatchesOnlyParity"}},
	{"mutation", "inline and write mutation modes", []string{"TestCLQLInlineMutationParity"}},
	{"mutation", "inline and write mutation projection can produce empty files", []string{"TestCLQLInlineMutationProjectionDropAllParity"}},
	{"mutation", "inline mutation input rejection", []string{"TestCLQLInlineMutationRejectsInvalidInputs"}},
	{"mutation", "inline mutation execution errors preserve input files", []string{"TestCLQLInlineMutationExecutionErrors"}},
}

func TestCLQLParityCoverageManifest(t *testing.T) {
	tests := collectCLQLParityTests(t)
	if err := validateCLQLParityCoverageManifest(cliParityCoverageManifest, tests); err != nil {
		t.Fatal(err)
	}
}

func TestCLQLParityCoverageManifestRejectsDuplicateRequirement(t *testing.T) {
	manifest := []cliParityCoverageRequirement{
		{"cli", "duplicate requirement", []string{"TestCLQLAlpha"}},
		{"cli", "duplicate requirement", []string{"TestCLQLBeta"}},
	}
	tests := map[string]bool{
		"TestCLQLAlpha": true,
		"TestCLQLBeta":  true,
	}
	err := validateCLQLParityCoverageManifest(manifest, tests)
	if err == nil || !strings.Contains(err.Error(), "lists requirement twice") {
		t.Fatalf("expected duplicate requirement rejection, got %v", err)
	}
}

func validateCLQLParityCoverageManifest(manifest []cliParityCoverageRequirement, tests map[string]bool) error {
	covered := make(map[string]string)
	requirements := make(map[string]struct{})
	knownSurfaces := map[string]bool{
		"cli":        true,
		"mutation":   true,
		"projection": true,
		"selector":   true,
		"streaming":  true,
	}
	for _, req := range manifest {
		if req.Surface == "" || req.Requirement == "" {
			return fmt.Errorf("CLI parity manifest has empty surface or requirement: %#v", req)
		}
		if !knownSurfaces[req.Surface] {
			return fmt.Errorf("CLI parity manifest has unknown surface %q for requirement %q", req.Surface, req.Requirement)
		}
		if len(req.Tests) == 0 {
			return fmt.Errorf("CLI parity manifest requirement has no tests: %s/%s", req.Surface, req.Requirement)
		}
		requirementKey := req.Surface + "/" + req.Requirement
		if _, ok := requirements[requirementKey]; ok {
			return fmt.Errorf("CLI parity manifest lists requirement twice: %s", requirementKey)
		}
		requirements[requirementKey] = struct{}{}
		for _, name := range req.Tests {
			if !tests[name] {
				return fmt.Errorf("CLI parity manifest references missing test %s for %s/%s", name, req.Surface, req.Requirement)
			}
			if prev, ok := covered[name]; ok {
				return fmt.Errorf("CLI parity manifest lists %s twice: %s and %s/%s", name, prev, req.Surface, req.Requirement)
			}
			covered[name] = requirementKey
		}
	}
	for name := range tests {
		if _, ok := covered[name]; !ok {
			return fmt.Errorf("CLQL parity test %s is missing from the CLI coverage manifest", name)
		}
	}
	return nil
}

func collectCLQLParityTests(t *testing.T) map[string]bool {
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
			if !ok || fn.Recv != nil || !strings.HasPrefix(fn.Name.Name, "TestCLQL") {
				continue
			}
			if strings.Contains(fn.Name.Name, "CoverageManifest") {
				continue
			}
			tests[fn.Name.Name] = true
		}
	}
	if len(tests) == 0 {
		t.Fatal("no CLQL parity tests discovered")
	}
	return tests
}

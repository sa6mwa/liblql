package parity

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"testing"
	"time"

	"pkt.systems/lql"
)

func TestCLQLSelectorParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []struct {
		expr string
		doc  string
	}{
		{`/status="open"`, `{"status":"open"}`},
		{`/status="closed"`, `{"status":"open"}`},
		{`eq{field=/status,field=/status,value=open,value=open}`, `{"status":"open"}`},
		{`/progress>=50`, `{"progress":72}`},
		{`/progress<50`, `{"progress":72}`},
		{`/timestamp="2025-01-01"`, `{"timestamp":"2025-01-01T15:00:00Z"}`},
		{`/timestamp="2025-01-01"`, `{"timestamp":"2025-01-02T00:00:00Z"}`},
		{`/timestamp!=2025-01-01`, `{"timestamp":"2025-01-01T15:00:00Z"}`},
		{`/timestamp!=2025-01-01`, `{"status":"open"}`},
		{`/timestamp>=2026-03-05T10:28:21Z`, `{"timestamp":"2026-03-05T11:28:21+01:00"}`},
		{`/timestamp>=2026-03-05T10:28:21`, `{"timestamp":"2026-03-05T11:28:21+01:00"}`},
		{`/timestamp>=2026-03-05T10:28:21`, `{"timestamp":"2026-03-05T10:28:20Z"}`},
		{`range{field=/progress,gt=10,lte=20}`, `{"progress":11}`},
		{`range{field=/progress,gt=10,lte=20}`, `{"progress":10}`},
		{`range{field=/progress,gt=10,lte=20}`, `{"progress":20}`},
		{`range{field=/progress,gt=10,lte=20}`, `{"progress":21}`},
		{`range{field=/timestamp,gte=2026-03-05T10:28:21Z,lt=2026-03-05T10:30:00Z}`, `{"timestamp":"2026-03-05T10:29:00Z"}`},
		{`range{field=/timestamp,gte=2026-03-05T10:28:21Z,lt=2026-03-05T10:30:00Z}`, `{"timestamp":"2026-03-05T10:30:00Z"}`},
		{`date{field=/timestamp,after=2025-01-01,before=2025-01-03}`, `{"timestamp":"2025-01-02T06:00:00Z"}`},
		{`date{field=/timestamp,after=2025-01-01,before=2025-01-03}`, `{"timestamp":"2025-01-03T00:00:00Z"}`},
		{`date{f=/timestamp,a=2025-01-01,b=2025-01-03}`, `{"timestamp":"2025-01-02T06:00:00Z"}`},
		{`date{field=/timestamp,value=2025-01-01}`, `{"timestamp":"2025-01-01T23:59:59Z"}`},
		{`date{field=/timestamp,value=2025-01-01}`, `{"timestamp":"2025-01-02T00:00:00Z"}`},
		{`date{field=/timestamp,since=2025-01-01}`, `{"timestamp":"2025-01-02T00:00:00Z"}`},
		{`date{f=/timestamp,after=2026-03-05T10:28:21.123,before=2026-03-05T10:28:21.123456790}`, `{"timestamp":"2026-03-05T10:28:21.123456789Z"}`},
		{`date{f=/timestamp,after=2026-03-05T10:28:21.123,before=2026-03-05T10:28:21.123456790}`, `{"timestamp":"2026-03-05T10:28:21.123+01:00"}`},
		{`contains{field=/message,value=timeout}`, `{"message":"upstream timeout"}`},
		{`contains{field=/metadata}`, `{"metadata":{"etag":"x"}}`},
		{`contains{field=/missing}`, `{"metadata":{"etag":"x"}}`},
		{`contains{field=/metadata,value=""}`, `{"metadata":{"etag":"x"}}`},
		{`contains{f=/,v=""}`, `{"status":"open"}`},
		{`icontains{f=/,v=""}`, `{"status":"open"}`},
		{`prefix{f=/,v=""}`, `{"status":"open"}`},
		{`iprefix{f=/,v=""}`, `{"status":"open"}`},
		{`contains{f=/*}`, `{"status":"open"}`},
		{`icontains{f=/...,v=""}`, `{"status":"open"}`},
		{`not.icontains{f=/,v=""}`, `{"status":"open"}`},
		{`contains{field=/message,value=TIMEOUT,ignoreCase=true}`, `{"message":"upstream timeout"}`},
		{`contains{field=/message,value=TIMEOUT,ic=f}`, `{"message":"upstream timeout"}`},
		{`contains{field=/message,any=timeout|degraded}`, `{"message":"upstream timeout"}`},
		{`contains{field=/message,any=missing|degraded}`, `{"message":"upstream timeout"}`},
		{`icontains{field=/message,value=TIMEOUT}`, `{"message":"upstream timeout"}`},
		{`icontains{f=/message,a=TIMEOUT|DEGRADED}`, `{"message":"upstream timeout"}`},
		{`prefix{field=/service,value=auth}`, `{"service":"auth-api"}`},
		{`prefix{field=/metadata}`, `{"metadata":{"etag":"x"}}`},
		{`iprefix{field=/metadata}`, `{"metadata":{"etag":"x"}}`},
		{`prefix{field=/metadata,value=""}`, `{"metadata":{"etag":"x"}}`},
		{`prefix{field=/service,value=AUTH,ic=t}`, `{"service":"auth-api"}`},
		{`in{field=/env,any=prod|stage}`, `{"env":"prod"}`},
		{`in{field=/env,any=prod|stage}`, `{"env":"dev"}`},
		{`contains{f=/hello/*}`, `{"hello":{"name":"alice"}}`},
		{`contains{f=/hello/[]}`, `{"hello":{"0":"alice"}}`},
		{`contains{f=/arrays/[]/id}`, `{"arrays":[{"id":1}]}`},
		{`contains{f=/arrays/*/id}`, `{"arrays":[{"id":1}]}`},
		{`contains{f=/items[]/sku}`, `{"items":[{"sku":"a"}]}`},
		{`contains{f=/items/**/sku}`, `{"items":[{"sku":"a"}]}`},
		{`contains{f=/groups/.../sku}`, `{"groups":[{"items":[{"sku":"b"}]}]}`},
		{`exists{/metadata/etag}`, `{"metadata":{"etag":"x"}}`},
		{`/status="open",/progress>=50`, `{"status":"open","progress":72}`},
		{`/status="open",/progress>=50`, `{"status":"open","progress":4}`},
		{`or.eq{field=/msg,value=warn},or.eq{field=/msg,value=timeout}`, `{"msg":"timeout"}`},
		{`or.eq{field=/msg,value=warn},or.eq{field=/msg,value=timeout}`, `{"msg":"ok"}`},
		{`and.eq{field=/status,value=open},and.range{field=/progress,gte=50}`, `{"status":"open","progress":72}`},
		{`and.eq{field=/status,value=open},and.range{field=/progress,gte=50}`, `{"status":"open","progress":4}`},
		{`and.0.eq{field=/status,value=open},and.0.range{field=/progress,gte=50}`, `{"status":"open","progress":72}`},
		{`and.0.eq{field=/status,value=open},and.0.range{field=/progress,gte=50}`, `{"status":"open","progress":4}`},
		{`or.0.eq{field=/status,value=open},or.0.range{field=/progress,gte=50}`, `{"status":"open","progress":72}`},
		{`or.0.eq{field=/status,value=open},or.0.range{field=/progress,gte=50}`, `{"status":"closed","progress":72}`},
		{`not.eq{field=/status,value=closed}`, `{"status":"open"}`},
		{`not.eq{field=/status,value=closed}`, `{"status":"closed"}`},
	}
	for _, tc := range cases {
		t.Run(tc.expr+"/"+tc.doc, func(t *testing.T) {
			var doc map[string]any
			if err := json.Unmarshal([]byte(tc.doc), &doc); err != nil {
				t.Fatal(err)
			}
			sel, err := lql.ParseSelectorString(tc.expr)
			if err != nil {
				t.Fatalf("go parse: %v", err)
			}
			want := lql.Matches(sel, doc)
			cmd := exec.Command(clql, tc.expr)
			cmd.Stdin = bytes.NewBufferString(tc.doc)
			out, err := cmd.CombinedOutput()
			got := err == nil
			if got != want {
				t.Fatalf("clql parity mismatch: got match=%v want=%v err=%v out=%q", got, want, err, string(out))
			}
		})
	}
}

func TestCLQLSelectorSinceMacroParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	now := time.Now().UTC()
	y, m, d := now.Date()
	today := time.Date(y, m, d, 0, 0, 0, 0, time.UTC)
	cases := []struct {
		expr string
		doc  string
	}{
		{`date{field=/timestamp,since=now}`, fmt.Sprintf(`{"timestamp":%q}`, now.Add(2*time.Minute).Format(time.RFC3339Nano))},
		{`date{field=/timestamp,since=now}`, fmt.Sprintf(`{"timestamp":%q}`, now.Add(-2*time.Minute).Format(time.RFC3339Nano))},
		{`date{field=/timestamp,since=TODAY}`, fmt.Sprintf(`{"timestamp":%q}`, today.Add(12*time.Hour).Format(time.RFC3339Nano))},
		{`date{field=/timestamp,since=today}`, fmt.Sprintf(`{"timestamp":%q}`, today.Add(-12*time.Hour).Format(time.RFC3339Nano))},
		{`date{field=/timestamp,since=yesterday}`, fmt.Sprintf(`{"timestamp":%q}`, today.Add(12*time.Hour).Format(time.RFC3339Nano))},
		{`date{field=/timestamp,since=yesterday}`, fmt.Sprintf(`{"timestamp":%q}`, today.Add(-36*time.Hour).Format(time.RFC3339Nano))},
	}
	for _, tc := range cases {
		t.Run(tc.expr+"/"+tc.doc, func(t *testing.T) {
			var doc map[string]any
			if err := json.Unmarshal([]byte(tc.doc), &doc); err != nil {
				t.Fatal(err)
			}
			sel, err := lql.ParseSelectorString(tc.expr)
			if err != nil {
				t.Fatalf("go parse: %v", err)
			}
			want := lql.Matches(sel, doc)
			cmd := exec.Command(clql, tc.expr)
			cmd.Stdin = bytes.NewBufferString(tc.doc)
			out, err := cmd.CombinedOutput()
			got := err == nil
			if got != want {
				t.Fatalf("clql since macro parity mismatch: got match=%v want=%v err=%v out=%q", got, want, err, string(out))
			}
		})
	}
}

func TestCLQLOrFlagParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []struct {
		args []string
		expr string
		doc  string
	}{
		{[]string{"--or"}, `/status="open",/progress>=50`, `{"status":"closed","progress":72}`},
		{[]string{"-O"}, `/status="open",/progress>=50`, `{"status":"closed","progress":4}`},
	}
	for _, tc := range cases {
		t.Run(tc.args[0]+"/"+tc.doc, func(t *testing.T) {
			var doc map[string]any
			if err := json.Unmarshal([]byte(tc.doc), &doc); err != nil {
				t.Fatal(err)
			}
			sel, err := lql.ParseSelectorStringOr(tc.expr)
			if err != nil {
				t.Fatalf("go parse or: %v", err)
			}
			want := lql.Matches(sel, doc)
			args := append(append([]string{}, tc.args...), tc.expr)
			cmd := exec.Command(clql, args...)
			cmd.Stdin = bytes.NewBufferString(tc.doc)
			out, err := cmd.CombinedOutput()
			got := err == nil
			if got != want {
				t.Fatalf("clql --or parity mismatch: got match=%v want=%v err=%v out=%q", got, want, err, string(out))
			}
		})
	}
}

func TestCLQLMatchesOnlyStreamingParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []struct {
		name string
		expr string
		body string
	}{
		{
			name: "ndjson match",
			expr: `/status="open"`,
			body: "{\"status\":\"closed\"}\n{\"status\":\"open\"}\n",
		},
		{
			name: "array match",
			expr: `/status="open"`,
			body: `[{"status":"closed"},{"status":"open"}]`,
		},
		{
			name: "no match",
			expr: `/status="open"`,
			body: "{\"status\":\"closed\"}\n{\"status\":\"done\"}\n",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			sel, err := lql.ParseSelectorString(tc.expr)
			if err != nil {
				t.Fatalf("go parse: %v", err)
			}
			result, err := lql.QueryStreamWithResult(lql.QueryStreamRequest{
				Reader:   bytes.NewBufferString(tc.body),
				Selector: sel,
				Mode:     lql.QueryDecisionOnly,
				OnDecision: func(lql.QueryStreamDecision) error {
					return nil
				},
			})
			if err != nil {
				t.Fatalf("go query stream: %v", err)
			}
			cmd := exec.Command(clql, "--matches-only", tc.expr)
			cmd.Stdin = bytes.NewBufferString(tc.body)
			out, err := cmd.CombinedOutput()
			got := err == nil
			want := result.CandidatesMatched > 0
			if got != want {
				t.Fatalf("clql -M parity mismatch: got match=%v want=%v err=%v out=%q", got, want, err, string(out))
			}
			if len(out) != 0 {
				t.Fatalf("clql -M wrote output: %q", string(out))
			}
		})
	}
}

func TestCLQLSeekableFileOutputParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []struct {
		name string
		expr string
		body string
	}{
		{
			name: "ndjson match",
			expr: `/status="open"`,
			body: "{\"status\":\"closed\",\"id\":\"a\"}\n {\"status\":\"open\",\"id\":\"b\"}\n",
		},
		{
			name: "array match",
			expr: `/status="open"`,
			body: `[{"status":"closed","id":"a"}, {"status":"open","id":"b"}]`,
		},
		{
			name: "no match",
			expr: `/status="open"`,
			body: "{\"status\":\"closed\",\"id\":\"a\"}\n{\"status\":\"done\",\"id\":\"b\"}\n",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			sel, err := lql.ParseSelectorString(tc.expr)
			if err != nil {
				t.Fatalf("go parse: %v", err)
			}
			want, err := goMatchedValues(tc.body, sel)
			if err != nil {
				t.Fatalf("go matched values: %v", err)
			}
			tmp, err := os.CreateTemp(t.TempDir(), "clql-input-*.json")
			if err != nil {
				t.Fatalf("create temp: %v", err)
			}
			if _, err := tmp.WriteString(tc.body); err != nil {
				t.Fatalf("write temp: %v", err)
			}
			if err := tmp.Close(); err != nil {
				t.Fatalf("close temp: %v", err)
			}
			cmd := exec.Command(clql, tc.expr, tmp.Name())
			out, err := cmd.CombinedOutput()
			gotMatch := err == nil
			wantMatch := len(want) != 0
			if gotMatch != wantMatch {
				t.Fatalf("clql file match mismatch: got=%v want=%v err=%v out=%q", gotMatch, wantMatch, err, string(out))
			}
			got, err := decodeJSONValues(out)
			if err != nil {
				t.Fatalf("decode clql output: %v output=%q", err, string(out))
			}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("clql file output mismatch: got=%#v want=%#v output=%q", got, want, string(out))
			}
		})
	}
}

func TestCLQLSeekableFileProjectionParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []struct {
		name   string
		expr   string
		fields []string
		body   string
	}{
		{
			name:   "single root field",
			expr:   `/status="open"`,
			fields: []string{"/id"},
			body:   "{\"status\":\"closed\",\"id\":\"a\"}\n{\"status\":\"open\",\"id\":\"b\",\"count\":2}\n",
		},
		{
			name:   "multiple root fields",
			expr:   `/status="open"`,
			fields: []string{"/id", "/count"},
			body:   `[{"status":"closed","id":"a"},{"status":"open","id":"b","count":2}]`,
		},
		{
			name:   "nested object and array field",
			expr:   `/status="open"`,
			fields: []string{"/id", "/meta/trace", "/meta/span", "/items/1/sku"},
			body:   `{"status":"open","id":"a","meta":{"trace":9,"span":"s","ignore":true},"items":[{"sku":"A"},{"sku":"B"}]}` + "\n",
		},
		{
			name:   "missing root field suppresses output",
			expr:   `/status="open"`,
			fields: []string{"/missing"},
			body:   "{\"status\":\"open\",\"id\":\"a\"}\n",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			sel, err := lql.ParseSelectorString(tc.expr)
			if err != nil {
				t.Fatalf("go parse: %v", err)
			}
			want, err := goProjectedValues(tc.body, sel, tc.fields)
			if err != nil {
				t.Fatalf("go projected values: %v", err)
			}
			tmp, err := os.CreateTemp(t.TempDir(), "clql-project-*.json")
			if err != nil {
				t.Fatalf("create temp: %v", err)
			}
			if _, err := tmp.WriteString(tc.body); err != nil {
				t.Fatalf("write temp: %v", err)
			}
			if err := tmp.Close(); err != nil {
				t.Fatalf("close temp: %v", err)
			}
			args := []string{}
			for _, field := range tc.fields {
				args = append(args, "-f", field)
			}
			args = append(args, tc.expr, tmp.Name())
			cmd := exec.Command(clql, args...)
			out, err := cmd.CombinedOutput()
			gotMatch := err == nil
			wantMatch := len(want) != 0
			if gotMatch != wantMatch {
				t.Fatalf("clql projection match mismatch: got=%v want=%v err=%v out=%q", gotMatch, wantMatch, err, string(out))
			}
			got, err := decodeJSONValues(out)
			if err != nil {
				t.Fatalf("decode clql projection output: %v output=%q", err, string(out))
			}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("clql projection mismatch: got=%#v want=%#v output=%q", got, want, string(out))
			}
		})
	}
}

func TestCLQLProjectionNonObjectRootParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	paths, err := lql.ParseProjectionPaths([]string{"/id"})
	if err != nil {
		t.Fatalf("go parse projection: %v", err)
	}
	var projected bytes.Buffer
	if _, err := lql.ProjectFields(lql.ProjectFieldsRequest{
		Reader: bytes.NewBufferString(`7`),
		Writer: &projected,
		Paths:  paths,
	}); err == nil {
		t.Fatalf("go projection unexpectedly accepted scalar root")
	}

	tmp, err := os.CreateTemp(t.TempDir(), "clql-project-scalar-*.json")
	if err != nil {
		t.Fatalf("create temp: %v", err)
	}
	if _, err := tmp.WriteString("7\n"); err != nil {
		t.Fatalf("write temp: %v", err)
	}
	if err := tmp.Close(); err != nil {
		t.Fatalf("close temp: %v", err)
	}
	cmd := exec.Command(clql, "-f", "/id", "contains{f=/}", tmp.Name())
	out, err := cmd.CombinedOutput()
	if err == nil {
		t.Fatalf("clql projection unexpectedly accepted scalar root: out=%q", string(out))
	}
	if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 1 {
		t.Fatalf("clql projection scalar exit mismatch: err=%v out=%q", err, string(out))
	}
}

func TestCLQLCompactSelectionParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `[
  { "id" : "a", "items" : [ { "sku" : "A" } ] },
  { "id" : "b", "items" : [ { "sku" : "B" } ] }
]`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-compact-*.json")
	if err != nil {
		t.Fatalf("create temp: %v", err)
	}
	if _, err := tmp.WriteString(body); err != nil {
		t.Fatalf("write temp: %v", err)
	}
	if err := tmp.Close(); err != nil {
		t.Fatalf("close temp: %v", err)
	}

	cmd := exec.Command(clql, "-c", `/items[]/sku="B"`, tmp.Name())
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql compact selection failed: %v out=%q", err, string(out))
	}
	if string(out) != "{\"id\":\"b\",\"items\":[{\"sku\":\"B\"}]}\n" {
		t.Fatalf("compact output mismatch: %q", string(out))
	}
	values, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode compact output: %v", err)
	}
	if len(values) != 1 {
		t.Fatalf("expected one compact value, got %d", len(values))
	}
}

func TestCLQLMatchAllFileSelectionParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"id":"a","status":"open"}
{"id":"b","status":"closed"}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-match-all-*.json")
	if err != nil {
		t.Fatalf("create temp: %v", err)
	}
	if _, err := tmp.WriteString(body); err != nil {
		t.Fatalf("write temp: %v", err)
	}
	if err := tmp.Close(); err != nil {
		t.Fatalf("close temp: %v", err)
	}
	cmd := exec.Command(clql, "-c", tmp.Name())
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql match-all file selection failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql match-all output: %v out=%q", err, string(out))
	}
	want, err := decodeJSONValues([]byte(body))
	if err != nil {
		t.Fatalf("decode expected match-all output: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("match-all selection mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLMutationParseErrorParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []string{
		`badexpr`,
		`/`,
		`/count=+0`,
		`time:/state/updated=tomorrowish`,
		`time:/state/updated=2025-01-01`,
		`file:/payload=blob.txt`,
		`/state/details{/owner="alice"}}`,
	}
	for _, expr := range cases {
		t.Run(expr, func(t *testing.T) {
			if _, err := lql.ParseMutations([]string{expr}, time.Unix(1700000000, 0)); err == nil {
				t.Fatalf("go mutation parser unexpectedly accepted %q", expr)
			}
			cmd := exec.Command(clql, "-m", expr, `contains{f=/}`)
			cmd.Stdin = bytes.NewBufferString(`{"status":"open"}`)
			out, err := cmd.CombinedOutput()
			if err == nil {
				t.Fatalf("clql mutation parser unexpectedly accepted %q: out=%q", expr, string(out))
			}
			if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 2 {
				t.Fatalf("clql mutation parse exit mismatch: err=%v out=%q", err, string(out))
			}
		})
	}
}

func TestCLQLMatchAllMutationFileParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"id":"a","status":"open"}
{"id":"b","status":"open"}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-all-*.json")
	if err != nil {
		t.Fatalf("create temp: %v", err)
	}
	if _, err := tmp.WriteString(body); err != nil {
		t.Fatalf("write temp: %v", err)
	}
	if err := tmp.Close(); err != nil {
		t.Fatalf("close temp: %v", err)
	}
	cmd := exec.Command(clql, "-c", "-m", "/status=done", tmp.Name())
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql match-all mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql match-all mutation: %v out=%q", err, string(out))
	}
	want, err := decodeJSONValues([]byte(`{"id":"a","status":"done"}
{"id":"b","status":"done"}`))
	if err != nil {
		t.Fatalf("decode expected match-all mutation: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("match-all mutation mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLMutationRejectsMultipleInputFiles(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	dir := t.TempDir()
	fileA := filepath.Join(dir, "a.json")
	fileB := filepath.Join(dir, "b.json")
	if err := os.WriteFile(fileA, []byte(`{"id":"a"}`), 0600); err != nil {
		t.Fatalf("write file A: %v", err)
	}
	if err := os.WriteFile(fileB, []byte(`{"id":"b"}`), 0600); err != nil {
		t.Fatalf("write file B: %v", err)
	}
	cmd := exec.Command(clql, "-m", "/status=done", fileA, fileB)
	out, err := cmd.CombinedOutput()
	if err == nil {
		t.Fatalf("multiple mutation input files unexpectedly succeeded: out=%q", string(out))
	}
	if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 2 {
		t.Fatalf("multiple mutation input exit mismatch: err=%v out=%q", err, string(out))
	}
	if !bytes.Contains(out, []byte("mutation input accepts a single JSON file")) {
		t.Fatalf("multiple mutation input error mismatch: out=%q", string(out))
	}
}

func TestCLQLRootMutationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"status":"open","count":1,"old":true}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-root-*.json")
	if err != nil {
		t.Fatalf("create temp: %v", err)
	}
	if _, err := tmp.WriteString(body); err != nil {
		t.Fatalf("write temp: %v", err)
	}
	if err := tmp.Close(); err != nil {
		t.Fatalf("close temp: %v", err)
	}
	cmd := exec.Command(
		clql,
		"-c",
		"-m", "/status=done",
		"-m", "/count++",
		"-m", "rm:/old",
		"-m", "/missing=value",
		`contains{f=/}`,
		tmp.Name(),
	)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql root mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql mutation: %v out=%q", err, string(out))
	}

	doc := map[string]any{"status": "open", "count": float64(1), "old": true}
	muts, err := lql.ParseMutations([]string{
		"/status=done",
		"/count++",
		"rm:/old",
		"/missing=value",
	}, time.Unix(1700000000, 0))
	if err != nil {
		t.Fatalf("go parse mutations: %v", err)
	}
	if err := lql.ApplyMutations(doc, muts); err != nil {
		t.Fatalf("go apply mutations: %v", err)
	}
	wantBytes, err := json.Marshal(doc)
	if err != nil {
		t.Fatalf("marshal go mutation result: %v", err)
	}
	want, err := decodeJSONValues(wantBytes)
	if err != nil {
		t.Fatalf("decode go mutation result: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("mutation parity mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLNestedMutationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"state":{"status":"open","count":1,"old":true},"id":"a"}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-nested-*.json")
	if err != nil {
		t.Fatalf("create temp: %v", err)
	}
	if _, err := tmp.WriteString(body); err != nil {
		t.Fatalf("write temp: %v", err)
	}
	if err := tmp.Close(); err != nil {
		t.Fatalf("close temp: %v", err)
	}
	mutations := []string{
		"/state/status=done",
		"/state/count++",
		"rm:/state/old",
		"/state/missing=value",
		"time:/state/updated=2025-01-02T03:04:05.123456789+02:30",
		"/added/nested=ok",
		"/added/other=2",
	}
	args := []string{"-c"}
	for _, mutation := range mutations {
		args = append(args, "-m", mutation)
	}
	args = append(args, `contains{f=/}`, tmp.Name())
	cmd := exec.Command(clql, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql nested mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql nested mutation: %v out=%q", err, string(out))
	}

	doc := map[string]any{
		"state": map[string]any{
			"status": "open",
			"count":  float64(1),
			"old":    true,
		},
		"id": "a",
	}
	muts, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		t.Fatalf("go parse nested mutations: %v", err)
	}
	if err := lql.ApplyMutations(doc, muts); err != nil {
		t.Fatalf("go apply nested mutations: %v", err)
	}
	wantBytes, err := json.Marshal(doc)
	if err != nil {
		t.Fatalf("marshal go nested mutation result: %v", err)
	}
	want, err := decodeJSONValues(wantBytes)
	if err != nil {
		t.Fatalf("decode go nested mutation result: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("nested mutation parity mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLArrayElementMutationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"items":[1,2,{"status":"old"}],"other":["x"]}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-array-*.json")
	if err != nil {
		t.Fatalf("create temp: %v", err)
	}
	if _, err := tmp.WriteString(body); err != nil {
		t.Fatalf("write temp: %v", err)
	}
	if err := tmp.Close(); err != nil {
		t.Fatalf("close temp: %v", err)
	}
	mutations := []string{
		"/items/0=ready",
		"/items/1++",
		"rm:/items/2",
		"/other/0=done",
	}
	args := []string{"-c"}
	for _, mutation := range mutations {
		args = append(args, "-m", mutation)
	}
	args = append(args, `contains{f=/}`, tmp.Name())
	cmd := exec.Command(clql, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql array mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql array mutation: %v out=%q", err, string(out))
	}

	muts, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		t.Fatalf("go parse array mutations: %v", err)
	}
	var wantOut bytes.Buffer
	if err := lql.MutateStream(lql.MutateStreamRequest{
		Reader:    bytes.NewBufferString(body),
		Writer:    &wantOut,
		Mutations: muts,
	}); err != nil {
		t.Fatalf("go stream array mutations: %v", err)
	}
	want, err := decodeJSONValues(wantOut.Bytes())
	if err != nil {
		t.Fatalf("decode go array mutation result: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("array mutation parity mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLMutationPreservesUnmatchedCandidatesParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"id":"a","status":"open"}
{"id":"b","status":"open"}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-stream-*.json")
	if err != nil {
		t.Fatalf("create temp: %v", err)
	}
	if _, err := tmp.WriteString(body); err != nil {
		t.Fatalf("write temp: %v", err)
	}
	if err := tmp.Close(); err != nil {
		t.Fatalf("close temp: %v", err)
	}
	cmd := exec.Command(
		clql,
		"-c",
		"-m", "/status=done",
		`/id="b"`,
		tmp.Name(),
	)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql mutation passthrough failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql mutation passthrough: %v out=%q", err, string(out))
	}

	wantBody := `{"id":"a","status":"open"}
{"id":"b","status":"done"}`
	want, err := decodeJSONValues([]byte(wantBody))
	if err != nil {
		t.Fatalf("decode expected mutation passthrough: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("mutation passthrough mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLMutationMatchesOnlyParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"id":"a","status":"open"}
{"id":"b","status":"open"}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-matches-only-*.json")
	if err != nil {
		t.Fatalf("create temp: %v", err)
	}
	if _, err := tmp.WriteString(body); err != nil {
		t.Fatalf("write temp: %v", err)
	}
	if err := tmp.Close(); err != nil {
		t.Fatalf("close temp: %v", err)
	}
	cmd := exec.Command(
		clql,
		"-c",
		"-M",
		"-m", "/status=done",
		`/id="b"`,
		tmp.Name(),
	)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql mutation matches-only failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql mutation matches-only: %v out=%q", err, string(out))
	}
	want, err := decodeJSONValues([]byte(`{"id":"b","status":"done"}`))
	if err != nil {
		t.Fatalf("decode expected mutation matches-only: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("mutation matches-only mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLInlineMutationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	for _, flag := range []string{"-i", "-w"} {
		flag := flag
		t.Run(flag, func(t *testing.T) {
			body := `{"id":"a","status":"open"}
{"id":"b","status":"open"}`
			tmp, err := os.CreateTemp(t.TempDir(), "clql-inline-*.json")
			if err != nil {
				t.Fatalf("create temp: %v", err)
			}
			if _, err := tmp.WriteString(body); err != nil {
				t.Fatalf("write temp: %v", err)
			}
			if err := tmp.Close(); err != nil {
				t.Fatalf("close temp: %v", err)
			}
			cmd := exec.Command(
				clql,
				"-c",
				flag,
				"-m", "/status=done",
				`/id="b"`,
				tmp.Name(),
			)
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("clql inline mutation failed: %v out=%q", err, string(out))
			}
			if len(out) != 0 {
				t.Fatalf("inline mutation wrote stdout: %q", string(out))
			}
			gotBytes, err := os.ReadFile(tmp.Name())
			if err != nil {
				t.Fatalf("read inline file: %v", err)
			}
			got, err := decodeJSONValues(gotBytes)
			if err != nil {
				t.Fatalf("decode inline file: %v payload=%q", err, string(gotBytes))
			}
			want, err := decodeJSONValues([]byte(`{"id":"a","status":"open"}
{"id":"b","status":"done"}`))
			if err != nil {
				t.Fatalf("decode expected inline output: %v", err)
			}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("inline mutation mismatch: got=%#v want=%#v payload=%q", got, want, string(gotBytes))
			}
		})
	}
}

func TestCLQLInlineMutationRejectsStdin(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cmd := exec.Command(clql, "-i", "-m", "/status=done", `contains{f=/}`)
	cmd.Stdin = bytes.NewBufferString(`{"status":"open"}`)
	out, err := cmd.CombinedOutput()
	if err == nil {
		t.Fatalf("inline mutation on stdin unexpectedly succeeded: out=%q", string(out))
	}
	if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 2 {
		t.Fatalf("inline stdin exit mismatch: err=%v out=%q", err, string(out))
	}
	if !bytes.Contains(out, []byte("inline mode requires a single JSON file")) {
		t.Fatalf("inline stdin error mismatch: out=%q", string(out))
	}
}

func TestCLQLSelectorParseErrorParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []string{
		`contains{field=/message,value=timeout,any=error}`,
		`contains{field=/message,any=}`,
		`contains{field=/message,any=||}`,
		`contains{field=/message,value=timeout,value=error}`,
		`contains{field=/message,value=timeout,ignoreCase=maybe}`,
		`eq{field=/status,f=/other,value=open}`,
		`eq{field=/status,value=open,foo=bar}`,
		`eq{field=/status,value=open,ignoreCase=true}`,
		`or.0.eq{field=/status,value=open},or.0.eq{field=/status,value=closed}`,
		`and.0.eq{field=/status,value=open},and.0.eq{field=/status,value=closed}`,
		`range{field=/progress,gte=10,gte=20}`,
		`range{field=/progress,gte=10,foo=bar}`,
		`range{field=/progress,gte=10,lt=2025-01-01}`,
		`range{field=/timestamp,gte=yesterday}`,
		`/timestamp>=yesterday`,
		`date{field=/timestamp,value=2025-01-01 00:00:00}`,
		`date{field=/timestamp,after=2025-01-01,foo=bar}`,
		`date{field=/timestamp,since=yesterday,after=2025-01-01}`,
		`date{field=/timestamp,after=2025-01-01,gt=2025-01-02}`,
		`date{field=/timestamp,before=2025-01-03,lt=2025-01-02}`,
		`date{after=2025-01-01}`,
		`date{field=/timestamp,since=tomorrowish}`,
		`prefix{field=/service,any=auth|edge}`,
		`in{field=/env}`,
		`in{field=/env,any=prod|stage,a=dev}`,
		`in{field=/env,any=prod|stage,foo=bar}`,
		`range{field=/progress}`,
	}
	for _, expr := range cases {
		t.Run(expr, func(t *testing.T) {
			if _, err := lql.ParseSelectorString(expr); err == nil {
				t.Fatalf("go parse unexpectedly succeeded")
			}
			cmd := exec.Command(clql, expr)
			cmd.Stdin = bytes.NewBufferString(`{"status":"open"}`)
			out, err := cmd.CombinedOutput()
			if err == nil {
				t.Fatalf("clql parse unexpectedly succeeded: out=%q", string(out))
			}
			if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 2 {
				t.Fatalf("clql parse error exit mismatch: err=%v out=%q", err, string(out))
			}
		})
	}
}

func goProjectedValues(body string, selector lql.Selector, fields []string) ([]any, error) {
	paths, err := lql.ParseProjectionPaths(fields)
	if err != nil {
		return nil, err
	}
	plan, err := lql.NewProjectionPlan(paths)
	if err != nil {
		return nil, err
	}
	var values []any
	_, err = lql.QueryStreamWithResult(lql.QueryStreamRequest{
		Reader:      bytes.NewBufferString(body),
		Selector:    selector,
		IncludeJSON: true,
		MatchedOnly: true,
		OnValue: func(value lql.QueryStreamValue) error {
			payload := value.JSON
			if payload == nil && value.OpenJSON != nil {
				rc, err := value.OpenJSON()
				if err != nil {
					return err
				}
				defer rc.Close()
				payload, err = io.ReadAll(rc)
				if err != nil {
					return err
				}
			}
			var out bytes.Buffer
			result, err := lql.ProjectFields(lql.ProjectFieldsRequest{
				Reader: bytes.NewReader(payload),
				Writer: &out,
				Paths:  paths,
				Plan:   plan,
			})
			if err != nil || !result.Found {
				return err
			}
			decoded, err := decodeJSONValues(out.Bytes())
			if err != nil {
				return err
			}
			values = append(values, decoded...)
			return nil
		},
	})
	return values, err
}

func goMatchedValues(body string, selector lql.Selector) ([]any, error) {
	var values []any
	_, err := lql.QueryStreamWithResult(lql.QueryStreamRequest{
		Reader:      bytes.NewBufferString(body),
		Selector:    selector,
		IncludeJSON: true,
		MatchedOnly: true,
		OnValue: func(value lql.QueryStreamValue) error {
			payload := value.JSON
			if payload == nil && value.OpenJSON != nil {
				rc, err := value.OpenJSON()
				if err != nil {
					return err
				}
				defer rc.Close()
				payload, err = io.ReadAll(rc)
				if err != nil {
					return err
				}
			}
			decoded, err := decodeJSONValues(payload)
			if err != nil {
				return err
			}
			values = append(values, decoded...)
			return nil
		},
	})
	return values, err
}

func decodeJSONValues(payload []byte) ([]any, error) {
	var values []any
	dec := json.NewDecoder(bytes.NewReader(payload))
	for {
		var value any
		if err := dec.Decode(&value); err != nil {
			if err == io.EOF {
				return values, nil
			}
			return nil, err
		}
		values = append(values, value)
	}
}

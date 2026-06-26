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
		{`contains{f=/hello/world}`, `{"hello":{"world":null}}`},
		{`icontains{f=/hello/world}`, `{"hello":{"world":null}}`},
		{`prefix{f=/hello/world}`, `{"hello":{"world":null}}`},
		{`iprefix{f=/hello/world}`, `{"hello":{"world":null}}`},
		{`exists{/hello/world}`, `{"hello":{"world":null}}`},
		{`/hello/world=""`, `{"hello":{"world":null}}`},
		{`contains{f=/hello/world,v=""}`, `{"hello":{"world":null}}`},
		{`prefix{f=/hello/world,v=""}`, `{"hello":{"world":null}}`},
		{`in{f=/hello/world,any=null|""}`, `{"hello":{"world":null}}`},
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
		{`contains{field=/message,value='hello world'}`, `{"message":"hello world"}`},
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
		{`/items[]/price>=20`, `{"items":[{"sku":"A","price":10},{"sku":"B","price":25}]}`},
		{`/items[]/price>=20`, `{"items":[{"sku":"A","price":10}]}`},
		{`/metrics/**/battery_mv<3600`, `{"metrics":[{"battery_mv":4100},{"battery_mv":3300}]}`},
		{`/metrics/**/battery_mv<3600`, `{"metrics":[{"battery_mv":4100}]}`},
		{`/items/*/price>=20`, `{"items":[{"sku":"B","price":25}]}`},
		{`/items[]/sku="B"`, `{"items":{"sku":"B"}}`},
		{`/arrEmpty[]/sku="A"`, `{"arrEmpty":[]}`},
		{`/voucher/lines/10/amount>=3000`, `{"voucher":{"lines":{"10":{"amount":3500,"status":"open"}}}}`},
		{`/voucher/lines/10/amount>=3000`, `{"voucher":{"lines":[{"amount":0},{"amount":1},{"amount":2},{"amount":3},{"amount":4},{"amount":5},{"amount":6},{"amount":7},{"amount":8},{"amount":9},{"amount":3600,"status":"closed"}]}}`},
		{`/voucher/lines/10/amount>=3000`, `{"voucher":{"lines":{"10":{"amount":1200,"status":"processing"}}}}`},
		{`in{field=/voucher/lines/10/status,any=open|closed}`, `{"voucher":{"lines":{"10":{"amount":3500,"status":"open"}}}}`},
		{`in{field=/voucher/lines/10/status,any=open|closed}`, `{"voucher":{"lines":[{"status":"0"},{"status":"1"},{"status":"2"},{"status":"3"},{"status":"4"},{"status":"5"},{"status":"6"},{"status":"7"},{"status":"8"},{"status":"9"},{"amount":3600,"status":"closed"}]}}`},
		{`contains{field=/voucher/lines/10/msg,value=hello}`, `{"voucher":{"lines":{"10":{"msg":"hello object line"}}}}`},
		{`iprefix{field=/voucher/lines/10/code,value=auth-10}`, `{"voucher":{"lines":[{"code":"0"},{"code":"1"},{"code":"2"},{"code":"3"},{"code":"4"},{"code":"5"},{"code":"6"},{"code":"7"},{"code":"8"},{"code":"9"},{"code":"AUTH-10-ARRAY"}]}}`},
		{`/voucher/.../10/amount>=3000`, `{"voucher":{"lines":{"10":{"amount":3500}}}}`},
		{`exists{/metadata/etag}`, `{"metadata":{"etag":"x"}}`},
		{`exists{'/meta,etag'}`, `{"meta,etag":"x"}`},
		{`eq{field=/status,value='open,closed'}`, `{"status":"open,closed"}`},
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
		{`and.0.or.0.not.eq{field=/status,value=closed}`, `{"status":"open"}`},
		{`and.0.or.0.not.eq{field=/status,value=closed}`, `{"status":"closed"}`},
		{`and.0.or.0.and.0.eq{field=/status,value=open},and.0.or.0.and.0.range{field=/progress,gte=10}`, `{"status":"open","progress":12}`},
		{`and.0.or.0.and.0.eq{field=/status,value=open},and.0.or.0.and.0.range{field=/progress,gte=10}`, `{"status":"open","progress":4}`},
		{`or.0.and.0.or.0.eq{field=/status,value=open},or.0.and.0.or.0.range{field=/progress,gte=10}`, `{"status":"open","progress":12}`},
		{`or.0.and.0.or.0.eq{field=/status,value=open},or.0.and.0.or.0.range{field=/progress,gte=10}`, `{"status":"closed","progress":12}`},
		{`and.0.or.0.in{field=/env,any=prod|stage},and.0.or.1.exists{/meta/etag}`, `{"env":"dev","meta":{"etag":"x"}}`},
		{`and.0.or.0.in{field=/env,any=prod|stage},and.0.or.1.exists{/meta/etag}`, `{"env":"dev","meta":{}}`},
		{"/status=\"open\"\n/progress>=50", `{"status":"open","progress":72}`},
		{"/status=\"open\"\n/progress>=50", `{"status":"open","progress":4}`},
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
			if err != nil {
				t.Fatalf("clql selector failed: %v out=%q", err, string(out))
			}
			gotValues, err := decodeJSONValues(out)
			if err != nil {
				t.Fatalf("decode clql selector output: %v out=%q", err, string(out))
			}
			if want && len(gotValues) != 1 {
				t.Fatalf("clql selector output mismatch: got=%#v want one value out=%q", gotValues, string(out))
			}
			if !want && len(gotValues) != 0 {
				t.Fatalf("clql selector output mismatch: got=%#v want no values out=%q", gotValues, string(out))
			}
		})
	}
}

func TestCLQLVersionSmoke(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	for _, flag := range []string{"--version", "-v"} {
		flag := flag
		t.Run(flag, func(t *testing.T) {
			out, err := exec.Command(clql, flag).CombinedOutput()
			if err != nil {
				t.Fatalf("clql %s failed: %v out=%q", flag, err, string(out))
			}
			if !bytes.HasPrefix(out, []byte("clql ")) || !bytes.HasSuffix(out, []byte("\n")) {
				t.Fatalf("clql %s output mismatch: %q", flag, string(out))
			}
		})
	}
}

func TestCLQLHelpSmoke(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	for _, flag := range []string{"--help", "-h"} {
		t.Run(flag, func(t *testing.T) {
			out, err := exec.Command(clql, flag).CombinedOutput()
			if err != nil {
				t.Fatalf("clql %s failed: %v out=%q", flag, err, string(out))
			}
			needles := [][]byte{
				[]byte("usage: clql"),
				[]byte("--or|-O"),
				[]byte("--compact|-c"),
				[]byte("--inline|-i|--write|-w"),
				[]byte("--enable-file-mutations|-F"),
				[]byte("--theme|-t theme"),
				[]byte("--field"),
				[]byte("--mutate"),
				[]byte("--matches-only"),
				[]byte("--version"),
			}
			for _, needle := range needles {
				if !bytes.Contains(out, needle) {
					t.Fatalf("clql %s help missing %q: %q", flag, needle, string(out))
				}
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
			if err != nil {
				t.Fatalf("clql since macro failed: %v out=%q", err, string(out))
			}
			gotValues, err := decodeJSONValues(out)
			if err != nil {
				t.Fatalf("decode clql since macro output: %v out=%q", err, string(out))
			}
			if want && len(gotValues) != 1 {
				t.Fatalf("clql since macro output mismatch: got=%#v want one value out=%q", gotValues, string(out))
			}
			if !want && len(gotValues) != 0 {
				t.Fatalf("clql since macro output mismatch: got=%#v want no values out=%q", gotValues, string(out))
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
			if err != nil {
				t.Fatalf("clql --or failed: %v out=%q", err, string(out))
			}
			gotValues, err := decodeJSONValues(out)
			if err != nil {
				t.Fatalf("decode clql --or output: %v out=%q", err, string(out))
			}
			if want && len(gotValues) != 1 {
				t.Fatalf("clql --or output mismatch: got=%#v want one value out=%q", gotValues, string(out))
			}
			if !want && len(gotValues) != 0 {
				t.Fatalf("clql --or output mismatch: got=%#v want no values out=%q", gotValues, string(out))
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
				OnValue: func(lql.QueryStreamValue) error {
					return nil
				},
			})
			if err != nil {
				t.Fatalf("go query stream: %v", err)
			}
			want, err := goMatchedValues(tc.body, sel)
			if err != nil {
				t.Fatalf("go matched values: %v", err)
			}
			cmd := exec.Command(clql, "--matches-only", tc.expr)
			cmd.Stdin = bytes.NewBufferString(tc.body)
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("clql -M failed: %v out=%q", err, string(out))
			}
			if result.CandidatesMatched != int64(len(want)) {
				t.Fatalf("go stream count mismatch: result=%d values=%d", result.CandidatesMatched, len(want))
			}
			got, err := decodeJSONValues(out)
			if err != nil {
				t.Fatalf("decode clql -M output: %v out=%q", err, string(out))
			}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("clql -M output mismatch: got=%#v want=%#v out=%q", got, want, string(out))
			}
		})
	}
}

func TestCLQLStdinOutputStreamingParity(t *testing.T) {
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
			body: "{\"status\":\"closed\",\"id\":\"a\"}\n { \"status\" : \"open\" , \"id\" : \"b\" }\n",
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
			cmd := exec.Command(clql, tc.expr)
			cmd.Stdin = bytes.NewBufferString(tc.body)
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("clql stdin selection failed: %v out=%q", err, string(out))
			}
			got, err := decodeJSONValues(out)
			if err != nil {
				t.Fatalf("decode clql stdin output: %v output=%q", err, string(out))
			}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("clql stdin output mismatch: got=%#v want=%#v output=%q", got, want, string(out))
			}
		})
	}
}

func TestCLQLStdinCompactOutputParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := "{\n  \"status\" : \"closed\" , \"id\" : \"a\"\n}\n" +
		"{\n  \"status\" : \"open\" , \"id\" : \"b\" , \"items\" : [ 1, 2 ]\n}\n"
	cmd := exec.Command(clql, "-c", `/status="open"`)
	cmd.Stdin = bytes.NewBufferString(body)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql compact stdin selection failed: %v out=%q", err, string(out))
	}
	if string(out) != "{\"status\":\"open\",\"id\":\"b\",\"items\":[1,2]}\n" {
		t.Fatalf("compact stdin output mismatch: %q", string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode compact stdin output: %v out=%q", err, string(out))
	}
	sel, err := lql.ParseSelectorString(`/status="open"`)
	if err != nil {
		t.Fatalf("go parse: %v", err)
	}
	want, err := goMatchedValues(body, sel)
	if err != nil {
		t.Fatalf("go matched values: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("compact stdin parity mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLStdinProjectionParity(t *testing.T) {
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
			name:   "project matched stdin candidate",
			expr:   `/status="open"`,
			fields: []string{"/id", "/meta/trace", "/items/1/sku"},
			body:   "{\"status\":\"closed\",\"id\":\"a\"}\n { \"status\" : \"open\" , \"id\" : \"b\" , \"meta\" : { \"trace\" : 7 }, \"items\" : [ { \"sku\" : \"A\" }, { \"sku\" : \"B\" } ] }\n",
		},
		{
			name:   "missing field suppresses stdin output",
			expr:   `/status="open"`,
			fields: []string{"/missing"},
			body:   "{\"status\":\"open\",\"id\":\"a\"}\n",
		},
		{
			name:   "escaped pointer fields",
			expr:   `/status="open"`,
			fields: []string{"/a~1b/~0key"},
			body:   "{\"status\":\"open\",\"a/b\":{\"~key\":7},\"id\":\"a\"}\n",
		},
		{
			name:   "duplicate field is idempotent",
			expr:   `/status="open"`,
			fields: []string{"/id", "/id"},
			body:   "{\"status\":\"open\",\"id\":\"a\",\"other\":true}\n",
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
			args := []string{}
			for _, field := range tc.fields {
				args = append(args, "-f", field)
			}
			args = append(args, tc.expr)
			cmd := exec.Command(clql, args...)
			cmd.Stdin = bytes.NewBufferString(tc.body)
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("clql stdin projection failed: %v out=%q", err, string(out))
			}
			got, err := decodeJSONValues(out)
			if err != nil {
				t.Fatalf("decode clql stdin projection output: %v output=%q", err, string(out))
			}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("clql stdin projection mismatch: got=%#v want=%#v output=%q", got, want, string(out))
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
			if err != nil {
				t.Fatalf("clql file selection failed: %v out=%q", err, string(out))
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

func TestCLQLMultipleSelectorArgumentParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"id":"a","status":"open","progress":72}
{"id":"b","status":"open","progress":4}
{"id":"c","status":"closed","progress":80}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-multi-selector-*.json")
	if err != nil {
		t.Fatalf("create temp: %v", err)
	}
	if _, err := tmp.WriteString(body); err != nil {
		t.Fatalf("write temp: %v", err)
	}
	if err := tmp.Close(); err != nil {
		t.Fatalf("close temp: %v", err)
	}

	statusOpen, err := lql.ParseSelectorString(`/status="open"`)
	if err != nil {
		t.Fatalf("go parse status selector: %v", err)
	}
	progressHigh, err := lql.ParseSelectorString(`/progress>=50`)
	if err != nil {
		t.Fatalf("go parse progress selector: %v", err)
	}
	cases := []struct {
		name string
		args []string
		sel  lql.Selector
	}{
		{
			name: "and",
			args: []string{`/status="open"`, `/progress>=50`, tmp.Name()},
			sel:  lql.Selector{And: []lql.Selector{statusOpen, progressHigh}},
		},
		{
			name: "or",
			args: []string{"--or", `/status="open"`, `/progress>=50`, tmp.Name()},
			sel:  lql.Selector{Or: []lql.Selector{statusOpen, progressHigh}},
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			want, err := goMatchedValues(body, tc.sel)
			if err != nil {
				t.Fatalf("go matched values: %v", err)
			}
			cmd := exec.Command(clql, tc.args...)
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("clql multi-selector failed: %v out=%q", err, string(out))
			}
			got, err := decodeJSONValues(out)
			if err != nil {
				t.Fatalf("decode clql multi-selector output: %v output=%q", err, string(out))
			}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("clql multi-selector mismatch: got=%#v want=%#v output=%q", got, want, string(out))
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
			name:   "escaped pointer fields",
			expr:   `/status="open"`,
			fields: []string{"/a~1b/~0key"},
			body:   "{\"status\":\"open\",\"a/b\":{\"~key\":7},\"id\":\"a\"}\n",
		},
		{
			name:   "missing root field suppresses output",
			expr:   `/status="open"`,
			fields: []string{"/missing"},
			body:   "{\"status\":\"open\",\"id\":\"a\"}\n",
		},
		{
			name:   "duplicate field is idempotent",
			expr:   `/status="open"`,
			fields: []string{"/id", "/id"},
			body:   "{\"status\":\"open\",\"id\":\"a\",\"other\":true}\n",
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
			if err != nil {
				t.Fatalf("clql projection failed: %v out=%q", err, string(out))
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

func TestCLQLProjectionPathConflictParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []struct {
		name   string
		fields []string
	}{
		{
			name:   "parent before descendant",
			fields: []string{"/meta", "/meta/trace"},
		},
		{
			name:   "descendant before parent",
			fields: []string{"/items/0/sku", "/items"},
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			paths, err := lql.ParseProjectionPaths(tc.fields)
			if err != nil {
				t.Fatalf("go projection parse: %v", err)
			}
			if _, err := lql.NewProjectionPlan(paths); err == nil {
				t.Fatalf("go projection plan unexpectedly accepted fields: %#v", tc.fields)
			}
			args := []string{"-c"}
			for _, field := range tc.fields {
				args = append(args, "-f", field)
			}
			args = append(args, `contains{f=/}`)
			cmd := exec.Command(clql, args...)
			cmd.Stdin = bytes.NewBufferString(`{"meta":{"trace":7},"items":[{"sku":"A"}]}`)
			out, err := cmd.CombinedOutput()
			if err == nil {
				t.Fatalf("clql projection unexpectedly accepted fields %#v: out=%q", tc.fields, string(out))
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

func TestCLQLThemeFlagCompatibility(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []struct {
		name string
		args []string
	}{
		{"long separate", []string{"-c", "--theme", "default"}},
		{"long equals", []string{"-c", "--theme=default"}},
		{"long none", []string{"-c", "--theme=none"}},
		{"long jq alias", []string{"-c", "--theme=jq"}},
		{"short separate", []string{"-c", "-t", "default"}},
		{"short joined", []string{"-c", "-tdefault"}},
		{"short equals", []string{"-c", "-t=default"}},
		{"clustered short joined", []string{"-ctdefault"}},
		{"clustered short equals", []string{"-ct=default"}},
	}
	for _, tc := range cases {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			cmdArgs := append([]string{}, tc.args...)
			cmdArgs = append(cmdArgs, `/status="open"`)
			cmd := exec.Command(clql, cmdArgs...)
			cmd.Stdin = bytes.NewBufferString(`{"status":"open"}`)
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("clql theme flag failed: %v out=%q", err, string(out))
			}
			if string(out) != "{\"status\":\"open\"}\n" {
				t.Fatalf("theme flag changed compact output: %q", string(out))
			}
		})
	}
	invalidCases := []struct {
		name string
		args []string
	}{
		{"long separate", []string{"-c", "--theme", "definitely-not-a-theme"}},
		{"long equals", []string{"-c", "--theme=definitely-not-a-theme"}},
		{"short separate", []string{"-c", "-t", "definitely-not-a-theme"}},
		{"short joined", []string{"-c", "-tdefinitely-not-a-theme"}},
		{"short equals", []string{"-c", "-t=definitely-not-a-theme"}},
		{"clustered short joined", []string{"-ctdefinitely-not-a-theme"}},
		{"clustered short equals", []string{"-ct=definitely-not-a-theme"}},
	}
	for _, tc := range invalidCases {
		tc := tc
		t.Run("invalid "+tc.name, func(t *testing.T) {
			cmdArgs := append([]string{}, tc.args...)
			cmdArgs = append(cmdArgs, `/status="open"`)
			cmd := exec.Command(clql, cmdArgs...)
			cmd.Stdin = bytes.NewBufferString(`{"status":"open"}`)
			out, err := cmd.CombinedOutput()
			if err == nil {
				t.Fatalf("clql invalid theme unexpectedly succeeded: out=%q", string(out))
			}
			if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 2 {
				t.Fatalf("clql invalid theme exit mismatch: err=%v out=%q", err, string(out))
			}
			if !bytes.Contains(out, []byte("unknown theme")) {
				t.Fatalf("clql invalid theme diagnostic mismatch: out=%q", string(out))
			}
		})
	}
}

func TestCLQLBooleanFlagValueCompatibility(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	t.Run("compact true", func(t *testing.T) {
		cmd := exec.Command(clql, "--compact=true", `/status="open"`)
		cmd.Stdin = bytes.NewBufferString("{\n  \"status\" : \"open\" , \"id\" : \"a\"\n}")
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql --compact=true failed: %v out=%q", err, string(out))
		}
		if string(out) != "{\"status\":\"open\",\"id\":\"a\"}\n" {
			t.Fatalf("clql --compact=true output mismatch: %q", string(out))
		}
	})
	t.Run("matches only true", func(t *testing.T) {
		cmd := exec.Command(clql, "--matches-only=true", `/status="open"`)
		cmd.Stdin = bytes.NewBufferString(`{"status":"open"}`)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql --matches-only=true failed: %v out=%q", err, string(out))
		}
		if !bytes.Contains(out, []byte(`"status"`)) {
			t.Fatalf("clql --matches-only=true did not write matched JSON: %q", string(out))
		}
	})
	t.Run("matches only false", func(t *testing.T) {
		cmd := exec.Command(clql, "--matches-only=false", "-c", `/status="open"`)
		cmd.Stdin = bytes.NewBufferString(`{"status":"open"}`)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql --matches-only=false failed: %v out=%q", err, string(out))
		}
		if string(out) != "{\"status\":\"open\"}\n" {
			t.Fatalf("clql --matches-only=false output mismatch: %q", string(out))
		}
	})
	t.Run("or true", func(t *testing.T) {
		cmd := exec.Command(clql, "--or=true", "-c", `/status="open",/progress>=50`)
		cmd.Stdin = bytes.NewBufferString(`{"status":"closed","progress":72}`)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql --or=true failed: %v out=%q", err, string(out))
		}
		if string(out) != "{\"status\":\"closed\",\"progress\":72}\n" {
			t.Fatalf("clql --or=true output mismatch: %q", string(out))
		}
	})
	t.Run("or false", func(t *testing.T) {
		cmd := exec.Command(clql, "--or=false", "-c", `/status="open",/progress>=50`)
		cmd.Stdin = bytes.NewBufferString(`{"status":"closed","progress":72}`)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql --or=false failed: %v out=%q", err, string(out))
		}
		if len(out) != 0 {
			t.Fatalf("clql --or=false wrote output: %q", string(out))
		}
	})
	t.Run("uppercase aliases", func(t *testing.T) {
		cmd := exec.Command(clql, "--compact=TRUE", "--matches-only=F", `/status="open"`)
		cmd.Stdin = bytes.NewBufferString("{\n  \"status\" : \"open\"\n}")
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql uppercase boolean aliases failed: %v out=%q", err, string(out))
		}
		if string(out) != "{\"status\":\"open\"}\n" {
			t.Fatalf("clql uppercase boolean aliases output mismatch: %q", string(out))
		}
	})
	t.Run("enable file mutations true", func(t *testing.T) {
		dir := t.TempDir()
		blob := filepath.Join(dir, "blob.txt")
		if err := os.WriteFile(blob, []byte("hello"), 0600); err != nil {
			t.Fatalf("write blob: %v", err)
		}
		cmd := exec.Command(clql, "--enable-file-mutations=true", "-c", "-m", `textfile:/payload=`+blob, `contains{f=/}`)
		cmd.Stdin = bytes.NewBufferString(`{}`)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql --enable-file-mutations=true failed: %v out=%q", err, string(out))
		}
		if string(out) != "{\"payload\":\"hello\"}\n" {
			t.Fatalf("clql --enable-file-mutations=true output mismatch: %q", string(out))
		}
	})
	t.Run("enable file mutations false", func(t *testing.T) {
		cmd := exec.Command(clql, "--enable-file-mutations=false", "-m", `textfile:/payload=blob.txt`, `contains{f=/}`)
		cmd.Stdin = bytes.NewBufferString(`{}`)
		out, err := cmd.CombinedOutput()
		if err == nil {
			t.Fatalf("clql --enable-file-mutations=false unexpectedly accepted file value: out=%q", string(out))
		}
		if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 2 {
			t.Fatalf("clql --enable-file-mutations=false exit mismatch: err=%v out=%q", err, string(out))
		}
	})
	t.Run("inline true", func(t *testing.T) {
		tmp, err := os.CreateTemp(t.TempDir(), "clql-inline-true-*.json")
		if err != nil {
			t.Fatalf("create inline true temp: %v", err)
		}
		if _, err := tmp.WriteString(`{"status":"open"}`); err != nil {
			t.Fatalf("write inline true temp: %v", err)
		}
		if err := tmp.Close(); err != nil {
			t.Fatalf("close inline true temp: %v", err)
		}
		cmd := exec.Command(clql, "--inline=true", "-m", "/status=done", `contains{f=/}`, tmp.Name())
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql --inline=true failed: %v out=%q", err, string(out))
		}
		if len(out) != 0 {
			t.Fatalf("clql --inline=true wrote stdout: %q", string(out))
		}
		got, err := os.ReadFile(tmp.Name())
		if err != nil {
			t.Fatalf("read inline true temp: %v", err)
		}
		if string(got) != "{\"status\":\"done\"}\n" {
			t.Fatalf("clql --inline=true file mismatch: %q", string(got))
		}
	})
	t.Run("write true", func(t *testing.T) {
		tmp, err := os.CreateTemp(t.TempDir(), "clql-write-true-*.json")
		if err != nil {
			t.Fatalf("create write true temp: %v", err)
		}
		if _, err := tmp.WriteString(`{"status":"open"}`); err != nil {
			t.Fatalf("write write true temp: %v", err)
		}
		if err := tmp.Close(); err != nil {
			t.Fatalf("close write true temp: %v", err)
		}
		cmd := exec.Command(clql, "--write=true", "-m", "/status=done", `contains{f=/}`, tmp.Name())
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql --write=true failed: %v out=%q", err, string(out))
		}
		if len(out) != 0 {
			t.Fatalf("clql --write=true wrote stdout: %q", string(out))
		}
		got, err := os.ReadFile(tmp.Name())
		if err != nil {
			t.Fatalf("read write true temp: %v", err)
		}
		if string(got) != "{\"status\":\"done\"}\n" {
			t.Fatalf("clql --write=true file mismatch: %q", string(got))
		}
	})
	for _, flag := range []string{"--inline=false", "--write=false"} {
		flag := flag
		t.Run(flag, func(t *testing.T) {
			tmp, err := os.CreateTemp(t.TempDir(), "clql-inline-false-*.json")
			if err != nil {
				t.Fatalf("create %s temp: %v", flag, err)
			}
			if _, err := tmp.WriteString(`{"status":"open"}`); err != nil {
				t.Fatalf("write %s temp: %v", flag, err)
			}
			if err := tmp.Close(); err != nil {
				t.Fatalf("close %s temp: %v", flag, err)
			}
			cmd := exec.Command(clql, flag, "-m", "/status=done", `contains{f=/}`, tmp.Name())
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("clql %s failed: %v out=%q", flag, err, string(out))
			}
			if string(out) != "{\"status\":\"done\"}\n" {
				t.Fatalf("clql %s stdout mismatch: %q", flag, string(out))
			}
			got, err := os.ReadFile(tmp.Name())
			if err != nil {
				t.Fatalf("read %s temp: %v", flag, err)
			}
			if string(got) != `{"status":"open"}` {
				t.Fatalf("clql %s changed file despite false value: %q", flag, string(got))
			}
		})
	}
	invalidBoolCases := []struct {
		name       string
		args       []string
		diagnostic string
	}{
		{"compact", []string{"--compact=maybe", `/status="open"`}, "invalid boolean value for --compact"},
		{"matches only", []string{"--matches-only=maybe", `/status="open"`}, "invalid boolean value for --matches-only"},
		{"or", []string{"--or=maybe", `/status="open"`}, "invalid boolean value for --or"},
		{"inline", []string{"--inline=maybe", "-m", "/status=done", `/status="open"`}, "invalid boolean value for --inline"},
		{"write", []string{"--write=maybe", "-m", "/status=done", `/status="open"`}, "invalid boolean value for --write"},
		{"enable file mutations", []string{"--enable-file-mutations=maybe", "-m", "/status=done", `/status="open"`}, "invalid boolean value for --enable-file-mutations"},
		{"short compact", []string{"-c=maybe", `/status="open"`}, "invalid boolean value for short option"},
		{"short cluster", []string{"-cO=maybe", `/status="open"`}, "invalid boolean value for short option"},
	}
	for _, tc := range invalidBoolCases {
		tc := tc
		t.Run("invalid boolean "+tc.name, func(t *testing.T) {
			cmd := exec.Command(clql, tc.args...)
			cmd.Stdin = bytes.NewBufferString(`{"status":"open"}`)
			out, err := cmd.CombinedOutput()
			if err == nil {
				t.Fatalf("clql invalid boolean %s unexpectedly succeeded: out=%q", tc.name, string(out))
			}
			if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 2 {
				t.Fatalf("clql invalid boolean %s exit mismatch: err=%v out=%q", tc.name, err, string(out))
			}
			if !bytes.Contains(out, []byte(tc.diagnostic)) {
				t.Fatalf("clql invalid boolean %s diagnostic mismatch: out=%q", tc.name, string(out))
			}
		})
	}
}

func TestCLQLShortOptionClusterCompatibility(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	t.Run("boolean cluster", func(t *testing.T) {
		cmd := exec.Command(clql, "-cO", `/status="open",/progress>=50`)
		cmd.Stdin = bytes.NewBufferString(`{"status":"closed","progress":72}`)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql -cO failed: %v out=%q", err, string(out))
		}
		if string(out) != "{\"status\":\"closed\",\"progress\":72}\n" {
			t.Fatalf("clql -cO output mismatch: %q", string(out))
		}
	})
	t.Run("short boolean false value", func(t *testing.T) {
		cmd := exec.Command(clql, "-cO=false", `/status="open",/progress>=50`)
		cmd.Stdin = bytes.NewBufferString(`{"status":"closed","progress":72}`)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql -cO=false failed: %v out=%q", err, string(out))
		}
		if len(out) != 0 {
			t.Fatalf("clql -cO=false wrote output: %q", string(out))
		}
	})
	t.Run("clustered field value", func(t *testing.T) {
		cmd := exec.Command(clql, "-cf/status", `/status="open"`)
		cmd.Stdin = bytes.NewBufferString(`{"status":"open","id":"a"}`)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql -cf/status failed: %v out=%q", err, string(out))
		}
		if string(out) != "{\"status\":\"open\"}\n" {
			t.Fatalf("clql -cf/status output mismatch: %q", string(out))
		}
	})
	t.Run("clustered mutation value", func(t *testing.T) {
		cmd := exec.Command(clql, "-cm/status=done", `/status="open"`)
		cmd.Stdin = bytes.NewBufferString(`{"status":"open"}`)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql -cm/status=done failed: %v out=%q", err, string(out))
		}
		if string(out) != "{\"status\":\"done\"}\n" {
			t.Fatalf("clql -cm/status=done output mismatch: %q", string(out))
		}
	})
	t.Run("clustered or and matches only", func(t *testing.T) {
		cmd := exec.Command(clql, "-cMm/status=done", `/status="open"`)
		cmd.Stdin = bytes.NewBufferString(`{"status":"open"}`)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql -cMm/status=done failed: %v out=%q", err, string(out))
		}
		if string(out) != "{\"status\":\"done\"}\n" {
			t.Fatalf("clql -cMm/status=done output mismatch: %q", string(out))
		}
	})
}

func TestCLQLEndOfOptionsCompatibility(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	clqlPath, err := filepath.Abs(clql)
	if err != nil {
		t.Fatalf("resolve clql path: %v", err)
	}
	dir := t.TempDir()
	inputName := "--input.json"
	if err := os.WriteFile(filepath.Join(dir, inputName), []byte(`{"status":"open"}`), 0600); err != nil {
		t.Fatalf("write dash-prefixed input: %v", err)
	}
	cmd := exec.Command(clqlPath, "-c", `/status="open"`, "--", inputName)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql -- terminator failed: %v out=%q", err, string(out))
	}
	if string(out) != "{\"status\":\"open\"}\n" {
		t.Fatalf("clql -- terminator output mismatch: %q", string(out))
	}
}

func TestCLQLInterspersedFlagOrderCompatibility(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	dir := t.TempDir()
	inputPath := filepath.Join(dir, "input.jsonl")
	if err := os.WriteFile(inputPath, []byte(`{"id":"a","status":"open"}`+"\n"+`{"id":"b","status":"closed"}`), 0600); err != nil {
		t.Fatalf("write input: %v", err)
	}
	run := func(name string, args ...string) []any {
		t.Helper()
		cmd := exec.Command(clql, args...)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("%s failed: %v out=%q", name, err, string(out))
		}
		values, err := decodeJSONValues(out)
		if err != nil {
			t.Fatalf("%s decode output: %v out=%q", name, err, string(out))
		}
		return values
	}
	before := run("selector before field", `/status="open"`, "-f", "/id", inputPath)
	after := run("field before selector", "-f", "/id", `/status="open"`, inputPath)
	want, err := decodeJSONValues([]byte(`{"id":"a"}`))
	if err != nil {
		t.Fatalf("decode expected interspersed output: %v", err)
	}
	if !reflect.DeepEqual(before, want) || !reflect.DeepEqual(after, want) {
		t.Fatalf("interspersed flag output mismatch: before=%#v after=%#v want=%#v", before, after, want)
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

func TestCLQLMalformedJSONExecutionErrors(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	type malformedCorpus struct {
		name       string
		body       string
		wantStdout map[string]string
	}
	corpora := []malformedCorpus{
		{
			name:       "no completed candidates",
			body:       `{"status":`,
			wantStdout: map[string]string{},
		},
		{
			name: "one completed match before malformed tail",
			body: "{\"status\":\"open\",\"id\":\"a\"}\n{\"status\":",
			wantStdout: map[string]string{
				"selection":    "{\"status\":\"open\",\"id\":\"a\"}\n",
				"matches-only": "{\"status\":\"open\",\"id\":\"a\"}\n",
				"compact":      "{\"status\":\"open\",\"id\":\"a\"}\n",
				"projection":   "{\"id\":\"a\"}\n",
				"mutation":     "{\"status\":\"done\",\"id\":\"a\"}\n",
			},
		},
	}
	selector, err := lql.ParseSelectorString(`/status="open"`)
	if err != nil {
		t.Fatalf("go parse selector: %v", err)
	}
	dir := t.TempDir()
	cases := []struct {
		name      string
		stdoutKey string
		args      func(string) []string
		stdin     bool
	}{
		{"stdin selection", "selection", func(_ string) []string { return []string{`/status="open"`} }, true},
		{"stdin matches-only", "matches-only", func(_ string) []string { return []string{"-M", `/status="open"`} }, true},
		{"stdin compact", "compact", func(_ string) []string { return []string{"-c", `/status="open"`} }, true},
		{"stdin projection", "projection", func(_ string) []string { return []string{"-f", "/id", `/status="open"`} }, true},
		{"stdin mutation", "mutation", func(_ string) []string { return []string{"-m", "/status=done", `/status="open"`} }, true},
		{"file selection", "selection", func(path string) []string { return []string{`/status="open"`, path} }, false},
		{"file matches-only", "matches-only", func(path string) []string { return []string{"-M", `/status="open"`, path} }, false},
		{"file compact", "compact", func(path string) []string { return []string{"-c", `/status="open"`, path} }, false},
		{"file projection", "projection", func(path string) []string { return []string{"-f", "/id", `/status="open"`, path} }, false},
		{"file mutation", "mutation", func(path string) []string { return []string{"-m", "/status=done", `/status="open"`, path} }, false},
	}
	for _, corpus := range corpora {
		t.Run(corpus.name, func(t *testing.T) {
			if _, err := lql.QueryStreamWithResult(lql.QueryStreamRequest{
				Reader:   bytes.NewBufferString(corpus.body),
				Selector: selector,
				Mode:     lql.QueryDecisionOnly,
				OnDecision: func(lql.QueryStreamDecision) error {
					return nil
				},
			}); err == nil {
				t.Fatalf("go stream unexpectedly accepted malformed JSON")
			}
			inputPath := filepath.Join(dir, corpus.name+".json")
			if err := os.WriteFile(inputPath, []byte(corpus.body), 0600); err != nil {
				t.Fatalf("write malformed input: %v", err)
			}
			for _, tc := range cases {
				t.Run(tc.name, func(t *testing.T) {
					cmd := exec.Command(clql, tc.args(inputPath)...)
					if tc.stdin {
						cmd.Stdin = bytes.NewBufferString(corpus.body)
					}
					var stdout bytes.Buffer
					var stderr bytes.Buffer
					cmd.Stdout = &stdout
					cmd.Stderr = &stderr
					err := cmd.Run()
					if err == nil {
						t.Fatalf("clql unexpectedly accepted malformed JSON: stdout=%q stderr=%q",
							stdout.String(), stderr.String())
					}
					if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 1 {
						t.Fatalf("clql malformed JSON exit mismatch: err=%v stdout=%q stderr=%q",
							err, stdout.String(), stderr.String())
					}
					if !bytes.Contains(stderr.Bytes(), []byte("clql: ")) {
						t.Fatalf("clql malformed JSON diagnostic missing prefix: stderr=%q",
							stderr.String())
					}
					if stdout.String() != corpus.wantStdout[tc.stdoutKey] {
						t.Fatalf("clql malformed JSON stdout mismatch: got=%q want=%q stderr=%q",
							stdout.String(), corpus.wantStdout[tc.stdoutKey],
							stderr.String())
					}
				})
			}
		})
	}
}

func TestCLQLMutationParseErrorParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []struct {
		name              string
		expr              string
		enableFileValues  bool
		goEnableFileValue bool
	}{
		{name: "bad expression", expr: `badexpr`},
		{name: "root path", expr: `/`},
		{name: "zero increment", expr: `/count=+0`},
		{name: "invalid time", expr: `time:/state/updated=tomorrowish`},
		{name: "date only time value", expr: `time:/state/updated=2025-01-01`},
		{name: "disabled file backed value", expr: `file:/payload=blob.txt`},
		{name: "file backed increment", expr: `file:/payload++`, enableFileValues: true, goEnableFileValue: true},
		{name: "file backed remove", expr: `file:rm:/payload=blob.txt`, enableFileValues: true, goEnableFileValue: true},
		{name: "file backed time", expr: `file:time:/payload=blob.txt`, enableFileValues: true, goEnableFileValue: true},
		{name: "brace parse error", expr: `/state/details{/owner="alice"}}`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			opts := lql.ParseMutationsOptions{
				EnableFileValues: tc.goEnableFileValue,
				FileValueBaseDir: t.TempDir(),
			}
			if _, err := lql.ParseMutationsWithOptions([]string{tc.expr}, time.Unix(1700000000, 0), opts); err == nil {
				t.Fatalf("go mutation parser unexpectedly accepted %q", tc.expr)
			}
			args := []string{"-m", tc.expr, `contains{f=/}`}
			if tc.enableFileValues {
				args = append([]string{"-F"}, args...)
			}
			cmd := exec.Command(clql, args...)
			cmd.Stdin = bytes.NewBufferString(`{"status":"open"}`)
			out, err := cmd.CombinedOutput()
			if err == nil {
				t.Fatalf("clql mutation parser unexpectedly accepted %q: out=%q", tc.expr, string(out))
			}
			if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 2 {
				t.Fatalf("clql mutation parse exit mismatch: err=%v out=%q", err, string(out))
			}
		})
	}
}

func TestCLQLEnableFileMutationsStreamsExplicitFileBackedValues(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	dir := t.TempDir()
	textPath := filepath.Join(dir, "blob.txt")
	binPath := filepath.Join(dir, "blob.bin")
	if err := os.WriteFile(textPath, []byte("hello\n\"quoted\""), 0600); err != nil {
		t.Fatalf("write text blob: %v", err)
	}
	if err := os.WriteFile(binPath, []byte{0x00, 0x01, 0x02, 'a'}, 0600); err != nil {
		t.Fatalf("write binary blob: %v", err)
	}
	if _, err := lql.ParseMutationsWithOptions(
		[]string{
			`textfile:/payload=` + textPath,
			`base64file:/encoded=` + binPath,
			`file:/auto_text=` + textPath,
			`file:/auto_bin=` + binPath,
		},
		time.Unix(1700000000, 0),
		lql.ParseMutationsOptions{EnableFileValues: true},
	); err != nil {
		t.Fatalf("go parse enabled file-backed mutation: %v", err)
	}
	inputPath := filepath.Join(dir, "input.json")
	if err := os.WriteFile(inputPath, []byte(`{}`), 0600); err != nil {
		t.Fatalf("write input: %v", err)
	}
	cmd := exec.Command(
		clql,
		"-F",
		"-c",
		"-m", `textfile:/payload=`+textPath,
		"-m", `base64file:/encoded=`+binPath,
		"-m", `file:/auto_text=`+textPath,
		"-m", `file:/auto_bin=`+binPath,
		`contains{f=/}`,
		inputPath,
	)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("file-backed mutation execution failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode file-backed mutation output: %v out=%q", err, string(out))
	}
	want, err := decodeJSONValues([]byte(`{"payload":"hello\n\"quoted\"","encoded":"AAECYQ==","auto_text":"hello\n\"quoted\"","auto_bin":"AAECYQ=="}`))
	if err != nil {
		t.Fatalf("decode expected file-backed mutation output: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("file-backed mutation mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLFileBackedTextValidationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []struct {
		name   string
		file   string
		body   []byte
		needle string
	}{
		{name: "invalid UTF-8", file: "invalid.txt", body: []byte{'h', 'i', 0xff}, needle: "UTF"},
		{name: "NUL byte", file: "nul.txt", body: []byte{'h', 'i', 0x00, 'x'}, needle: "NUL"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			path := filepath.Join(dir, tc.file)
			mutation := `textfile:/payload=` + path
			if err := os.WriteFile(path, tc.body, 0600); err != nil {
				t.Fatalf("write text payload: %v", err)
			}
			parsed, err := lql.ParseMutationsWithOptions(
				[]string{mutation},
				time.Unix(1700000000, 0),
				lql.ParseMutationsOptions{EnableFileValues: true},
			)
			if err != nil {
				t.Fatalf("go parse textfile mutation: %v", err)
			}
			var goOut bytes.Buffer
			if err := lql.MutateStream(lql.MutateStreamRequest{
				Reader:    bytes.NewBufferString(`{}`),
				Writer:    &goOut,
				Mutations: parsed,
			}); err == nil {
				t.Fatalf("go mutation unexpectedly accepted %s textfile", tc.name)
			}

			cmd := exec.Command(clql, "-F", "-c", "-m", mutation, `contains{f=/}`)
			cmd.Stdin = bytes.NewBufferString(`{}`)
			out, err := cmd.CombinedOutput()
			if err == nil {
				t.Fatalf("clql unexpectedly accepted %s textfile: out=%q", tc.name, string(out))
			}
			if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 1 {
				t.Fatalf("clql textfile validation exit mismatch: err=%v out=%q", err, string(out))
			}
			if !bytes.Contains(out, []byte(tc.needle)) {
				t.Fatalf("clql textfile validation diagnostic missing %q: out=%q", tc.needle, string(out))
			}
		})
	}
}

func TestCLQLStdinFileBackedMutationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	dir := t.TempDir()
	textPath := filepath.Join(dir, "blob.txt")
	binPath := filepath.Join(dir, "blob.bin")
	if err := os.WriteFile(textPath, []byte("stdin\npayload"), 0600); err != nil {
		t.Fatalf("write text blob: %v", err)
	}
	if err := os.WriteFile(binPath, []byte{0x00, 0x10, 0x20, 0x7f}, 0600); err != nil {
		t.Fatalf("write binary blob: %v", err)
	}

	cmd := exec.Command(
		clql,
		"-F",
		"-c",
		"-m", `textfile:/payload=`+textPath,
		"-m", `base64file:/encoded=`+binPath,
		`/id="b"`,
	)
	cmd.Stdin = bytes.NewBufferString(`{"id":"a"}
{"id":"b"}`)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("stdin file-backed mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode stdin file-backed mutation output: %v out=%q", err, string(out))
	}
	want, err := decodeJSONValues([]byte(`{"id":"a"}
{"id":"b","payload":"stdin\npayload","encoded":"ABAgfw=="}`))
	if err != nil {
		t.Fatalf("decode expected stdin file-backed mutation output: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("stdin file-backed mutation mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLFileBackedMutationExpandsHomeParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	homeDir := t.TempDir()
	t.Setenv("HOME", homeDir)
	if err := os.WriteFile(filepath.Join(homeDir, "blob.txt"), []byte("hello from home"), 0600); err != nil {
		t.Fatalf("write home blob: %v", err)
	}
	dir := t.TempDir()
	inputPath := filepath.Join(dir, "input.json")
	if err := os.WriteFile(inputPath, []byte(`{"id":"a"}`), 0600); err != nil {
		t.Fatalf("write input: %v", err)
	}
	if _, err := lql.ParseMutationsWithOptions(
		[]string{`textfile:/payload=~/blob.txt`},
		time.Unix(1700000000, 0),
		lql.ParseMutationsOptions{EnableFileValues: true},
	); err != nil {
		t.Fatalf("go parse home-expanded file-backed mutation: %v", err)
	}

	cmd := exec.Command(
		clql,
		"-F",
		"-c",
		"-m", `textfile:/payload=~/blob.txt`,
		inputPath,
	)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("home-expanded file-backed mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode home-expanded mutation output: %v out=%q", err, string(out))
	}
	want, err := decodeJSONValues([]byte(`{"id":"a","payload":"hello from home"}`))
	if err != nil {
		t.Fatalf("decode expected home-expanded mutation output: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("home-expanded file-backed mutation mismatch: got=%#v want=%#v out=%q", got, want, string(out))
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

func TestCLQLMutationMultipleInputFilesParity(t *testing.T) {
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
	cmd := exec.Command(clql, "-c", "-m", "/status=done", fileA, fileB)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("multiple mutation input files failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode multiple mutation inputs: %v out=%q", err, string(out))
	}
	want, err := decodeJSONValues([]byte(`{"id":"a","status":"done"}
{"id":"b","status":"done"}`))
	if err != nil {
		t.Fatalf("decode expected multiple mutation inputs: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("multiple mutation input mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}

	cmd = exec.Command(clql, "-c", "-m", "/status=done", fileA, "-")
	cmd.Stdin = bytes.NewBufferString(`{"id":"stdin"}`)
	out, err = cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("mixed mutation input files failed: %v out=%q", err, string(out))
	}
	got, err = decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode mixed mutation inputs: %v out=%q", err, string(out))
	}
	want, err = decodeJSONValues([]byte(`{"id":"a","status":"done"}
{"id":"stdin","status":"done"}`))
	if err != nil {
		t.Fatalf("decode expected mixed mutation inputs: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("mixed mutation input mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLRootMutationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"status":"open","count":1,"score":5,"old":true,"remove_me":true,"delete_me":true,"del_me":true}`
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
		"-m", "/count--",
		"-m", "/score=-2",
		"-m", "rm:/old",
		"-m", "remove:/remove_me",
		"-m", "delete:/delete_me",
		"-m", "del:/del_me",
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

	doc := map[string]any{
		"status":    "open",
		"count":     float64(1),
		"score":     float64(5),
		"old":       true,
		"remove_me": true,
		"delete_me": true,
		"del_me":    true,
	}
	muts, err := lql.ParseMutations([]string{
		"/status=done",
		"/count--",
		"/score=-2",
		"rm:/old",
		"remove:/remove_me",
		"delete:/delete_me",
		"del:/del_me",
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

func TestCLQLQuotedMutationValueParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"state":{"status":"open"}}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-quoted-*.json")
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
		`/state/text="a\"b\\c"`,
		`/state/truth="true"`,
		`/state/nothing="null"`,
		`/state/number="2"`,
	}
	args := []string{"-c"}
	for _, mutation := range mutations {
		args = append(args, "-m", mutation)
	}
	args = append(args, `contains{f=/}`, tmp.Name())
	cmd := exec.Command(clql, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql quoted mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql quoted mutation: %v out=%q", err, string(out))
	}

	muts, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		t.Fatalf("go parse quoted mutations: %v", err)
	}
	var wantOut bytes.Buffer
	if err := lql.MutateStream(lql.MutateStreamRequest{
		Reader:    bytes.NewBufferString(body),
		Writer:    &wantOut,
		Mutations: muts,
	}); err != nil {
		t.Fatalf("go stream quoted mutations: %v", err)
	}
	want, err := decodeJSONValues(wantOut.Bytes())
	if err != nil {
		t.Fatalf("decode go quoted mutation result: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("quoted mutation parity mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLBraceMutationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"state":{"status":"open","count":1,"old":true,"owner":"bob"},"id":"a"}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-brace-*.json")
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
		`/state{/status=done,/count=+2,rm:/old,/owner="alice"}`,
		`/audit{/created=true,/nested/score=3}`,
	}
	args := []string{"-c"}
	for _, mutation := range mutations {
		args = append(args, "-m", mutation)
	}
	args = append(args, `contains{f=/}`, tmp.Name())
	cmd := exec.Command(clql, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql brace mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql brace mutation: %v out=%q", err, string(out))
	}

	muts, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		t.Fatalf("go parse brace mutations: %v", err)
	}
	var wantOut bytes.Buffer
	if err := lql.MutateStream(lql.MutateStreamRequest{
		Reader:    bytes.NewBufferString(body),
		Writer:    &wantOut,
		Mutations: muts,
	}); err != nil {
		t.Fatalf("go stream brace mutations: %v", err)
	}
	want, err := decodeJSONValues(wantOut.Bytes())
	if err != nil {
		t.Fatalf("decode go brace mutation result: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("brace mutation parity mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLEscapedMutationPathParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"a/b":{"~key":"old","remove":true},"plain":"keep"}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-escaped-*.json")
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
		`/a~1b/~0key=ready`,
		`rm:/a~1b/remove`,
		`/a~1b/created=1`,
	}
	args := []string{"-c"}
	for _, mutation := range mutations {
		args = append(args, "-m", mutation)
	}
	args = append(args, `contains{f=/}`, tmp.Name())
	cmd := exec.Command(clql, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql escaped mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql escaped mutation: %v out=%q", err, string(out))
	}

	muts, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		t.Fatalf("go parse escaped mutations: %v", err)
	}
	var wantOut bytes.Buffer
	if err := lql.MutateStream(lql.MutateStreamRequest{
		Reader:    bytes.NewBufferString(body),
		Writer:    &wantOut,
		Mutations: muts,
	}); err != nil {
		t.Fatalf("go stream escaped mutations: %v", err)
	}
	want, err := decodeJSONValues(wantOut.Bytes())
	if err != nil {
		t.Fatalf("decode go escaped mutation result: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("escaped mutation parity mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLStdinEscapedMutationPathParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"a/b":{"~key":"old","remove":true},"plain":"keep"}`
	mutations := []string{
		`/a~1b/~0key=ready`,
		`rm:/a~1b/remove`,
		`/a~1b/created=1`,
	}
	args := []string{"-c"}
	for _, mutation := range mutations {
		args = append(args, "-m", mutation)
	}
	args = append(args, `contains{f=/}`)
	cmd := exec.Command(clql, args...)
	cmd.Stdin = bytes.NewBufferString(body)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql stdin escaped mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql stdin escaped mutation: %v out=%q", err, string(out))
	}

	muts, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		t.Fatalf("go parse stdin escaped mutations: %v", err)
	}
	var wantOut bytes.Buffer
	if err := lql.MutateStream(lql.MutateStreamRequest{
		Reader:    bytes.NewBufferString(body),
		Writer:    &wantOut,
		Mutations: muts,
	}); err != nil {
		t.Fatalf("go stream stdin escaped mutations: %v", err)
	}
	want, err := decodeJSONValues(wantOut.Bytes())
	if err != nil {
		t.Fatalf("decode go stdin escaped mutation result: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("stdin escaped mutation parity mismatch: got=%#v want=%#v out=%q", got, want, string(out))
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

func TestCLQLWildcardMutationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"items":[{"sku":"A","status":"old"},{"sku":"B","status":"new"}],"labels":{"env":"prod","tier":"edge"},"numeric_object":{"0":{"status":"unchanged"}}}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-wildcard-*.json")
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
		"/items[]/status=ready",
		"/labels/*=tagged",
		"/numeric_object[]/status=bad",
	}
	args := []string{"-c"}
	for _, mutation := range mutations {
		args = append(args, "-m", mutation)
	}
	args = append(args, `contains{f=/}`, tmp.Name())
	cmd := exec.Command(clql, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql wildcard mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql wildcard mutation: %v out=%q", err, string(out))
	}

	muts, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		t.Fatalf("go parse wildcard mutations: %v", err)
	}
	var wantOut bytes.Buffer
	if err := lql.MutateStream(lql.MutateStreamRequest{
		Reader:    bytes.NewBufferString(body),
		Writer:    &wantOut,
		Mutations: muts,
	}); err != nil {
		t.Fatalf("go stream wildcard mutations: %v", err)
	}
	want, err := decodeJSONValues(wantOut.Bytes())
	if err != nil {
		t.Fatalf("decode go wildcard mutation result: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("wildcard mutation parity mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLRecursiveMutationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"items":[{"status":"old"}],"boxes":{"a":{"status":"old"}},"groups":[{"items":[{"sku":"A","count":1,"drop":true}]}]}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-recursive-*.json")
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
		"/items/**/status=ready",
		"/boxes/**/status=ready",
		"/groups/.../sku=Z",
		"/groups/.../count=+2",
	}
	args := []string{"-c"}
	for _, mutation := range mutations {
		args = append(args, "-m", mutation)
	}
	args = append(args, `contains{f=/}`, tmp.Name())
	cmd := exec.Command(clql, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql recursive mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql recursive mutation: %v out=%q", err, string(out))
	}

	muts, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		t.Fatalf("go parse recursive mutations: %v", err)
	}
	var wantOut bytes.Buffer
	if err := lql.MutateStream(lql.MutateStreamRequest{
		Reader:    bytes.NewBufferString(body),
		Writer:    &wantOut,
		Mutations: muts,
	}); err != nil {
		t.Fatalf("go stream recursive mutations: %v", err)
	}
	want, err := decodeJSONValues(wantOut.Bytes())
	if err != nil {
		t.Fatalf("decode go recursive mutation result: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("recursive mutation parity mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLArrayWildcardValueMutationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"nums":[1,2],"words":["a","b"],"drops":[true,false],"objects":[{"a":1}],"groups":[{"items":[{"count":1}]}]}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-array-wildcard-*.json")
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
		"/nums[]=+2",
		"/words[]=ready",
		"rm:/drops[]",
		"/objects[]=done",
		"/groups/.../count=+2",
	}
	args := []string{"-c"}
	for _, mutation := range mutations {
		args = append(args, "-m", mutation)
	}
	args = append(args, `contains{f=/}`, tmp.Name())
	cmd := exec.Command(clql, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql array wildcard value mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql array wildcard value mutation: %v out=%q", err, string(out))
	}

	muts, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		t.Fatalf("go parse array wildcard value mutations: %v", err)
	}
	var wantOut bytes.Buffer
	if err := lql.MutateStream(lql.MutateStreamRequest{
		Reader:    bytes.NewBufferString(body),
		Writer:    &wantOut,
		Mutations: muts,
	}); err != nil {
		t.Fatalf("go stream array wildcard value mutations: %v", err)
	}
	want, err := decodeJSONValues(wantOut.Bytes())
	if err != nil {
		t.Fatalf("decode go array wildcard value mutation result: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("array wildcard value mutation parity mismatch: got=%#v want=%#v out=%q", got, want, string(out))
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

func TestCLQLStdinMutationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"id":"a","status":"open"}
{"id":"b","status":"open"}`
	cmd := exec.Command(
		clql,
		"-c",
		"-m", "/status=done",
		`/id="b"`,
	)
	cmd.Stdin = bytes.NewBufferString(body)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql stdin mutation failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql stdin mutation: %v out=%q", err, string(out))
	}

	want, err := decodeJSONValues([]byte(`{"id":"a","status":"open"}
{"id":"b","status":"done"}`))
	if err != nil {
		t.Fatalf("decode expected stdin mutation: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("stdin mutation mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLTopLevelArrayMutationParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `[{"id":"a","status":"open"},{"id":"b","status":"open"}]`
	mutations := []string{`/status=done`}
	muts, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		t.Fatalf("go parse mutations: %v", err)
	}
	var wantOut bytes.Buffer
	if err := lql.MutateStream(lql.MutateStreamRequest{
		Reader:    bytes.NewBufferString(body),
		Writer:    &wantOut,
		Mutations: muts,
	}); err != nil {
		t.Fatalf("go mutate top-level array: %v", err)
	}
	want, err := decodeJSONValues(wantOut.Bytes())
	if err != nil {
		t.Fatalf("decode go top-level array mutation: %v", err)
	}

	t.Run("stdin", func(t *testing.T) {
		args := []string{"-c"}
		for _, mutation := range mutations {
			args = append(args, "-m", mutation)
		}
		cmd := exec.Command(clql, args...)
		cmd.Stdin = bytes.NewBufferString(body)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql stdin top-level array mutation failed: %v out=%q", err, string(out))
		}
		got, err := decodeJSONValues(out)
		if err != nil {
			t.Fatalf("decode clql stdin top-level array mutation: %v out=%q", err, string(out))
		}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("stdin top-level array mutation mismatch: got=%#v want=%#v out=%q", got, want, string(out))
		}
	})

	t.Run("file", func(t *testing.T) {
		tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-top-array-*.json")
		if err != nil {
			t.Fatalf("create temp: %v", err)
		}
		if _, err := tmp.WriteString(body); err != nil {
			t.Fatalf("write temp: %v", err)
		}
		if err := tmp.Close(); err != nil {
			t.Fatalf("close temp: %v", err)
		}
		args := []string{"-c"}
		for _, mutation := range mutations {
			args = append(args, "-m", mutation)
		}
		args = append(args, tmp.Name())
		cmd := exec.Command(clql, args...)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql file top-level array mutation failed: %v out=%q", err, string(out))
		}
		got, err := decodeJSONValues(out)
		if err != nil {
			t.Fatalf("decode clql file top-level array mutation: %v out=%q", err, string(out))
		}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("file top-level array mutation mismatch: got=%#v want=%#v out=%q", got, want, string(out))
		}
	})
}

func TestCLQLMatchAllMutationMixedStreamParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `1
{"id":"a","status":"new"}
[{"id":"b","status":"new"},3]`
	mutations := []string{`/status=ready`}
	muts, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		t.Fatalf("go parse mutations: %v", err)
	}
	var wantOut bytes.Buffer
	if err := lql.MutateStream(lql.MutateStreamRequest{
		Reader:    bytes.NewBufferString(body),
		Writer:    &wantOut,
		Mutations: muts,
	}); err != nil {
		t.Fatalf("go mutate mixed stream: %v", err)
	}
	want, err := decodeJSONValues(wantOut.Bytes())
	if err != nil {
		t.Fatalf("decode go mixed stream mutation: %v", err)
	}

	t.Run("stdin", func(t *testing.T) {
		args := []string{"-c"}
		for _, mutation := range mutations {
			args = append(args, "-m", mutation)
		}
		cmd := exec.Command(clql, args...)
		cmd.Stdin = bytes.NewBufferString(body)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql stdin mixed mutation failed: %v out=%q", err, string(out))
		}
		got, err := decodeJSONValues(out)
		if err != nil {
			t.Fatalf("decode clql stdin mixed mutation: %v out=%q", err, string(out))
		}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("stdin mixed mutation mismatch: got=%#v want=%#v out=%q", got, want, string(out))
		}
	})

	t.Run("file", func(t *testing.T) {
		tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-mixed-*.json")
		if err != nil {
			t.Fatalf("create temp: %v", err)
		}
		if _, err := tmp.WriteString(body); err != nil {
			t.Fatalf("write temp: %v", err)
		}
		if err := tmp.Close(); err != nil {
			t.Fatalf("close temp: %v", err)
		}
		args := []string{"-c"}
		for _, mutation := range mutations {
			args = append(args, "-m", mutation)
		}
		args = append(args, tmp.Name())
		cmd := exec.Command(clql, args...)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("clql file mixed mutation failed: %v out=%q", err, string(out))
		}
		got, err := decodeJSONValues(out)
		if err != nil {
			t.Fatalf("decode clql file mixed mutation: %v out=%q", err, string(out))
		}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("file mixed mutation mismatch: got=%#v want=%#v out=%q", got, want, string(out))
		}
	})
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

func TestCLQLMutationProjectionParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"uri":"/a","status":404,"drop":true}
{"uri":"/b","status":200,"drop":true}`
	tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-project-*.json")
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
		"-f", "/uri",
		"-m", "/hello=world",
		`/status=404`,
		tmp.Name(),
	)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql mutation projection failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql mutation projection: %v out=%q", err, string(out))
	}
	want, err := decodeJSONValues([]byte(`{"uri":"/a","hello":"world"}
{"uri":"/b"}`))
	if err != nil {
		t.Fatalf("decode expected mutation projection: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("mutation projection mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLMutationProjectionMissingFieldsParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []struct {
		name string
		body string
		args []string
		want string
	}{
		{
			name: "all projected outputs missing",
			body: `{"id":"a"}
{"id":"b"}`,
			args: []string{
				"-c",
				"-f", "/missing",
				"-m", "/noop=1",
				`contains{f=/}`,
			},
			want: "",
		},
		{
			name: "matched candidate missing projection is dropped",
			body: `{"status":404}
{"status":200,"uri":"/ok"}`,
			args: []string{
				"-c",
				"-f", "/uri",
				"-m", "/hello=world",
				`/status=404`,
			},
			want: `{"uri":"/ok"}` + "\n",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			tmp, err := os.CreateTemp(t.TempDir(), "clql-mutate-project-missing-*.json")
			if err != nil {
				t.Fatalf("create temp: %v", err)
			}
			if _, err := tmp.WriteString(tc.body); err != nil {
				t.Fatalf("write temp: %v", err)
			}
			if err := tmp.Close(); err != nil {
				t.Fatalf("close temp: %v", err)
			}
			args := append([]string{}, tc.args...)
			args = append(args, tmp.Name())
			cmd := exec.Command(clql, args...)
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("clql mutation projection missing fields failed: %v out=%q", err, string(out))
			}
			got, err := decodeJSONValues(out)
			if err != nil {
				t.Fatalf("decode clql mutation projection missing output: %v out=%q", err, string(out))
			}
			want, err := decodeJSONValues([]byte(tc.want))
			if err != nil {
				t.Fatalf("decode expected mutation projection missing output: %v", err)
			}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("mutation projection missing fields mismatch: got=%#v want=%#v out=%q", got, want, string(out))
			}
		})
	}
}

func TestCLQLStdinMutationProjectionMatchesOnlyParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"uri":"/a","status":404,"drop":true}
{"uri":"/b","status":200,"drop":true}`
	cmd := exec.Command(
		clql,
		"-c",
		"-M",
		"-f", "/uri",
		"-m", "/hello=world",
		`/status=404`,
	)
	cmd.Stdin = bytes.NewBufferString(body)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql stdin mutation projection matches-only failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql stdin mutation projection matches-only: %v out=%q", err, string(out))
	}
	want, err := decodeJSONValues([]byte(`{"uri":"/a","hello":"world"}`))
	if err != nil {
		t.Fatalf("decode expected stdin mutation projection matches-only: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("stdin mutation projection matches-only mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLStdinMutationProjectionPreservesUnmatchedParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"uri":"/a","status":404,"drop":true}
{"uri":"/b","status":200,"drop":true}`
	cmd := exec.Command(
		clql,
		"-c",
		"-f", "/uri",
		"-m", "/hello=world",
		`/status=404`,
	)
	cmd.Stdin = bytes.NewBufferString(body)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql stdin mutation projection failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql stdin mutation projection: %v out=%q", err, string(out))
	}
	want, err := decodeJSONValues([]byte(`{"uri":"/a","hello":"world"}
{"uri":"/b"}`))
	if err != nil {
		t.Fatalf("decode expected stdin mutation projection: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("stdin mutation projection mismatch: got=%#v want=%#v out=%q", got, want, string(out))
	}
}

func TestCLQLStdinMutationMatchesOnlyParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	body := `{"id":"a","status":"open"}
{"id":"b","status":"open"}`
	cmd := exec.Command(
		clql,
		"-c",
		"-M",
		"-m", "/status=done",
		`/id="b"`,
	)
	cmd.Stdin = bytes.NewBufferString(body)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("clql stdin mutation matches-only failed: %v out=%q", err, string(out))
	}
	got, err := decodeJSONValues(out)
	if err != nil {
		t.Fatalf("decode clql stdin mutation matches-only: %v out=%q", err, string(out))
	}
	want, err := decodeJSONValues([]byte(`{"id":"b","status":"done"}`))
	if err != nil {
		t.Fatalf("decode expected stdin mutation matches-only: %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("stdin mutation matches-only mismatch: got=%#v want=%#v out=%q", got, want, string(out))
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

func TestCLQLInlineMutationRejectsInvalidInputs(t *testing.T) {
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
	cases := []struct {
		name   string
		args   []string
		stdin  string
		needle string
	}{
		{
			name:   "no file path",
			args:   []string{"-i", "-m", "/status=done", `contains{f=/}`},
			stdin:  `{"status":"open"}`,
			needle: "inline mode requires a file path",
		},
		{
			name:   "stdin marker",
			args:   []string{"-i", "-m", "/status=done", `contains{f=/}`, "-"},
			stdin:  `{"status":"open"}`,
			needle: "inline mode requires a single JSON file",
		},
		{
			name:   "multiple files",
			args:   []string{"-i", "-m", "/status=done", fileA, fileB},
			needle: "inline mode requires a single JSON file",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			cmd := exec.Command(clql, tc.args...)
			if tc.stdin != "" {
				cmd.Stdin = bytes.NewBufferString(tc.stdin)
			}
			out, err := cmd.CombinedOutput()
			if err == nil {
				t.Fatalf("inline mutation unexpectedly succeeded: out=%q", string(out))
			}
			if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 2 {
				t.Fatalf("inline rejection exit mismatch: err=%v out=%q", err, string(out))
			}
			if !bytes.Contains(out, []byte(tc.needle)) {
				t.Fatalf("inline rejection error mismatch: want %q out=%q", tc.needle, string(out))
			}
		})
	}
}

func TestCLQLInlineMutationExecutionErrors(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []struct {
		name   string
		body   string
		needle string
	}{
		{name: "empty input", body: "", needle: "no JSON input"},
		{name: "invalid JSON", body: `{"id":`, needle: "json"},
	}
	for _, flag := range []string{"-i", "-w"} {
		flag := flag
		for _, tc := range cases {
			tc := tc
			t.Run(flag+"/"+tc.name, func(t *testing.T) {
				path := filepath.Join(t.TempDir(), "input.json")
				if err := os.WriteFile(path, []byte(tc.body), 0600); err != nil {
					t.Fatalf("write input: %v", err)
				}
				cmd := exec.Command(
					clql,
					"-c",
					flag,
					"-m", "/status=done",
					`contains{f=/}`,
					path,
				)
				out, err := cmd.CombinedOutput()
				if err == nil {
					t.Fatalf("inline execution unexpectedly succeeded: out=%q", string(out))
				}
				if exitErr, ok := err.(*exec.ExitError); !ok || exitErr.ExitCode() != 1 {
					t.Fatalf("inline execution exit mismatch: err=%v out=%q", err, string(out))
				}
				if !bytes.Contains(bytes.ToLower(out), bytes.ToLower([]byte(tc.needle))) {
					t.Fatalf("inline execution diagnostic mismatch: want %q out=%q", tc.needle, string(out))
				}
				got, err := os.ReadFile(path)
				if err != nil {
					t.Fatalf("read input after failure: %v", err)
				}
				if string(got) != tc.body {
					t.Fatalf("inline failure changed input file: got=%q want=%q", string(got), tc.body)
				}
			})
		}
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
		`in{field=/env,any= prod | stage }`,
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

package parity

import (
	"bytes"
	"encoding/json"
	"os"
	"os/exec"
	"testing"

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

func TestCLQLSelectorParseErrorParity(t *testing.T) {
	clql := os.Getenv("CLQL_PATH")
	if clql == "" {
		t.Skip("CLQL_PATH not set")
	}
	cases := []string{
		`contains{field=/message,value=timeout,any=error}`,
		`contains{field=/message,value=timeout,value=error}`,
		`contains{field=/message,value=timeout,ignoreCase=maybe}`,
		`eq{field=/status,f=/other,value=open}`,
		`eq{field=/status,value=open,foo=bar}`,
		`eq{field=/status,value=open,ignoreCase=true}`,
		`range{field=/progress,gte=10,gte=20}`,
		`range{field=/progress,gte=10,foo=bar}`,
		`range{field=/progress,gte=10,lt=2025-01-01}`,
		`range{field=/timestamp,gte=yesterday}`,
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

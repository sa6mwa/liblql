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
		{`/progress>=50`, `{"progress":72}`},
		{`/progress<50`, `{"progress":72}`},
		{`contains{field=/message,value=timeout}`, `{"message":"upstream timeout"}`},
		{`icontains{field=/message,value=TIMEOUT}`, `{"message":"upstream timeout"}`},
		{`prefix{field=/service,value=auth}`, `{"service":"auth-api"}`},
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

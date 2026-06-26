//go:build cgo && liblql_sdk_parity

package parity

import (
	"bytes"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"

	"pkt.systems/lql"
)

type sdkSelectorMatchCase struct {
	expr string
	doc  string
}

func fromGoSelectorCapabilities(c lql.SelectorCapabilities) lqlSelectorCapabilities {
	return lqlSelectorCapabilities{
		and:           c.And,
		or:            c.Or,
		not:           c.Not,
		eq:            c.Eq,
		rng:           c.Range,
		date:          c.Date,
		in:            c.In,
		prefix:        c.Prefix,
		contains:      c.Contains,
		exists:        c.Exists,
		wildcardPath:  c.WildcardPath,
		recursivePath: c.RecursivePath,
	}
}

func fromGoSelectorExecutionTraits(t lql.SelectorExecutionTraits) lqlSelectorExecutionTraits {
	return lqlSelectorExecutionTraits{
		usesContainsLike:    t.UsesContainsLike,
		usesRecursivePath:   t.UsesRecursivePath,
		usesWildcardPath:    t.UsesWildcardPath,
		requiresObjectRoot:  t.RequiresObjectRoot,
		earlyNonMatchLikely: t.EarlyNonMatchLikely,
	}
}

func TestSDKSelectorMatchesJSONParity(t *testing.T) {
	cases := []sdkSelectorMatchCase{
		{`/status="open"`, `{"status":"open"}`},
		{` /status = "open" `, `{"status":"open"}`},
		{`/status="closed"`, `{"status":"open"}`},
		{`eq{field=/status,field=/status,value=open,value=open}`, `{"status":"open"}`},
		{`eq{field=/status value=open}`, `{"status":"open"}`},
		{`eq{field=/status,value='open,closed'}`, `{"status":"open,closed"}`},
		{"and.eq{\nfield=/message\nvalue=\"hi, world\"},and.eq{field=/status value=\"okili dokili\"}", `{"message":"hi, world","status":"okili dokili"}`},
		{"and.eq{\nfield=/message\nvalue=\"hi, world\"},and.eq{field=/status value=\"okili dokili\"}", `{"message":"hi","status":"okili dokili"}`},
		{`/progress>=50`, `{"progress":72}`},
		{` /progress >= 50 `, `{"progress":72}`},
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
		{`in{field=/greeting,any="hello world|goodbye jupiter"}`, `{"greeting":"goodbye jupiter"}`},
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
		{`exists{/items/.../sku}`, `{"items":[{"sku":"A"}]}`},
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
		{`and.0.eq{field=/status,value=open},and.1.or.0.in{field=/env,any=prod|stage},and.1.or.1.exists{/meta/etag}`, `{"status":"open","env":"dev","meta":{"etag":"x"}}`},
		{`and.0.eq{field=/status,value=open},and.1.or.0.in{field=/env,any=prod|stage},and.1.or.1.exists{/meta/etag}`, `{"status":"open","env":"dev","meta":{}}`},
		{`or.0.eq{field=/status,value=open},or.1.and.0.range{field=/progress,gte=10},or.1.and.0.exists{/meta/etag}`, `{"status":"closed","progress":11,"meta":{"etag":"x"}}`},
		{`or.0.eq{field=/status,value=open},or.1.and.0.range{field=/progress,gte=10},or.1.and.0.exists{/meta/etag}`, `{"status":"closed","progress":11,"meta":{}}`},
		{`and.or.0.eq{field=/status,value=open}`, `{"status":"open"}`},
		{`and.or.0.eq{field=/status,value=open}`, `{"status":"closed"}`},
		{`or.and.0.eq{field=/status,value=open}`, `{"status":"open"}`},
		{`or.and.0.eq{field=/status,value=open}`, `{"status":"closed"}`},
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
			got, err := cMatchesJSON(tc.expr, tc.doc, false)
			if err != nil {
				t.Fatalf("liblql match: %v", err)
			}
			if got != want {
				t.Fatalf("liblql selector parity mismatch: got match=%v want=%v", got, want)
			}
		})
	}
}

func TestSDKSelectorMatchesJSONOrParity(t *testing.T) {
	cases := []sdkSelectorMatchCase{
		{`/status="open",/progress>=50`, `{"status":"closed","progress":72}`},
		{`/status="open",/progress>=50`, `{"status":"closed","progress":4}`},
		{"/status=\"open\"\n/progress>=50", `{"status":"closed","progress":72}`},
		{"/status=\"open\"\n/progress>=50", `{"status":"closed","progress":4}`},
		{`eq{field=/status,value=open},range{field=/progress,gte=50}`, `{"status":"open","progress":4}`},
		{`eq{field=/status,value=open},range{field=/progress,gte=50}`, `{"status":"closed","progress":72}`},
	}
	for _, tc := range cases {
		t.Run(tc.expr+"/"+tc.doc, func(t *testing.T) {
			var doc map[string]any
			if err := json.Unmarshal([]byte(tc.doc), &doc); err != nil {
				t.Fatal(err)
			}
			sel, err := lql.ParseSelectorStringOr(tc.expr)
			if err != nil {
				t.Fatalf("go parse OR: %v", err)
			}
			want := lql.Matches(sel, doc)
			got, err := cMatchesJSON(tc.expr, tc.doc, true)
			if err != nil {
				t.Fatalf("liblql OR match: %v", err)
			}
			if got != want {
				t.Fatalf("liblql selector OR parity mismatch: got match=%v want=%v", got, want)
			}
		})
	}
}

func TestSDKSelectorParseErrorParity(t *testing.T) {
	cases := []string{
		`contains{field=/message,value=timeout,any=error}`,
		`icontains{field=/message,value=timeout,any=error}`,
		`contains{field=/message,any=}`,
		`contains{field=/message,any=||}`,
		`contains{field=/message,value=timeout,value=error}`,
		`contains{field=/message,value=timeout,ignoreCase=maybe}`,
		`eq{field=/status,f=/other,value=open}`,
		`eq{field=/msg,any=foo|bar}`,
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
		`iprefix{field=/service,any=auth|edge}`,
		`in{field=/env,any=}`,
		`in{field=/env}`,
		`in{any=prod|stage}`,
		`in{field=/env,any= prod | stage }`,
		`in{field=/env,any=prod|stage,a=dev}`,
		`in{field=/env,any=prod|stage,foo=bar}`,
		`range{field=/progress}`,
		`range{gte=10}`,
		`exists{}`,
		`exists{/meta/etag,field=/status}`,
		`exists{/meta/etag,/meta/id}`,
		`exists{field=/meta/etag}`,
		`and..eq{field=/status,value=open}`,
		`and.foo.eq{field=/status,value=open}`,
		`or.0.and.foo.exists{/meta/etag}`,
		`or.0..eq{field=/status,value=open}`,
		`nonsense`,
		`eq{field=/status,value=open},nonsense`,
		`eq{field=/status,value="open}`,
		`and.eq{field=/status,value=open`,
		`eq{field=/status,value=open}}`,
		`/count>=`,
	}
	for _, expr := range cases {
		t.Run(expr, func(t *testing.T) {
			if _, err := lql.ParseSelectorString(expr); err == nil {
				t.Fatalf("go parse unexpectedly succeeded")
			}
			if status, message := cParseSelector(expr, false); status == 0 {
				t.Fatalf("liblql parse unexpectedly succeeded: %s", message)
			}
		})
	}
}

func TestSDKSelectorInspectionParity(t *testing.T) {
	cases := []struct {
		name  string
		expr  string
		empty bool
		or    bool
	}{
		{name: "empty", empty: true},
		{name: "families", expr: `and.eq{field=/status,value=open},and.range{field=/progress,gte=5},or.in{field=/env,any=prod|stage},not.eq{field=/state,value=disabled},exists{/meta/etag},icontains{field=/msg,value=timeout},iprefix{field=/service,value=auth}`},
		{name: "path-complexity", expr: `/items[]/sku="A",/groups/**/sku="B",exists{/meta/.../etag}`},
		{name: "traits-recursive", expr: `and.eq{field=/status,value=open},icontains{field=/msg,value=timeout},exists{/meta/**/etag}`},
		{name: "match-all-string", expr: `icontains{f=/,v=""}`},
		{name: "simple", expr: `/status="open"`},
		{name: "top-level-or", expr: `/status="open",/progress>=50`, or: true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var sel lql.Selector
			var err error
			if !tc.empty {
				if tc.or {
					sel, err = lql.ParseSelectorStringOr(tc.expr)
				} else {
					sel, err = lql.ParseSelectorString(tc.expr)
				}
				if err != nil {
					t.Fatalf("Go parse: %v", err)
				}
			}
			wantCaps := fromGoSelectorCapabilities(lql.InspectSelectorCapabilities(sel))
			wantTraits := fromGoSelectorExecutionTraits(lql.InspectSelectorExecutionTraits(sel))
			got, err := cInspectSelector(tc.expr, tc.empty, tc.or)
			if err != nil {
				t.Fatalf("C inspect: %v", err)
			}
			if !reflect.DeepEqual(got.capabilities, wantCaps) {
				t.Fatalf("capabilities mismatch\ngot:  %+v\nwant: %+v", got.capabilities, wantCaps)
			}
			if !reflect.DeepEqual(got.traits, wantTraits) {
				t.Fatalf("traits mismatch\ngot:  %+v\nwant: %+v", got.traits, wantTraits)
			}
		})
	}
}

func TestSDKProjectionJSONParity(t *testing.T) {
	for _, tc := range sdkProjectionCases() {
		t.Run(tc.name, func(t *testing.T) {
			wantJSON, wantFound, err := goProjectJSON(tc.fields, tc.doc)
			if err != nil {
				t.Fatalf("go project: %v", err)
			}
			gotJSON, gotFound, err := cProjectJSON(tc.fields, tc.doc)
			if err != nil {
				t.Fatalf("liblql project: %v", err)
			}
			assertProjectionJSONParity(t, gotJSON, gotFound, wantJSON, wantFound)
		})
	}
}

func TestSDKProjectionFileRangeParity(t *testing.T) {
	for _, tc := range sdkProjectionCases() {
		t.Run(tc.name, func(t *testing.T) {
			wantJSON, wantFound, err := goProjectJSON(tc.fields, tc.doc)
			if err != nil {
				t.Fatalf("go project: %v", err)
			}
			gotJSON, gotFound, err := cProjectFileRange(tc.fields, `{"outside":`, tc.doc, `}`)
			if err != nil {
				t.Fatalf("liblql file-range project: %v", err)
			}
			assertProjectionJSONParity(t, gotJSON, gotFound, wantJSON, wantFound)
		})
	}
}

func TestSDKProjectionSourceParity(t *testing.T) {
	for _, tc := range sdkProjectionCases() {
		t.Run(tc.name, func(t *testing.T) {
			wantJSON, wantFound, err := goProjectJSON(tc.fields, tc.doc)
			if err != nil {
				t.Fatalf("go project: %v", err)
			}
			gotJSON, gotFound, err := cProjectSource(tc.fields, tc.doc)
			if err != nil {
				t.Fatalf("liblql source project: %v", err)
			}
			assertProjectionJSONParity(t, gotJSON, gotFound, wantJSON, wantFound)
		})
	}
}

func sdkProjectionCases() []struct {
	name   string
	fields []string
	doc    string
} {
	return []struct {
		name   string
		fields []string
		doc    string
	}{
		{
			name:   "single root field",
			fields: []string{"/id"},
			doc:    `{"status":"open","id":"b","count":2}`,
		},
		{
			name:   "multiple root fields",
			fields: []string{"/id", "/count"},
			doc:    `{"status":"open","id":"b","count":2}`,
		},
		{
			name:   "nested object and array field",
			fields: []string{"/id", "/meta/trace", "/meta/span", "/items/1/sku"},
			doc:    `{"status":"open","id":"a","meta":{"trace":9,"span":"s","ignore":true},"items":[{"sku":"A"},{"sku":"B"}]}`,
		},
		{
			name:   "escaped pointer fields",
			fields: []string{"/a~1b/~0key"},
			doc:    `{"status":"open","a/b":{"~key":7},"id":"a"}`,
		},
		{
			name:   "missing root field suppresses output",
			fields: []string{"/missing"},
			doc:    `{"status":"open","id":"a"}`,
		},
		{
			name:   "duplicate field is idempotent",
			fields: []string{"/id", "/id"},
			doc:    `{"status":"open","id":"a","other":true}`,
		},
	}
}

func TestSDKProjectionErrorParity(t *testing.T) {
	conflicts := [][]string{
		{"/meta", "/meta/trace"},
		{"/items/0/sku", "/items"},
	}
	for _, fields := range conflicts {
		t.Run("conflict", func(t *testing.T) {
			paths, err := lql.ParseProjectionPaths(fields)
			if err != nil {
				t.Fatalf("go projection parse: %v", err)
			}
			if _, err := lql.NewProjectionPlan(paths); err == nil {
				t.Fatalf("go projection plan unexpectedly accepted fields: %#v", fields)
			}
			if _, _, err := cProjectJSON(fields, `{"meta":{"trace":7},"items":[{"sku":"A"}]}`); err == nil {
				t.Fatalf("liblql projection unexpectedly accepted fields: %#v", fields)
			}
		})
	}

	if _, _, err := cProjectJSON([]string{"/id"}, `7`); err == nil {
		t.Fatalf("liblql projection unexpectedly accepted scalar root")
	}
	if _, _, err := goProjectJSON([]string{"/id"}, `7`); err == nil {
		t.Fatalf("go projection unexpectedly accepted scalar root")
	}
}

func TestSDKProjectionParseErrorParity(t *testing.T) {
	cases := []struct {
		name   string
		fields []string
	}{
		{name: "empty field set", fields: nil},
		{name: "blank field set", fields: []string{"  ", "\t"}},
		{name: "root path", fields: []string{"/"}},
		{name: "missing leading slash", fields: []string{"id"}},
		{name: "leading array index", fields: []string{"/0/id"}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			paths, goErr := lql.ParseProjectionPaths(tc.fields)
			if goErr == nil {
				_, goErr = lql.NewProjectionPlan(paths)
			}
			if goErr == nil {
				t.Fatalf("go projection unexpectedly accepted fields: %#v", tc.fields)
			}
			if status, message := cParseProjection(tc.fields); status == 0 {
				t.Fatalf("liblql projection unexpectedly accepted fields: %#v message=%q", tc.fields, message)
			}
		})
	}
}

func TestSDKProjectionExecutionErrorParity(t *testing.T) {
	fields := []string{"/id"}
	cases := []struct {
		name string
		doc  string
	}{
		{name: "truncated object", doc: `{"id":`},
		{name: "trailing comma", doc: `{"id":"a",}`},
		{name: "invalid literal", doc: `{"id": tru}`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, _, err := goProjectJSON(fields, tc.doc); err == nil {
				t.Fatalf("go projection unexpectedly accepted malformed JSON: %q", tc.doc)
			}
			if _, _, err := cProjectJSON(fields, tc.doc); err == nil {
				t.Fatalf("liblql buffered projection unexpectedly accepted malformed JSON: %q", tc.doc)
			}
			if _, _, err := cProjectSource(fields, tc.doc); err == nil {
				t.Fatalf("liblql source projection unexpectedly accepted malformed JSON: %q", tc.doc)
			}
			if _, _, err := cProjectFileRange(fields, `{"outside":`, tc.doc, `}`); err == nil {
				t.Fatalf("liblql file-range projection unexpectedly accepted malformed JSON: %q", tc.doc)
			}
		})
	}
}

func TestSDKMutationJSONParity(t *testing.T) {
	for _, tc := range sdkMutationCases() {
		t.Run(tc.name, func(t *testing.T) {
			wantJSON, err := goMutateJSON(tc.mutations, tc.doc)
			if err != nil {
				t.Fatalf("go mutate: %v", err)
			}
			gotJSON, err := cMutateJSON(tc.mutations, tc.doc)
			if err != nil {
				t.Fatalf("liblql mutate: %v", err)
			}
			assertDecodedJSONValuesParity(t, gotJSON, wantJSON, "mutation")
		})
	}
}

func TestSDKMutationSourceParity(t *testing.T) {
	for _, tc := range sdkMutationCases() {
		t.Run(tc.name, func(t *testing.T) {
			wantJSON, err := goMutateJSON(tc.mutations, tc.doc)
			if err != nil {
				t.Fatalf("go mutate: %v", err)
			}
			gotJSON, err := cMutateSource(tc.mutations, tc.doc)
			if err != nil {
				t.Fatalf("liblql source mutate: %v", err)
			}
			assertDecodedJSONValuesParity(t, gotJSON, wantJSON, "mutation")
		})
	}
}

func TestSDKMutationFileRangeParity(t *testing.T) {
	for _, tc := range sdkMutationCases() {
		t.Run(tc.name, func(t *testing.T) {
			wantJSON, err := goMutateJSON(tc.mutations, tc.doc)
			if err != nil {
				t.Fatalf("go mutate: %v", err)
			}
			gotJSON, err := cMutateFileRange(tc.mutations, `{"outside":`, tc.doc, `}`)
			if err != nil {
				t.Fatalf("liblql file-range mutate: %v", err)
			}
			assertDecodedJSONValuesParity(t, gotJSON, wantJSON, "mutation")
		})
	}
}

func TestSDKMutationFileRangeCandidateStreamParity(t *testing.T) {
	cases := []struct {
		name        string
		selector    string
		doc         string
		mutations   []string
		matchesOnly bool
	}{
		{
			name:      "top-level array match all",
			doc:       `[{"id":"a","status":"open"},{"id":"b","status":"open"}]`,
			mutations: []string{`/status=done`},
		},
		{
			name:        "top-level array matches only",
			selector:    `/status="open"`,
			doc:         `[{"id":"a","status":"open"},{"id":"b","status":"closed"}]`,
			mutations:   []string{`/status=done`},
			matchesOnly: true,
		},
		{
			name:      "nested top-level array match all",
			doc:       `[{"id":"a","status":"open"},[{"id":"b","status":"open"}],{"id":"c","status":"closed"}]`,
			mutations: []string{`/status=done`},
		},
		{
			name:        "nested top-level array selector",
			selector:    `/status="open"`,
			doc:         `[{"id":"a","status":"open"},[{"id":"b","status":"open"}],{"id":"c","status":"closed"}]`,
			mutations:   []string{`/status=done`},
			matchesOnly: false,
		},
		{
			name:        "nested top-level array matches only",
			selector:    `/status="open"`,
			doc:         `[{"id":"a","status":"open"},[{"id":"b","status":"open"}],{"id":"c","status":"closed"}]`,
			mutations:   []string{`/status=done`},
			matchesOnly: true,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var wantJSON []byte
			var err error
			if tc.selector == "" {
				wantJSON, err = goMutateJSON(tc.mutations, tc.doc)
				if err != nil {
					t.Fatalf("go mutate stream: %v", err)
				}
			} else {
				wantJSON, err = goQueryCandidateMutateJSON(tc.selector, tc.mutations, tc.doc, tc.matchesOnly)
				if err != nil {
					t.Fatalf("go query candidate mutate stream: %v", err)
				}
			}
			gotJSON, err := cMutateFileRangeCandidates(tc.selector, tc.mutations, `{"outside":`, tc.doc, `}`, tc.matchesOnly)
			if err != nil {
				t.Fatalf("liblql file-range candidate mutate: %v", err)
			}
			assertDecodedJSONValuesParity(t, gotJSON, wantJSON, "candidate stream mutation")

			gotSourceJSON, err := cMutateSourceCandidates(tc.selector, tc.mutations, tc.doc, tc.matchesOnly)
			if err != nil {
				t.Fatalf("liblql source candidate mutate: %v", err)
			}
			assertDecodedJSONValuesParity(t, gotSourceJSON, wantJSON, "source candidate stream mutation")
		})
	}
}

func TestSDKMutationProjectedCandidateStreamParity(t *testing.T) {
	cases := []struct {
		name        string
		selector    string
		fields      []string
		doc         string
		mutations   []string
		matchesOnly bool
	}{
		{
			name:     "preserve unmatched projected candidates",
			selector: `/status=404`,
			fields:   []string{`/uri`},
			doc: `{"uri":"/a","status":404,"drop":true}
{"uri":"/b","status":200,"drop":true}`,
			mutations: []string{`/hello=world`},
		},
		{
			name:        "matches only projected candidates",
			selector:    `/status=404`,
			fields:      []string{`/uri`},
			doc:         `[{"uri":"/a","status":404,"drop":true},{"uri":"/b","status":200,"drop":true}]`,
			mutations:   []string{`/hello=world`},
			matchesOnly: true,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			wantJSON, err := goQueryProjectMutateJSON(tc.selector, tc.fields, tc.mutations, tc.doc, tc.matchesOnly)
			if err != nil {
				t.Fatalf("go query project mutate stream: %v", err)
			}
			gotRangeJSON, err := cMutateFileRangeProjectedCandidates(tc.selector, tc.fields, tc.mutations, `{"outside":`, tc.doc, `}`, tc.matchesOnly)
			if err != nil {
				t.Fatalf("liblql file-range projected candidate mutate: %v", err)
			}
			assertDecodedJSONValuesParity(t, gotRangeJSON, wantJSON, "file-range projected candidate stream mutation")

			gotSourceJSON, err := cMutateSourceProjectedCandidates(tc.selector, tc.fields, tc.mutations, tc.doc, tc.matchesOnly)
			if err != nil {
				t.Fatalf("liblql source projected candidate mutate: %v", err)
			}
			assertDecodedJSONValuesParity(t, gotSourceJSON, wantJSON, "source projected candidate stream mutation")
		})
	}
}

func TestSDKMutationRootFieldFileRangeParity(t *testing.T) {
	cases := []struct {
		name      string
		doc       string
		mutations []string
	}{
		{
			name: "root set increment delete create",
			doc:  `{"status":"open","count":1,"old":true}`,
			mutations: []string{
				"/status=done",
				"/count++",
				"rm:/old",
				"/missing=value",
				`/quoted_number="2"`,
			},
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			wantJSON, err := goMutateJSON(tc.mutations, tc.doc)
			if err != nil {
				t.Fatalf("go root mutate: %v", err)
			}
			gotJSON, err := cMutateFileRangeRootFields(tc.mutations, `{"outside":`, tc.doc, `}`)
			if err != nil {
				t.Fatalf("liblql root-field file-range mutate: %v", err)
			}
			assertDecodedJSONValuesParity(t, gotJSON, wantJSON, "root-field mutation")
		})
	}
}

func TestSDKMutationFileBackedValueParity(t *testing.T) {
	dir := t.TempDir()
	textPath := filepath.Join(dir, "blob.txt")
	binPath := filepath.Join(dir, "blob.bin")
	if err := os.WriteFile(textPath, []byte("hello\n\"quoted\""), 0600); err != nil {
		t.Fatalf("write text payload: %v", err)
	}
	if err := os.WriteFile(binPath, []byte{0x00, 0x01, 0x02, 'a'}, 0600); err != nil {
		t.Fatalf("write binary payload: %v", err)
	}
	mutations := []string{
		`textfile:/payload=blob.txt`,
		`base64file:/encoded=blob.bin`,
		`file:/auto_text=blob.txt`,
		`file:/auto_bin=blob.bin`,
	}
	opts := lql.ParseMutationsOptions{
		EnableFileValues: true,
		FileValueBaseDir: dir,
	}
	wantJSON, err := goMutateJSONWithOptions(mutations, `{}`, opts)
	if err != nil {
		t.Fatalf("go mutate file-backed values: %v", err)
	}
	gotBuffered, err := cMutateJSONWithOptions(mutations, `{}`, true, dir)
	if err != nil {
		t.Fatalf("liblql buffered file-backed mutate: %v", err)
	}
	gotSource, err := cMutateSourceWithOptions(mutations, `{}`, true, dir)
	if err != nil {
		t.Fatalf("liblql source file-backed mutate: %v", err)
	}
	gotRange, err := cMutateFileRangeWithOptions(mutations, `{"outside":`, `{}`, `}`, true, dir)
	if err != nil {
		t.Fatalf("liblql file-range file-backed mutate: %v", err)
	}
	assertDecodedJSONValuesParity(t, gotBuffered, wantJSON, "buffered file-backed mutation")
	assertDecodedJSONValuesParity(t, gotSource, wantJSON, "source file-backed mutation")
	assertDecodedJSONValuesParity(t, gotRange, wantJSON, "file-range file-backed mutation")
}

func TestSDKMutationFileBackedTextValidationParity(t *testing.T) {
	cases := []struct {
		name     string
		fileName string
		payload  []byte
	}{
		{name: "invalid UTF-8", fileName: "invalid.txt", payload: []byte{'h', 'i', 0xff}},
		{name: "NUL byte", fileName: "nul.txt", payload: []byte{'h', 'i', 0x00, 'x'}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			path := filepath.Join(dir, tc.fileName)
			if err := os.WriteFile(path, tc.payload, 0600); err != nil {
				t.Fatalf("write text payload: %v", err)
			}
			mutations := []string{`textfile:/payload=` + tc.fileName}
			opts := lql.ParseMutationsOptions{
				EnableFileValues: true,
				FileValueBaseDir: dir,
			}
			if _, err := goMutateJSONWithOptions(mutations, `{}`, opts); err == nil {
				t.Fatalf("go mutation unexpectedly accepted %s textfile", tc.name)
			}
			if _, err := cMutateJSONWithOptions(mutations, `{}`, true, dir); err == nil {
				t.Fatalf("liblql buffered mutation unexpectedly accepted %s textfile", tc.name)
			}
			if _, err := cMutateSourceWithOptions(mutations, `{}`, true, dir); err == nil {
				t.Fatalf("liblql source mutation unexpectedly accepted %s textfile", tc.name)
			}
			if _, err := cMutateFileRangeWithOptions(mutations, `{"outside":`, `{}`, `}`, true, dir); err == nil {
				t.Fatalf("liblql file-range mutation unexpectedly accepted %s textfile", tc.name)
			}
		})
	}
}

func TestSDKMutationPlanParseParity(t *testing.T) {
	cases := []struct {
		name             string
		mutations        []string
		stringInput      bool
		enableFileValues bool
		baseDir          string
	}{
		{
			name: "brace pointer time and remove",
			mutations: []string{
				"/state/progress=ready",
				"/state/metrics++",
				`/state/details{/owner="alice",/note="hi, world"}`,
				"/state/metrics=+3",
				"time:/state/updated=NOW",
				"rm:/state/legacy",
			},
		},
		{
			name: "wildcard and recursive paths",
			mutations: []string{
				"/items/*/status=ready",
				"/groups/.../sku=ok",
				"/records[]/count=+1",
			},
		},
		{
			name:        "single newline separated expression string",
			mutations:   []string{"/state/status=ready\n/state/count=+2\nrm:/state/old"},
			stringInput: true,
		},
		{
			name:             "explicit file backed values",
			mutations:        []string{`textfile:/payload=blob.txt`, `base64file:/encoded=blob.bin`},
			enableFileValues: true,
			baseDir:          "temp",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			baseDir := tc.baseDir
			if baseDir == "temp" {
				baseDir = t.TempDir()
			}
			opts := lql.ParseMutationsOptions{
				EnableFileValues: tc.enableFileValues,
				FileValueBaseDir: baseDir,
			}
			var want []lql.Mutation
			var err error
			if tc.stringInput {
				want, err = lql.ParseMutationsString(tc.mutations[0], time.Unix(1700000000, 0))
			} else {
				want, err = lql.ParseMutationsWithOptions(tc.mutations, time.Unix(1700000000, 0), opts)
			}
			if err != nil {
				t.Fatalf("go mutation parse: %v", err)
			}
			status, count, message := cParseMutations(tc.mutations, tc.enableFileValues, baseDir)
			if status != 0 {
				t.Fatalf("liblql mutation parse failed: status=%d message=%s", status, message)
			}
			if count != len(want) {
				t.Fatalf("liblql mutation plan count mismatch: got=%d want=%d", count, len(want))
			}
		})
	}
}

func TestSDKMutationParseErrorParity(t *testing.T) {
	cases := []struct {
		name             string
		mutations        []string
		enableFileValues bool
		baseDir          string
	}{
		{name: "empty", mutations: nil},
		{name: "blank only", mutations: []string{""}},
		{name: "bad expression", mutations: []string{`badexpr`}},
		{name: "root path", mutations: []string{`/`}},
		{name: "zero increment", mutations: []string{`/count=+0`}},
		{name: "invalid time", mutations: []string{`time:/state/updated=tomorrowish`}},
		{name: "date only time value", mutations: []string{`time:/state/updated=2025-01-01`}},
		{name: "disabled file backed value", mutations: []string{`file:/payload=blob.txt`}},
		{name: "relative file backed without base dir", mutations: []string{`file:/payload=blob.txt`}, enableFileValues: true},
		{name: "file backed increment", mutations: []string{`file:/payload++`}, enableFileValues: true, baseDir: "temp"},
		{name: "file backed remove", mutations: []string{`file:rm:/payload=blob.txt`}, enableFileValues: true, baseDir: "temp"},
		{name: "file backed time", mutations: []string{`file:time:/payload=blob.txt`}, enableFileValues: true, baseDir: "temp"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			baseDir := tc.baseDir
			if baseDir == "temp" {
				baseDir = t.TempDir()
			}
			opts := lql.ParseMutationsOptions{
				EnableFileValues: tc.enableFileValues,
				FileValueBaseDir: baseDir,
			}
			if _, err := lql.ParseMutationsWithOptions(tc.mutations, time.Unix(1700000000, 0), opts); err == nil {
				t.Fatalf("go mutation parse unexpectedly succeeded")
			}
			if status, _, message := cParseMutations(tc.mutations, tc.enableFileValues, baseDir); status == 0 {
				t.Fatalf("liblql mutation parse unexpectedly succeeded: %s", message)
			}
		})
	}
}

func sdkMutationCases() []struct {
	name      string
	doc       string
	mutations []string
} {
	return []struct {
		name      string
		doc       string
		mutations []string
	}{
		{
			name: "root set increment delete create",
			doc:  `{"status":"open","count":1,"score":5,"old":true,"remove_me":true,"delete_me":true,"del_me":true}`,
			mutations: []string{
				"/status=done",
				"/count--",
				"/score=-2",
				"rm:/old",
				"remove:/remove_me",
				"delete:/delete_me",
				"del:/del_me",
				"/missing=value",
			},
		},
		{
			name: "nested set increment delete create",
			doc:  `{"state":{"status":"open","count":1,"old":true},"id":"a"}`,
			mutations: []string{
				"/state/status=done",
				"/state/count++",
				"rm:/state/old",
				"/state/missing=value",
				"/added/nested=ok",
				"/added/other=2",
			},
		},
		{
			name: "array element set",
			doc:  `{"items":[{"sku":"A","qty":1},{"sku":"B","qty":2}]}`,
			mutations: []string{
				"/items/1/sku=C",
				"/items/0/qty=+3",
			},
		},
		{
			name: "numeric object path segment",
			doc:  `{"voucher":{"lines":{"10":{"amount":5,"status":"open","code":"before"}}}}`,
			mutations: []string{
				`/voucher/lines/10/amount=+2`,
				`/voucher/lines/10/status=patched`,
				`/voucher/.../10/code=patched`,
			},
		},
		{
			name: "numeric path segment creates object key",
			doc:  `{"voucher":{"lines":{}}}`,
			mutations: []string{
				`/voucher/lines/10/status=created`,
				`/voucher/lines/10/amount=+3`,
			},
		},
		{
			name: "quoted numeric typing follows Go",
			doc:  `{"value":"old"}`,
			mutations: []string{
				`/value="42"`,
			},
		},
		{
			name: "escaped JSON pointer paths",
			doc:  `{"a/b":{"~key":"old","count":1},"plain":true}`,
			mutations: []string{
				`/a~1b/~0key=done`,
				`/a~1b/count=+2`,
				`/a~1b/missing=value`,
			},
		},
		{
			name: "wildcard object and array paths",
			doc:  `{"items":[{"status":"old"},{"status":"new"}],"labels":{"env":"prod","tier":"edge"},"numeric_object":{"0":{"status":"unchanged"}}}`,
			mutations: []string{
				`/items[]/status=ready`,
				`/labels/*=tagged`,
				`/numeric_object[]/status=bad`,
			},
		},
		{
			name: "recursive one child segments",
			doc:  `{"items":[{"status":"old"}],"boxes":{"a":{"status":"old"}},"groups":[{"items":[{"sku":"A","count":1,"drop":true}]}]}`,
			mutations: []string{
				`/items/**/status=ready`,
				`/boxes/**/status=ready`,
				`/groups/.../sku=Z`,
				`/groups/.../count=+2`,
			},
		},
		{
			name: "array wildcard value mutation",
			doc:  `{"nums":[1,2],"words":["a","b"],"drops":[true,false],"objects":[{"a":1}],"groups":[{"items":[{"count":1}]}]}`,
			mutations: []string{
				`/nums[]=+2`,
				`/words[]=ready`,
				`rm:/drops[]`,
				`/objects[]=done`,
				`/groups/.../count=+2`,
			},
		},
	}
}

func TestSDKMutationExecutionErrorParity(t *testing.T) {
	cases := []struct {
		name      string
		mutations []string
		doc       string
	}{
		{name: "truncated object", mutations: []string{"/status=done", "/count++"}, doc: `{"status":"open","count":`},
		{name: "trailing comma", mutations: []string{"/status=done", "/count++"}, doc: `{"status":"open","count":1,}`},
		{name: "invalid literal", mutations: []string{"/status=done", "/count++"}, doc: `{"status":tru,"count":1}`},
		{name: "later parent set does not mask earlier wildcard increment error", mutations: []string{`/a/*=+1`, `/a=2`}, doc: `{"a":{"b":"x"}}`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := goMutateJSON(tc.mutations, tc.doc); err == nil {
				t.Fatalf("go mutation unexpectedly accepted malformed JSON: %q", tc.doc)
			}
			if _, err := cMutateJSON(tc.mutations, tc.doc); err == nil {
				t.Fatalf("liblql buffered mutation unexpectedly accepted malformed JSON: %q", tc.doc)
			}
			if _, err := cMutateSource(tc.mutations, tc.doc); err == nil {
				t.Fatalf("liblql source mutation unexpectedly accepted malformed JSON: %q", tc.doc)
			}
			if _, err := cMutateFileRange(tc.mutations, `{"outside":`, tc.doc, `}`); err == nil {
				t.Fatalf("liblql file-range mutation unexpectedly accepted malformed JSON: %q", tc.doc)
			}
		})
	}
}

func TestSDKCompactParity(t *testing.T) {
	cases := []struct {
		name string
		doc  string
	}{
		{name: "object", doc: "{ \"id\" : 1, \"items\" : [ true, null, \"x\" ] }"},
		{name: "array", doc: "[ { \"id\" : 1 }, { \"id\" : 2 } ]"},
		{name: "escaped string", doc: "{ \"message\" : \"line\\nfeed\", \"slash\" : \"a/b\" }"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			wantJSON, err := goCompactJSON(tc.doc)
			if err != nil {
				t.Fatalf("go compact: %v", err)
			}
			gotBuffered, err := cCompactJSON(tc.doc)
			if err != nil {
				t.Fatalf("liblql compact json: %v", err)
			}
			gotSource, err := cCompactSource(tc.doc)
			if err != nil {
				t.Fatalf("liblql compact source: %v", err)
			}
			gotRange, err := cCompactFileRange(`{"outside":`, tc.doc, `}`)
			if err != nil {
				t.Fatalf("liblql compact file range: %v", err)
			}
			if !bytes.Equal(gotBuffered, wantJSON) {
				t.Fatalf("buffered compact mismatch: got=%q want=%q", string(gotBuffered), string(wantJSON))
			}
			if !bytes.Equal(gotSource, wantJSON) {
				t.Fatalf("source compact mismatch: got=%q want=%q", string(gotSource), string(wantJSON))
			}
			if !bytes.Equal(gotRange, wantJSON) {
				t.Fatalf("file-range compact mismatch: got=%q want=%q", string(gotRange), string(wantJSON))
			}
		})
	}
}

func TestSDKCompactErrorParity(t *testing.T) {
	cases := []struct {
		name string
		doc  string
	}{
		{name: "truncated object", doc: `{"id":`},
		{name: "trailing comma", doc: `[1,]`},
		{name: "invalid literal", doc: `{"ok":tru}`},
		{name: "trailing token", doc: `{"ok":true} false`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := goCompactJSON(tc.doc); err == nil {
				t.Fatalf("go compact unexpectedly accepted invalid JSON: %q", tc.doc)
			}
			if _, err := cCompactJSON(tc.doc); err == nil {
				t.Fatalf("liblql buffered compact unexpectedly accepted invalid JSON: %q", tc.doc)
			}
			if _, err := cCompactSource(tc.doc); err == nil {
				t.Fatalf("liblql source compact unexpectedly accepted invalid JSON: %q", tc.doc)
			}
			if _, err := cCompactFileRange(`{"outside":`, tc.doc, `}`); err == nil {
				t.Fatalf("liblql file-range compact unexpectedly accepted invalid JSON: %q", tc.doc)
			}
		})
	}
}

func TestSDKStreamingDecisionParity(t *testing.T) {
	cases := []struct {
		name string
		expr string
		doc  string
	}{
		{
			name: "ndjson candidates",
			expr: `/status="open"`,
			doc:  "{\"status\":\"open\",\"id\":1}\n{\"status\":\"closed\",\"id\":2}\n{\"status\":\"open\",\"id\":3}\n",
		},
		{
			name: "array candidates",
			expr: `/status="open"`,
			doc:  `[{"status":"open","id":1},{"status":"closed","id":2},{"status":"open","id":3}]`,
		},
		{
			name: "nested array candidates",
			expr: `/id="b"`,
			doc:  `[{"id":"a"},[{"id":"b"}],{"id":"c"}]`,
		},
		{
			name: "mixed scalar and object candidates",
			expr: `/id="x"`,
			doc:  "\"x\"\n{\"id\":\"x\"}\n123\n",
		},
		{
			name: "match-all string term over mixed scalar candidates",
			expr: `icontains{f=/,v=""}`,
			doc:  "\"x\"\n{\"id\":\"x\"}\n123\n",
		},
	}
	for _, tc := range cases {
		for _, mode := range []struct {
			name string
			id   int
		}{
			{name: "file", id: 0},
			{name: "source", id: 3},
		} {
			t.Run(tc.name+"/"+mode.name, func(t *testing.T) {
				want, err := goStreamQuery(tc.expr, tc.doc, mode.id, 0, 0, 0, false)
				if err != nil {
					t.Fatalf("go stream decision: %v", err)
				}
				got, err := cStreamQuery(tc.expr, tc.doc, mode.id, 0, 0, 0, false)
				if err != nil {
					t.Fatalf("liblql stream decision: %v", err)
				}
				assertStreamSummaryParity(t, got, want)
				if got.DecisionCallbacks != want.DecisionCallbacks {
					t.Fatalf("decision callback mismatch: got=%d want=%d", got.DecisionCallbacks, want.DecisionCallbacks)
				}
			})
		}
	}
}

func TestSDKStreamingNestedArrayFileDecisionParity(t *testing.T) {
	doc := `[{"id":"a"},[{"id":"b"}],{"id":"c"}]`
	want, err := goStreamQuery(`/id="b"`, doc, 0, 0, 0, 0, false)
	if err != nil {
		t.Fatalf("go nested array file decision: %v", err)
	}
	got, err := cStreamQuery(`/id="b"`, doc, 0, 0, 0, 0, false)
	if err != nil {
		t.Fatalf("liblql nested array file decision: %v", err)
	}
	assertStreamSummaryParity(t, got, want)
	if got.DecisionCallbacks != want.DecisionCallbacks {
		t.Fatalf("nested array decision callback mismatch: got=%d want=%d", got.DecisionCallbacks, want.DecisionCallbacks)
	}
}

func TestSDKStreamingPayloadParity(t *testing.T) {
	for _, tc := range []struct {
		name                 string
		expr                 string
		doc                  string
		mode                 int
		wantSeekablePayloads int
		wantSpooledPayloads  int
	}{
		{
			name:                 "seekable file ranges",
			expr:                 `/status="open"`,
			doc:                  "{\"status\":\"open\",\"id\":1}\n{\"status\":\"closed\",\"id\":2}\n{\"status\":\"open\",\"id\":3}\n",
			mode:                 1,
			wantSeekablePayloads: 2,
		},
		{
			name:                "callback source spooled payloads",
			expr:                `/status="open"`,
			doc:                 "{\"status\":\"open\",\"id\":1}\n{\"status\":\"closed\",\"id\":2}\n{\"status\":\"open\",\"id\":3}\n",
			mode:                2,
			wantSpooledPayloads: 2,
		},
		{
			name:                 "mixed scalar seekable file range",
			expr:                 `/id="x"`,
			doc:                  "\"x\"\n{\"id\":\"x\"}\n123\n",
			mode:                 1,
			wantSeekablePayloads: 1,
		},
		{
			name:                "mixed scalar callback source spooled payload",
			expr:                `/id="x"`,
			doc:                 "\"x\"\n{\"id\":\"x\"}\n123\n",
			mode:                2,
			wantSpooledPayloads: 1,
		},
		{
			name:                 "nested array seekable file range",
			expr:                 `/id="b"`,
			doc:                  `[{"id":"a"},[{"id":"b"}],{"id":"c"}]`,
			mode:                 1,
			wantSeekablePayloads: 1,
		},
		{
			name:                "nested array callback source spooled payload",
			expr:                `/id="b"`,
			doc:                 `[{"id":"a"},[{"id":"b"}],{"id":"c"}]`,
			mode:                2,
			wantSpooledPayloads: 1,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			want, err := goStreamQuery(tc.expr, tc.doc, tc.mode, 0, 0, 0, false)
			if err != nil {
				t.Fatalf("go stream payload: %v", err)
			}
			got, err := cStreamQuery(tc.expr, tc.doc, tc.mode, 0, 0, 0, false)
			if err != nil {
				t.Fatalf("liblql stream payload: %v", err)
			}
			assertStreamSummaryParity(t, got, want)
			if got.MatchCallbacks != want.MatchCallbacks {
				t.Fatalf("match callback mismatch: got=%d want=%d", got.MatchCallbacks, want.MatchCallbacks)
			}
			if got.SeekablePayloads != tc.wantSeekablePayloads || got.SpooledPayloads != tc.wantSpooledPayloads {
				t.Fatalf("payload kind mismatch: seekable=%d spooled=%d", got.SeekablePayloads, got.SpooledPayloads)
			}
			gotPayload, err := decodeJSONValues(got.PayloadJSON)
			if err != nil {
				t.Fatalf("decode liblql payload: %v json=%q", err, string(got.PayloadJSON))
			}
			wantPayload, err := decodeJSONValues(want.PayloadJSON)
			if err != nil {
				t.Fatalf("decode go payload: %v json=%q", err, string(want.PayloadJSON))
			}
			if !reflect.DeepEqual(gotPayload, wantPayload) {
				t.Fatalf("payload parity mismatch: got=%#v want=%#v got_json=%q want_json=%q", gotPayload, wantPayload, string(got.PayloadJSON), string(want.PayloadJSON))
			}
		})
	}
}

func TestSDKStreamingStopParity(t *testing.T) {
	doc := "{\"status\":\"open\"}\n{\"status\":\"open\"}\n{\"status\":\"closed\"}\n"
	cases := []struct {
		name          string
		mode          int
		maxMatches    int64
		maxCandidates int64
		maxBytes      int64
		stopCallback  bool
	}{
		{name: "max matches", mode: 0, maxMatches: 1},
		{name: "max candidates", mode: 0, maxCandidates: 2},
		{name: "max bytes", mode: 0, maxBytes: 17},
		{name: "decision callback stop", mode: 0, stopCallback: true},
		{name: "source max matches", mode: 3, maxMatches: 1},
		{name: "source max candidates", mode: 3, maxCandidates: 2},
		{name: "source max bytes", mode: 3, maxBytes: 17},
		{name: "source decision callback stop", mode: 3, stopCallback: true},
		{name: "payload max matches", mode: 1, maxMatches: 1},
		{name: "payload max candidates", mode: 1, maxCandidates: 2},
		{name: "payload max bytes", mode: 1, maxBytes: 17},
		{name: "payload callback stop", mode: 1, stopCallback: true},
		{name: "source payload max matches", mode: 2, maxMatches: 1},
		{name: "source payload max candidates", mode: 2, maxCandidates: 2},
		{name: "source payload max bytes", mode: 2, maxBytes: 17},
		{name: "source payload callback stop", mode: 2, stopCallback: true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			want, err := goStreamQuery(`/status="open"`, doc, tc.mode, tc.maxMatches, tc.maxCandidates, tc.maxBytes, tc.stopCallback)
			if err != nil {
				t.Fatalf("go stream stop: %v", err)
			}
			got, err := cStreamQuery(`/status="open"`, doc, tc.mode, tc.maxMatches, tc.maxCandidates, tc.maxBytes, tc.stopCallback)
			if err != nil {
				t.Fatalf("liblql stream stop: %v", err)
			}
			assertStreamSummaryParity(t, got, want)
		})
	}
}

func TestSDKStreamingErrorParity(t *testing.T) {
	cases := []struct {
		name string
		doc  string
	}{
		{name: "truncated object candidate", doc: `{"status":"open"}` + "\n" + `{"status":`},
		{name: "truncated array stream", doc: `[{"status":"open"},{"status":`},
		{name: "invalid literal", doc: `{"status": tru}`},
	}
	for _, tc := range cases {
		for _, mode := range []struct {
			name string
			id   int
		}{
			{name: "file decisions", id: 0},
			{name: "file payloads", id: 1},
			{name: "source spooled payloads", id: 2},
			{name: "source decisions", id: 3},
		} {
			t.Run(tc.name+"/"+mode.name, func(t *testing.T) {
				want, err := goStreamQuery(`/status="open"`, tc.doc, mode.id, 0, 0, 0, false)
				if err == nil {
					t.Fatalf("go stream unexpectedly accepted malformed JSON: %q", tc.doc)
				}
				got, err := cStreamQuery(`/status="open"`, tc.doc, mode.id, 0, 0, 0, false)
				if err == nil {
					t.Fatalf("liblql stream unexpectedly accepted malformed JSON: %q", tc.doc)
				}
				assertStreamSummaryParity(t, got, want)
			})
		}
	}
}

func goProjectJSON(fields []string, doc string) ([]byte, bool, error) {
	paths, err := lql.ParseProjectionPaths(fields)
	if err != nil {
		return nil, false, err
	}
	plan, err := lql.NewProjectionPlan(paths)
	if err != nil {
		return nil, false, err
	}
	var out bytes.Buffer
	result, err := lql.ProjectFields(lql.ProjectFieldsRequest{
		Reader: bytes.NewBufferString(doc),
		Writer: &out,
		Paths:  paths,
		Plan:   plan,
	})
	return out.Bytes(), result.Found, err
}

func goMutateJSON(mutations []string, doc string) ([]byte, error) {
	return goMutateJSONWithOptions(mutations, doc, lql.ParseMutationsOptions{})
}

func goMutateJSONWithOptions(mutations []string, doc string, opts lql.ParseMutationsOptions) ([]byte, error) {
	parsed, err := lql.ParseMutationsWithOptions(mutations, time.Unix(1700000000, 0), opts)
	if err != nil {
		return nil, err
	}
	var out bytes.Buffer
	if err := lql.MutateStream(lql.MutateStreamRequest{
		Reader:    bytes.NewBufferString(doc),
		Writer:    &out,
		Mutations: parsed,
	}); err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}

func goQueryMutateJSON(selectorExpr string, mutations []string, doc string) ([]byte, error) {
	selector, err := lql.ParseSelectorString(selectorExpr)
	if err != nil {
		return nil, err
	}
	parsed, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		return nil, err
	}
	var out bytes.Buffer
	_, err = lql.QueryMutateStreamWithResult(lql.QueryMutateStreamRequest{
		Reader:    bytes.NewBufferString(doc),
		Writer:    &out,
		Selector:  selector,
		Mutations: parsed,
	})
	if err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}

func goQueryProjectMutateJSON(selectorExpr string, fields []string, mutations []string, doc string, matchesOnly bool) ([]byte, error) {
	selector, err := lql.ParseSelectorString(selectorExpr)
	if err != nil {
		return nil, err
	}
	paths, err := lql.ParseProjectionPaths(fields)
	if err != nil {
		return nil, err
	}
	plan, err := lql.NewProjectionPlan(paths)
	if err != nil {
		return nil, err
	}
	parsed, err := lql.ParseMutations(mutations, time.Unix(1700000000, 0))
	if err != nil {
		return nil, err
	}
	var out bytes.Buffer
	err = lql.QueryStream(lql.QueryStreamRequest{
		Reader:      bytes.NewBufferString(doc),
		Selector:    selector,
		Mode:        lql.QueryDecisionPlusValue,
		IncludeJSON: true,
		MatchedOnly: matchesOnly,
		OnValue: func(value lql.QueryStreamValue) error {
			var payload []byte
			if value.JSON != nil {
				payload = value.JSON
			} else if value.OpenJSON != nil {
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
			var projected bytes.Buffer
			result, err := lql.ProjectFields(lql.ProjectFieldsRequest{
				Reader: bytes.NewReader(payload),
				Writer: &projected,
				Plan:   plan,
			})
			if err != nil || !result.Found {
				return err
			}
			if value.Matched {
				if err := lql.MutateStream(lql.MutateStreamRequest{
					Reader:    bytes.NewReader(projected.Bytes()),
					Writer:    &out,
					Mutations: parsed,
				}); err != nil {
					return err
				}
			} else {
				if _, err := out.Write(projected.Bytes()); err != nil {
					return err
				}
			}
			if err := out.WriteByte('\n'); err != nil {
				return err
			}
			return nil
		},
	})
	if err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}

func goCompactJSON(doc string) ([]byte, error) {
	var out bytes.Buffer
	if err := json.Compact(&out, []byte(doc)); err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}

func goStreamQuery(expr, doc string, mode int, maxMatches, maxCandidates, maxBytes int64, stopAfterFirst bool) (cStreamSummary, error) {
	sel, err := lql.ParseSelectorString(expr)
	if err != nil {
		return cStreamSummary{}, err
	}
	var summary cStreamSummary
	req := lql.QueryStreamRequest{
		Reader:        bytes.NewBufferString(doc),
		Selector:      sel,
		MaxMatches:    maxMatches,
		MaxCandidates: maxCandidates,
		MaxBytesRead:  maxBytes,
	}
	if mode == 0 || mode == 3 {
		req.Mode = lql.QueryDecisionOnly
		req.OnDecision = func(decision lql.QueryStreamDecision) error {
			summary.DecisionCallbacks++
			if decision.Matched {
				summary.MatchCallbacks++
			}
			if stopAfterFirst && summary.DecisionCallbacks == 1 {
				return lql.ErrStreamStop
			}
			return nil
		}
	} else {
		req.Mode = lql.QueryDecisionPlusValue
		req.MatchedOnly = true
		req.OnValue = func(value lql.QueryStreamValue) error {
			summary.MatchCallbacks++
			var payload []byte
			if value.JSON != nil {
				payload = value.JSON
			} else if value.OpenJSON != nil {
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
			summary.PayloadJSON = append(summary.PayloadJSON, payload...)
			if stopAfterFirst && summary.MatchCallbacks == 1 {
				return lql.ErrStreamStop
			}
			return nil
		}
	}
	result, err := lql.QueryStreamWithResult(req)
	summary.CandidatesSeen = result.CandidatesSeen
	summary.CandidatesMatched = result.CandidatesMatched
	summary.BytesRead = result.BytesRead
	summary.StoppedEarly = result.StoppedEarly
	summary.StopReason = goStopReasonCode(result.StopReason)
	if err != nil {
		return summary, err
	}
	return summary, nil
}

func assertProjectionJSONParity(t *testing.T, gotJSON []byte, gotFound bool, wantJSON []byte, wantFound bool) {
	t.Helper()
	if gotFound != wantFound {
		t.Fatalf("projection found mismatch: got=%v want=%v got_json=%q", gotFound, wantFound, string(gotJSON))
	}
	assertDecodedJSONValuesParity(t, gotJSON, wantJSON, "projection")
}

func assertDecodedJSONValuesParity(t *testing.T, gotJSON []byte, wantJSON []byte, label string) {
	t.Helper()
	got, err := decodeJSONValues(gotJSON)
	if err != nil {
		t.Fatalf("decode liblql %s: %v json=%q", label, err, string(gotJSON))
	}
	want, err := decodeJSONValues(wantJSON)
	if err != nil {
		t.Fatalf("decode go %s: %v json=%q", label, err, string(wantJSON))
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("%s parity mismatch: got=%#v want=%#v got_json=%q want_json=%q", label, got, want, string(gotJSON), string(wantJSON))
	}
}

func assertStreamSummaryParity(t *testing.T, got, want cStreamSummary) {
	t.Helper()
	if got.CandidatesSeen != want.CandidatesSeen ||
		got.CandidatesMatched != want.CandidatesMatched ||
		got.BytesRead != want.BytesRead ||
		got.StoppedEarly != want.StoppedEarly ||
		got.StopReason != want.StopReason {
		t.Fatalf("stream summary mismatch: got=%+v want=%+v", got, want)
	}
}

func goStopReasonCode(reason lql.QueryStreamStopReason) int {
	switch reason {
	case lql.QueryStreamStopNone:
		return 0
	case lql.QueryStreamStopMatchLimit:
		return 1
	case lql.QueryStreamStopCandidateLimit:
		return 2
	case lql.QueryStreamStopByteLimit:
		return 3
	case lql.QueryStreamStopCallbackStop:
		return 4
	default:
		return -1
	}
}

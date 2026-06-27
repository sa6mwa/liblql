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

func assertSDKSelectorMatchesJSON(t *testing.T, expr string, docJSON string, or bool) {
	t.Helper()
	var doc map[string]any
	if err := json.Unmarshal([]byte(docJSON), &doc); err != nil {
		t.Fatal(err)
	}
	var sel lql.Selector
	var err error
	if or {
		sel, err = lql.ParseSelectorStringOr(expr)
	} else {
		sel, err = lql.ParseSelectorString(expr)
	}
	if err != nil {
		t.Fatalf("go parse selector: %v", err)
	}
	want := lql.Matches(sel, doc)
	got, err := cMatchesJSON(expr, docJSON, or)
	if err != nil {
		t.Fatalf("liblql match: %v", err)
	}
	if got != want {
		t.Fatalf("liblql selector parity mismatch: got match=%v want=%v", got, want)
	}
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

func TestSDKSelectorASTJSONParity(t *testing.T) {
	now := time.Now().UTC()
	cases := []struct {
		name   string
		expr   string
		orMode bool
		docs   []string
	}{
		{name: "eq_shorthand", expr: `/status="open"`, docs: []string{
			`{"status":"open"}`,
			`{"status":"closed"}`,
		}},
		{name: "contains_omitted_value", expr: `contains{f=/hello/world}`, docs: []string{
			`{"hello":{"world":{"nested":true}}}`,
			`{"hello":{}}`,
		}},
		{name: "contains_explicit_empty", expr: `contains{f=/hello/world,v=""}`, docs: []string{
			`{"hello":{"world":""}}`,
			`{"hello":{"world":"non-empty"}}`,
		}},
		{name: "icontains_omitted_value", expr: `icontains{f=/hello/world}`, docs: []string{
			`{"hello":{"world":{"nested":true}}}`,
			`{"hello":{}}`,
		}},
		{name: "icontains_explicit_empty", expr: `icontains{f=/hello/world,v=""}`, docs: []string{
			`{"hello":{"world":""}}`,
			`{"hello":{"world":"non-empty"}}`,
		}},
		{name: "prefix_omitted_value", expr: `prefix{f=/hello/world}`, docs: []string{
			`{"hello":{"world":{"nested":true}}}`,
			`{"hello":{}}`,
		}},
		{name: "prefix_explicit_empty", expr: `prefix{f=/hello/world,v=""}`, docs: []string{
			`{"hello":{"world":""}}`,
			`{"hello":{"world":"non-empty"}}`,
		}},
		{name: "iprefix_omitted_value", expr: `iprefix{f=/hello/world}`, docs: []string{
			`{"hello":{"world":{"nested":true}}}`,
			`{"hello":{}}`,
		}},
		{name: "iprefix_explicit_empty", expr: `iprefix{f=/hello/world,v=""}`, docs: []string{
			`{"hello":{"world":""}}`,
			`{"hello":{"world":"non-empty"}}`,
		}},
		{name: "contains_any", expr: `contains{f=/msg,a=warn|timeout}`, docs: []string{
			`{"msg":"warn: timeout waiting for lock"}`,
			`{"msg":"all clear"}`,
		}},
		{name: "ignore_case_flag", expr: `contains{f=/msg,v=timeout,ic=t}`, docs: []string{
			`{"msg":"TIMEOUT waiting"}`,
			`{"msg":"still running"}`,
		}},
		{name: "implicit_and", expr: `eq{field=/status,value=open},range{field=/progress,gte=10}`, docs: []string{
			`{"status":"open","progress":25}`,
			`{"status":"open","progress":5}`,
		}},
		{name: "explicit_not", expr: `not.eq{field=/status,value=closed}`, docs: []string{
			`{"status":"open"}`,
			`{"status":"closed"}`,
		}},
		{name: "or_parse", expr: `eq{field=/region,value=us},eq{field=/region,value=eu}`, orMode: true, docs: []string{
			`{"region":"eu"}`,
			`{"region":"apac"}`,
		}},
		{name: "range_datetime", expr: `range{field=/timestamp,lt=2026-03-05T11:29:41.265+01:00}`, docs: []string{
			`{"timestamp":"2026-03-05T09:00:00Z"}`,
			`{"timestamp":"2026-03-05T12:00:00Z"}`,
		}},
		{name: "date_after_before", expr: `date{field=/timestamp,after=2025-01-01,before=2025-01-03}`, docs: []string{
			`{"timestamp":"2025-01-02T00:00:00Z"}`,
			`{"timestamp":"2025-01-04T00:00:00Z"}`,
		}},
		{name: "date_since_macro", expr: `date{field=/timestamp,since=yesterday}`, docs: []string{
			`{"timestamp":"` + now.Add(-12*time.Hour).Format(time.RFC3339Nano) + `"}`,
			`{"timestamp":"` + now.Add(-72*time.Hour).Format(time.RFC3339Nano) + `"}`,
		}},
		{name: "in_any", expr: `in{field=/env,any=prod|stage}`, docs: []string{
			`{"env":"stage"}`,
			`{"env":"dev"}`,
		}},
		{name: "exists", expr: `exists{/meta/etag}`, docs: []string{
			`{"meta":{"etag":"abc"}}`,
			`{"meta":{}}`,
		}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var sel lql.Selector
			var err error
			if tc.orMode {
				sel, err = lql.ParseSelectorStringOr(tc.expr)
			} else {
				sel, err = lql.ParseSelectorString(tc.expr)
			}
			if err != nil {
				t.Fatalf("go parse selector: %v", err)
			}
			want, err := json.Marshal(sel)
			if err != nil {
				t.Fatalf("go marshal selector: %v", err)
			}
			cJSON, err := cSelectorJSON(tc.expr, tc.orMode)
			if err != nil {
				t.Fatalf("liblql selector JSON export: %v", err)
			}
			var fromC lql.Selector
			if err := json.Unmarshal([]byte(cJSON), &fromC); err != nil {
				t.Fatalf("Go selector failed to parse liblql parsed-selector JSON: %v\njson: %s", err, cJSON)
			}
			for _, docJSON := range tc.docs {
				var doc map[string]any
				if err := json.Unmarshal([]byte(docJSON), &doc); err != nil {
					t.Fatalf("unmarshal candidate: %v", err)
				}
				wantMatch := lql.Matches(sel, doc)
				gotMatch, err := cSelectorJSONMatches(string(want), docJSON)
				if err != nil {
					t.Fatalf("liblql selector JSON import match: %v\nselector json: %s", err, string(want))
				}
				if gotMatch != wantMatch {
					t.Fatalf("selector AST JSON behavior mismatch doc=%s got=%v want=%v\nselector json: %s",
						docJSON, gotMatch, wantMatch, string(want))
				}
				goFromC := lql.Matches(fromC, doc)
				if goFromC != wantMatch {
					t.Fatalf("Go behavior after liblql parsed-selector JSON import mismatch doc=%s got=%v want=%v\nc selector json: %s",
						docJSON, goFromC, wantMatch, cJSON)
				}
			}
		})
	}
}

func TestSDKSelectorASTBuilderParity(t *testing.T) {
	cases := []struct {
		name     string
		caseID   int
		selector lql.Selector
		docs     []string
	}{
		{
			name:     "all",
			caseID:   0,
			selector: lql.Selector{},
			docs: []string{
				`{"status":"open"}`,
				`{"status":"closed"}`,
			},
		},
		{
			name:     "eq",
			caseID:   1,
			selector: lql.Selector{Eq: &lql.Term{Field: "/status", Value: "open"}},
			docs: []string{
				`{"status":"open"}`,
				`{"status":"closed"}`,
			},
		},
		{
			name:     "contains omitted value path assertion",
			caseID:   2,
			selector: lql.Selector{Contains: &lql.Term{Field: "/hello/world"}},
			docs: []string{
				`{"hello":{"world":{"nested":true}}}`,
				`{"hello":{}}`,
			},
		},
		{
			name:     "contains any ignore case",
			caseID:   3,
			selector: lql.Selector{Contains: &lql.Term{Field: "/msg", Any: []string{"warn", "timeout"}, IgnoreCase: true}},
			docs: []string{
				`{"msg":"WARN: lock timeout"}`,
				`{"msg":"all clear"}`,
			},
		},
		{
			name:   "numeric range",
			caseID: 4,
			selector: lql.Selector{Range: &lql.RangeTerm{
				Field: "/progress",
				GTE:   lql.NewNumericRangeBound(10),
				LT:    lql.NewNumericRangeBound(90),
			}},
			docs: []string{
				`{"progress":25}`,
				`{"progress":95}`,
			},
		},
		{
			name:   "datetime range",
			caseID: 5,
			selector: lql.Selector{Range: &lql.RangeTerm{
				Field: "/timestamp",
				GTE:   lql.NewDatetimeRangeBound(" 2026-03-05T10:28:21Z "),
				LT:    lql.NewDatetimeRangeBound("2026-03-05T10:30:00Z"),
			}},
			docs: []string{
				`{"timestamp":"2026-03-05T10:29:00Z"}`,
				`{"timestamp":"2026-03-05T10:31:00Z"}`,
			},
		},
		{
			name:   "date after before",
			caseID: 6,
			selector: lql.Selector{Date: &lql.DateTerm{
				Field:  "/timestamp",
				After:  "2025-01-01",
				Before: "2025-01-03",
			}},
			docs: []string{
				`{"timestamp":"2025-01-02T00:00:00Z"}`,
				`{"timestamp":"2025-01-04T00:00:00Z"}`,
			},
		},
		{
			name:     "in any",
			caseID:   7,
			selector: lql.Selector{In: &lql.InTerm{Field: "/env", Any: []string{"prod", "stage"}}},
			docs: []string{
				`{"env":"stage"}`,
				`{"env":"dev"}`,
			},
		},
		{
			name:     "exists",
			caseID:   8,
			selector: lql.Selector{Exists: "/meta/etag"},
			docs: []string{
				`{"meta":{"etag":"abc"}}`,
				`{"meta":{}}`,
			},
		},
		{
			name:   "and",
			caseID: 9,
			selector: lql.Selector{And: []lql.Selector{
				{Eq: &lql.Term{Field: "/status", Value: "open"}},
				{Range: &lql.RangeTerm{Field: "/progress", GTE: lql.NewNumericRangeBound(10)}},
			}},
			docs: []string{
				`{"status":"open","progress":25}`,
				`{"status":"open","progress":5}`,
			},
		},
		{
			name:   "not",
			caseID: 10,
			selector: lql.Selector{Not: &lql.Selector{
				Eq: &lql.Term{Field: "/status", Value: "closed"},
			}},
			docs: []string{
				`{"status":"open"}`,
				`{"status":"closed"}`,
			},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			cJSON, err := cSelectorBuilderJSON(tc.caseID)
			if err != nil {
				t.Fatalf("liblql builder JSON: %v", err)
			}
			var fromC lql.Selector
			if err := json.Unmarshal([]byte(cJSON), &fromC); err != nil {
				t.Fatalf("Go selector failed to parse liblql builder JSON: %v\njson: %s", err, cJSON)
			}
			for _, docJSON := range tc.docs {
				var doc map[string]any
				if err := json.Unmarshal([]byte(docJSON), &doc); err != nil {
					t.Fatalf("unmarshal candidate: %v", err)
				}
				want := lql.Matches(tc.selector, doc)
				got, err := cSelectorBuilderMatches(tc.caseID, docJSON)
				if err != nil {
					t.Fatalf("liblql builder match: %v", err)
				}
				if got != want {
					t.Fatalf("builder behavior mismatch doc=%s got=%v want=%v\nc selector json: %s",
						docJSON, got, want, cJSON)
				}
				goFromC := lql.Matches(fromC, doc)
				if goFromC != want {
					t.Fatalf("Go behavior after liblql builder JSON import mismatch doc=%s got=%v want=%v\nc selector json: %s",
						docJSON, goFromC, want, cJSON)
				}
			}
		})
	}
}

func TestSDKSelectorWildcardPathParity(t *testing.T) {
	cases := []sdkSelectorMatchCase{
		{`/labels/*="alice"`, `{"labels":{"env":"prod","owner":"alice"},"items":[{"sku":"A"},{"sku":"B"}],"groups":[{"items":[{"sku":"A"},{"sku":"B"}]}],"scalar":"x","arrEmpty":[]}`},
		{`/items/*/sku="B"`, `{"labels":{"env":"prod","owner":"alice"},"items":[{"sku":"A"},{"sku":"B"}],"groups":[{"items":[{"sku":"A"},{"sku":"B"}]}],"scalar":"x","arrEmpty":[]}`},
		{`/items/**/sku="B"`, `{"labels":{"env":"prod","owner":"alice"},"items":[{"sku":"A"},{"sku":"B"}],"groups":[{"items":[{"sku":"A"},{"sku":"B"}]}],"scalar":"x","arrEmpty":[]}`},
		{`/items[]/sku="B"`, `{"labels":{"env":"prod","owner":"alice"},"items":[{"sku":"A"},{"sku":"B"}],"groups":[{"items":[{"sku":"A"},{"sku":"B"}]}],"scalar":"x","arrEmpty":[]}`},
		{`/groups[]/items/**/sku="B"`, `{"labels":{"env":"prod","owner":"alice"},"items":[{"sku":"A"},{"sku":"B"}],"groups":[{"items":[{"sku":"A"},{"sku":"B"}]}],"scalar":"x","arrEmpty":[]}`},
		{`/groups/.../sku="B"`, `{"labels":{"env":"prod","owner":"alice"},"items":[{"sku":"A"},{"sku":"B"}],"groups":[{"items":[{"sku":"A"},{"sku":"B"}]}],"scalar":"x","arrEmpty":[]}`},
		{`/items[]/sku="C"`, `{"labels":{"env":"prod","owner":"alice"},"items":[{"sku":"A"},{"sku":"B"}],"groups":[{"items":[{"sku":"A"},{"sku":"B"}]}],"scalar":"x","arrEmpty":[]}`},
		{`/scalar/*="x"`, `{"labels":{"env":"prod","owner":"alice"},"items":[{"sku":"A"},{"sku":"B"}],"groups":[{"items":[{"sku":"A"},{"sku":"B"}]}],"scalar":"x","arrEmpty":[]}`},
		{`/arrEmpty[]/sku="A"`, `{"labels":{"env":"prod","owner":"alice"},"items":[{"sku":"A"},{"sku":"B"}],"groups":[{"items":[{"sku":"A"},{"sku":"B"}]}],"scalar":"x","arrEmpty":[]}`},
		{`/items[]/sku="B"`, `{"items":{"sku":"B"}}`},
		{`/items[]/price>=20`, `{"items":[{"sku":"A","price":10},{"sku":"B","price":25}],"metrics":[{"battery_mv":4100},{"battery_mv":3300}]}`},
		{`/metrics/**/battery_mv<3600`, `{"items":[{"sku":"A","price":10},{"sku":"B","price":25}],"metrics":[{"battery_mv":4100},{"battery_mv":3300}]}`},
		{`/items/*/price>=20`, `{"items":[{"sku":"A","price":10},{"sku":"B","price":25}],"metrics":[{"battery_mv":4100},{"battery_mv":3300}]}`},
		{`in{field=/labels/*,any=prod|stage}`, `{"labels":{"env":"prod","owner":"alice"}}`},
		{`exists{/items/.../sku}`, `{"items":[{"sku":"A"}]}`},
	}
	for _, tc := range cases {
		t.Run(tc.expr+"/"+tc.doc, func(t *testing.T) {
			assertSDKSelectorMatchesJSON(t, tc.expr, tc.doc, false)
		})
	}
}

func TestSDKSelectorStringTermParity(t *testing.T) {
	cases := []sdkSelectorMatchCase{
		{`contains{field=/msg,value=Timeout}`, `{"msg":"Error: Timeout while reading","service":"Auth-Service","labels":{"owner":"ALICE-Team","env":"prod"}}`},
		{`contains{field=/msg,value=timeout}`, `{"msg":"Error: Timeout while reading","service":"Auth-Service","labels":{"owner":"ALICE-Team","env":"prod"}}`},
		{`contains{field=/msg,value=timeout,ic=t}`, `{"msg":"Error: Timeout while reading","service":"Auth-Service","labels":{"owner":"ALICE-Team","env":"prod"}}`},
		{`contains{field=/msg,value=timeout,ignoreCase=f}`, `{"msg":"Error: Timeout while reading","service":"Auth-Service","labels":{"owner":"ALICE-Team","env":"prod"}}`},
		{`icontains{field=/msg,value=timeout}`, `{"msg":"Error: Timeout while reading","service":"Auth-Service","labels":{"owner":"ALICE-Team","env":"prod"}}`},
		{`icontains{field=/msg,value=timeout,ignoreCase=f}`, `{"msg":"Error: Timeout while reading","service":"Auth-Service","labels":{"owner":"ALICE-Team","env":"prod"}}`},
		{`prefix{field=/service,value=auth}`, `{"msg":"Error: Timeout while reading","service":"Auth-Service","labels":{"owner":"ALICE-Team","env":"prod"}}`},
		{`prefix{field=/service,value=auth,ignoreCase=true}`, `{"msg":"Error: Timeout while reading","service":"Auth-Service","labels":{"owner":"ALICE-Team","env":"prod"}}`},
		{`iprefix{field=/service,value=auth}`, `{"msg":"Error: Timeout while reading","service":"Auth-Service","labels":{"owner":"ALICE-Team","env":"prod"}}`},
		{`iprefix{field=/service,value=auth,ignoreCase=f}`, `{"msg":"Error: Timeout while reading","service":"Auth-Service","labels":{"owner":"ALICE-Team","env":"prod"}}`},
		{`icontains{field=/labels/*,value=alice}`, `{"msg":"Error: Timeout while reading","service":"Auth-Service","labels":{"owner":"ALICE-Team","env":"prod"}}`},
		{`contains{f=/msg,a=warn|Timeout}`, `{"msg":"Error: Timeout while reading"}`},
		{`contains{f=/msg,a=warn|fatal}`, `{"msg":"Error: Timeout while reading"}`},
		{`icontains{f=/msg,a=warn|timeout}`, `{"msg":"Error: Timeout while reading"}`},
		{`icontains{f=/msg,a=warn|fatal}`, `{"msg":"Error: Timeout while reading"}`},
		{`contains{f=/,v=""}`, `{"status":"open"}`},
		{`icontains{f=/,v=""}`, `{"status":"open"}`},
		{`prefix{f=/,v=""}`, `{"status":"open"}`},
		{`iprefix{f=/,v=""}`, `{"status":"open"}`},
		{`not.icontains{f=/,v=""}`, `{"status":"open"}`},
		{`contains{f=/hello/world}`, `{"hello":{"world":{"nested":true}}}`},
		{`contains{f=/hello/world}`, `{"hello":{"world":[1,2,3]}}`},
		{`contains{f=/hello/world}`, `{"hello":{"world":null}}`},
		{`contains{f=/hello/world}`, `{"hello":{"other":"x"}}`},
		{`icontains{f=/hello/world}`, `{"hello":{"world":{"nested":true}}}`},
		{`prefix{f=/hello/world}`, `{"hello":{"world":[1,2,3]}}`},
		{`iprefix{f=/hello/world}`, `{"hello":{"world":null}}`},
		{`contains{f=/hello/*}`, `{"hello":{"world":{"nested":true},"names":["alice","bob"]},"arrays":[{"id":1},{"id":2}]}`},
		{`contains{f=/hello/...}`, `{"hello":{"world":{"nested":true},"names":["alice","bob"]},"arrays":[{"id":1},{"id":2}]}`},
		{`contains{f=/arrays/[]}`, `{"hello":{"world":{"nested":true},"names":["alice","bob"]},"arrays":[{"id":1},{"id":2}]}`},
		{`contains{f=/arrays/[]/id}`, `{"hello":{"world":{"nested":true},"names":["alice","bob"]},"arrays":[{"id":1},{"id":2}]}`},
		{`contains{f=/hello/missing}`, `{"hello":{"world":{"nested":true},"names":["alice","bob"]},"arrays":[{"id":1},{"id":2}]}`},
		{`contains{f=/missing/*}`, `{"hello":{"world":{"nested":true},"names":["alice","bob"]},"arrays":[{"id":1},{"id":2}]}`},
		{`contains{f=/missing/...}`, `{"hello":{"world":{"nested":true},"names":["alice","bob"]},"arrays":[{"id":1},{"id":2}]}`},
		{`contains{f=/missing/[]}`, `{"hello":{"world":{"nested":true},"names":["alice","bob"]},"arrays":[{"id":1},{"id":2}]}`},
	}
	for _, tc := range cases {
		t.Run(tc.expr+"/"+tc.doc, func(t *testing.T) {
			assertSDKSelectorMatchesJSON(t, tc.expr, tc.doc, false)
		})
	}
}

func TestSDKSelectorLogicalCompositionParity(t *testing.T) {
	andCases := []sdkSelectorMatchCase{
		{`/field="value",/status="ok"`, `{"field":"value","status":"ok"}`},
		{`/field="value",/status="ok"`, `{"field":"value","status":"nope"}`},
		{`/field="value",/status="ok"`, `{"field":"nope","status":"ok"}`},
		{`and.eq{field=/status,value=ok},/msg="done"`, `{"status":"ok","msg":"done"}`},
		{`and.eq{field=/status,value=ok},/msg="done"`, `{"status":"ok","msg":"nope"}`},
		{`or.0.eq{field=/status,value=ok},or.0.range{field=/progress,gte=10}`, `{"status":"ok","progress":10}`},
		{`or.0.eq{field=/status,value=ok},or.0.range{field=/progress,gte=10}`, `{"status":"ok","progress":5}`},
		{`and.0.eq{field=/status,value=ok},and.0.range{field=/progress,gte=10}`, `{"status":"ok","progress":10}`},
		{`and.0.eq{field=/status,value=ok},and.0.range{field=/progress,gte=10}`, `{"status":"nope","progress":10}`},
		{`not.eq{field=/status,value=closed},/region="us"`, `{"status":"open","region":"us"}`},
		{`not.eq{field=/status,value=closed},/region="us"`, `{"status":"closed","region":"us"}`},
		{`eq{f=/status,v=ok},in{f=/env,a=prod|stage}`, `{"status":"ok","env":"prod"}`},
		{`eq{f=/status,v=ok},in{f=/env,a=prod|stage}`, `{"status":"ok","env":"dev"}`},
		{`/field="value",/status="ok",or.eq{field=/msg,value=done},or.eq{field=/msg,value=complete}`, `{"field":"value","status":"ok","msg":"done"}`},
		{`/field="value",/status="ok",or.eq{field=/msg,value=done},or.eq{field=/msg,value=complete}`, `{"field":"value","status":"ok","msg":"nope"}`},
	}
	orCases := []sdkSelectorMatchCase{
		{`/field="value",/status="ok"`, `{"field":"value","status":"nope"}`},
		{`/field="value",/status="ok"`, `{"field":"nope","status":"ok"}`},
		{`/field="value",/status="ok"`, `{"field":"nope","status":"nope"}`},
		{`exists{/meta/etag},/status="ok"`, `{"meta":{"etag":"abc"}}`},
		{`exists{/meta/etag},/status="ok"`, `{"status":"ok"}`},
		{`exists{/meta/etag},/status="ok"`, `{"status":"nope"}`},
		{`in{field=/env,any=prod|stage},/status="ok"`, `{"env":"prod"}`},
		{`in{field=/env,any=prod|stage},/status="ok"`, `{"status":"ok"}`},
		{`in{field=/env,any=prod|stage},/status="ok"`, `{"env":"dev"}`},
	}
	for _, tc := range andCases {
		t.Run("and/"+tc.expr+"/"+tc.doc, func(t *testing.T) {
			assertSDKSelectorMatchesJSON(t, tc.expr, tc.doc, false)
		})
	}
	for _, tc := range orCases {
		t.Run("or/"+tc.expr+"/"+tc.doc, func(t *testing.T) {
			assertSDKSelectorMatchesJSON(t, tc.expr, tc.doc, true)
		})
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
		{`/a~1b/~0key="ready"`, `{"a/b":{"~key":"ready"}}`},
		{`/a~1b/~0key="ready"`, `{"a":{"b":{"~key":"ready"}}}`},
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

func TestSDKTemporalFormatParity(t *testing.T) {
	cases := []sdkSelectorMatchCase{
		{`/timestamp="2025-01-01"`, `{"timestamp":"2025-01-01T23:59:59Z"}`},
		{`/timestamp="2025-01-01"`, `{"timestamp":"2025-01-02T00:00:00Z"}`},
		{`/timestamp="2026-03-11T01:11:28Z"`, `{"timestamp":"2026-03-11T01:11:28Z"}`},
		{`/timestamp="2026-03-11T01:11:28"`, `{"timestamp":"2026-03-11T01:11:28Z"}`},
		{`/timestamp="2026-03-11T01:11:28"`, `{"timestamp":"2026-03-11T01:11:28+01:00"}`},
		{`/timestamp="2026-03-11T01:11:28.1"`, `{"timestamp":"2026-03-11T01:11:28.100000000Z"}`},
		{`/timestamp="2026-03-11T01:11:28.123456789"`, `{"timestamp":"2026-03-11T01:11:28.123456789Z"}`},
		{`/timestamp="2026-03-11T01:11:28.123456789Z"`, `{"timestamp":"2026-03-11T01:11:28.123456789Z"}`},
		{`/timestamp="2026-03-11T01:11:28.123456789+01:30"`, `{"timestamp":"2026-03-10T23:41:28.123456789Z"}`},
		{`/timestamp="2026-03-11T01:11:28.123456789-02:30"`, `{"timestamp":"2026-03-11T03:41:28.123456789Z"}`},
		{`/timestamp>=2026-03-11T01:11:28.123456789`, `{"timestamp":"2026-03-11T01:11:28.123456790Z"}`},
		{`date{field=/timestamp,value=2026-03-11}`, `{"timestamp":"2026-03-11T23:59:59.999999999Z"}`},
		{`date{field=/timestamp,value=2026-03-11}`, `{"timestamp":"2026-03-12T00:00:00Z"}`},
		{`date{field=/timestamp,after=2026-03-11T01:11:28.123456789+01:00,before=2026-03-11T01:11:28.123456791+01:00}`, `{"timestamp":"2026-03-11T00:11:28.123456790Z"}`},
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
				t.Fatalf("liblql temporal format parity mismatch: got match=%v want=%v", got, want)
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
		`date{field=/timestamp,value=2026-03-11t01:11:28Z}`,
		`date{field=/timestamp,value=2026-03-11T01:11:28z}`,
		`date{field=/timestamp,value=2026-03-11T01:11}`,
		`date{field=/timestamp,value=2026-03-11T01:11:28+0100}`,
		`date{field=/timestamp,value=2026-03-11T01:11:28+01}`,
		`date{field=/timestamp,value=2026-03-11T01:11:60Z}`,
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
		{
			name: "escaped JSON Pointer selector path",
			expr: `/a~1b/~0key="ready"`,
			doc:  "{\"a/b\":{\"~key\":\"ready\"}}\n{\"a/b\":{\"~key\":\"old\"}}\n{\"a\":{\"b\":{\"~key\":\"ready\"}}}\n",
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

func TestSDKStreamingNumericSegmentOracleParity(t *testing.T) {
	doc := sdkNumericSegmentParityStream(t)
	selectors := []string{
		`/voucher/lines/10/amount>=3000`,
		`exists{/voucher/lines/10/amount}`,
		`in{field=/voucher/lines/10/status,any=open|closed}`,
		`contains{field=/voucher/lines/10/msg,value=hello}`,
		`iprefix{field=/voucher/lines/10/code,value=auth-10}`,
		`/voucher/.../10/amount>=3000`,
		`/batches[]/lines/10/amount>=4000`,
		`/batches[]/.../10/status="open"`,
	}
	for _, expr := range selectors {
		for _, mode := range []struct {
			name string
			id   int
		}{
			{name: "decision", id: 0},
			{name: "payload", id: 1},
		} {
			t.Run(expr+"/"+mode.name, func(t *testing.T) {
				want, err := goStreamQuery(expr, doc, mode.id, 0, 0, 0, false)
				if err != nil {
					t.Fatalf("go numeric stream: %v", err)
				}
				got, err := cStreamQuery(expr, doc, mode.id, 0, 0, 0, false)
				if err != nil {
					t.Fatalf("liblql numeric stream: %v", err)
				}
				assertStreamSummaryParity(t, got, want)
				if mode.id == 0 {
					if got.DecisionCallbacks != want.DecisionCallbacks {
						t.Fatalf("decision callback mismatch: got=%d want=%d", got.DecisionCallbacks, want.DecisionCallbacks)
					}
					return
				}
				assertDecodedJSONValuesParity(t, got.PayloadJSON, want.PayloadJSON, "numeric stream payload")
			})
		}
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

func TestSDKStreamingStdlibOracleParity(t *testing.T) {
	invalidUTF8 := string([]byte{'"', 'a', 0xff, 'b', '"', '\n', '{', '"', 's', '"', ':', '"', 'x', 0xfe, 'y', '"', '}'})
	valid := []struct {
		name string
		doc  string
	}{
		{
			name: "ndjson-and-top-level-array",
			doc:  "{\"id\":\"a\",\"n\":1}\n{\"id\":\"b\",\"n\":-2.5e+3}\n[{\"id\":\"c\",\"n\":3},[{\"id\":\"d\",\"n\":4}],true,null,\"x\"]",
		},
		{
			name: "raw-invalid-utf8-bytes",
			doc:  invalidUTF8,
		},
		{
			name: "mixed-numbers",
			doc:  `0 -0 1e-9 -1.2E+3 42`,
		},
	}
	for _, tc := range valid {
		t.Run("valid/"+tc.name, func(t *testing.T) {
			want, err := goStreamQuery("", tc.doc, 1, 0, 0, 0, false)
			if err != nil {
				t.Fatalf("go valid stream: %v", err)
			}
			got, err := cStreamQuery("", tc.doc, 1, 0, 0, 0, false)
			if err != nil {
				t.Fatalf("liblql valid stream: %v", err)
			}
			assertStreamSummaryParity(t, got, want)
			assertDecodedJSONValuesParity(t, got.PayloadJSON, want.PayloadJSON, "stdlib valid stream payload")
		})
	}

	control := string([]byte{'"', 'a', '\n', 'b', '"'})
	invalid := []struct {
		name string
		doc  string
	}{
		{name: "unterminated-object", doc: `{"id":1`},
		{name: "invalid-unicode-escape", doc: `"\uZZZZ"`},
		{name: "invalid-surrogate-followup-escape", doc: `{"a":"\uD800\uZZZZ"}`},
		{name: "trailing-comma-array", doc: `[1,2,]`},
		{name: "control-character-in-string", doc: control},
		{name: "invalid-literal", doc: `tru`},
	}
	for _, tc := range invalid {
		t.Run("invalid/"+tc.name, func(t *testing.T) {
			want, wantErr := goStreamQuery("", tc.doc, 1, 0, 0, 0, false)
			got, gotErr := cStreamQuery("", tc.doc, 1, 0, 0, 0, false)
			if (gotErr != nil) != (wantErr != nil) {
				t.Fatalf("stream error presence mismatch: got=%v want=%v", gotErr, wantErr)
			}
			assertStreamSummaryParity(t, got, want)
			assertDecodedJSONValuesParity(t, got.PayloadJSON, want.PayloadJSON, "stdlib invalid stream partial payload")
		})
	}
}

func TestSDKStreamingMultiFieldSelectorOracleParity(t *testing.T) {
	doc := `[
  {
    "id": "a",
    "status": "open",
    "region": "eu",
    "msg": "Timeout while reading",
    "service": "Auth-Service",
    "progress": 12,
    "latency": 180,
    "env": "prod",
    "meta": {"etag": "x", "trace": 1},
    "items": [{"sku": "A", "price": 10}, {"sku": "B", "price": 25}],
    "groups": [{"items": [{"sku": "A"}, {"sku": "B"}]}]
  },
  [
    {
      "id": "b",
      "status": "closed",
      "region": "us",
      "msg": "done",
      "service": "billing",
      "progress": 5,
      "latency": 90,
      "env": "stage",
      "meta": {"trace": 2},
      "items": [{"sku": "C", "price": 5}],
      "groups": [{"items": [{"sku": "C"}]}]
    }
  ],
  {
    "id": "c",
    "status": "ok",
    "region": "us",
    "msg": "Complete",
    "service": "auth-api",
    "progress": 15,
    "latency": 205,
    "env": "dev",
    "meta": {"etag": null, "trace": 3},
    "items": [{"sku": "B", "price": 30}],
    "groups": [{"items": [{"sku": "B"}]}]
  },
  7
]`
	for _, expr := range sdkParitySelectorExpressions() {
		t.Run(expr, func(t *testing.T) {
			want, err := goStreamQuery(expr, doc, 1, 0, 0, 0, false)
			if err != nil {
				t.Fatalf("go multi-field stream: %v", err)
			}
			got, err := cStreamQuery(expr, doc, 1, 0, 0, 0, false)
			if err != nil {
				t.Fatalf("liblql multi-field stream: %v", err)
			}
			assertStreamSummaryParity(t, got, want)
			assertDecodedJSONValuesParity(t, got.PayloadJSON, want.PayloadJSON, "multi-field stream payload")
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

func sdkNumericSegmentParityStream(t *testing.T) string {
	t.Helper()
	makeLineArray := func(amount int, status, msg, code string) []any {
		lines := make([]any, 11)
		for i := range lines {
			lines[i] = map[string]any{"amount": i}
		}
		lines[10] = map[string]any{
			"amount": amount,
			"status": status,
			"msg":    msg,
			"code":   code,
		}
		return lines
	}
	docs := []any{
		map[string]any{
			"voucher": map[string]any{
				"lines": map[string]any{
					"10": map[string]any{
						"amount": 3500,
						"status": "open",
						"msg":    "hello object line",
						"code":   "AUTH-10-OBJECT",
					},
				},
			},
		},
		map[string]any{
			"voucher": map[string]any{
				"lines": makeLineArray(3600, "closed", "hello array line", "AUTH-10-ARRAY"),
			},
		},
		map[string]any{
			"batches": []any{
				map[string]any{
					"lines": map[string]any{
						"10": map[string]any{
							"amount": 4100,
							"status": "open",
						},
					},
				},
				map[string]any{
					"lines": makeLineArray(4200, "closed", "nested array line", "AUTH-10-NESTED"),
				},
			},
		},
		map[string]any{
			"voucher": map[string]any{
				"lines": map[string]any{
					"10": map[string]any{
						"amount": 1200,
						"status": "processing",
						"msg":    "low amount",
						"code":   "AUTH-10-LOW",
					},
				},
			},
		},
	}
	return encodeSDKJSONLines(t, docs)
}

func encodeSDKJSONLines(t *testing.T, docs []any) string {
	t.Helper()
	var out bytes.Buffer
	for i, doc := range docs {
		if i > 0 {
			out.WriteByte('\n')
		}
		raw, err := json.Marshal(doc)
		if err != nil {
			t.Fatalf("marshal stream doc: %v", err)
		}
		out.Write(raw)
	}
	return out.String()
}

func sdkParitySelectorExpressions() []string {
	return []string{
		`/status="open"`,
		`/status="open",/region="eu"`,
		`and.eq{field=/status,value=open},and.range{field=/progress,gte=10}`,
		`and.eq{field=/status,value=open},and.range{field=/latency,gte=150},/region="eu"`,
		`or.eq{field=/status,value=open},or.eq{field=/status,value=closed}`,
		`or.0.eq{field=/status,value=ok},or.0.range{field=/progress,gte=15}`,
		`and.0.eq{field=/status,value=ok},and.0.range{field=/progress,gte=15}`,
		`not.eq{field=/status,value=closed},/region="us"`,
		`contains{field=/msg,value=Timeout}`,
		`icontains{field=/msg,value=complete}`,
		`contains{f=/msg,a=Timeout|queue}`,
		`icontains{f=/msg,a=timeout|queue}`,
		`icontains{f=/,v=""}`,
		`prefix{field=/service,value=Auth}`,
		`iprefix{field=/service,value=auth}`,
		`in{field=/env,any=prod|stage}`,
		`exists{/meta/etag}`,
		`/items[]/sku="B"`,
		`/items[]/price>=20,/region="eu"`,
		`/groups/.../sku="B"`,
		`/items/**/sku="B",exists{/meta/etag}`,
		`/items/*/sku="B"`,
		`/voucher/lines/10/amount>=1`,
		`exists{/voucher/lines/10/status}`,
		`in{field=/voucher/lines/10/status,any=open|closed|ok|processing}`,
		`contains{field=/voucher/lines/10/msg,value=line}`,
		`/voucher/.../10/amount>=1`,
		`/timestamp="2026-03-05"`,
		`/timestamp>=2026-03-05T10:28:21Z`,
		`range{field=/timestamp,gte=2026-03-05T10:28:21Z,lt=2026-03-05T10:30:00Z}`,
		`date{field=/timestamp,after=2026-03-05T10:28:21Z,before=2026-03-05T10:30:00Z}`,
		`/status="open",or.eq{field=/msg,value="Timeout while reading"},or.eq{field=/msg,value=fail}`,
		`or.eq{field=/status,value=open},or.eq{field=/status,value=closed},not.eq{field=/region,value=apac},range{field=/latency,gte=50},in{field=/env,any=prod|stage},exists{/meta}`,
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
			if len(summary.PayloadJSON) > 0 {
				summary.PayloadJSON = append(summary.PayloadJSON, '\n')
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

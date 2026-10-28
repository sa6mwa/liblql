package parity

import (
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"math/big"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"syscall"
	"testing"
	"time"
)

var executed bool

func TestMain(m *testing.M) {
	code := m.Run()
	if code == 0 && !executed && flag.Lookup("test.list").Value.String() == "" {
		fmt.Fprintln(os.Stderr, "empty parity test selection")
		code = 1
	}
	os.Exit(code)
}

func root(t *testing.T) string {
	t.Helper()
	executed = true
	r := os.Getenv("LQL_TEST_ROOT")
	if r == "" {
		t.Fatal("LQL_TEST_ROOT is required")
	}
	return r
}
func scratch(t *testing.T) string {
	t.Helper()
	d, e := os.MkdirTemp(filepath.Join(root(t), "build", "tmp"), "cli-parity-")
	if e != nil {
		t.Fatal(e)
	}
	t.Cleanup(func() { os.RemoveAll(d) })
	return d
}
func write(t *testing.T, path, data string) string {
	t.Helper()
	if e := os.WriteFile(path, []byte(data), 0600); e != nil {
		t.Fatal(e)
	}
	return path
}

type result struct {
	code     int
	out, err string
}

func run(t *testing.T, kind string, args ...string) result {
	t.Helper()
	r := root(t)
	var binary string
	switch kind {
	case "c":
		binary = filepath.Join(r, "build/release/clql")
	case "go":
		binary = filepath.Join(r, "build/reference-lql")
		args = append([]string{"-c"}, args...)
	case "lua":
		binary = filepath.Join(r, "build/debug-lua/lql_lua_runner")
		scripts, e := filepath.Glob(filepath.Join(r, "build/luarocks/lib/luarocks/rocks-5.5/liblql/*/bin/lql.lua"))
		if e != nil || len(scripts) != 1 {
			t.Fatalf("expected exactly one installed Lua CLI: %v %v", scripts, e)
		}
		args = append([]string{scripts[0]}, args...)
	default:
		t.Fatalf("unknown CLI %q", kind)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, binary, args...)
	cmd.Dir = r
	cmd.Env = append(os.Environ(), "LUA_PATH="+r+"/build/luarocks/share/lua/5.5/?.lua;"+r+"/build/luarocks/share/lua/5.5/?/init.lua;;", "LUA_CPATH="+r+"/build/luarocks/lib/lua/5.5/?.so;"+r+"/build/luarocks/lib/lua/5.5/?/core.so;;")
	var out, err bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &err
	e := cmd.Run()
	code := 0
	if e != nil {
		if x, ok := e.(*exec.ExitError); ok {
			code = x.ExitCode()
		} else {
			t.Fatalf("start %s: %v", kind, e)
		}
	}
	if ctx.Err() != nil {
		t.Fatalf("%s timed out: %v", kind, args)
	}
	return result{code, out.String(), err.String()}
}

type jsonNumber string

func normalize(v any) any {
	switch x := v.(type) {
	case json.Number:
		n, ok := new(big.Rat).SetString(string(x))
		if !ok {
			panic(x)
		}
		return jsonNumber(n.RatString())
	case []any:
		for i := range x {
			x[i] = normalize(x[i])
		}
	case map[string]any:
		for k := range x {
			x[k] = normalize(x[k])
		}
	}
	return v
}
func records(t *testing.T, s string) []any {
	t.Helper()
	var rows []any
	for _, line := range strings.Split(s, "\n") {
		if line == "" {
			continue
		}
		d := json.NewDecoder(strings.NewReader(line))
		d.UseNumber()
		var v any
		if e := d.Decode(&v); e != nil {
			t.Fatalf("invalid JSON %q: %v", line, e)
		}
		if d.More() {
			t.Fatalf("trailing JSON in %q", line)
		}
		rows = append(rows, normalize(v))
	}
	return rows
}
func jsonEqual(t *testing.T, a, b string) {
	t.Helper()
	if !reflect.DeepEqual(records(t, a), records(t, b)) {
		t.Fatalf("JSON mismatch\nleft=%s\nright=%s", a, b)
	}
}
func same(t *testing.T, a, b result, exact bool) {
	t.Helper()
	if a.code != b.code {
		t.Fatalf("exit mismatch: %+v / %+v", a, b)
	}
	if exact {
		if a.out != b.out {
			t.Fatalf("output mismatch: %+v / %+v", a, b)
		}
	} else if a.code == 0 {
		jsonEqual(t, a.out, b.out)
	}
}
func table(t *testing.T, kind string, count int) {
	t.Helper()
	data, e := os.ReadFile(filepath.Join(root(t), "tests/cli-parity", kind+".json"))
	if e != nil {
		t.Fatal(e)
	}
	var fixture struct {
		Inputs []string
		Cases  []struct {
			Name     string
			Input    int
			Args     []string
			Expected json.RawMessage
		}
	}
	if e = json.Unmarshal(data, &fixture); e != nil {
		t.Fatal(e)
	}
	if len(fixture.Cases) != count {
		t.Fatalf("expected %d cases, got %d", count, len(fixture.Cases))
	}
	dir := scratch(t)
	for i, c := range fixture.Cases {
		t.Run(fmt.Sprintf("%03d_%s", i, c.Name), func(t *testing.T) {
			if c.Input < 0 || c.Input >= len(fixture.Inputs) {
				t.Fatal("invalid input index")
			}
			input := write(t, filepath.Join(dir, "input.ndjson"), fixture.Inputs[c.Input])
			args := append(append([]string{}, c.Args...), input)
			a, b := run(t, "c", args...), run(t, "go", args...)
			if len(c.Expected) > 0 {
				if a.code != 0 {
					t.Fatalf("intentional divergence failed: %+v", a)
				}
				var values []json.RawMessage
				if e := json.Unmarshal(c.Expected, &values); e != nil {
					t.Fatal(e)
				}
				var expected strings.Builder
				for _, v := range values {
					var compact bytes.Buffer
					if err := json.Compact(&compact, v); err != nil {
						t.Fatal(err)
					}
					expected.Write(compact.Bytes())
					expected.WriteByte('\n')
				}
				jsonEqual(t, a.out, expected.String())
				return
			}
			// The CLI pair may report different nonzero statuses for rejected input.
			if a.code != 0 && b.code != 0 {
				return
			}
			same(t, a, b, false)
		})
	}
}
func TestSelector(t *testing.T) { table(t, "selector", 89) }
func TestMutation(t *testing.T) { table(t, "mutation", 361) }
func inline(t *testing.T, dir, other, data string) {
	t.Helper()
	a := write(t, filepath.Join(dir, "lua-inline.ndjson"), data)
	b := write(t, filepath.Join(dir, "other-inline.ndjson"), data)
	args := []string{"-i", "-m", "/status=ready", `/id="b"`}
	same(t, run(t, "lua", append(args, a)...), run(t, other, append(args, b)...), false)
	x, e := os.ReadFile(a)
	if e != nil {
		t.Fatal(e)
	}
	y, e := os.ReadFile(b)
	if e != nil {
		t.Fatal(e)
	}
	jsonEqual(t, string(x), string(y))
}
func TestLuaGo(t *testing.T) {
	dir := scratch(t)
	data := `{"id":"a","status":"open","progress":75,"service":"AUTH-edge","msg":"timeout on auth edge","labels":{"env":"production"},"items":[{"sku":"ABC-123"},{"sku":"ZZZ"}],"emoji":"日本語 😀"}
{"id":"b","status":"queued","progress":40,"service":"billing","msg":"all good","labels":{"env":"staging"},"items":[{"sku":"NOPE"}]}
{"id":"c","status":"closed","progress":55,"service":"EDGE-cache","msg":"degraded cache","labels":{"env":"production"},"items":[{"parts":[{"sku":"ABC-123"}]}]}
`
	input := write(t, filepath.Join(dir, "input.ndjson"), data)
	cases := [][]string{{`/status="open"`}, {"-O", `/status="open"`, `/status="queued"`}, {"contains{field=/msg,value=timeout}"}, {`/labels/*="production"`}, {`/items/**/sku="ABC-123"`}, {"-f", "/id", "-f", "/status", "/status!=missing"}, {"-m", "/seen=true"}, {"-M", "-m", "/status=ready", `/id="b"`}, {"-f", "/id", "-m", "/status=ready", "/status!=missing"}}
	for i, args := range cases {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			args = append(args, input)
			a, b := run(t, "lua", args...), run(t, "go", args...)
			same(t, a, b, false)
			for _, line := range strings.Split(a.out, "\n") {
				if line == "" {
					continue
				}
				var compact bytes.Buffer
				if e := json.Compact(&compact, []byte(line)); e != nil {
					t.Fatal(e)
				}
				if compact.String() != line {
					t.Fatal("Lua output is not compact JSON")
				}
			}
		})
	}
	same(t, run(t, "lua", `/status="open"`, input), run(t, "go", `/status="open"`, input), true)
	if r := run(t, "lua", "--count", "/status!=missing", input); r.code != 0 || r.out != "3\n" {
		t.Fatalf("count: %+v", r)
	}
	payload := write(t, filepath.Join(dir, "payload.txt"), "日本語 😀")
	write(t, input, "{\"id\":\"a\"}\n")
	args := []string{"-F", "-m", "textfile:/payload=" + payload, input}
	same(t, run(t, "lua", args...), run(t, "go", args...), false)
	inline(t, dir, "go", "{\"id\":\"a\",\"status\":\"new\"}\n{\"id\":\"b\",\"status\":\"old\"}\n")
	if r := run(t, "lua", "--theme", "dark", `/status="open"`); r.code == 0 || !strings.Contains(r.err, "unsupported") {
		t.Fatalf("theme: %+v", r)
	}
}
func TestLuaC(t *testing.T) {
	dir := scratch(t)
	data := `{"id":"a","status":"new","keep":1,"labels":{"env":"prod"}}
{"id":"b","status":"old","keep":2,"labels":{"env":"stage"}}
{"id":"c","status":"new","keep":3,"labels":{"env":"prod"}}
`
	input := write(t, filepath.Join(dir, "input.ndjson"), data)
	cases := [][]string{{`/status="new"`}, {"--count", `/status="new"`}, {"-cM", `/status="new"`}, {"--compact=false", "--matches-only=false", `/status="new"`}, {"-O", `/status="new"`, `/id="b"`}, {"--or=false", `/status="new"`, `/id="c"`}, {"-f/id", `/status="new"`}, {"-f=/id", `/status="new"`}, {`/status="new"`, "-f", "/id"}, {"-m/status=ready", `/id="b"`}, {"-m=/status=ready", `/id="b"`}, {`/id="b"`, "-m", "/status=ready"}, {"-M", "-m", "/status=ready", `/id="b"`}, {"--count", "-m", "/status=ready", `/id="b"`}, {"-f", "/id", "-m", "/status=ready", `/id="b"`}, {"--inline=false", "-m", "/status=ready", `/id="b"`}}
	for i, args := range cases {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			args = append(args, input)
			same(t, run(t, "lua", args...), run(t, "c", args...), true)
		})
	}
	for _, payload := range []struct{ mode, data string }{{"textfile", "日本語 😀 こんにちは"}, {"base64file", "a\x00b\xff"}} {
		path := write(t, filepath.Join(dir, payload.mode), payload.data)
		args := []string{"-F", "-M", "-m", payload.mode + ":/payload=" + path, `/id="a"`, input}
		same(t, run(t, "lua", args...), run(t, "c", args...), true)
	}
	inline(t, dir, "c", data)
	for _, kind := range []string{"lua", "c"} {
		r := run(t, kind, "-vh")
		if r.code != 0 {
			t.Fatalf("help: %+v", r)
		}
		for _, text := range []string{"usage:", "Selector examples (full LQL):", "Notes:", "only date{...,since=...} supports relative macros"} {
			if !strings.Contains(r.out, text) {
				t.Fatalf("missing help %q", text)
			}
		}
	}
	target := write(t, filepath.Join(dir, "target.ndjson"), data)
	link := filepath.Join(dir, "link.ndjson")
	if e := os.Symlink("target.ndjson", link); e != nil {
		t.Fatal(e)
	}
	fifo := filepath.Join(dir, "input.fifo")
	if e := syscall.Mkfifo(fifo, 0600); e != nil {
		t.Fatal(e)
	}
	selector := write(t, filepath.Join(dir, "selector-looking-file"), "not a selector\n")
	failures := []struct {
		name               string
		args               []string
		contains, excludes string
	}{
		{"help cluster", []string{"-hZ"}, "unknown flag", ""}, {"version cluster", []string{"-vZ"}, "unknown flag", ""},
		{"boolean yes", []string{"--compact=yes", `/status="new"`, input}, "invalid boolean value for --compact", ""},
		{"boolean no", []string{"--or=no", `/status="new"`, input}, "invalid boolean value for --or", ""},
		{"write boolean", []string{"--write=maybe", "-m", "/status=ready", `/id="b"`, input}, "invalid boolean value for --write", ""},
		{"theme missing", []string{"-t"}, "--theme requires a value", "prettyx"},
		{"theme unsupported", []string{"-t", "jq", input}, "prettyx", ""},
		{"file disabled", []string{"-m", "file:/payload=blob.txt", input}, "file-backed mutations are disabled", ""},
		{"inline count", []string{"--count", "-i", "-m", "/status=ready", `/id="b"`, input}, "inline mutation cannot be combined with --count", ""},
		{"symlink", []string{"-i", "-m", "/status=ready", link}, "inline mode does not rewrite symlink paths", ""},
		{"fifo", []string{"-i", "-m", "/status=ready", fifo}, "inline mode requires a single JSON file", ""},
		{"selector path", []string{"-i", "-m", "/status=ready", selector, target}, "inline mode requires a single JSON file", ""},
	}
	for _, f := range failures {
		t.Run(f.name, func(t *testing.T) {
			for _, kind := range []string{"lua", "c"} {
				r := run(t, kind, f.args...)
				if r.code == 0 || !strings.Contains(r.err, f.contains) || (f.excludes != "" && strings.Contains(r.err, f.excludes)) {
					t.Fatalf("%s: %+v", kind, r)
				}
			}
		})
	}
}

func TestClqlXattr(t *testing.T) {
	path := write(t, filepath.Join(scratch(t), "inline.ndjson"), "{\"id\":\"a\",\"status\":\"old\"}\n")
	if err := syscall.Setxattr(path, "user.lqltest", []byte("kept"), 0); err != nil {
		t.Fatal(err)
	}
	if err := syscall.Setxattr(path, "user.lqlempty", []byte{}, 0); err != nil {
		t.Fatal(err)
	}
	if r := run(t, "c", "-i", "-m", "/status=ready", `/id="a"`, path); r.code != 0 {
		t.Fatalf("inline mutation: %+v", r)
	}
	value := make([]byte, 32)
	n, err := syscall.Getxattr(path, "user.lqltest", value)
	if err != nil || string(value[:n]) != "kept" {
		t.Fatalf("extended attribute changed: %q %v", value[:n], err)
	}
	if n, err = syscall.Getxattr(path, "user.lqlempty", value); err != nil || n != 0 {
		t.Fatalf("empty extended attribute changed: len=%d %v", n, err)
	}
}

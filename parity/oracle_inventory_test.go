package parity

import (
	"bytes"
	"encoding/json"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

type oracleModule struct {
	Path    string
	Version string
	Dir     string
}

type oracleInventoryRow struct {
	File        string
	SymbolCount int
	Surface     string
	Status      string
	Evidence    string
	Next        string
}

func TestOracleInventory(t *testing.T) {
	module := loadOracleModule(t)
	if module.Path != "pkt.systems/lql" {
		t.Fatalf("unexpected oracle module path: %s", module.Path)
	}
	if module.Version != "v0.17.1" {
		t.Fatalf("unexpected oracle module version: %s", module.Version)
	}
	actual := collectOracleSymbols(t, module.Dir)
	inventory := readOracleInventory(t, "oracle_inventory.tsv")
	if err := validateOracleInventory(inventory, actual); err != nil {
		t.Fatal(err)
	}
}

func loadOracleModule(t *testing.T) oracleModule {
	t.Helper()
	cmd := exec.Command("go", "list", "-m", "-json", "pkt.systems/lql")
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("go list oracle module: %v", err)
	}
	var module oracleModule
	if err := json.Unmarshal(out, &module); err != nil {
		t.Fatalf("decode oracle module: %v", err)
	}
	if module.Dir == "" {
		t.Fatal("oracle module directory is empty")
	}
	return module
}

func collectOracleSymbols(t *testing.T, root string) map[string]int {
	t.Helper()
	counts := make(map[string]int)
	fset := token.NewFileSet()
	err := filepath.Walk(root, func(path string, info os.FileInfo, walkErr error) error {
		var rel string
		if walkErr != nil {
			return walkErr
		}
		if info.IsDir() {
			name := info.Name()
			if name == ".git" || name == "vendor" {
				return filepath.SkipDir
			}
			return nil
		}
		if !strings.HasSuffix(info.Name(), "_test.go") {
			return nil
		}
		file, err := parser.ParseFile(fset, path, nil, 0)
		if err != nil {
			return err
		}
		rel, err = filepath.Rel(root, path)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		for _, decl := range file.Decls {
			fn, ok := decl.(*ast.FuncDecl)
			if !ok || fn.Recv != nil {
				continue
			}
			if strings.HasPrefix(fn.Name.Name, "Test") ||
				strings.HasPrefix(fn.Name.Name, "Benchmark") ||
				strings.HasPrefix(fn.Name.Name, "Example") {
				counts[rel]++
			}
		}
		return nil
	})
	if err != nil {
		t.Fatalf("collect oracle symbols: %v", err)
	}
	if len(counts) == 0 {
		t.Fatal("no oracle test, benchmark, or example symbols found")
	}
	return counts
}

func readOracleInventory(t *testing.T, path string) []oracleInventoryRow {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read oracle inventory: %v", err)
	}
	lines := bytes.Split(data, []byte{'\n'})
	var rows []oracleInventoryRow
	for i, raw := range lines {
		line := string(bytes.TrimSpace(raw))
		if line == "" {
			continue
		}
		if i == 0 {
			if line != "oracle_file\tsymbol_count\tsurface\tstatus\tevidence\tnext" {
				t.Fatalf("unexpected oracle inventory header: %q", line)
			}
			continue
		}
		fields := strings.Split(line, "\t")
		if len(fields) != 6 {
			t.Fatalf("oracle inventory line %d has %d fields, want 6: %q",
				i+1, len(fields), line)
		}
		count, err := strconv.Atoi(fields[1])
		if err != nil {
			t.Fatalf("oracle inventory line %d has invalid symbol count: %v", i+1, err)
		}
		rows = append(rows, oracleInventoryRow{
			File:        fields[0],
			SymbolCount: count,
			Surface:     fields[2],
			Status:      fields[3],
			Evidence:    fields[4],
			Next:        fields[5],
		})
	}
	if len(rows) == 0 {
		t.Fatal("oracle inventory has no rows")
	}
	return rows
}

func validateOracleInventory(rows []oracleInventoryRow, actual map[string]int) error {
	seen := make(map[string]struct{})
	knownStatus := map[string]bool{
		"covered":        true,
		"partial":        true,
		"gap":            true,
		"not-applicable": true,
	}
	for _, row := range rows {
		if row.File == "" || row.Surface == "" || row.Status == "" ||
			row.Evidence == "" || row.Next == "" {
			return fmt.Errorf("oracle inventory has incomplete row: %#v", row)
		}
		if strings.Contains(row.File, "..") || strings.Contains(row.Evidence, "..") ||
			strings.Contains(row.Next, "..") {
			return fmt.Errorf("oracle inventory must not contain parent-relative paths: %s", row.File)
		}
		if strings.Contains(row.Evidence, "/home/") || strings.Contains(row.Next, "/home/") {
			return fmt.Errorf("oracle inventory must not contain local absolute paths: %s", row.File)
		}
		if !knownStatus[row.Status] {
			return fmt.Errorf("oracle inventory has unknown status %q for %s", row.Status, row.File)
		}
		if row.SymbolCount <= 0 {
			return fmt.Errorf("oracle inventory has non-positive symbol count for %s", row.File)
		}
		if _, ok := seen[row.File]; ok {
			return fmt.Errorf("oracle inventory lists %s more than once", row.File)
		}
		seen[row.File] = struct{}{}
		count, ok := actual[row.File]
		if !ok {
			return fmt.Errorf("oracle inventory lists missing oracle file %s", row.File)
		}
		if count != row.SymbolCount {
			return fmt.Errorf("oracle inventory count mismatch for %s: got %d want %d",
				row.File, row.SymbolCount, count)
		}
	}
	for file := range actual {
		if _, ok := seen[file]; !ok {
			return fmt.Errorf("oracle inventory is missing oracle file %s", file)
		}
	}
	return nil
}

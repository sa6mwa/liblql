package parity

import (
	"encoding/json"
	"testing"
	"time"

	"pkt.systems/lql"
)

func TestMutationLiteralParityContract(t *testing.T) {
	muts, err := lql.ParseMutations([]string{
		`/escaped="a\"b\n\uD83D\uDE00"`,
		`/japanese=日本語`,
		`/emoji=😀`,
		`/supplementary=𐐷`,
	}, time.Unix(1_700_000_000, 0))
	if err != nil {
		t.Fatalf("ParseMutations: %v", err)
	}
	doc := map[string]any{"status": "open"}
	if err := lql.ApplyMutations(doc, muts); err != nil {
		t.Fatalf("ApplyMutations: %v", err)
	}
	if got, want := doc["escaped"], `a\"b\n\uD83D\uDE00`; got != want {
		t.Fatalf("escaped quoted mutation: got %#v, want %#v", got, want)
	}
	for key, want := range map[string]string{
		"japanese":      "日本語",
		"emoji":         "😀",
		"supplementary": "𐐷",
	} {
		if got := doc[key]; got != want {
			t.Fatalf("%s mutation: got %#v, want %#v", key, got, want)
		}
	}
	encoded, err := json.Marshal(doc)
	if err != nil {
		t.Fatalf("json.Marshal: %v", err)
	}
	var roundTripped map[string]string
	if err := json.Unmarshal(encoded, &roundTripped); err != nil {
		t.Fatalf("json.Unmarshal: %v", err)
	}
	if got, want := roundTripped["escaped"], `a\"b\n\uD83D\uDE00`; got != want {
		t.Fatalf("escaped JSON round trip: got %#v, want %#v", got, want)
	}
}

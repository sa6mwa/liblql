//go:build cgo && liblql_sdk_parity

package parity

/*
#include <stdlib.h>
#include <string.h>
#include <lql/lql.h>

static int liblql_matches_json(const char *expr, const char *json, int or_mode,
                               int *out_matched, char *errbuf,
                               size_t errbuf_len) {
	lql_error error;
	lql_selector *selector;
	lql_status status;

	lql_error_init(&error);
	selector = NULL;
	if (or_mode) {
		status = lql_selector_parse_or(expr, &selector, &error);
	} else {
		status = lql_selector_parse(expr, &selector, &error);
	}
	if (status != LQL_STATUS_OK) {
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, error.message, errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return (int)status;
	}
	status = lql_matches_json(selector, json, strlen(json), out_matched, &error);
	lql_selector_free(selector);
	if (status != LQL_STATUS_OK) {
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, error.message, errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return (int)status;
	}
	return 0;
}

static int liblql_parse_selector(const char *expr, int or_mode, char *errbuf,
                                 size_t errbuf_len) {
	lql_error error;
	lql_selector *selector;
	lql_status status;

	lql_error_init(&error);
	selector = NULL;
	if (or_mode) {
		status = lql_selector_parse_or(expr, &selector, &error);
	} else {
		status = lql_selector_parse(expr, &selector, &error);
	}
	lql_selector_free(selector);
	if (status != LQL_STATUS_OK && errbuf != NULL && errbuf_len > 0u) {
		strncpy(errbuf, error.message, errbuf_len - 1u);
		errbuf[errbuf_len - 1u] = '\0';
	}
	return (int)status;
}
*/
import "C"

import "unsafe"

func cMatchesJSON(expr, doc string, orMode bool) (bool, error) {
	cExpr := C.CString(expr)
	cDoc := C.CString(doc)
	defer C.free(unsafe.Pointer(cExpr))
	defer C.free(unsafe.Pointer(cDoc))

	var matched C.int
	var errbuf [256]C.char
	status := C.liblql_matches_json(cExpr, cDoc, cBool(orMode), &matched,
		&errbuf[0], C.size_t(len(errbuf)))
	if status != 0 {
		return false, sdkParityError(C.GoString(&errbuf[0]))
	}
	return matched != 0, nil
}

func cParseSelector(expr string, orMode bool) (int, string) {
	cExpr := C.CString(expr)
	defer C.free(unsafe.Pointer(cExpr))

	var errbuf [256]C.char
	status := C.liblql_parse_selector(cExpr, cBool(orMode), &errbuf[0],
		C.size_t(len(errbuf)))
	return int(status), C.GoString(&errbuf[0])
}

func cBool(value bool) C.int {
	if value {
		return 1
	}
	return 0
}

type sdkParityError string

func (err sdkParityError) Error() string {
	if err == "" {
		return "liblql returned an error"
	}
	return string(err)
}

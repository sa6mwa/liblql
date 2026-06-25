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

static int liblql_read_tmp(FILE *tmp, char **out_json, size_t *out_len,
                           char *errbuf, size_t errbuf_len) {
	long size;
	char *buffer;
	size_t read_size;

	if (fflush(tmp) != 0 || fseek(tmp, 0L, SEEK_END) != 0) {
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, "failed to inspect temporary output", errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return -1;
	}
	size = ftell(tmp);
	if (size < 0L || fseek(tmp, 0L, SEEK_SET) != 0) {
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, "failed to rewind temporary output", errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return -1;
	}
	buffer = (char *)malloc((size_t)size + 1u);
	if (buffer == NULL) {
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, "out of memory", errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return -1;
	}
	read_size = fread(buffer, 1u, (size_t)size, tmp);
	if (read_size != (size_t)size) {
		free(buffer);
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, "failed to read temporary output", errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return -1;
	}
	buffer[size] = '\0';
	*out_json = buffer;
	*out_len = (size_t)size;
	return 0;
}

static int liblql_project_json_value(const char *const *fields,
                                     size_t field_count, const char *json,
                                     char **out_json, size_t *out_len,
                                     int *out_found, char *errbuf,
                                     size_t errbuf_len) {
	lql_error error;
	lql_projection *projection;
	lql_status status;
	FILE *tmp;

	lql_error_init(&error);
	projection = NULL;
	*out_json = NULL;
	*out_len = 0u;
	*out_found = 0;
	status = lql_projection_parse(fields, field_count, &projection, &error);
	if (status != LQL_STATUS_OK) {
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, error.message, errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return (int)status;
	}
	tmp = tmpfile();
	if (tmp == NULL) {
		lql_projection_free(projection);
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, "failed to create temporary output", errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return -1;
	}
	status = lql_project_json(projection, json, strlen(json), tmp, out_found,
	                          &error);
	lql_projection_free(projection);
	if (status != LQL_STATUS_OK) {
		fclose(tmp);
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, error.message, errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return (int)status;
	}
	if (liblql_read_tmp(tmp, out_json, out_len, errbuf, errbuf_len) != 0) {
		fclose(tmp);
		return -1;
	}
	fclose(tmp);
	return 0;
}

static int liblql_mutate_json_value(const char *const *exprs,
                                    size_t expr_count, const char *json,
                                    char **out_json, size_t *out_len,
                                    char *errbuf, size_t errbuf_len) {
	lql_error error;
	lql_mutation_plan *plan;
	lql_status status;
	FILE *tmp;

	lql_error_init(&error);
	plan = NULL;
	*out_json = NULL;
	*out_len = 0u;
	status = lql_mutation_plan_parse(exprs, expr_count, &plan, &error);
	if (status != LQL_STATUS_OK) {
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, error.message, errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return (int)status;
	}
	tmp = tmpfile();
	if (tmp == NULL) {
		lql_mutation_plan_free(plan);
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, "failed to create temporary output", errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return -1;
	}
	status = lql_mutate_json(plan, json, strlen(json), tmp, &error);
	lql_mutation_plan_free(plan);
	if (status != LQL_STATUS_OK) {
		fclose(tmp);
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, error.message, errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return (int)status;
	}
	if (liblql_read_tmp(tmp, out_json, out_len, errbuf, errbuf_len) != 0) {
		fclose(tmp);
		return -1;
	}
	fclose(tmp);
	return 0;
}
*/
import "C"

import (
	"unsafe"
)

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

func cProjectJSON(fields []string, doc string) ([]byte, bool, error) {
	cFields, freeFields := cStringArray(fields)
	defer freeFields()

	cDoc := C.CString(doc)
	defer C.free(unsafe.Pointer(cDoc))

	var out *C.char
	var outLen C.size_t
	var found C.int
	var errbuf [256]C.char
	status := C.liblql_project_json_value(cFields, C.size_t(len(fields)), cDoc,
		&out, &outLen, &found, &errbuf[0], C.size_t(len(errbuf)))
	if status != 0 {
		return nil, false, sdkParityError(C.GoString(&errbuf[0]))
	}
	defer C.free(unsafe.Pointer(out))
	return C.GoBytes(unsafe.Pointer(out), C.int(outLen)), found != 0, nil
}

func cMutateJSON(mutations []string, doc string) ([]byte, error) {
	cExprs, freeExprs := cStringArray(mutations)
	defer freeExprs()

	cDoc := C.CString(doc)
	defer C.free(unsafe.Pointer(cDoc))

	var out *C.char
	var outLen C.size_t
	var errbuf [256]C.char
	status := C.liblql_mutate_json_value(cExprs, C.size_t(len(mutations)), cDoc,
		&out, &outLen, &errbuf[0], C.size_t(len(errbuf)))
	if status != 0 {
		return nil, sdkParityError(C.GoString(&errbuf[0]))
	}
	defer C.free(unsafe.Pointer(out))
	return C.GoBytes(unsafe.Pointer(out), C.int(outLen)), nil
}

func cStringArray(values []string) (**C.char, func()) {
	if len(values) == 0 {
		return nil, func() {}
	}
	cValues := make([]*C.char, len(values))
	for i, value := range values {
		cValues[i] = C.CString(value)
	}
	return (**C.char)(unsafe.Pointer(&cValues[0])), func() {
		for _, value := range cValues {
			C.free(unsafe.Pointer(value))
		}
	}
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

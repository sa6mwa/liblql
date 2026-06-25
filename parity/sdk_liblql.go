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

static int liblql_parse_mutations(const char *const *exprs, size_t expr_count,
                                  int enable_file_values,
                                  const char *file_value_base_dir,
                                  size_t *out_count, char *errbuf,
                                  size_t errbuf_len) {
	lql_error error;
	lql_mutation_plan *plan;
	lql_mutation_parse_options options;
	lql_status status;

	lql_error_init(&error);
	memset(&options, 0, sizeof(options));
	plan = NULL;
	if (out_count != NULL) {
		*out_count = 0u;
	}
	if (enable_file_values) {
		options.enable_file_values = 1;
		options.file_value_base_dir = file_value_base_dir;
		status = lql_mutation_plan_parse_with_options(exprs, expr_count, &options,
		                                              &plan, &error);
	} else {
		status = lql_mutation_plan_parse(exprs, expr_count, &plan, &error);
	}
	if (status != LQL_STATUS_OK) {
		lql_mutation_plan_free(plan);
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, error.message, errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return (int)status;
	}
	if (out_count != NULL) {
		*out_count = lql_mutation_plan_count(plan);
	}
	lql_mutation_plan_free(plan);
	return 0;
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

static int liblql_prepare_range_input(const char *prefix, const char *json,
                                      const char *suffix, FILE **out_file,
                                      unsigned long long *out_offset,
                                      unsigned long long *out_size,
                                      char *errbuf, size_t errbuf_len) {
	FILE *input;
	size_t prefix_len;
	size_t json_len;
	size_t suffix_len;

	input = tmpfile();
	if (input == NULL) {
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, "failed to create temporary input", errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return -1;
	}
	prefix_len = strlen(prefix);
	json_len = strlen(json);
	suffix_len = strlen(suffix);
	if (fwrite(prefix, 1u, prefix_len, input) != prefix_len ||
	    fwrite(json, 1u, json_len, input) != json_len ||
	    fwrite(suffix, 1u, suffix_len, input) != suffix_len ||
	    fseek(input, 0L, SEEK_SET) != 0) {
		fclose(input);
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, "failed to prepare temporary input", errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return -1;
	}
	*out_file = input;
	*out_offset = (unsigned long long)prefix_len;
	*out_size = (unsigned long long)json_len;
	return 0;
}

static int liblql_project_file_range_value(const char *const *fields,
                                           size_t field_count,
                                           const char *prefix,
                                           const char *json,
                                           const char *suffix,
                                           char **out_json, size_t *out_len,
                                           int *out_found, char *errbuf,
                                           size_t errbuf_len) {
	lql_error error;
	lql_projection *projection;
	lql_status status;
	FILE *input;
	FILE *tmp;
	unsigned long long offset;
	unsigned long long size;

	lql_error_init(&error);
	projection = NULL;
	input = NULL;
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
	if (liblql_prepare_range_input(prefix, json, suffix, &input, &offset, &size,
	                               errbuf, errbuf_len) != 0) {
		lql_projection_free(projection);
		return -1;
	}
	tmp = tmpfile();
	if (tmp == NULL) {
		fclose(input);
		lql_projection_free(projection);
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, "failed to create temporary output", errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return -1;
	}
	status = lql_project_file_range(projection, input, (lql_uint64)offset,
	                                (lql_uint64)size, tmp, out_found, &error);
	fclose(input);
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
                                    int enable_file_values,
                                    const char *file_value_base_dir,
                                    char **out_json, size_t *out_len,
                                    char *errbuf, size_t errbuf_len) {
	lql_error error;
	lql_mutation_plan *plan;
	lql_mutation_parse_options options;
	lql_status status;
	FILE *tmp;

	lql_error_init(&error);
	memset(&options, 0, sizeof(options));
	plan = NULL;
	*out_json = NULL;
	*out_len = 0u;
	if (enable_file_values) {
		options.enable_file_values = 1;
		options.file_value_base_dir = file_value_base_dir;
		status = lql_mutation_plan_parse_with_options(exprs, expr_count, &options,
		                                              &plan, &error);
	} else {
		status = lql_mutation_plan_parse(exprs, expr_count, &plan, &error);
	}
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

static int liblql_mutate_file_range_value(const char *const *exprs,
                                          size_t expr_count,
                                          const char *prefix, const char *json,
                                          const char *suffix,
                                          int enable_file_values,
                                          const char *file_value_base_dir,
                                          int root_fields_only,
                                          char **out_json, size_t *out_len,
                                          char *errbuf, size_t errbuf_len) {
	lql_error error;
	lql_mutation_plan *plan;
	lql_mutation_parse_options options;
	lql_status status;
	FILE *input;
	FILE *tmp;
	unsigned long long offset;
	unsigned long long size;

	lql_error_init(&error);
	memset(&options, 0, sizeof(options));
	plan = NULL;
	input = NULL;
	*out_json = NULL;
	*out_len = 0u;
	if (enable_file_values) {
		options.enable_file_values = 1;
		options.file_value_base_dir = file_value_base_dir;
		status = lql_mutation_plan_parse_with_options(exprs, expr_count, &options,
		                                              &plan, &error);
	} else {
		status = lql_mutation_plan_parse(exprs, expr_count, &plan, &error);
	}
	if (status != LQL_STATUS_OK) {
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, error.message, errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return (int)status;
	}
	if (liblql_prepare_range_input(prefix, json, suffix, &input, &offset, &size,
	                               errbuf, errbuf_len) != 0) {
		lql_mutation_plan_free(plan);
		return -1;
	}
	tmp = tmpfile();
	if (tmp == NULL) {
		fclose(input);
		lql_mutation_plan_free(plan);
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, "failed to create temporary output", errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return -1;
	}
	if (root_fields_only) {
		status = lql_mutate_file_range_root_fields(
		    plan, input, (lql_uint64)offset, (lql_uint64)size, tmp, &error);
	} else {
		status = lql_mutate_file_range_paths(plan, input, (lql_uint64)offset,
		                                     (lql_uint64)size, tmp, &error);
	}
	fclose(input);
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

static int liblql_compact_json_value(const char *json, char **out_json,
                                     size_t *out_len, char *errbuf,
                                     size_t errbuf_len) {
	lql_error error;
	lql_status status;
	FILE *tmp;

	lql_error_init(&error);
	*out_json = NULL;
	*out_len = 0u;
	tmp = tmpfile();
	if (tmp == NULL) {
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, "failed to create temporary output", errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return -1;
	}
	status = lql_compact_json(json, strlen(json), tmp, &error);
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

static int liblql_compact_file_range_value(const char *prefix, const char *json,
                                           const char *suffix,
                                           char **out_json, size_t *out_len,
                                           char *errbuf, size_t errbuf_len) {
	lql_error error;
	lql_status status;
	FILE *input;
	FILE *tmp;
	unsigned long long offset;
	unsigned long long size;

	lql_error_init(&error);
	input = NULL;
	*out_json = NULL;
	*out_len = 0u;
	if (liblql_prepare_range_input(prefix, json, suffix, &input, &offset, &size,
	                               errbuf, errbuf_len) != 0) {
		return -1;
	}
	tmp = tmpfile();
	if (tmp == NULL) {
		fclose(input);
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, "failed to create temporary output", errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return -1;
	}
	status = lql_compact_file_range(input, (lql_uint64)offset, (lql_uint64)size,
	                                tmp, &error);
	fclose(input);
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

typedef struct liblql_stream_summary {
	unsigned long long candidates_seen;
	unsigned long long candidates_matched;
	unsigned long long bytes_read;
	int stopped_early;
	int stop_reason;
	int decision_callbacks;
	int match_callbacks;
	int seekable_payloads;
	int spooled_payloads;
	char *payload_json;
	size_t payload_len;
} liblql_stream_summary;

typedef struct liblql_stream_state {
	liblql_stream_summary *summary;
	FILE *payload_out;
	int stop_after_first;
} liblql_stream_state;

typedef struct liblql_chunk_source {
	const unsigned char *data;
	size_t len;
	size_t offset;
	size_t chunk_size;
} liblql_chunk_source;

static lql_status liblql_count_decision(void *user,
                                        const lql_query_decision *decision) {
	liblql_stream_state *state;
	state = (liblql_stream_state *)user;
	++state->summary->decision_callbacks;
	if (decision->matched) {
		++state->summary->match_callbacks;
	}
	if (state->stop_after_first && state->summary->decision_callbacks == 1) {
		return LQL_STATUS_STOP;
	}
	return LQL_STATUS_OK;
}

static lql_status liblql_collect_match(void *user,
                                       const lql_query_match *match) {
	liblql_stream_state *state;
	lql_error error;
	lql_status status;
	state = (liblql_stream_state *)user;
	++state->summary->match_callbacks;
	if (match->payload.kind == LQL_PAYLOAD_SEEKABLE_RANGE) {
		++state->summary->seekable_payloads;
	}
	if (match->payload.kind == LQL_PAYLOAD_SPOOLED) {
		++state->summary->spooled_payloads;
	}
	lql_error_init(&error);
	status = lql_payload_write_json(&match->payload, state->payload_out, &error);
	if (status != LQL_STATUS_OK) {
		return status;
	}
	if (state->stop_after_first && state->summary->match_callbacks == 1) {
		return LQL_STATUS_STOP;
	}
	return LQL_STATUS_OK;
}

static lql_read_result liblql_chunk_read(void *user, unsigned char *buffer,
                                         size_t capacity) {
	liblql_chunk_source *source;
	lql_read_result result;
	size_t remaining;
	size_t count;
	source = (liblql_chunk_source *)user;
	memset(&result, 0, sizeof(result));
	if (source->offset >= source->len) {
		result.eof = 1;
		return result;
	}
	remaining = source->len - source->offset;
	count = source->chunk_size;
	if (count == 0u || count > capacity) {
		count = capacity;
	}
	if (count > remaining) {
		count = remaining;
	}
	memcpy(buffer, source->data + source->offset, count);
	source->offset += count;
	result.bytes_read = count;
	result.eof = source->offset >= source->len ? 1 : 0;
	return result;
}

static void liblql_copy_query_result(liblql_stream_summary *summary,
                                     const lql_query_result *result) {
	summary->candidates_seen = (unsigned long long)result->candidates_seen;
	summary->candidates_matched = (unsigned long long)result->candidates_matched;
	summary->bytes_read = (unsigned long long)result->bytes_read;
	summary->stopped_early = result->stopped_early;
	summary->stop_reason = (int)result->stop_reason;
}

static int liblql_stream_query(const char *expr, const char *json, int mode,
                               unsigned long long max_matches,
                               unsigned long long max_candidates,
                               unsigned long long max_bytes_read,
                               int stop_after_first,
                               liblql_stream_summary *summary, char *errbuf,
                               size_t errbuf_len) {
	lql_error error;
	lql_selector *selector;
	lql_query_options options;
	lql_query_result result;
	lql_status status;
	FILE *input;
	FILE *payload_out;
	liblql_stream_state state;
	liblql_chunk_source source;

	lql_error_init(&error);
	memset(summary, 0, sizeof(*summary));
	memset(&options, 0, sizeof(options));
	memset(&result, 0, sizeof(result));
	memset(&state, 0, sizeof(state));
	memset(&source, 0, sizeof(source));
	selector = NULL;
	input = NULL;
	payload_out = NULL;
	options.max_matches = (lql_uint64)max_matches;
	options.max_candidates = (lql_uint64)max_candidates;
	options.max_bytes_read = (lql_uint64)max_bytes_read;
	status = lql_selector_parse(expr, &selector, &error);
	if (status != LQL_STATUS_OK) {
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, error.message, errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return (int)status;
	}
	state.summary = summary;
	state.stop_after_first = stop_after_first;
	if (mode == 0) {
		input = tmpfile();
		if (input == NULL) {
			lql_selector_free(selector);
			if (errbuf != NULL && errbuf_len > 0u) {
				strncpy(errbuf, "failed to create temporary input", errbuf_len - 1u);
				errbuf[errbuf_len - 1u] = '\0';
			}
			return -1;
		}
		if (fwrite(json, 1u, strlen(json), input) != strlen(json) ||
		    fseek(input, 0L, SEEK_SET) != 0) {
			fclose(input);
			lql_selector_free(selector);
			if (errbuf != NULL && errbuf_len > 0u) {
				strncpy(errbuf, "failed to prepare temporary input", errbuf_len - 1u);
				errbuf[errbuf_len - 1u] = '\0';
			}
			return -1;
		}
		status = lql_query_file_decisions_with_options(
		    selector, input, &options, liblql_count_decision, &state, &result,
		    &error);
		fclose(input);
	} else if (mode == 3) {
		source.data = (const unsigned char *)json;
		source.len = strlen(json);
		source.chunk_size = 5u;
		status = lql_query_source_decisions_with_options(
		    selector, liblql_chunk_read, &source, &options,
		    liblql_count_decision, &state, &result, &error);
	} else {
		payload_out = tmpfile();
		if (payload_out == NULL) {
			lql_selector_free(selector);
			if (errbuf != NULL && errbuf_len > 0u) {
				strncpy(errbuf, "failed to create temporary payload output",
				        errbuf_len - 1u);
				errbuf[errbuf_len - 1u] = '\0';
			}
			return -1;
		}
		state.payload_out = payload_out;
		if (mode == 1) {
			input = tmpfile();
			if (input == NULL) {
				fclose(payload_out);
				lql_selector_free(selector);
				if (errbuf != NULL && errbuf_len > 0u) {
					strncpy(errbuf, "failed to create temporary input",
					        errbuf_len - 1u);
					errbuf[errbuf_len - 1u] = '\0';
				}
				return -1;
			}
			if (fwrite(json, 1u, strlen(json), input) != strlen(json) ||
			    fseek(input, 0L, SEEK_SET) != 0) {
				fclose(input);
				fclose(payload_out);
				lql_selector_free(selector);
				if (errbuf != NULL && errbuf_len > 0u) {
					strncpy(errbuf, "failed to prepare temporary input",
					        errbuf_len - 1u);
					errbuf[errbuf_len - 1u] = '\0';
				}
				return -1;
			}
			status = lql_query_file_matches_with_options(
			    selector, input, &options, liblql_collect_match, &state, &result,
			    &error);
			fclose(input);
		} else if (mode == 2) {
			source.data = (const unsigned char *)json;
			source.len = strlen(json);
			source.chunk_size = 7u;
			status = lql_query_source_spooled_matches_with_options(
			    selector, liblql_chunk_read, &source, &options,
			    liblql_collect_match, &state, &result, &error);
		} else {
			fclose(payload_out);
			lql_selector_free(selector);
			if (errbuf != NULL && errbuf_len > 0u) {
				strncpy(errbuf, "unsupported stream parity mode", errbuf_len - 1u);
				errbuf[errbuf_len - 1u] = '\0';
			}
			return -1;
		}
		if (status == LQL_STATUS_OK &&
		    liblql_read_tmp(payload_out, &summary->payload_json,
		                    &summary->payload_len, errbuf, errbuf_len) != 0) {
			fclose(payload_out);
			lql_selector_free(selector);
			return -1;
		}
		fclose(payload_out);
	}
	lql_selector_free(selector);
	if (status != LQL_STATUS_OK) {
		if (errbuf != NULL && errbuf_len > 0u) {
			strncpy(errbuf, error.message, errbuf_len - 1u);
			errbuf[errbuf_len - 1u] = '\0';
		}
		return (int)status;
	}
	liblql_copy_query_result(summary, &result);
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

func cParseMutations(mutations []string, enableFileValues bool, fileValueBaseDir string) (int, int, string) {
	cExprs, freeExprs := cStringArray(mutations)
	defer freeExprs()

	var cBaseDir *C.char
	if fileValueBaseDir != "" {
		cBaseDir = C.CString(fileValueBaseDir)
		defer C.free(unsafe.Pointer(cBaseDir))
	}

	var count C.size_t
	var errbuf [256]C.char
	status := C.liblql_parse_mutations(cExprs, C.size_t(len(mutations)),
		cBool(enableFileValues), cBaseDir, &count, &errbuf[0],
		C.size_t(len(errbuf)))
	if status != 0 {
		return int(status), 0, C.GoString(&errbuf[0])
	}
	return int(status), int(count), ""
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

func cProjectFileRange(fields []string, prefix, doc, suffix string) ([]byte, bool, error) {
	cFields, freeFields := cStringArray(fields)
	defer freeFields()

	cPrefix := C.CString(prefix)
	cDoc := C.CString(doc)
	cSuffix := C.CString(suffix)
	defer C.free(unsafe.Pointer(cPrefix))
	defer C.free(unsafe.Pointer(cDoc))
	defer C.free(unsafe.Pointer(cSuffix))

	var out *C.char
	var outLen C.size_t
	var found C.int
	var errbuf [256]C.char
	status := C.liblql_project_file_range_value(cFields, C.size_t(len(fields)),
		cPrefix, cDoc, cSuffix, &out, &outLen, &found, &errbuf[0],
		C.size_t(len(errbuf)))
	if status != 0 {
		return nil, false, sdkParityError(C.GoString(&errbuf[0]))
	}
	defer C.free(unsafe.Pointer(out))
	return C.GoBytes(unsafe.Pointer(out), C.int(outLen)), found != 0, nil
}

func cMutateJSON(mutations []string, doc string) ([]byte, error) {
	return cMutateJSONWithOptions(mutations, doc, false, "")
}

func cMutateJSONWithOptions(mutations []string, doc string, enableFileValues bool, fileValueBaseDir string) ([]byte, error) {
	cExprs, freeExprs := cStringArray(mutations)
	defer freeExprs()

	cDoc := C.CString(doc)
	defer C.free(unsafe.Pointer(cDoc))
	var cBaseDir *C.char
	if fileValueBaseDir != "" {
		cBaseDir = C.CString(fileValueBaseDir)
		defer C.free(unsafe.Pointer(cBaseDir))
	}

	var out *C.char
	var outLen C.size_t
	var errbuf [256]C.char
	status := C.liblql_mutate_json_value(cExprs, C.size_t(len(mutations)), cDoc,
		cBool(enableFileValues), cBaseDir, &out, &outLen, &errbuf[0],
		C.size_t(len(errbuf)))
	if status != 0 {
		return nil, sdkParityError(C.GoString(&errbuf[0]))
	}
	defer C.free(unsafe.Pointer(out))
	return C.GoBytes(unsafe.Pointer(out), C.int(outLen)), nil
}

func cMutateFileRange(mutations []string, prefix, doc, suffix string) ([]byte, error) {
	return cMutateFileRangeWithOptions(mutations, prefix, doc, suffix, false, "")
}

func cMutateFileRangeWithOptions(mutations []string, prefix, doc, suffix string, enableFileValues bool, fileValueBaseDir string) ([]byte, error) {
	return cMutateFileRangeMode(mutations, prefix, doc, suffix, enableFileValues, fileValueBaseDir, false)
}

func cMutateFileRangeRootFields(mutations []string, prefix, doc, suffix string) ([]byte, error) {
	return cMutateFileRangeMode(mutations, prefix, doc, suffix, false, "", true)
}

func cMutateFileRangeMode(mutations []string, prefix, doc, suffix string, enableFileValues bool, fileValueBaseDir string, rootFieldsOnly bool) ([]byte, error) {
	cExprs, freeExprs := cStringArray(mutations)
	defer freeExprs()

	cPrefix := C.CString(prefix)
	cDoc := C.CString(doc)
	cSuffix := C.CString(suffix)
	defer C.free(unsafe.Pointer(cPrefix))
	defer C.free(unsafe.Pointer(cDoc))
	defer C.free(unsafe.Pointer(cSuffix))
	var cBaseDir *C.char
	if fileValueBaseDir != "" {
		cBaseDir = C.CString(fileValueBaseDir)
		defer C.free(unsafe.Pointer(cBaseDir))
	}

	var out *C.char
	var outLen C.size_t
	var errbuf [256]C.char
	status := C.liblql_mutate_file_range_value(cExprs, C.size_t(len(mutations)),
		cPrefix, cDoc, cSuffix, cBool(enableFileValues), cBaseDir,
		cBool(rootFieldsOnly), &out, &outLen, &errbuf[0], C.size_t(len(errbuf)))
	if status != 0 {
		return nil, sdkParityError(C.GoString(&errbuf[0]))
	}
	defer C.free(unsafe.Pointer(out))
	return C.GoBytes(unsafe.Pointer(out), C.int(outLen)), nil
}

func cCompactJSON(doc string) ([]byte, error) {
	cDoc := C.CString(doc)
	defer C.free(unsafe.Pointer(cDoc))

	var out *C.char
	var outLen C.size_t
	var errbuf [256]C.char
	status := C.liblql_compact_json_value(cDoc, &out, &outLen, &errbuf[0],
		C.size_t(len(errbuf)))
	if status != 0 {
		return nil, sdkParityError(C.GoString(&errbuf[0]))
	}
	defer C.free(unsafe.Pointer(out))
	return C.GoBytes(unsafe.Pointer(out), C.int(outLen)), nil
}

func cCompactFileRange(prefix, doc, suffix string) ([]byte, error) {
	cPrefix := C.CString(prefix)
	cDoc := C.CString(doc)
	cSuffix := C.CString(suffix)
	defer C.free(unsafe.Pointer(cPrefix))
	defer C.free(unsafe.Pointer(cDoc))
	defer C.free(unsafe.Pointer(cSuffix))

	var out *C.char
	var outLen C.size_t
	var errbuf [256]C.char
	status := C.liblql_compact_file_range_value(cPrefix, cDoc, cSuffix, &out,
		&outLen, &errbuf[0], C.size_t(len(errbuf)))
	if status != 0 {
		return nil, sdkParityError(C.GoString(&errbuf[0]))
	}
	defer C.free(unsafe.Pointer(out))
	return C.GoBytes(unsafe.Pointer(out), C.int(outLen)), nil
}

type cStreamSummary struct {
	CandidatesSeen    int64
	CandidatesMatched int64
	BytesRead         int64
	StoppedEarly      bool
	StopReason        int
	DecisionCallbacks int
	MatchCallbacks    int
	SeekablePayloads  int
	SpooledPayloads   int
	PayloadJSON       []byte
}

func cStreamQuery(expr, doc string, mode int, maxMatches, maxCandidates, maxBytes int64, stopAfterFirst bool) (cStreamSummary, error) {
	cExpr := C.CString(expr)
	cDoc := C.CString(doc)
	defer C.free(unsafe.Pointer(cExpr))
	defer C.free(unsafe.Pointer(cDoc))

	var summary C.liblql_stream_summary
	var errbuf [256]C.char
	status := C.liblql_stream_query(
		cExpr,
		cDoc,
		C.int(mode),
		C.ulonglong(maxMatches),
		C.ulonglong(maxCandidates),
		C.ulonglong(maxBytes),
		cBool(stopAfterFirst),
		&summary,
		&errbuf[0],
		C.size_t(len(errbuf)),
	)
	if status != 0 {
		return cStreamSummary{}, sdkParityError(C.GoString(&errbuf[0]))
	}
	defer C.free(unsafe.Pointer(summary.payload_json))
	return cStreamSummary{
		CandidatesSeen:    int64(summary.candidates_seen),
		CandidatesMatched: int64(summary.candidates_matched),
		BytesRead:         int64(summary.bytes_read),
		StoppedEarly:      summary.stopped_early != 0,
		StopReason:        int(summary.stop_reason),
		DecisionCallbacks: int(summary.decision_callbacks),
		MatchCallbacks:    int(summary.match_callbacks),
		SeekablePayloads:  int(summary.seekable_payloads),
		SpooledPayloads:   int(summary.spooled_payloads),
		PayloadJSON:       C.GoBytes(unsafe.Pointer(summary.payload_json), C.int(summary.payload_len)),
	}, nil
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

#ifndef LQL_LONEJSON_INTERNAL_H
#define LQL_LONEJSON_INTERNAL_H

#include "lql_internal.h"

#include <lonejson.h>

LQL_INTERNAL_SYMBOL lonejson *lql_lonejson_new(lql *self,
                                               lonejson_error *error);
LQL_INTERNAL_SYMBOL lonejson *
lql_lonejson_new_mapped_stream(lql *self, lonejson_error *error);

LQL_INTERNAL_SYMBOL lql_status lql_projection_render_top_level(
    lql *self, const lql_projection *projection, const lonejson_spooled *input,
    lonejson_sink_fn sink, void *sink_user, int *out_emitted, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_projection_capture_create(
    lql *self, const lql_projection *projection, lonejson *runtime,
    lql_projection_capture **out, lql_error *error);
LQL_INTERNAL_SYMBOL void
lql_projection_capture_destroy(lql_projection_capture *capture);
LQL_INTERNAL_SYMBOL void
lql_projection_capture_reset(lql_projection_capture *capture);
LQL_INTERNAL_SYMBOL void
lql_projection_capture_set_root_object(lql_projection_capture *capture);
LQL_INTERNAL_SYMBOL void
lql_projection_capture_visitor(lonejson_path_value_visitor *out);
LQL_INTERNAL_SYMBOL void *
lql_projection_capture_visitor_user(lql_projection_capture *capture);
LQL_INTERNAL_SYMBOL lql_status lql_projection_capture_render(
    lql_projection_capture *capture, lonejson_sink_fn sink, void *sink_user,
    int *out_emitted, lql_error *error);

LQL_INTERNAL_SYMBOL lql_status lql_mutation_render(
    lql *self, const lql_mutation *mutation, const lonejson_spooled *input,
    lonejson_sink_fn sink, void *sink_user, lql_error *error);

#endif

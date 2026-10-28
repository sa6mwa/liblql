/* Protect the entire native-to-Lua adapter, including argument construction and
 * result decoding. Protecting only the user function leaves OOM longjmps able
 * to skip native plan/spool cleanup. The frame survives Lua stack unwinding. */
typedef enum lua_lql_callback_kind {
  LUA_LQL_READ,
  LUA_LQL_WRITE,
  LUA_LQL_RANGE,
  LUA_LQL_DECISION,
  LUA_LQL_VALUE,
  LUA_LQL_CANCELLED,
  LUA_LQL_TIME,
  LUA_LQL_FILE_OPEN,
  LUA_LQL_FILE_READ,
  LUA_LQL_FILE_CLOSE,
  LUA_LQL_MUTATION_TIME
} lua_lql_callback_kind;

typedef struct lua_lql_callback_frame {
  lua_lql_callback_kind kind;
  void *user;
  unsigned char *buffer;
  const void *data;
  size_t offset;
  size_t len;
  size_t *out_len;
  int ref;
  lql_stream_writer_fn writer;
  void *writer_user;
  lql_stream_reader_fn *out_reader;
  void **out_reader_user;
  lql_error *error;
  lql_status status;
  lql_stream_callback_result result;
  int cancelled;
  time_t now;
  lua_lql_stream_value *value_handle;
  lua_lql_stream_writer *writer_handle;
  int root;
} lua_lql_callback_frame;

static lql_status lua_lql_stream_reader_body(void *, unsigned char *, size_t,
                                             size_t *, lql_error *);
static lql_status lua_lql_bridge_write_ref_body(lua_lql_stream_bridge *, int,
                                                const void *, size_t,
                                                lql_error *);
static lql_status lua_lql_stream_range_writer_body(void *, size_t, size_t,
                                                   lql_stream_writer_fn, void *,
                                                   lql_error *,
                                                   lua_lql_callback_frame *);
static lql_stream_callback_result
lua_lql_stream_decision_body(void *, const lql_stream_decision *, lql_error *);
static lql_stream_callback_result
lua_lql_stream_on_value_body(void *, const lql_stream_value *, lql_error *,
                             lua_lql_callback_frame *);
static int lua_lql_stream_cancelled_body(void *);
static time_t lua_lql_stream_time_now_body(void *);
static lql_status lua_lql_mutation_file_open_body(void *, lql_string_view,
                                                  lql_stream_reader_fn *,
                                                  void **, lql_error *);
static lql_status lua_lql_mutation_file_reader_body(void *, unsigned char *,
                                                    size_t, size_t *,
                                                    lql_error *);
static time_t lua_lql_mutation_time_now_body(void *);
static void lua_lql_mutation_file_close_body(void *);
static void lua_lql_bridge_fail_status(lua_lql_stream_bridge *, lql_status,
                                       const char *);
static void lua_lql_mutation_bridge_fail_status(lua_lql_mutation_bridge *,
                                                lql_status, const char *);

static int lua_lql_callback_dispatch(lua_State *lua) {
  lua_lql_callback_frame *frame;
  lql_string_view path;
  frame = (lua_lql_callback_frame *)lua_touserdata(lua, 1);
  switch (frame->kind) {
  case LUA_LQL_READ:
    frame->status = lua_lql_stream_reader_body(
        frame->user, frame->buffer, frame->len, frame->out_len, frame->error);
    break;
  case LUA_LQL_WRITE:
    frame->status = lua_lql_bridge_write_ref_body(
        (lua_lql_stream_bridge *)frame->user, frame->ref, frame->data,
        frame->len, frame->error);
    break;
  case LUA_LQL_RANGE:
    frame->status = lua_lql_stream_range_writer_body(
        frame->user, frame->offset, frame->len, frame->writer,
        frame->writer_user, frame->error, frame);
    break;
  case LUA_LQL_DECISION:
    frame->result = lua_lql_stream_decision_body(
        frame->user, (const lql_stream_decision *)frame->data, frame->error);
    break;
  case LUA_LQL_VALUE:
    frame->result = lua_lql_stream_on_value_body(
        frame->user, (const lql_stream_value *)frame->data, frame->error,
        frame);
    break;
  case LUA_LQL_CANCELLED:
    frame->cancelled = lua_lql_stream_cancelled_body(frame->user);
    break;
  case LUA_LQL_TIME:
    frame->now = lua_lql_stream_time_now_body(frame->user);
    break;
  case LUA_LQL_FILE_OPEN:
    path.data = (const char *)frame->data;
    path.len = frame->len;
    frame->status =
        lua_lql_mutation_file_open_body(frame->user, path, frame->out_reader,
                                        frame->out_reader_user, frame->error);
    break;
  case LUA_LQL_FILE_READ:
    frame->status = lua_lql_mutation_file_reader_body(
        frame->user, frame->buffer, frame->len, frame->out_len, frame->error);
    break;
  case LUA_LQL_FILE_CLOSE:
    lua_lql_mutation_file_close_body(frame->user);
    break;
  case LUA_LQL_MUTATION_TIME:
    frame->now = lua_lql_mutation_time_now_body(frame->user);
    break;
  }
  return 0;
}

static int lua_lql_callback_protected(lua_State *lua,
                                      lua_lql_callback_frame *frame) {
  int top;
  int status;
  const char *message;
  lua_lql_mutation_bridge *mutation;
  top = lua_gettop(lua);
  frame->root = LUA_NOREF;
  status = LUA_ERRMEM;
  if (lua_checkstack(lua, 16)) {
    lua_pushcfunction(lua, lua_lql_callback_dispatch);
    lua_pushlightuserdata(lua, frame);
    status = lua_pcall(lua, 1, 0, 0);
  }
  /* Do this before any further allocating Lua API, including error reporting.
   */
  if (frame->value_handle != NULL) {
    frame->value_handle->value = NULL;
  }
  if (frame->writer_handle != NULL) {
    frame->writer_handle->active = 0;
  }
  luaL_unref(lua, LUA_REGISTRYINDEX, frame->root);
  if (status != LUA_OK) {
    message = "out of memory preparing Lua callback";
    if (lua_gettop(lua) > top && lua_type(lua, -1) == LUA_TSTRING) {
      message = lua_tostring(lua, -1);
    }
    frame->status =
        status == LUA_ERRMEM ? LQL_STATUS_NO_MEMORY : LQL_STATUS_CALLBACK_ERROR;
    if (frame->error != NULL) {
      frame->error->code = frame->status;
      strncpy(frame->error->message, message,
              sizeof(frame->error->message) - 1u);
      frame->error->message[sizeof(frame->error->message) - 1u] = '\0';
    }
    if (frame->kind >= LUA_LQL_FILE_OPEN) {
      mutation = (frame->kind == LUA_LQL_FILE_READ ||
                  frame->kind == LUA_LQL_FILE_CLOSE)
                     ? ((lua_lql_mutation_reader *)frame->user)->bridge
                     : (lua_lql_mutation_bridge *)frame->user;
      lua_lql_mutation_bridge_fail_status(mutation, frame->status, message);
    } else {
      lua_lql_bridge_fail_status((lua_lql_stream_bridge *)frame->user,
                                 frame->status, message);
    }
  }
  lua_settop(lua, top);
  return status == LUA_OK;
}

static lql_status lua_lql_stream_reader(void *user, unsigned char *buffer,
                                        size_t capacity, size_t *out_len,
                                        lql_error *error) {
  lua_lql_callback_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.kind = LUA_LQL_READ;
  frame.user = user;
  frame.buffer = buffer;
  frame.len = capacity;
  frame.out_len = out_len;
  frame.error = error;
  (void)lua_lql_callback_protected(((lua_lql_stream_bridge *)user)->lua,
                                   &frame);
  return frame.status;
}

static lql_status lua_lql_bridge_write_ref(lua_lql_stream_bridge *bridge,
                                           int ref, const void *data,
                                           size_t len, lql_error *error) {
  lua_lql_callback_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.kind = LUA_LQL_WRITE;
  frame.user = bridge;
  frame.ref = ref;
  frame.data = data;
  frame.len = len;
  frame.error = error;
  (void)lua_lql_callback_protected(bridge->lua, &frame);
  return frame.status;
}

static lql_status lua_lql_stream_range_writer(void *user, size_t offset,
                                              size_t len,
                                              lql_stream_writer_fn writer,
                                              void *writer_user,
                                              lql_error *error) {
  lua_lql_callback_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.kind = LUA_LQL_RANGE;
  frame.user = user;
  frame.offset = offset;
  frame.len = len;
  frame.writer = writer;
  frame.writer_user = writer_user;
  frame.error = error;
  (void)lua_lql_callback_protected(((lua_lql_stream_bridge *)user)->lua,
                                   &frame);
  return frame.status;
}

static lql_stream_callback_result
lua_lql_stream_decision(void *user, const lql_stream_decision *decision,
                        lql_error *error) {
  lua_lql_callback_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.kind = LUA_LQL_DECISION;
  frame.user = user;
  frame.data = decision;
  frame.error = error;
  if (!lua_lql_callback_protected(((lua_lql_stream_bridge *)user)->lua,
                                  &frame)) {
    return LQL_STREAM_CALLBACK_ERROR;
  }
  return frame.result;
}

static lql_stream_callback_result
lua_lql_stream_on_value(void *user, const lql_stream_value *value,
                        lql_error *error) {
  lua_lql_callback_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.kind = LUA_LQL_VALUE;
  frame.user = user;
  frame.data = value;
  frame.error = error;
  if (!lua_lql_callback_protected(((lua_lql_stream_bridge *)user)->lua,
                                  &frame)) {
    return LQL_STREAM_CALLBACK_ERROR;
  }
  return frame.result;
}

static int lua_lql_stream_cancelled(void *user) {
  lua_lql_callback_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.kind = LUA_LQL_CANCELLED;
  frame.user = user;
  if (!lua_lql_callback_protected(((lua_lql_stream_bridge *)user)->lua,
                                  &frame)) {
    return 1;
  }
  return frame.cancelled;
}

static time_t lua_lql_stream_time_now(void *user) {
  lua_lql_callback_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.kind = LUA_LQL_TIME;
  frame.user = user;
  if (!lua_lql_callback_protected(((lua_lql_stream_bridge *)user)->lua,
                                  &frame)) {
    return time(NULL);
  }
  return frame.now;
}

static lql_status lua_lql_mutation_file_reader(void *user,
                                               unsigned char *buffer,
                                               size_t capacity, size_t *out_len,
                                               lql_error *error) {
  lua_lql_callback_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.kind = LUA_LQL_FILE_READ;
  frame.user = user;
  frame.buffer = buffer;
  frame.len = capacity;
  frame.out_len = out_len;
  frame.error = error;
  (void)lua_lql_callback_protected(
      ((lua_lql_mutation_reader *)user)->bridge->lua, &frame);
  return frame.status;
}

static lql_status lua_lql_mutation_file_open(void *user, lql_string_view path,
                                             lql_stream_reader_fn *out_reader,
                                             void **out_reader_user,
                                             lql_error *error) {
  lua_lql_callback_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.kind = LUA_LQL_FILE_OPEN;
  frame.user = user;
  frame.data = path.data;
  frame.len = path.len;
  frame.out_reader = out_reader;
  frame.out_reader_user = out_reader_user;
  frame.error = error;
  (void)lua_lql_callback_protected(((lua_lql_mutation_bridge *)user)->lua,
                                   &frame);
  return frame.status;
}

static time_t lua_lql_mutation_time_now(void *user) {
  lua_lql_callback_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.kind = LUA_LQL_MUTATION_TIME;
  frame.user = user;
  if (!lua_lql_callback_protected(((lua_lql_mutation_bridge *)user)->lua,
                                  &frame)) {
    return time(NULL);
  }
  return frame.now;
}

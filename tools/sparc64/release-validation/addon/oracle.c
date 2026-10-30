#include <node_api.h>
#include <stdint.h>

#define OK(call) do { if ((call) != napi_ok) { napi_throw_error(env, NULL, #call); return NULL; } } while (0)

static napi_value checksum(napi_env env, napi_callback_info info) {
  size_t argc = 1, length, offset;
  napi_value args[1], buffer, result;
  napi_typedarray_type type;
  void* data;
  OK(napi_get_cb_info(env, info, &argc, args, NULL, NULL));
  if (argc != 1) { napi_throw_type_error(env, NULL, "Expected Uint32Array"); return NULL; }
  OK(napi_get_typedarray_info(env, args[0], &type, &length, &data, &buffer, &offset));
  if (type != napi_uint32_array) { napi_throw_type_error(env, NULL, "Expected Uint32Array"); return NULL; }
  uint64_t sum = 0;
  for (size_t i = 0; i < length; i++) sum += (uint64_t)((uint32_t*)data)[i] * (i + 1);
  OK(napi_create_bigint_uint64(env, sum, &result));
  return result;
}

static napi_value callback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2], receiver, result;
  OK(napi_get_cb_info(env, info, &argc, args, NULL, NULL));
  if (argc != 2) { napi_throw_type_error(env, NULL, "Expected callback and value"); return NULL; }
  OK(napi_get_undefined(env, &receiver));
  OK(napi_call_function(env, receiver, args[0], 1, args + 1, &result));
  return result;
}

NAPI_MODULE_INIT() {
  napi_value fn;
  OK(napi_create_function(env, "checksum", NAPI_AUTO_LENGTH, checksum, NULL, &fn));
  OK(napi_set_named_property(env, exports, "checksum", fn));
  OK(napi_create_function(env, "callback", NAPI_AUTO_LENGTH, callback, NULL, &fn));
  OK(napi_set_named_property(env, exports, "callback", fn));
  return exports;
}

// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include <node_api.h>

#include "test/js-native-api/common.h"

static void FlushWindows(void) {
#if defined(__sparc__) && defined(__arch64__)
  __asm__ __volatile__("flushw" ::: "memory");
#endif
}

static napi_value RoundTrip(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  NODE_API_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));
  NODE_API_ASSERT(env, argc == 2, "Expected callback and payload");
  napi_valuetype type;
  NODE_API_CALL(env, napi_typeof(env, args[0], &type));
  NODE_API_ASSERT(env, type == napi_function, "Expected a callback");

  napi_escapable_handle_scope scope;
  NODE_API_CALL(env, napi_open_escapable_handle_scope(env, &scope));
  for (int n = 0; n < 1536; ++n) {
    napi_value temporary;
    NODE_API_CALL(env, napi_create_object(env, &temporary));
  }
  napi_value box;
  NODE_API_CALL(env, napi_create_object(env, &box));
  NODE_API_CALL(env, napi_set_named_property(env, box, "payload", args[1]));
  napi_ref reference;
  NODE_API_CALL(env, napi_create_reference(env, box, 1, &reference));
  napi_value receiver;
  NODE_API_CALL(env, napi_get_undefined(env, &receiver));
  napi_value callback_result;
  FlushWindows();
  napi_status status =
      napi_call_function(env, receiver, args[0], 1, &box, &callback_result);
  FlushWindows();
  if (status == napi_pending_exception) {
    NODE_API_CALL(env, napi_delete_reference(env, reference));
    NODE_API_CALL(env, napi_close_escapable_handle_scope(env, scope));
    return NULL;
  }
  NODE_API_CALL(env, status);
  napi_value rooted;
  NODE_API_CALL(env, napi_get_reference_value(env, reference, &rooted));
  bool identical;
  NODE_API_CALL(env, napi_strict_equals(env, rooted, box, &identical));
  NODE_API_ASSERT(env, identical, "Native reference changed object identity");
  napi_value payload;
  NODE_API_CALL(env, napi_get_named_property(env, rooted, "payload", &payload));
  NODE_API_CALL(env, napi_strict_equals(env, payload, args[1], &identical));
  NODE_API_ASSERT(env, identical, "Payload handle was not preserved");
  napi_value result;
  NODE_API_CALL(env, napi_create_object(env, &result));
  NODE_API_CALL(env, napi_set_named_property(env, result, "box", rooted));
  NODE_API_CALL(
      env, napi_set_named_property(env, result, "result", callback_result));
  napi_value escaped;
  NODE_API_CALL(env, napi_escape_handle(env, scope, result, &escaped));
  NODE_API_CALL(env, napi_delete_reference(env, reference));
  NODE_API_CALL(env, napi_close_escapable_handle_scope(env, scope));
  return escaped;
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_value function;
  NODE_API_CALL(env, napi_create_function(env, "roundTrip", NAPI_AUTO_LENGTH,
                                          RoundTrip, NULL, &function));
  NODE_API_CALL(env,
                napi_set_named_property(env, exports, "roundTrip", function));
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)

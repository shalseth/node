// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include <node.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

struct State {
  std::mutex mutex;
  std::condition_variable changed;
  bool started = false;
  bool collected = false;
  bool nested = false;
};

void Started(v8::Isolate*, void* pointer) {
  auto* state = static_cast<State*>(pointer);
  std::lock_guard<std::mutex> lock(state->mutex);
  state->started = true;
  state->changed.notify_one();
}

void Collect(v8::Isolate* isolate, void* pointer) {
  auto* state = static_cast<State*>(pointer);
  v8::HandleScope handles(isolate);
  isolate->RequestGarbageCollectionForTesting(
      v8::Isolate::kFullGarbageCollection);
  auto context = isolate->GetCurrentContext();
  auto pattern = v8::String::NewFromUtf8Literal(isolate, "(a|b)*c");
  auto subject = v8::String::NewFromUtf8Literal(isolate, "ababc");
  auto regexp =
      v8::RegExp::New(context, pattern, v8::RegExp::kNone).ToLocalChecked();
  auto match = regexp->Exec(context, subject).ToLocalChecked();
  std::lock_guard<std::mutex> lock(state->mutex);
  state->nested = !match->IsNull();
  state->collected = true;
  state->changed.notify_one();
}

void Run(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* isolate = args.GetIsolate();
  auto context = isolate->GetCurrentContext();
  const bool two_byte = args.Length() && args[0]->BooleanValue(isolate);
  auto pattern = v8::String::NewFromUtf8Literal(isolate, "((a*)*)*b");
  auto regexp =
      v8::RegExp::New(context, pattern, v8::RegExp::kNone).ToLocalChecked();
  auto warm = two_byte ? v8::String::NewFromUtf8Literal(isolate, "πaaab")
                       : v8::String::NewFromUtf8Literal(isolate, "aaab");
  for (int i = 0; i < 3; ++i) {
    if (regexp->Exec(context, warm).IsEmpty()) std::abort();
  }
  uint16_t wide[36];
  for (auto& c : wide) c = 'a';
  wide[0] = two_byte ? 0x03c0 : 'a';
  auto subject = two_byte
                     ? v8::String::NewFromTwoByte(
                           isolate, wide, v8::NewStringType::kNormal, 36)
                           .ToLocalChecked()
                     : v8::String::NewFromUtf8Literal(
                           isolate, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  State state;
  std::thread interrupt([&] {
    {
      std::unique_lock<std::mutex> lock(state.mutex);
      state.changed.wait(lock, [&] { return state.started; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    isolate->RequestInterrupt(Collect, &state);
    {
      std::unique_lock<std::mutex> lock(state.mutex);
      state.changed.wait(lock, [&] { return state.collected; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    isolate->TerminateExecution();
  });
  bool terminated;
  {
    v8::TryCatch caught(isolate);
    isolate->RequestInterrupt(Started, &state);
    auto result = regexp->Exec(context, subject);
    terminated = result.IsEmpty() && caught.HasTerminated();
  }
  interrupt.join();
  isolate->CancelTerminateExecution();
  if (!terminated || !state.collected || !state.nested) std::abort();
  args.GetReturnValue().Set(regexp);
}

NODE_MODULE_INIT() { NODE_SET_METHOD(exports, "run", Run); }

}  // namespace

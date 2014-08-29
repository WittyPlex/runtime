// Copyright 2014 Runtime.JS project authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "thread.h"
#include <kernel/kernel.h>
#include <kernel/thread-manager.h>
#include <kernel/mem-manager.h>
#include <kernel/engine.h>
#include <kernel/engines.h>
#include <kernel/platform.h>

namespace rt {

Thread::Thread(ThreadManager* thread_mgr, ResourceHandle<EngineThread> ethread)
    :	thread_mgr_(thread_mgr),
        type_(ethread.get()->type()),
        iv8_(nullptr),
        tpl_cache_(nullptr),
        stack_(GLOBAL_mem_manager()->virtual_allocator().AllocStack()),
        ethread_(ethread),
        exports_(this),
        ref_count_(0),
        terminate_(false),
        parent_promise_id_(0) {
    priority_.Set(1);
}

Thread::~Thread() {
    RT_ASSERT(!"TODO: Make sure it's safe to delete thread object first!");
}

ExternalFunction* FunctionExports::Add(v8::Local<v8::Value> v,
                                       ResourceHandle<EngineThread> recv) {
    uint32_t index = data_.size();
    RT_ASSERT(thread_);
    v8::Isolate* iv8 { thread_->IsolateV8() };
    RT_ASSERT(iv8);
    size_t export_id = ++export_id_;
    data_.push_back(std::move(FunctionExportData(iv8, v, export_id)));
    return new ExternalFunction(index, export_id, thread_, recv);
}

v8::Local<v8::Value> FunctionExports::Get(uint32_t index, size_t export_id) {
    RT_ASSERT(thread_);
    v8::Isolate* iv8 { thread_->IsolateV8() };

    RT_ASSERT(index < data_.size());
    v8::EscapableHandleScope scope(iv8);
    if (data_[index].export_id() != export_id) {
        return scope.Escape(v8::Local<v8::Value>());
    }

    return scope.Escape(data_[index].GetValue(iv8));
}


void Thread::SetTimeout(uint32_t timeout_id, uint64_t timeout_ms) {
    uint64_t time_now { GLOBAL_platform()->BootTimeMicroseconds() };
    uint64_t when = time_now + timeout_ms * 1000;
    timeouts_.Set(timeout_id, when);
}

void Thread::SetUp() {
    // Skip initialization for idle thread
    if (ThreadType::IDLE == type_) {
        return;
    }

    RT_ASSERT(nullptr == iv8_);
    RT_ASSERT(nullptr == tpl_cache_);
    iv8_ = v8::Isolate::New();
    iv8_->SetData(0, this);
    printf("[V8] new isolate\n");
    v8::Isolate::Scope ivscope(iv8_);
    v8::HandleScope local_handle_scope(iv8_);
    tpl_cache_ = new TemplateCache(iv8_);
}

void Thread::TearDown() {
    RT_ASSERT(ThreadType::DEFAULT == type_);
    RT_ASSERT(iv8_);
    RT_ASSERT(iv8_->GetData(0) == this);
    RT_ASSERT(tpl_cache_);

    {	v8::Isolate::Scope ivscope(iv8_);
        v8::HandleScope local_handle_scope(iv8_);

        auto promise_id = parent_promise_id();
        auto thread = parent_thread();

        LockingPtr<EngineThread> lptr { thread.get() };
        Thread* recv { lptr->thread() };
        RT_ASSERT(recv);

        TransportData data;
        if (!exit_value_.IsEmpty()) {
            TransportData::SerializeError err { data.MoveValue(this, recv,
                v8::Local<v8::Value>::New(iv8_, exit_value_)) };
        }

        {	std::unique_ptr<ThreadMessage> msg(new ThreadMessage(
                ThreadMessage::Type::FUNCTION_RETURN_RESOLVE,
                handle(),
                std::move(data), nullptr, promise_id));
            lptr->PushMessage(std::move(msg));
        }
    }

    RT_ASSERT(thread_mgr_);
    RT_ASSERT(this == thread_mgr_->current_thread());

    timeout_data_.Clear();
    irq_data_.Clear();
    promises_.Clear();

    printf("[V8] delete context\n");
    context_.Reset();
    args_.Reset();
    exit_value_.Reset();
    call_wrapper_.Reset();

    RT_ASSERT(tpl_cache_);
    delete tpl_cache_;

    RT_ASSERT(iv8_);
    printf("[V8] delete isolate\n");
    iv8_->Dispose(); // This deletes v8 isolate object

    type_ = ThreadType::TERMINATED;
}

v8::Local<v8::Object> Thread::GetIsolateGlobal() {
    v8::EscapableHandleScope scope(iv8_);
    auto s_isolate = v8::String::NewFromUtf8(iv8_, "isolate");

    auto context = v8::Local<v8::Context>::New(iv8_, context_);
    RT_ASSERT(!context.IsEmpty());
    auto isolate_value = context->Global()->Get(s_isolate);
    RT_ASSERT(!isolate_value.IsEmpty());
    RT_ASSERT(isolate_value->IsObject());
    return scope.Escape(isolate_value->ToObject());
}

bool Thread::Run() {
    // Not possible to run terminated thread
    RT_ASSERT(ThreadType::TERMINATED != type_);

    // Idle thread does nothing and never terminates
    if (ThreadType::IDLE == type_) {
        return true;
    }

    RT_ASSERT(iv8_);
    RT_ASSERT(tpl_cache_);


    uint64_t time_now { GLOBAL_platform()->BootTimeMicroseconds() };

    EngineThread::ThreadMessagesVector messages = ethread_.get()->TakeMessages();
    if (0 == messages.size() && !timeouts_.Elapsed(time_now)) {
        return true;
    }

    v8::Isolate::Scope ivscope(iv8_);
    v8::HandleScope local_handle_scope(iv8_);

    if (context_.IsEmpty()) {
        printf("[V8] new context\n");
        v8::Local<v8::Context> context = tpl_cache_->NewContext();
        context_ = std::move(v8::UniquePersistent<v8::Context>(iv8_, context));
    }

    RT_ASSERT(!context_.IsEmpty());
    v8::Local<v8::Context> context = v8::Local<v8::Context>::New(iv8_, context_);
    v8::Context::Scope cs(context);

    v8::TryCatch trycatch;

    // Run timeout callbacks
    {   const uint32_t kMaxTimeoutsPerTick = 8;
        uint32_t timeouts_count = 0;

        while (timeouts_.Elapsed(time_now)) {
            uint32_t timeout_id { timeouts_.Take() };

            TimeoutData data(TakeTimeoutData(timeout_id));
            auto fnv = v8::Local<v8::Value>::New(iv8_, data.TakeCallback());
            RT_ASSERT(!fnv.IsEmpty());
            RT_ASSERT(fnv->IsFunction());
            auto fn = fnv.As<v8::Function>();
            fn->Call(context->Global(), 0, nullptr);

            // Prevent too many timeout callbacks in a row
            if (++timeouts_count > kMaxTimeoutsPerTick) {
                break;
            }
        }
    }

    for (ThreadMessage* message : messages) {
        if (nullptr == message) {
            continue; // Skip removed message
        }

        ThreadMessage::Type type = message->type();

        switch (type) {
        case ThreadMessage::Type::SET_ARGUMENTS_NOPARENT: {
            RT_ASSERT(args_.IsEmpty());
            v8::Local<v8::Value> unpacked { message->data().Unpack(this) };
            RT_ASSERT(!unpacked.IsEmpty());

            args_ = std::move(v8::UniquePersistent<v8::Value>(iv8_, unpacked));

            auto s_data = v8::String::NewFromUtf8(iv8_, "data");
            GetIsolateGlobal()->Set(s_data, unpacked);
        }
            break;
        case ThreadMessage::Type::SET_ARGUMENTS: {
            RT_ASSERT(args_.IsEmpty());
            v8::Local<v8::Value> unpacked { message->data().Unpack(this) };
            RT_ASSERT(!unpacked.IsEmpty());

            args_ = std::move(v8::UniquePersistent<v8::Value>(iv8_, unpacked));

            // Install args [isolate.data, isolate.env, isolate.system]
            auto s_data = v8::String::NewFromUtf8(iv8_, "data");
            auto s_env = v8::String::NewFromUtf8(iv8_, "env");
            auto s_system = v8::String::NewFromUtf8(iv8_, "system");
            auto isolate_obj = GetIsolateGlobal();
            RT_ASSERT(!unpacked.IsEmpty());
            RT_ASSERT(unpacked->IsArray());
            auto args_array = unpacked.As<v8::Array>();
            RT_ASSERT(3 == args_array->Length());
            isolate_obj->Set(s_data, args_array->Get(0));
            isolate_obj->Set(s_env, args_array->Get(1));
            isolate_obj->Set(s_system, args_array->Get(2));

            parent_thread_ = message->sender();
            parent_promise_id_ = message->recv_index();
        }
            break;
        case ThreadMessage::Type::EVALUATE: {
            v8::Local<v8::Value> unpacked { message->data().Unpack(this) };
            RT_ASSERT(!unpacked.IsEmpty());
            RT_ASSERT(unpacked->IsArray());
            v8::Local<v8::Array> arr { v8::Local<v8::Array>::Cast(unpacked) };
            v8::Local<v8::Value> code { arr->Get(0) };
            v8::Local<v8::Value> filename { arr->Get(1) };
            RT_ASSERT(code->IsString());
            RT_ASSERT(filename->IsString());

            v8::ScriptOrigin origin(filename);
            v8::ScriptCompiler::Source source(code->ToString(), origin);
            v8::Local<v8::Script> script = v8::ScriptCompiler::Compile(iv8_, &source,
                v8::ScriptCompiler::CompileOptions::kNoCompileOptions);
            if (!script.IsEmpty()) {
                script->Run();
            }
        }
            break;
        case ThreadMessage::Type::FUNCTION_CALL: {
            v8::Local<v8::Value> unpacked { message->data().Unpack(this) };
            RT_ASSERT(!unpacked.IsEmpty());

            ExternalFunction* efn { message->exported_func() };
            RT_ASSERT(efn);

            v8::Local<v8::Value> fnval { exports_.Get(efn->index(), efn->export_id()) };
            if (fnval.IsEmpty()) {
                fnval = v8::Null(iv8_);
            } else {
                RT_ASSERT(fnval->IsFunction());
            }

            {   RT_ASSERT(!call_wrapper_.IsEmpty());
                v8::Local<v8::Function> fnwrap { v8::Local<v8::Function>::New(iv8_, call_wrapper_) };
                v8::Local<v8::Value> argv[] {
                   fnval,
                   message->sender().NewExternal(iv8_),
                   unpacked,
                   v8::Uint32::NewFromUnsigned(iv8_, message->recv_index()),
                };
                fnwrap->Call(context->Global(), 4, argv);
            }
        }
            break;
        case ThreadMessage::Type::FUNCTION_RETURN_RESOLVE: {
            v8::Local<v8::Value> unpacked { message->data().Unpack(this) };
            RT_ASSERT(!unpacked.IsEmpty());

            v8::Local<v8::Promise::Resolver> resolver {
                v8::Local<v8::Promise::Resolver>::New(iv8_, TakePromise(message->recv_index())) };

            resolver->Resolve(unpacked);
            iv8_->RunMicrotasks();
        }
            break;
        case ThreadMessage::Type::FUNCTION_RETURN_REJECT: {
            v8::Local<v8::Value> unpacked { message->data().Unpack(this) };
            RT_ASSERT(!unpacked.IsEmpty());

            v8::Local<v8::Promise::Resolver> resolver {
                v8::Local<v8::Promise::Resolver>::New(iv8_, TakePromise(message->recv_index())) };

            resolver->Reject(unpacked);
            iv8_->RunMicrotasks();
        }
            break;
        case ThreadMessage::Type::IRQ_RAISE: {
            v8::Local<v8::Value> fnv { v8::Local<v8::Value>::New(iv8_,
                GetIRQData(message->recv_index())) };
            RT_ASSERT(fnv->IsFunction());
            v8::Local<v8::Function> fn { v8::Local<v8::Function>::Cast(fnv) };
            fn->Call(context->Global(), 0, nullptr);
        }
            break;
        case ThreadMessage::Type::EMPTY:
            break;
        default:
            RT_ASSERT(!"Unknown thread message");
            break;
        }

        if (!message->reusable()) {
            delete message;
        }
    }

    v8::Local<v8::Value> ex = trycatch.Exception();
    if (!ex.IsEmpty() && !trycatch.HasTerminated()) {
        v8::String::Utf8Value exception_str(ex);
        v8::Local<v8::Message> message = trycatch.Message();
        if (message.IsEmpty()) {
            printf("Uncaught exception: %s\n", *exception_str);
        } else {
            v8::String::Utf8Value script_name(message->GetScriptResourceName());
            int linenum = message->GetLineNumber();
            printf("Uncaught exception: %s:%i: %s\n", *script_name, linenum, *exception_str);
        }

        v8::String::Utf8Value stack(trycatch.StackTrace());
        if (stack.length() > 0) {
            printf("%s\n", *stack);
        }
    }

    trycatch.Reset();

    if (0 == ref_count_ || terminate_) {
        if (terminate_) {
            printf("[ terminate thread (reason: isolate.exit() called) ]\n");
        } else {
            printf("[ terminate thread (reason: refcount 0) ]\n");
        }

        terminate_ = true;
        return false;
    }

    return true;
}

} // namespace rt

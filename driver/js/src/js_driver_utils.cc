/*
 * Tencent is pleased to support the open source community by making
 * Hippy available.
 *
 * Copyright (C) 2022 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "driver/js_driver_utils.h"

#include <sys/stat.h>

#include <any>
#include <functional>
#include <future>
#include <utility>

#include "driver/napi/callback_info.h"
#include "driver/napi/js_ctx.h"
#include "driver/napi/js_ctx_value.h"
#include "driver/napi/js_try_catch.h"
#include "driver/napi/callback_info.h"
#include "footstone/check.h"
#include "footstone/deserializer.h"
#include "footstone/hippy_value.h"
#include "footstone/logging.h"
#include "footstone/string_view_utils.h"
#include "footstone/task.h"
#include "footstone/task_runner.h"
#include "footstone/worker_impl.h"
#include "vfs/file.h"

#ifdef JS_V8
#include "driver/napi/v8/v8_ctx.h"
#include "driver/napi/v8/v8_ctx_value.h"
#include "driver/napi/v8/v8_try_catch.h"
#include "driver/vm/v8/v8_vm.h"
#include "driver/napi/v8/serializer.h"
#endif

namespace hippy {
inline namespace driver {

using byte_string = std::string;
using string_view = footstone::stringview::string_view;
using StringViewUtils = footstone::stringview::StringViewUtils;
using TaskRunner = footstone::runner::TaskRunner;
using WorkerManager = footstone::runner::WorkerManager;
using string_view = footstone::string_view;
using u8string = string_view::u8string;
using Ctx = hippy::Ctx;
using CtxValue = hippy::napi::CtxValue;
using Deserializer = footstone::value::Deserializer;
using HippyValue = footstone::value::HippyValue;
using HippyFile = hippy::vfs::HippyFile;
using VMInitParam = hippy::VM::VMInitParam;
using ScopeWrapper = hippy::ScopeWrapper;
using CallbackInfo = hippy::CallbackInfo;

#ifdef JS_V8
using V8VM = hippy::V8VM;
using V8Ctx = hippy::V8Ctx;
#endif

constexpr char kBridgeName[] = "hippyBridge";
constexpr char kWorkerRunnerName[] = "hippy_worker";
constexpr char kGlobalKey[] = "global";
constexpr char kHippyKey[] = "Hippy";
constexpr char kNativeGlobalKey[] = "__HIPPYNATIVEGLOBAL__";
constexpr char kCallHostKey[] = "hippyCallNatives";

#if defined(JS_V8) && defined(ENABLE_INSPECTOR) && !defined(V8_WITHOUT_INSPECTOR)
using V8InspectorClientImpl = hippy::inspector::V8InspectorClientImpl;
#endif

static std::unordered_map<int64_t, std::pair<std::shared_ptr<Engine>, uint32_t>> reuse_engine_map;
static std::mutex engine_mutex;

std::shared_ptr<Engine> JsDriverUtils::CreateEngine(bool is_debug, int64_t group_id) {
  std::shared_ptr<Engine> engine = nullptr;
  auto group = group_id;
  if (is_debug) {
    group = VM::kDebuggerGroupId;
  }
  {
    std::lock_guard<std::mutex> lock(engine_mutex);
    auto it = reuse_engine_map.find(group);
    if (it != reuse_engine_map.end()) {
      engine = std::get<std::shared_ptr<Engine>>(it->second);
      std::get<uint32_t>(it->second) += 1;
      FOOTSTONE_DLOG(INFO) << "engine cnt = " << std::get<uint32_t>(it->second)
                           << ", use_count = " << engine.use_count();
    }
  }
  if (!engine) {
    engine = std::make_shared<Engine>();
    if (group != VM::kDefaultGroupId) {
      std::lock_guard<std::mutex> lock(engine_mutex);
      reuse_engine_map[group] = std::make_pair(engine, 1);
    }
  }
  return engine;
}

void RegisterGlobalObjectAndGlobalConfig(const std::shared_ptr<Scope>& scope, const string_view& global_config) {
  auto ctx = scope->GetContext();
  auto global_object = ctx->GetGlobalObject();
  auto user_global_object_key = ctx->CreateString(kGlobalKey);
  ctx->SetProperty(global_object, user_global_object_key, global_object);
  auto hippy_key = ctx->CreateString(kHippyKey);
  ctx->SetProperty(global_object, hippy_key, ctx->CreateObject());
  auto native_global_key = ctx->CreateString(kNativeGlobalKey);
  auto engine = scope->GetEngine().lock();
  FOOTSTONE_CHECK(engine);
  auto global_config_object = engine->GetVM()->ParseJson(ctx, global_config);
  auto flag = ctx->SetProperty(global_object, native_global_key, global_config_object);
  FOOTSTONE_CHECK(flag) << "set " << kNativeGlobalKey << " failed";
}

void RegisterCallHostObject(const std::shared_ptr<Scope>& scope, const JsCallback& call_host_callback) {
  auto func_wrapper = std::make_unique<hippy::napi::FunctionWrapper>(call_host_callback, nullptr);
  auto ctx = scope->GetContext();
  auto native_func_cb = ctx->CreateFunction(func_wrapper);
  scope->SaveFunctionWrapper(std::move(func_wrapper));
  auto global_object = ctx->GetGlobalObject();
  auto call_host_key = ctx->CreateString(kCallHostKey);
  ctx->SetProperty(global_object, call_host_key, native_func_cb, hippy::napi::PropertyAttribute::ReadOnly);
}

void AsyncInitEngine(const std::shared_ptr<Engine>& engine,
                     const std::shared_ptr<TaskRunner>& task_runner,
                     const std::shared_ptr<VMInitParam>& param) {
  auto engine_initialized_callback = [](const std::shared_ptr<Engine>& engine) {
#ifdef JS_V8
    auto v8_vm = std::static_pointer_cast<V8VM>(engine->GetVM());
    auto wrapper = std::make_unique<FunctionWrapper>([](CallbackInfo& info, void* data) {
      auto scope_wrapper = reinterpret_cast<ScopeWrapper*>(std::any_cast<void*>(info.GetSlot()));
      auto scope = scope_wrapper->scope.lock();
      FOOTSTONE_CHECK(scope);
      auto exception = info[0];
      V8VM::HandleUncaughtException(scope->GetContext(), exception);
      auto engine = scope->GetEngine().lock();
      FOOTSTONE_CHECK(engine);
      auto callback = engine->GetVM()->GetUncaughtExceptionCallback();
      auto context = scope->GetContext();
      string_view description;
      auto flag = context->GetValueString(info[1], &description);
      FOOTSTONE_CHECK(flag);
      string_view stack;
      flag = context->GetValueString(info[2], &stack);
      FOOTSTONE_CHECK(flag);
      callback(scope->GetBridge(), description, stack);
    }, nullptr);
    v8_vm->AddUncaughtExceptionMessageListener(wrapper);
    v8_vm->SaveUncaughtExceptionCallback(std::move(wrapper));
#if defined(ENABLE_INSPECTOR) && !defined(V8_WITHOUT_INSPECTOR)
    if (v8_vm->IsDebug()) {
      if (!v8_vm->GetInspectorClient()) {
        v8_vm->SetInspectorClient(std::make_shared<V8InspectorClientImpl>());
      }
      v8_vm->GetInspectorClient()->SetJsRunner(engine->GetJsTaskRunner());
    }
#endif
#endif
  };
  engine->AsyncInitialize(task_runner, param, engine_initialized_callback);
}

void CreateScopeAndAsyncInitialize(const std::shared_ptr<Engine>& engine,
                                   const string_view& global_config,
                                   const JsCallback& call_host_callback,
                                   const std::function<void(std::shared_ptr<Scope>)>& scope_initialized_callback) {
  auto scope = engine->CreateScope("");
  engine->GetJsTaskRunner()->PostTask([global_config, scope, call_host_callback, scope_initialized_callback]() {
    scope->CreateContext();
    RegisterGlobalObjectAndGlobalConfig(scope, global_config);
    scope->SyncInitialize();
    RegisterCallHostObject(scope, call_host_callback);
#if defined(JS_V8) && defined(ENABLE_INSPECTOR) && !defined(V8_WITHOUT_INSPECTOR)
    auto engine = scope->GetEngine().lock();
    FOOTSTONE_CHECK(engine);
    auto vm = std::static_pointer_cast<V8VM>(engine->GetVM());
    if (vm->IsDebug()) {
      auto inspector_client = vm->GetInspectorClient();
      if (inspector_client) {
        inspector_client->CreateInspector(scope);
        auto inspector_context = inspector_client->CreateInspectorContext(
            scope, vm->GetDevtoolsDataSource());
        scope->SetInspectorContext(inspector_context);
      }
    }
#endif
    scope_initialized_callback(scope);
  });
}

void InitDevTools(const std::shared_ptr<Scope>& scope, const std::shared_ptr<VM>& vm) {
#if defined(JS_V8) && defined(ENABLE_INSPECTOR)
  if (vm->IsDebug()) {
    auto v8_vm = std::static_pointer_cast<V8VM>(vm);
    scope->SetDevtoolsDataSource(v8_vm->GetDevtoolsDataSource());
#ifndef V8_WITHOUT_INSPECTOR
    std::weak_ptr<V8VM> weak_v8_vm = v8_vm;
    std::weak_ptr<Scope> weak_scope = scope;
    scope->GetDevtoolsDataSource()->SetVmRequestHandler([weak_v8_vm, weak_scope](const std::string& data) {
      auto v8_vm = weak_v8_vm.lock();
      if (!v8_vm) {
        FOOTSTONE_DLOG(FATAL) << "RunApp send_v8_func_ vm invalid or not debugger";
        return;
      }
      auto scope = weak_scope.lock();
      if (!scope) {
        return;
      }
      auto inspector_client = v8_vm->GetInspectorClient();
      if (inspector_client) {
        auto u16str = StringViewUtils::ConvertEncoding(
            string_view(data), string_view::Encoding::Utf16);
        inspector_client->SendMessageToV8(scope->GetInspectorContext(), std::move(u16str));
      }
    });
  }
#endif
#endif
}

void JsDriverUtils::InitInstance(
    const std::shared_ptr<Engine>& engine,
    const std::shared_ptr<TaskRunner>& task_runner,
    const std::shared_ptr<VMInitParam>& param,
    const string_view& global_config,
    std::function<void(std::shared_ptr<Scope>)>&& scope_initialized_callback,
    const JsCallback& call_host_callback) {
  AsyncInitEngine(engine, task_runner, param);
  CreateScopeAndAsyncInitialize(engine, global_config, call_host_callback,
                                [callback = std::move(scope_initialized_callback)](const std::shared_ptr<Scope>& scope) {
    auto engine = scope->GetEngine().lock();
    if (!engine) {
      return;
    }
    InitDevTools(scope, engine->GetVM());
    callback(scope);
  });
}

bool JsDriverUtils::RunScript(const std::shared_ptr<Scope>& scope,
                              const string_view& file_name,
                              bool is_use_code_cache,
                              const string_view& code_cache_dir,
                              const string_view& uri,
                              bool is_local_file) {
  FOOTSTONE_LOG(INFO) << "RunScript begin, file_name = " << file_name
                      << ", is_use_code_cache = " << is_use_code_cache
                      << ", code_cache_dir = " << code_cache_dir
                      << ", uri = " << uri
                      << ", is_local_file = " << is_local_file;
  string_view code_cache_content;
  uint64_t modify_time = 0;
  std::future<std::string> read_file_future;
  string_view code_cache_path;
  auto loader = scope->GetUriLoader().lock();
  FOOTSTONE_CHECK(loader);
  if (is_use_code_cache) {
    if (is_local_file) {
      modify_time = HippyFile::GetFileModifyTime(uri);
    }
    code_cache_path = code_cache_dir + file_name + string_view("_") + string_view(std::to_string(modify_time));
    std::promise<std::string> read_file_promise;
    read_file_future = read_file_promise.get_future();
    auto engine = scope->GetEngine().lock();
    FOOTSTONE_CHECK(engine);
    loader->RequestUntrustedContent(file_name, {}, MakeCopyable([p = std::move(read_file_promise)](
        UriLoader::RetCode ret_code,
        const std::unordered_map<std::string, std::string>& meta,
        UriLoader::bytes content) mutable {
      p.set_value(std::move(content));
    }));
  }
  UriLoader::RetCode code;
  std::unordered_map<std::string, std::string> meta;
  UriLoader::bytes content;
  loader->RequestUntrustedContent(uri, {}, code, meta, content);
  auto script_content = string_view::new_from_utf8(content.c_str(), content.length());
  auto read_script_flag = false;
  if (code == UriLoader::RetCode::Success && !StringViewUtils::IsEmpty(script_content)) {
    read_script_flag = true;
  }
  if (is_use_code_cache) {
    code_cache_content = read_file_future.get();
  }

  FOOTSTONE_DLOG(INFO) << "uri = " << uri
                       << "read_script_flag = " << read_script_flag
                       << ", script content = " << script_content;

  if (!read_script_flag || StringViewUtils::IsEmpty(script_content)) {
    FOOTSTONE_LOG(WARNING) << "read_script_flag = " << read_script_flag
                           << ", script content empty, uri = " << uri;
    return false;
  }
#ifdef JS_V8
  auto ret = std::static_pointer_cast<V8Ctx>(scope->GetContext())->RunScript(
      script_content, file_name, is_use_code_cache,&code_cache_content, true);
  if (is_use_code_cache) {
    if (!StringViewUtils::IsEmpty(code_cache_content)) {
      auto func = [code_cache_path, code_cache_dir, code_cache_content] {
        int check_dir_ret = HippyFile::CheckDir(code_cache_dir, F_OK);
        FOOTSTONE_DLOG(INFO) << "check_parent_dir_ret = " << check_dir_ret;
        if (check_dir_ret) {
          HippyFile::CreateDir(code_cache_dir, S_IRWXU);
        }

        size_t pos = StringViewUtils::FindLastOf(code_cache_path, EXTEND_LITERAL('/'));
        string_view code_cache_parent_dir = StringViewUtils::SubStr(code_cache_path, 0, pos);
        int check_parent_dir_ret = HippyFile::CheckDir(code_cache_parent_dir, F_OK);
        FOOTSTONE_DLOG(INFO) << "check_parent_dir_ret = " << check_parent_dir_ret;
        if (check_parent_dir_ret) {
          HippyFile::CreateDir(code_cache_parent_dir, S_IRWXU);
        }

        auto u8_code_cache_content = StringViewUtils::ToStdString(StringViewUtils::ConvertEncoding(
            code_cache_content, string_view::Encoding::Utf8).utf8_value());
        bool save_file_ret = HippyFile::SaveFile(code_cache_path, u8_code_cache_content);
        FOOTSTONE_LOG(INFO) << "code cache save_file_ret = " << save_file_ret;
        FOOTSTONE_USE(save_file_ret);
      };
      auto engine = scope->GetEngine().lock();
      FOOTSTONE_CHECK(engine);
      auto& worker_manager = loader->GetWorkerManager();
      auto worker_task_runner = worker_manager->CreateTaskRunner(kWorkerRunnerName);
      worker_task_runner->PostTask(std::move(func));
    }
  }
#else
  auto ret = scope->GetContext()->RunScript(script_content, file_name);
#endif

  auto flag = (ret != nullptr);
  FOOTSTONE_LOG(INFO) << "runScript end, flag = " << flag;
  return flag;
}

void JsDriverUtils::DestroyInstance(const std::shared_ptr<Engine>& engine,
                                    const std::shared_ptr<Scope>& scope,
                                    const std::function<void(bool)>& callback,
                                    bool is_reload) {
  auto scope_destroy_callback = [engine, scope, is_reload, callback] {
#if defined(JS_V8) && defined(ENABLE_INSPECTOR) && !defined(V8_WITHOUT_INSPECTOR)
    auto v8_vm = std::static_pointer_cast<V8VM>(engine->GetVM());
    if (v8_vm->IsDebug()) {
      auto inspector_client = v8_vm->GetInspectorClient();
      if (inspector_client) {
        auto inspector_context = scope->GetInspectorContext();
        inspector_client->DestroyInspectorContext(is_reload, inspector_context);
      }
    } else {
      scope->WillExit();
    }
#else
    scope->WillExit();
#endif
    FOOTSTONE_LOG(INFO) << "js destroy end";
    callback(true);
  };
  int64_t group = engine->GetVM()->GetGroupId();
  if (group == VM::kDebuggerGroupId) {
    scope->WillExit();
  }
  if ((group == VM::kDebuggerGroupId && !is_reload) || (group != VM::kDebuggerGroupId && group != VM::kDefaultGroupId)) {
    std::lock_guard<std::mutex> lock(engine_mutex);
    auto it = reuse_engine_map.find(group);
    if (it != reuse_engine_map.end()) {
      uint32_t cnt = std::get<uint32_t>(it->second);
      FOOTSTONE_DLOG(INFO) << "reuse_engine_map cnt = " << cnt;
      if (cnt == 1) {
        reuse_engine_map.erase(it);
      } else {
        std::get<uint32_t>(it->second) = cnt - 1;
      }
    } else {
      FOOTSTONE_DLOG(FATAL) << "engine not find";
    }
  }
  auto runner = engine->GetJsTaskRunner();
  runner->PostTask(std::move(scope_destroy_callback));
  FOOTSTONE_DLOG(INFO) << "destroy, group = " << group;
}

void JsDriverUtils::CallJs(const string_view& action,
                           const std::shared_ptr<Scope>& scope,
                           std::function<void(CALL_FUNCTION_CB_STATE, string_view)> cb,
                           byte_string buffer_data,
                           std::function<void()> on_js_runner) {
  auto runner = scope->GetTaskRunner();
  auto callback = [scope, cb = std::move(cb), action,
      buffer_data_ = std::move(buffer_data),
      on_js_runner = std::move(on_js_runner)] {
    on_js_runner();
    if (!scope) {
      FOOTSTONE_DLOG(WARNING) << "CallJs scope invalid";
      return;
    }
    auto engine = scope->GetEngine().lock();
    FOOTSTONE_DCHECK(engine);
    if (!engine) {
      return;
    }
    auto context = scope->GetContext();
    if (!scope->GetBridgeObject()) {
      FOOTSTONE_DLOG(INFO) << "init bridge func";
      auto func_name = context->CreateString(kBridgeName);
      auto global_object = context->GetGlobalObject();
      auto function = context->GetProperty(global_object, func_name);
      bool is_function = context->IsFunction(function);
      FOOTSTONE_DLOG(INFO) << "is_fn = " << is_function;
      if (!is_function) {
        cb(CALL_FUNCTION_CB_STATE::NO_METHOD_ERROR, u"hippyBridge not find");
        return;
      } else {
        scope->SetBridgeObject(function);
      }
    }
    FOOTSTONE_DCHECK(action.encoding() == string_view::Encoding::Utf16);
    std::shared_ptr<CtxValue> action_value = context->CreateString(action);
    std::shared_ptr<CtxValue> params;
    auto vm = engine->GetVM();
#ifdef JS_V8
    auto v8_vm = std::static_pointer_cast<V8VM>(vm);
    if (v8_vm->IsEnableV8Serialization()) {
      auto result = v8_vm->Deserializer(scope->GetContext(), buffer_data_);
      if (result.flag) {
        params = result.result;
      } else {
        auto msg = u"deserializer error";
        if (!StringViewUtils::IsEmpty(result.message)) {
          msg = StringViewUtils::ConvertEncoding(result.message,
                                                 string_view::Encoding::Utf16).utf16_value().c_str();
        }
        cb(CALL_FUNCTION_CB_STATE::DESERIALIZER_FAILED, msg);
        return;
      }
    } else {
#endif
      std::u16string str(reinterpret_cast<const char16_t*>(&buffer_data_[0]),
                         buffer_data_.length() / sizeof(char16_t));
      string_view buf_str(std::move(str));
      FOOTSTONE_DLOG(INFO) << "action = " << action << ", buf_str = " << buf_str;
      params = vm->ParseJson(context, buf_str);
#ifdef JS_V8
    }
#endif
    if (!params) {
      params = context->CreateNull();
    }
    std::shared_ptr<CtxValue> argv[] = {action_value, params};
    context->CallFunction(scope->GetBridgeObject(), 2, argv);
    cb(CALL_FUNCTION_CB_STATE::SUCCESS, "");
  };

  runner->PostTask(std::move(callback));
}

void JsDriverUtils::CallNative(hippy::napi::CallbackInfo& info, const std::function<void(
    std::shared_ptr<Scope>,
    string_view,
    string_view,
    string_view,
    bool,
    byte_string)>& callback) {
  FOOTSTONE_DLOG(INFO) << "CallHost";
  auto scope_wrapper = reinterpret_cast<ScopeWrapper*>(std::any_cast<void*>(info.GetSlot()));
  auto scope = scope_wrapper->scope.lock();
  FOOTSTONE_CHECK(scope);
  auto context = scope->GetContext();

  string_view module_name;
  if (info[0]) {
    if (!context->GetValueString(info[0], &module_name)) {
      info.GetExceptionValue()->Set(context,"module name error");
      return;
    }
    FOOTSTONE_DLOG(INFO) << "CallJava module_name = " << module_name;
  } else {
    info.GetExceptionValue()->Set(context, "info error");
    return;
  }

  string_view fn_name;
  if (info[1]) {
    if (!context->GetValueString(info[1], &fn_name)) {
      info.GetExceptionValue()->Set(context,"func name error");
      return;
    }
    FOOTSTONE_DLOG(INFO) << "CallJava fn_name = " << fn_name;
  } else {
    info.GetExceptionValue()->Set(context, "info error");
    return;
  }

  string_view cb_id_str;
  if (info[2]) {
    double cb_id;
    if (context->GetValueString(info[2], &cb_id_str)) {
      FOOTSTONE_DLOG(INFO) << "CallJava cb_id = " << cb_id_str;
    } else if (context->GetValueNumber(info[2], &cb_id)) {
      cb_id_str = std::to_string(cb_id);
      FOOTSTONE_DLOG(INFO) << "CallJava cb_id = " << cb_id_str;
    }
  }

  std::string buffer_data;
  if (info[3] && context->IsObject(info[3])) {
#ifdef JS_V8
    auto engine = scope->GetEngine().lock();
    FOOTSTONE_DCHECK(engine);
    if (!engine) {
      return;
    }
    auto vm = engine->GetVM();
    auto v8_vm = std::static_pointer_cast<V8VM>(vm);
    if (v8_vm->IsEnableV8Serialization()) {
      auto v8_ctx = std::static_pointer_cast<hippy::napi::V8Ctx>(context);
      buffer_data = v8_ctx->GetSerializationBuffer(info[3], v8_vm->GetBuffer());
#endif
    } else {
      string_view json;
      auto flag = context->GetValueJson(info[3], &json);
      FOOTSTONE_DCHECK(flag);
      FOOTSTONE_DLOG(INFO) << "CallJava json = " << json;
      buffer_data = StringViewUtils::ToStdString(
          StringViewUtils::ConvertEncoding(json, string_view::Encoding::Utf8).utf8_value());
#ifdef JS_V8
    }
#endif
  }

  int32_t transfer_type = 0;
  if (info[4]) {
    context->GetValueNumber(info[4], &transfer_type);
  }
  callback(scope, module_name, fn_name, cb_id_str, transfer_type != 0, buffer_data);
}

void JsDriverUtils::LoadInstance(const std::shared_ptr<Scope>& scope, byte_string&& buffer_data) {
  auto runner = scope->GetTaskRunner();
  std::weak_ptr<Scope> weak_scope = scope;
  auto callback = [weak_scope, buffer_data_ = std::move(buffer_data)] {
    std::shared_ptr<Scope> scope = weak_scope.lock();
    if (!scope) {
      return;
    }
    Deserializer deserializer(
        reinterpret_cast<const uint8_t*>(buffer_data_.c_str()),
        buffer_data_.length());
    HippyValue value;
    deserializer.ReadHeader();
    auto ret = deserializer.ReadValue(value);
    if (ret) {
      scope->LoadInstance(std::make_shared<HippyValue>(std::move(value)));
    } else {
      scope->GetContext()->ThrowException("LoadInstance param error");
    }
  };
  runner->PostTask(std::move(callback));
}

void JsDriverUtils::UnloadInstance(const std::shared_ptr<Scope>& scope, byte_string&& buffer_data) {
    auto runner = scope->GetTaskRunner();
    std::weak_ptr<Scope> weak_scope = scope;
    auto callback = [weak_scope, buffer_data_ = std::move(buffer_data)] {
        std::shared_ptr<Scope> scope = weak_scope.lock();
        if (!scope) {
            return;
        }
        Deserializer deserializer(
                reinterpret_cast<const uint8_t*>(buffer_data_.c_str()),
                buffer_data_.length());
        HippyValue value;
        deserializer.ReadHeader();
        auto ret = deserializer.ReadValue(value);
        if (ret) {
            scope->UnloadInstance(std::make_shared<HippyValue>(std::move(value)));
        } else {
            scope->GetContext()->ThrowException("UnloadInstance param error");
        }
    };
    runner->PostTask(std::move(callback));
}

}
}
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "node.h"
#include "node_buffer.h"
#include "util.h"

#include "async_wrap-inl.h"
#include "env-inl.h"
#include "memory_tracker-inl.h"
#include "stream_base-inl.h"
#include "v8.h"
#include "llhttp.h"

#include <cstdlib>  // free()
#include <cstring>  // strdup(), strchr()


// This is a binding to llhttp (https://github.com/nodejs/llhttp)
// The goal is to decouple sockets from parsing for more javascript-level
// agility. A Buffer is read from a socket and passed to parser.execute().
// The parser then issues callbacks with slices of the data
//     parser.onMessageBegin
//     parser.onPath
//     parser.onBody
//     ...
// No copying is performed when slicing the buffer, only small reference
// allocations.


namespace node {
namespace {  // NOLINT(build/namespaces)

using v8::Array;
using v8::Boolean;
using v8::Context;
using v8::EscapableHandleScope;
using v8::Exception;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Int32;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Number;
using v8::Object;
using v8::String;
using v8::Uint32;
using v8::Undefined;
using v8::Value;

const uint32_t kOnMessageBegin = 0;
const uint32_t kOnHeaders = 1;
const uint32_t kOnHeadersComplete = 2;
const uint32_t kOnBody = 3;
const uint32_t kOnMessageComplete = 4;
const uint32_t kOnExecute = 5;
const uint32_t kOnTimeout = 6;
// Any more fields than this will be flushed into JS
const size_t kMaxHeaderFieldsCount = 32;

const uint32_t kLenientNone = 0;
const uint32_t kLenientHeaders = 1 << 0;
const uint32_t kLenientChunkedLength = 1 << 1;
const uint32_t kLenientKeepAlive = 1 << 2;
const uint32_t kLenientAll = kLenientHeaders | kLenientChunkedLength |
  kLenientKeepAlive;

inline bool IsOWS(char c) {
  return c == ' ' || c == '\t';
}

class BindingData : public BaseObject {
 public:
  BindingData(Environment* env, Local<Object> obj)
      : BaseObject(env, obj) {}

  static constexpr FastStringKey type_name { "http_parser" };

  std::vector<char> parser_buffer;
  bool parser_buffer_in_use = false;

  void MemoryInfo(MemoryTracker* tracker) const override {
    tracker->TrackField("parser_buffer", parser_buffer);
  }
  SET_SELF_SIZE(BindingData)
  SET_MEMORY_INFO_NAME(BindingData)
};

// helper class for the Parser
struct StringPtr {
  StringPtr() {
    on_heap_ = false;
    Reset();
  }


  ~StringPtr() {
    Reset();
  }


  // If str_ does not point to a heap string yet, this function makes it do
  // so. This is called at the end of each http_parser_execute() so as not
  // to leak references. See issue #2438 and test-http-parser-bad-ref.js.
  void Save() {
    if (!on_heap_ && size_ > 0) {
      char* s = new char[size_];
      memcpy(s, str_, size_);
      str_ = s;
      on_heap_ = true;
    }
  }


  void Reset() {
    if (on_heap_) {
      delete[] str_;
      on_heap_ = false;
    }

    str_ = nullptr;
    size_ = 0;
  }


  void Update(const char* str, size_t size) {
    if (str_ == nullptr) {
      str_ = str;
    } else if (on_heap_ || str_ + size_ != str) {
      // Non-consecutive input, make a copy on the heap.
      // TODO(bnoordhuis) Use slab allocation, O(n) allocs is bad.
      char* s = new char[size_ + size];
      memcpy(s, str_, size_);
      memcpy(s + size_, str, size);

      if (on_heap_)
        delete[] str_;
      else
        on_heap_ = true;

      str_ = s;
    }
    size_ += size;
  }


  Local<String> ToString(Environment* env) const {
    if (size_ != 0)
      return OneByteString(env->isolate(), str_, size_);
    else
      return String::Empty(env->isolate());
  }


  // Strip trailing OWS (SPC or HTAB) from string.
  Local<String> ToTrimmedString(Environment* env) {
    while (size_ > 0 && IsOWS(str_[size_ - 1])) {
      size_--;
    }
    return ToString(env);
  }


  const char* str_;
  bool on_heap_;
  size_t size_;
};

class Parser;

struct ParserComparator {
  bool operator()(const Parser* lhs, const Parser* rhs) const;
};

class ConnectionsList : public BaseObject {
 public:
    static void New(const FunctionCallbackInfo<Value>& args);

    static void All(const FunctionCallbackInfo<Value>& args);

    static void Idle(const FunctionCallbackInfo<Value>& args);

    static void Active(const FunctionCallbackInfo<Value>& args);

    static void Expired(const FunctionCallbackInfo<Value>& args);

    void Push(Parser* parser) {
      all_connections_.insert(parser);
    }

    void Pop(Parser* parser) {
      all_connections_.erase(parser);
    }

    void PushActive(Parser* parser) {
      active_connections_.insert(parser);
    }

    void PopActive(Parser* parser) {
      active_connections_.erase(parser);
    }

    SET_NO_MEMORY_INFO()
    SET_MEMORY_INFO_NAME(ConnectionsList)
    SET_SELF_SIZE(ConnectionsList)

 private:
    ConnectionsList(Environment* env, Local<Object> object)
      : BaseObject(env, object) {
        MakeWeak();
      }

    std::set<Parser*, ParserComparator> all_connections_;
    std::set<Parser*, ParserComparator> active_connections_;
};

class Parser : public AsyncWrap, public StreamListener {
  friend class ConnectionsList;
  friend struct ParserComparator;

 public:
  Parser(BindingData* binding_data, Local<Object> wrap)
      : AsyncWrap(binding_data->env(), wrap),
        current_buffer_len_(0),
        current_buffer_data_(nullptr),
        binding_data_(binding_data) {
  }


  void MemoryInfo(MemoryTracker* tracker) const override {
    tracker->TrackField("current_buffer", current_buffer_);
  }

  SET_MEMORY_INFO_NAME(Parser)
  SET_SELF_SIZE(Parser)

  int on_message_begin() {
    // Important: Pop from the lists BEFORE resetting the last_message_start_
    // otherwise std::set.erase will fail.
    if (connectionsList_ != nullptr) {
      connectionsList_->Pop(this);
      connectionsList_->PopActive(this);
    }

    num_fields_ = num_values_ = 0;
    headers_completed_ = false;
    last_message_start_ = uv_hrtime();
    url_.Reset();
    status_message_.Reset();

    if (connectionsList_ != nullptr) {
      connectionsList_->Push(this);
      connectionsList_->PushActive(this);
    }

    Local<Value> cb = object()->Get(env()->context(), kOnMessageBegin)
                              .ToLocalChecked();
    if (cb->IsFunction()) {
      InternalCallbackScope callback_scope(
        this, InternalCallbackScope::kSkipTaskQueues);

      MaybeLocal<Value> r = cb.As<Function>()->Call(
        env()->context(), object(), 0, nullptr);

      if (r.IsEmpty()) callback_scope.MarkAsFailed();
    }

    return 0;
  }


  int on_url(const char* at, size_t length) {
    int rv = TrackHeader(length);
    if (rv != 0) {
      return rv;
    }

    url_.Update(at, length);
    return 0;
  }


  int on_status(const char* at, size_t length) {
    int rv = TrackHeader(length);
    if (rv != 0) {
      return rv;
    }

    status_message_.Update(at, length);
    return 0;
  }


  int on_header_field(const char* at, size_t length) {
    int rv = TrackHeader(length);
    if (rv != 0) {
      return rv;
    }

    if (num_fields_ == num_values_) {
      // start of new field name
      num_fields_++;
      if (num_fields_ == kMaxHeaderFieldsCount) {
        // ran out of space - flush to javascript land
        Flush();
        num_fields_ = 1;
        num_values_ = 0;
      }
      fields_[num_fields_ - 1].Reset();
    }

    CHECK_LT(num_fields_, kMaxHeaderFieldsCount);
    CHECK_EQ(num_fields_, num_values_ + 1);

    fields_[num_fields_ - 1].Update(at, length);

    return 0;
  }


  int on_header_value(const char* at, size_t length) {
    int rv = TrackHeader(length);
    if (rv != 0) {
      return rv;
    }

    if (num_values_ != num_fields_) {
      // start of new header value
      num_values_++;
      values_[num_values_ - 1].Reset();
    }

    CHECK_LT(num_values_, arraysize(values_));
    CHECK_EQ(num_values_, num_fields_);

    values_[num_values_ - 1].Update(at, length);

    return 0;
  }


  int on_headers_complete() {
    headers_completed_ = true;
    header_nread_ = 0;

    // Arguments for the on-headers-complete javascript callback. This
    // list needs to be kept in sync with the actual argument list for
    // `parserOnHeadersComplete` in lib/_http_common.js.
    enum on_headers_complete_arg_index {
      A_VERSION_MAJOR = 0,
      A_VERSION_MINOR,
      A_HEADERS,
      A_METHOD,
      A_URL,
      A_STATUS_CODE,
      A_STATUS_MESSAGE,
      A_UPGRADE,
      A_SHOULD_KEEP_ALIVE,
      A_MAX
    };

    Local<Value> argv[A_MAX];
    Local<Object> obj = object();
    Local<Value> cb = obj->Get(env()->context(),
                               kOnHeadersComplete).ToLocalChecked();

    if (!cb->IsFunction())
      return 0;

    Local<Value> undefined = Undefined(env()->isolate());
    for (size_t i = 0; i < arraysize(argv); i++)
      argv[i] = undefined;

    if (have_flushed_) {
      // Slow case, flush remaining headers.
      Flush();
    } else {
      // Fast case, pass headers and URL to JS land.
      argv[A_HEADERS] = CreateHeaders();
      if (parser_.type == HTTP_REQUEST)
        argv[A_URL] = url_.ToString(env());
    }

    num_fields_ = 0;
    num_values_ = 0;

    // METHOD
    if (parser_.type == HTTP_REQUEST) {
      argv[A_METHOD] =
          Uint32::NewFromUnsigned(env()->isolate(), parser_.method);
    }

    // STATUS
    if (parser_.type == HTTP_RESPONSE) {
      argv[A_STATUS_CODE] =
          Integer::New(env()->isolate(), parser_.status_code);
      argv[A_STATUS_MESSAGE] = status_message_.ToString(env());
    }

    // VERSION
    argv[A_VERSION_MAJOR] = Integer::New(env()->isolate(), parser_.http_major);
    argv[A_VERSION_MINOR] = Integer::New(env()->isolate(), parser_.http_minor);

    bool should_keep_alive;
    should_keep_alive = llhttp_should_keep_alive(&parser_);

    argv[A_SHOULD_KEEP_ALIVE] =
        Boolean::New(env()->isolate(), should_keep_alive);

    argv[A_UPGRADE] = Boolean::New(env()->isolate(), parser_.upgrade);

    MaybeLocal<Value> head_response;
    {
      InternalCallbackScope callback_scope(
          this, InternalCallbackScope::kSkipTaskQueues);
      head_response = cb.As<Function>()->Call(
          env()->context(), object(), arraysize(argv), argv);
      if (head_response.IsEmpty()) callback_scope.MarkAsFailed();
    }

    int64_t val;

    if (head_response.IsEmpty() || !head_response.ToLocalChecked()
                                        ->IntegerValue(env()->context())
                                        .To(&val)) {
      got_exception_ = true;
      return -1;
    }

    return static_cast<int>(val);
  }


  int on_body(const char* at, size_t length) {
    EscapableHandleScope scope(env()->isolate());

    Local<Object> obj = object();
    Local<Value> cb = obj->Get(env()->context(), kOnBody).ToLocalChecked();

    if (!cb->IsFunction())
      return 0;

    // We came from consumed stream
    if (current_buffer_.IsEmpty()) {
      // Make sure Buffer will be in parent HandleScope
      current_buffer_ = scope.Escape(Buffer::Copy(
          env()->isolate(),
          current_buffer_data_,
          current_buffer_len_).ToLocalChecked());
    }

    Local<Value> argv[3] = {
        current_buffer_,
        Integer::NewFromUnsigned(
            env()->isolate(), static_cast<uint32_t>(at - current_buffer_data_)),
        Integer::NewFromUnsigned(env()->isolate(), length)};

    MaybeLocal<Value> r = MakeCallback(cb.As<Function>(),
                                       arraysize(argv),
                                       argv);

    if (r.IsEmpty()) {
      got_exception_ = true;
      llhttp_set_error_reason(&parser_, "HPE_JS_EXCEPTION:JS Exception");
      return HPE_USER;
    }

    return 0;
  }


  int on_message_complete() {
    HandleScope scope(env()->isolate());

    // Important: Pop from the lists BEFORE resetting the last_message_start_
    // otherwise std::set.erase will fail.
    if (connectionsList_ != nullptr) {
      connectionsList_->Pop(this);
      connectionsList_->PopActive(this);
    }

    last_message_start_ = 0;

    if (connectionsList_ != nullptr) {
      connectionsList_->Push(this);
    }

    if (num_fields_)
      Flush();  // Flush trailing HTTP headers.

    Local<Object> obj = object();
    Local<Value> cb = obj->Get(env()->context(),
                               kOnMessageComplete).ToLocalChecked();

    if (!cb->IsFunction())
      return 0;

    MaybeLocal<Value> r;
    {
      InternalCallbackScope callback_scope(
          this, InternalCallbackScope::kSkipTaskQueues);
      r = cb.As<Function>()->Call(env()->context(), object(), 0, nullptr);
      if (r.IsEmpty()) callback_scope.MarkAsFailed();
    }

    if (r.IsEmpty()) {
      got_exception_ = true;
      return -1;
    }

    return 0;
  }

  // Reset nread for the next chunk
  int on_chunk_header() {
    header_nread_ = 0;
    return 0;
  }


  // Reset nread for the next chunk
  int on_chunk_complete() {
    header_nread_ = 0;
    return 0;
  }

  static void New(const FunctionCallbackInfo<Value>& args) {
    BindingData* binding_data = Environment::GetBindingData<BindingData>(args);
    new Parser(binding_data, args.This());
  }


  static void Close(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    delete parser;
  }


  static void Free(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    if (parser->connectionsList_ != nullptr) {
      parser->connectionsList_->Pop(parser);
      parser->connectionsList_->PopActive(parser);
    }

    // Since the Parser destructor isn't going to run the destroy() callbacks
    // it needs to be triggered manually.
    parser->EmitTraceEventDestroy();
    parser->EmitDestroy();
  }


  void Save() {
    url_.Save();
    status_message_.Save();

    for (size_t i = 0; i < num_fields_; i++) {
      fields_[i].Save();
    }

    for (size_t i = 0; i < num_values_; i++) {
      values_[i].Save();
    }
  }

  // var bytesParsed = parser->execute(buffer);
  static void Execute(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());
    CHECK(parser->current_buffer_.IsEmpty());
    CHECK_EQ(parser->current_buffer_len_, 0);
    CHECK_NULL(parser->current_buffer_data_);

    ArrayBufferViewContents<char> buffer(args[0]);

    // This is a hack to get the current_buffer to the callbacks with the least
    // amount of overhead. Nothing else will run while http_parser_execute()
    // runs, therefore this pointer can be set and used for the execution.
    parser->current_buffer_ = args[0].As<Object>();

    Local<Value> ret = parser->Execute(buffer.data(), buffer.length());

    if (!ret.IsEmpty())
      args.GetReturnValue().Set(ret);
  }


  static void Finish(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    CHECK(parser->current_buffer_.IsEmpty());
    Local<Value> ret = parser->Execute(nullptr, 0);

    if (!ret.IsEmpty())
      args.GetReturnValue().Set(ret);
  }


  static void Initialize(const FunctionCallbackInfo<Value>& args) {
    Environment* env = Environment::GetCurrent(args);

    uint64_t max_http_header_size = 0;
    uint32_t lenient_flags = kLenientNone;
    ConnectionsList* connectionsList = nullptr;

    CHECK(args[0]->IsInt32());
    CHECK(args[1]->IsObject());

    if (args.Length() > 2) {
      CHECK(args[2]->IsNumber());
      max_http_header_size =
          static_cast<uint64_t>(args[2].As<Number>()->Value());
    }
    if (max_http_header_size == 0) {
      max_http_header_size = env->options()->max_http_header_size;
    }

    if (args.Length() > 3) {
      CHECK(args[3]->IsInt32());
      lenient_flags = args[3].As<Int32>()->Value();
    }

    if (args.Length() > 4 && !args[4]->IsNullOrUndefined()) {
      CHECK(args[4]->IsObject());
      ASSIGN_OR_RETURN_UNWRAP(&connectionsList, args[4]);
    }

    llhttp_type_t type =
        static_cast<llhttp_type_t>(args[0].As<Int32>()->Value());

    CHECK(type == HTTP_REQUEST || type == HTTP_RESPONSE);
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());
    // Should always be called from the same context.
    CHECK_EQ(env, parser->env());

    AsyncWrap::ProviderType provider =
        (type == HTTP_REQUEST ?
            AsyncWrap::PROVIDER_HTTPINCOMINGMESSAGE
            : AsyncWrap::PROVIDER_HTTPCLIENTREQUEST);

    parser->set_provider_type(provider);
    parser->AsyncReset(args[1].As<Object>());
    parser->Init(type, max_http_header_size, lenient_flags);

    if (connectionsList != nullptr) {
      parser->connectionsList_ = connectionsList;

      // This protects from a DoS attack where an attacker establishes
      // the connection without sending any data on applications where
      // server.timeout is left to the default value of zero.
      parser->last_message_start_ = uv_hrtime();

      // Important: Push into the lists AFTER setting the last_message_start_
      // otherwise std::set.erase will fail later.
      parser->connectionsList_->Push(parser);
      parser->connectionsList_->PushActive(parser);
    } else {
      parser->connectionsList_ = nullptr;
    }
  }

  template <bool should_pause>
  static void Pause(const FunctionCallbackInfo<Value>& args) {
    Environment* env = Environment::GetCurrent(args);
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());
    // Should always be called from the same context.
    CHECK_EQ(env, parser->env());

    if (parser->execute_depth_) {
      parser->pending_pause_ = should_pause;
      return;
    }

    if (should_pause) {
      llhttp_pause(&parser->parser_);
    } else {
      llhttp_resume(&parser->parser_);
    }
  }


  static void Consume(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());
    CHECK(args[0]->IsObject());
    StreamBase* stream = StreamBase::FromObject(args[0].As<Object>());
    CHECK_NOT_NULL(stream);
    stream->PushStreamListener(parser);
  }


  static void Unconsume(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    // Already unconsumed
    if (parser->stream_ == nullptr)
      return;

    parser->stream_->RemoveStreamListener(parser);
  }


  static void GetCurrentBuffer(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    Local<Object> ret = Buffer::Copy(
        parser->env(),
        parser->current_buffer_data_,
        parser->current_buffer_len_).ToLocalChecked();

    args.GetReturnValue().Set(ret);
  }

  static void Duration(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    if (parser->last_message_start_ == 0) {
      args.GetReturnValue().Set(0);
      return;
    }

    double duration = (uv_hrtime() - parser->last_message_start_) / 1e6;
    args.GetReturnValue().Set(duration);
  }

  static void HeadersCompleted(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    args.GetReturnValue().Set(parser->headers_completed_);
  }

 protected:
  static const size_t kAllocBufferSize = 64 * 1024;

  uv_buf_t OnStreamAlloc(size_t suggested_size) override {
    // For most types of streams, OnStreamRead will be immediately after
    // OnStreamAlloc, and will consume all data, so using a static buffer for
    // reading is more efficient. For other streams, just use Malloc() directly.
    if (binding_data_->parser_buffer_in_use)
      return uv_buf_init(Malloc(suggested_size), suggested_size);
    binding_data_->parser_buffer_in_use = true;

    if (binding_data_->parser_buffer.empty())
      binding_data_->parser_buffer.resize(kAllocBufferSize);

    return uv_buf_init(binding_data_->parser_buffer.data(), kAllocBufferSize);
  }


  void OnStreamRead(ssize_t nread, const uv_buf_t& buf) override {
    HandleScope scope(env()->isolate());
    // Once we’re done here, either indicate that the HTTP parser buffer
    // is free for re-use, or free() the data if it didn’t come from there
    // in the first place.
    auto on_scope_leave = OnScopeLeave([&]() {
      if (buf.base == binding_data_->parser_buffer.data())
        binding_data_->parser_buffer_in_use = false;
      else
        free(buf.base);
    });

    if (nread < 0) {
      PassReadErrorToPreviousListener(nread);
      return;
    }

    // Ignore, empty reads have special meaning in http parser
    if (nread == 0)
      return;

    current_buffer_.Clear();
    Local<Value> ret = Execute(buf.base, nread);

    // Exception
    if (ret.IsEmpty())
      return;

    Local<Value> cb =
        object()->Get(env()->context(), kOnExecute).ToLocalChecked();

    if (!cb->IsFunction())
      return;

    // Hooks for GetCurrentBuffer
    current_buffer_len_ = nread;
    current_buffer_data_ = buf.base;

    MakeCallback(cb.As<Function>(), 1, &ret);

    current_buffer_len_ = 0;
    current_buffer_data_ = nullptr;
  }


  Local<Value> Execute(const char* data, size_t len) {
    EscapableHandleScope scope(env()->isolate());

    current_buffer_len_ = len;
    current_buffer_data_ = data;
    got_exception_ = false;

    llhttp_errno_t err;

    // Do not allow re-entering `http_parser_execute()`
    CHECK_EQ(execute_depth_, 0);

    execute_depth_++;
    if (data == nullptr) {
      err = llhttp_finish(&parser_);
    } else {
      err = llhttp_execute(&parser_, data, len);
      Save();
    }
    execute_depth_--;

    // Calculate bytes read and resume after Upgrade/CONNECT pause
    size_t nread = len;
    if (err != HPE_OK) {
      nread = llhttp_get_error_pos(&parser_) - data;

      // This isn't a real pause, just a way to stop parsing early.
      if (err == HPE_PAUSED_UPGRADE) {
        err = HPE_OK;
        llhttp_resume_after_upgrade(&parser_);
      }
    }

    // Apply pending pause
    if (pending_pause_) {
      pending_pause_ = false;
      llhttp_pause(&parser_);
    }

    // Unassign the 'buffer_' variable
    current_buffer_.Clear();
    current_buffer_len_ = 0;
    current_buffer_data_ = nullptr;

    // If there was an exception in one of the callbacks
    if (got_exception_)
      return scope.Escape(Local<Value>());

    Local<Integer> nread_obj = Integer::New(env()->isolate(), nread);

    // If there was a parse error in one of the callbacks
    // TODO(bnoordhuis) What if there is an error on EOF?
    if (!parser_.upgrade && err != HPE_OK) {
      Local<Value> e = Exception::Error(env()->parse_error_string());
      Local<Object> obj = e->ToObject(env()->isolate()->GetCurrentContext())
        .ToLocalChecked();
      obj->Set(env()->context(),
               env()->bytes_parsed_string(),
               nread_obj).Check();
      const char* errno_reason = llhttp_get_error_reason(&parser_);

      Local<String> code;
      Local<String> reason;
      if (err == HPE_USER) {
        const char* colon = strchr(errno_reason, ':');
        CHECK_NOT_NULL(colon);
        code = OneByteString(env()->isolate(),
                             errno_reason,
                             static_cast<int>(colon - errno_reason));
        reason = OneByteString(env()->isolate(), colon + 1);
      } else {
        code = OneByteString(env()->isolate(), llhttp_errno_name(err));
        reason = OneByteString(env()->isolate(), errno_reason);
      }

      obj->Set(env()->context(), env()->code_string(), code).Check();
      obj->Set(env()->context(), env()->reason_string(), reason).Check();
      return scope.Escape(e);
    }

    // No return value is needed for `Finish()`
    if (data == nullptr) {
      return scope.Escape(Local<Value>());
    }
    return scope.Escape(nread_obj);
  }

  Local<Array> CreateHeaders() {
    // There could be extra entries but the max size should be fixed
    Local<Value> headers_v[kMaxHeaderFieldsCount * 2];

    for (size_t i = 0; i < num_values_; ++i) {
      headers_v[i * 2] = fields_[i].ToString(env());
      headers_v[i * 2 + 1] = values_[i].ToTrimmedString(env());
    }

    return Array::New(env()->isolate(), headers_v, num_values_ * 2);
  }


  // spill headers and request path to JS land
  void Flush() {
    HandleScope scope(env()->isolate());

    Local<Object> obj = object();
    Local<Value> cb = obj->Get(env()->context(), kOnHeaders).ToLocalChecked();

    if (!cb->IsFunction())
      return;

    Local<Value> argv[2] = {
      CreateHeaders(),
      url_.ToString(env())
    };

    MaybeLocal<Value> r = MakeCallback(cb.As<Function>(),
                                       arraysize(argv),
                                       argv);

    if (r.IsEmpty())
      got_exception_ = true;

    url_.Reset();
    have_flushed_ = true;
  }


  void Init(llhttp_type_t type, uint64_t max_http_header_size,
            uint32_t lenient_flags) {
    llhttp_init(&parser_, type, &settings);

    if (lenient_flags & kLenientHeaders) {
      llhttp_set_lenient_headers(&parser_, 1);
    }
    if (lenient_flags & kLenientChunkedLength) {
      llhttp_set_lenient_chunked_length(&parser_, 1);
    }
    if (lenient_flags & kLenientKeepAlive) {
      llhttp_set_lenient_keep_alive(&parser_, 1);
    }

    header_nread_ = 0;
    url_.Reset();
    status_message_.Reset();
    num_fields_ = 0;
    num_values_ = 0;
    have_flushed_ = false;
    got_exception_ = false;
    headers_completed_ = false;
    max_http_header_size_ = max_http_header_size;
  }


  int TrackHeader(size_t len) {
    header_nread_ += len;
    if (header_nread_ >= max_http_header_size_) {
      llhttp_set_error_reason(&parser_, "HPE_HEADER_OVERFLOW:Header overflow");
      return HPE_USER;
    }
    return 0;
  }


  int MaybePause() {
    CHECK_NE(execute_depth_, 0);

    if (!pending_pause_) {
      return 0;
    }

    pending_pause_ = false;
    llhttp_set_error_reason(&parser_, "Paused in callback");
    return HPE_PAUSED;
  }


  bool IsNotIndicativeOfMemoryLeakAtExit() const override {
    // HTTP parsers are able to emit events without any GC root referring
    // to them, because they receive events directly from the underlying
    // libuv resource.
    return true;
  }


  llhttp_t parser_;
  StringPtr fields_[kMaxHeaderFieldsCount];  // header fields
  StringPtr values_[kMaxHeaderFieldsCount];  // header values
  StringPtr url_;
  StringPtr status_message_;
  size_t num_fields_;
  size_t num_values_;
  bool have_flushed_;
  bool got_exception_;
  Local<Object> current_buffer_;
  size_t current_buffer_len_;
  const char* current_buffer_data_;
  unsigned int execute_depth_ = 0;
  bool headers_completed_ = false;
  bool pending_pause_ = false;
  uint64_t header_nread_ = 0;
  uint64_t max_http_header_size_;
  uint64_t last_message_start_;
  ConnectionsList* connectionsList_;

  BaseObjectPtr<BindingData> binding_data_;

  // These are helper functions for filling `http_parser_settings`, which turn
  // a member function of Parser into a C-style HTTP parser callback.
  template <typename Parser, Parser> struct Proxy;
  template <typename Parser, typename ...Args, int (Parser::*Member)(Args...)>
  struct Proxy<int (Parser::*)(Args...), Member> {
    static int Raw(llhttp_t* p, Args ... args) {
      Parser* parser = ContainerOf(&Parser::parser_, p);
      int rv = (parser->*Member)(std::forward<Args>(args)...);
      if (rv == 0) {
        rv = parser->MaybePause();
      }
      return rv;
    }
  };

  typedef int (Parser::*Call)();
  typedef int (Parser::*DataCall)(const char* at, size_t length);

  static const llhttp_settings_t settings;
};

bool ParserComparator::operator()(const Parser* lhs, const Parser* rhs) const {
  if (lhs->last_message_start_ == 0 && rhs->last_message_start_ == 0) {
    // When both parsers are idle, guarantee strict order by
    // comparing pointers as ints.
    return lhs < rhs;
  } else if (lhs->last_message_start_ == 0) {
    return true;
  } else if (rhs->last_message_start_ == 0) {
    return false;
  }

  return lhs->last_message_start_ < rhs->last_message_start_;
}

void ConnectionsList::New(const FunctionCallbackInfo<Value>& args) {
  Local<Context> context = args.GetIsolate()->GetCurrentContext();
  Environment* env = Environment::GetCurrent(context);

  new ConnectionsList(env, args.This());
}

void ConnectionsList::All(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  Local<Array> all = Array::New(isolate);
  ConnectionsList* list;

  ASSIGN_OR_RETURN_UNWRAP(&list, args.Holder());

  uint32_t i = 0;
  for (auto parser : list->all_connections_) {
    if (all->Set(context, i++, parser->object()).IsNothing()) {
      return;
    }
  }

  return args.GetReturnValue().Set(all);
}

void ConnectionsList::Idle(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  Local<Array> idle = Array::New(isolate);
  ConnectionsList* list;

  ASSIGN_OR_RETURN_UNWRAP(&list, args.Holder());

  uint32_t i = 0;
  for (auto parser : list->all_connections_) {
    if (parser->last_message_start_ == 0) {
      if (idle->Set(context, i++, parser->object()).IsNothing()) {
        return;
      }
    }
  }

  return args.GetReturnValue().Set(idle);
}

void ConnectionsList::Active(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  Local<Array> active = Array::New(isolate);
  ConnectionsList* list;

  ASSIGN_OR_RETURN_UNWRAP(&list, args.Holder());

  uint32_t i = 0;
  for (auto parser : list->active_connections_) {
    if (active->Set(context, i++, parser->object()).IsNothing()) {
      return;
    }
  }

  return args.GetReturnValue().Set(active);
}

void ConnectionsList::Expired(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  Local<Array> expired = Array::New(isolate);
  ConnectionsList* list;

  ASSIGN_OR_RETURN_UNWRAP(&list, args.Holder());
  CHECK(args[0]->IsNumber());
  CHECK(args[1]->IsNumber());
  uint64_t headers_timeout =
    static_cast<uint64_t>(args[0].As<Uint32>()->Value()) * 1000000;
  uint64_t request_timeout =
    static_cast<uint64_t>(args[1].As<Uint32>()->Value()) * 1000000;

  if (headers_timeout == 0 && request_timeout == 0) {
    return args.GetReturnValue().Set(expired);
  } else if (request_timeout > 0 && headers_timeout > request_timeout) {
    std::swap(headers_timeout, request_timeout);
  }

  const uint64_t now = uv_hrtime();
  const uint64_t headers_deadline =
    headers_timeout > 0 ? now - headers_timeout : 0;
  const uint64_t request_deadline =
    request_timeout > 0 ? now - request_timeout : 0;

  uint32_t i = 0;
  auto iter = list->active_connections_.begin();
  auto end = list->active_connections_.end();
  while (iter != end) {
    Parser* parser = *iter;
    iter++;

    // Check for expiration.
    if (
      (!parser->headers_completed_ && headers_deadline > 0 &&
        parser->last_message_start_ < headers_deadline) ||
      (
        request_deadline > 0 &&
        parser->last_message_start_ < request_deadline)
    ) {
      if (expired->Set(context, i++, parser->object()).IsNothing()) {
        return;
      }

      list->active_connections_.erase(parser);
    }
  }

  return args.GetReturnValue().Set(expired);
}

const llhttp_settings_t Parser::settings = {
  Proxy<Call, &Parser::on_message_begin>::Raw,
  Proxy<DataCall, &Parser::on_url>::Raw,
  Proxy<DataCall, &Parser::on_status>::Raw,
  Proxy<DataCall, &Parser::on_header_field>::Raw,
  Proxy<DataCall, &Parser::on_header_value>::Raw,
  Proxy<Call, &Parser::on_headers_complete>::Raw,
  Proxy<DataCall, &Parser::on_body>::Raw,
  Proxy<Call, &Parser::on_message_complete>::Raw,
  Proxy<Call, &Parser::on_chunk_header>::Raw,
  Proxy<Call, &Parser::on_chunk_complete>::Raw,

  // on_url_complete
  nullptr,
  // on_status_complete
  nullptr,
  // on_header_field_complete
  nullptr,
  // on_header_value_complete
  nullptr,
};


void InitializeHttpParser(Local<Object> target,
                          Local<Value> unused,
                          Local<Context> context,
                          void* priv) {
  Environment* env = Environment::GetCurrent(context);
  BindingData* const binding_data =
      env->AddBindingData<BindingData>(context, target);
  if (binding_data == nullptr) return;

  Local<FunctionTemplate> t = env->NewFunctionTemplate(Parser::New);
  t->InstanceTemplate()->SetInternalFieldCount(Parser::kInternalFieldCount);

  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "REQUEST"),
         Integer::New(env->isolate(), HTTP_REQUEST));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "RESPONSE"),
         Integer::New(env->isolate(), HTTP_RESPONSE));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnMessageBegin"),
         Integer::NewFromUnsigned(env->isolate(), kOnMessageBegin));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnHeaders"),
         Integer::NewFromUnsigned(env->isolate(), kOnHeaders));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnHeadersComplete"),
         Integer::NewFromUnsigned(env->isolate(), kOnHeadersComplete));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnBody"),
         Integer::NewFromUnsigned(env->isolate(), kOnBody));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnMessageComplete"),
         Integer::NewFromUnsigned(env->isolate(), kOnMessageComplete));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnExecute"),
         Integer::NewFromUnsigned(env->isolate(), kOnExecute));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnTimeout"),
         Integer::NewFromUnsigned(env->isolate(), kOnTimeout));

  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kLenientNone"),
         Integer::NewFromUnsigned(env->isolate(), kLenientNone));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kLenientHeaders"),
         Integer::NewFromUnsigned(env->isolate(), kLenientHeaders));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kLenientChunkedLength"),
         Integer::NewFromUnsigned(env->isolate(), kLenientChunkedLength));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kLenientKeepAlive"),
         Integer::NewFromUnsigned(env->isolate(), kLenientKeepAlive));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kLenientAll"),
         Integer::NewFromUnsigned(env->isolate(), kLenientAll));

  Local<Array> methods = Array::New(env->isolate());
#define V(num, name, string)                                                  \
    methods->Set(env->context(),                                              \
        num, FIXED_ONE_BYTE_STRING(env->isolate(), #string)).Check();
  HTTP_METHOD_MAP(V)
#undef V
  target->Set(env->context(),
              FIXED_ONE_BYTE_STRING(env->isolate(), "methods"),
              methods).Check();

  t->Inherit(AsyncWrap::GetConstructorTemplate(env));
  env->SetProtoMethod(t, "close", Parser::Close);
  env->SetProtoMethod(t, "free", Parser::Free);
  env->SetProtoMethod(t, "execute", Parser::Execute);
  env->SetProtoMethod(t, "finish", Parser::Finish);
  env->SetProtoMethod(t, "initialize", Parser::Initialize);
  env->SetProtoMethod(t, "pause", Parser::Pause<true>);
  env->SetProtoMethod(t, "resume", Parser::Pause<false>);
  env->SetProtoMethod(t, "consume", Parser::Consume);
  env->SetProtoMethod(t, "unconsume", Parser::Unconsume);
  env->SetProtoMethod(t, "getCurrentBuffer", Parser::GetCurrentBuffer);
  env->SetProtoMethod(t, "duration", Parser::Duration);
  env->SetProtoMethod(t, "headersCompleted", Parser::HeadersCompleted);

  env->SetConstructorFunction(target, "HTTPParser", t);

  Local<FunctionTemplate> c = env->NewFunctionTemplate(ConnectionsList::New);
  c->InstanceTemplate()
    ->SetInternalFieldCount(ConnectionsList::kInternalFieldCount);
  env->SetProtoMethod(c, "all", ConnectionsList::All);
  env->SetProtoMethod(c, "idle", ConnectionsList::Idle);
  env->SetProtoMethod(c, "active", ConnectionsList::Active);
  env->SetProtoMethod(c, "expired", ConnectionsList::Expired);
  env->SetConstructorFunction(target, "ConnectionsList", c);
}

}  // anonymous namespace
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(http_parser, node::InitializeHttpParser)

#include <node.h>
#include <node_events.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>

#include "utils.h"

#define CHUNK 16384

using namespace v8;
using namespace node;

typedef ScopedOutputBuffer<Bytef> ScopedBytesBlob;

class Gzip : public EventEmitter {
 public:
  static void
  Initialize (v8::Handle<v8::Object> target)
  {
    HandleScope scope;

    Local<FunctionTemplate> t = FunctionTemplate::New(New);

    t->Inherit(EventEmitter::constructor_template);
    t->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(t, "init", GzipInit);
    NODE_SET_PROTOTYPE_METHOD(t, "deflate", GzipDeflate);
    NODE_SET_PROTOTYPE_METHOD(t, "end", GzipEnd);

    target->Set(String::NewSymbol("Gzip"), t->GetFunction());
  }

 private:
  int GzipInit(int level) {
    COND_RETURN(state_ != State::Idle, Z_STREAM_ERROR);

    /* allocate deflate state */
    stream_.zalloc = Z_NULL;
    stream_.zfree = Z_NULL;
    stream_.opaque = Z_NULL;

    int ret = deflateInit2(&stream_, level,
                           Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (ret == Z_OK) {
      state_ = State::Data;
    }
    return ret;
  }


  int GzipDeflate(char* data, int data_len,
                  ScopedBytesBlob &out, int* out_len) {
    *out_len = 0;
    COND_RETURN(state_ != State::Data, Z_STREAM_ERROR);

    State::Transition t(state_, State::Error);

    int ret = Z_OK;
    while (data_len > 0) {    
      if (data_len > CHUNK) {
        stream_.avail_in = CHUNK;
      } else {
        stream_.avail_in = data_len;
      }

      stream_.next_in = (Bytef*)data;
      do {
        COND_RETURN(!out.GrowBy(CHUNK), Z_MEM_ERROR);

        stream_.avail_out = CHUNK;
        stream_.next_out = out.data() + *out_len;

        ret = deflate(&stream_, Z_NO_FLUSH);
        assert(ret != Z_STREAM_ERROR);  /* state not clobbered */
        COND_RETURN(ret != Z_OK, ret);

        *out_len += (CHUNK - stream_.avail_out);
      } while (stream_.avail_out == 0);

      data += CHUNK;
      data_len -= CHUNK;
    }

    t.alter(State::Data);
    return ret;
  }


  int GzipEnd(ScopedBytesBlob &out, int *out_len) {
    *out_len = 0;
    COND_RETURN(state_ == State::Idle, Z_OK);
    assert(state_ == State::Data || state_ == State::Error);

    State::Transition t(state_, State::Idle);
    int ret = Z_OK;
    if (state_ == State::Data) {
      ret = GzipEndWithData(out, out_len);
    }

    deflateEnd(&stream_);
    return ret;
  }


  int GzipEndWithData(ScopedBytesBlob &out, int *out_len) {
    int ret;

    stream_.avail_in = 0;
    stream_.next_in = NULL;
    do {
      COND_RETURN(!out.GrowBy(CHUNK), Z_MEM_ERROR);

      stream_.avail_out = CHUNK;
      stream_.next_out = out.data() + *out_len;

      ret = deflate(&stream_, Z_FINISH);
      assert(ret != Z_STREAM_ERROR);  /* state not clobbered */
      COND_RETURN(ret != Z_OK && ret != Z_STREAM_END, ret);

      *out_len += (CHUNK - stream_.avail_out);
    } while (ret != Z_STREAM_END);
    return ret;
  }


 protected:

  static Handle<Value>
  New (const Arguments& args)
  {
    HandleScope scope;

    Gzip *gzip = new Gzip();
    gzip->Wrap(args.This());

    return args.This();
  }

  static Handle<Value>
  GzipInit (const Arguments& args)
  {
    Gzip *gzip = ObjectWrap::Unwrap<Gzip>(args.This());

    HandleScope scope;

    int level=Z_DEFAULT_COMPRESSION;
    int r = gzip->GzipInit(level);
    return scope.Close(Integer::New(r));
  }

  static Handle<Value>
  GzipDeflate(const Arguments& args) {
    Gzip *gzip = ObjectWrap::Unwrap<Gzip>(args.This());

    HandleScope scope;

    enum encoding enc = ParseEncoding(args[1]);
    ssize_t len = DecodeBytes(args[0], enc);

    if (len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }
    ScopedArray<char> buf(len);
    ssize_t written = DecodeWrite(buf.data(), len, args[0], enc);
    assert(written == len);

    ScopedBytesBlob out;
    int out_size;
    int r = gzip->GzipDeflate(buf.data(), len, out, &out_size);

    if (out_size == 0) {
      return scope.Close(String::New(""));
    }

    Local<Value> outString = Encode(out.data(), out_size, BINARY);
    return scope.Close(outString);
  }

  static Handle<Value>
  GzipEnd(const Arguments& args) {
    Gzip *gzip = ObjectWrap::Unwrap<Gzip>(args.This());

    HandleScope scope;

    ScopedBytesBlob out;
    int out_size;
    bool hex_format = false;

    if (args.Length() > 0 && args[0]->IsString()) {
      String::Utf8Value format_type(args[1]->ToString());
    }  

    int r = gzip->GzipEnd(out, &out_size);

    if (out_size==0) {
      return String::New("");
    }
    Local<Value> outString = Encode(out.data(), out_size, BINARY);
    return scope.Close(outString);
  }


  Gzip() 
    : EventEmitter(), state_(State::Idle)
  {}

  ~Gzip()
  {
    if (state_ != State::Idle) {
      // Release zlib structures.
      deflateEnd(&stream_);
    }
  }

 private:
  z_stream stream_;
  State::Value state_;

};


class Gunzip : public EventEmitter {
 public:
  static void
  Initialize (v8::Handle<v8::Object> target)
  {
    HandleScope scope;

    Local<FunctionTemplate> t = FunctionTemplate::New(New);

    t->Inherit(EventEmitter::constructor_template);
    t->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(t, "init", GunzipInit);
    NODE_SET_PROTOTYPE_METHOD(t, "inflate", GunzipInflate);
    NODE_SET_PROTOTYPE_METHOD(t, "end", GunzipEnd);

    target->Set(String::NewSymbol("Gunzip"), t->GetFunction());
  }

 private:
  int GunzipInit() {
    COND_RETURN(state_ != State::Idle, Z_STREAM_ERROR);

    /* allocate inflate state */
    stream_.zalloc = Z_NULL;
    stream_.zfree = Z_NULL;
    stream_.opaque = Z_NULL;
    stream_.avail_in = 0;
    stream_.next_in = Z_NULL;

    int ret = inflateInit2(&stream_, 16 + MAX_WBITS);
    if (ret == Z_OK) {
      state_ = State::Data;
    }
    return ret;
  }


  int GunzipInflate(const char* data, int data_len,
                    ScopedBytesBlob &out, int* out_len) {
    *out_len = 0;
    COND_RETURN(state_ == State::Eos, Z_OK);
    COND_RETURN(state_ != State::Data, Z_STREAM_ERROR);

    State::Transition t(state_, State::Error);

    int ret = Z_OK;
    while (data_len > 0) { 
      if (data_len > CHUNK) {
        stream_.avail_in = CHUNK;
      } else {
        stream_.avail_in = data_len;
      }

      stream_.next_in = (Bytef*)data;
      do {
        COND_RETURN(!out.GrowBy(CHUNK), Z_MEM_ERROR);
        
        stream_.avail_out = CHUNK;
        stream_.next_out = out.data() + *out_len;

        ret = inflate(&stream_, Z_NO_FLUSH);
        assert(ret != Z_STREAM_ERROR);  /* state not clobbered */

        switch (ret) {
        case Z_NEED_DICT:
          ret = Z_DATA_ERROR;     /* and fall through */
        case Z_DATA_ERROR:
        case Z_MEM_ERROR:
          t.abort();
          GunzipEnd();
          return ret;
        }
        COND_RETURN(ret != Z_OK && ret != Z_STREAM_END, ret);
        
        *out_len += (CHUNK - stream_.avail_out);

        if (ret == Z_STREAM_END) {
          t.alter(State::Eos);
          return ret;
        }
      } while (stream_.avail_out == 0);
      data += CHUNK;
      data_len -= CHUNK;
    }
    t.alter(State::Data);
    return ret;
  }


  void GunzipEnd() {
    if (state_ != State::Idle) {
      state_ = State::Idle;
      inflateEnd(&stream_);
    }
  }

 protected:

  static Handle<Value>
  New(const Arguments& args) {
    HandleScope scope;

    Gunzip *gunzip = new Gunzip();
    gunzip->Wrap(args.This());

    return args.This();
  }

  static Handle<Value>
  GunzipInit(const Arguments& args) {
    Gunzip *gunzip = ObjectWrap::Unwrap<Gunzip>(args.This());

    HandleScope scope;

    int r = gunzip->GunzipInit();

    return scope.Close(Integer::New(r));
  }


  static Handle<Value>
  GunzipInflate(const Arguments& args) {
    Gunzip *gunzip = ObjectWrap::Unwrap<Gunzip>(args.This());

    HandleScope scope;

    enum encoding enc = ParseEncoding(args[1]);
    ssize_t len = DecodeBytes(args[0], enc);

    if (len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }

    ScopedArray<char> buf(len);
    ssize_t written = DecodeWrite(buf.data(), len, args[0], BINARY);
    assert(written == len);

    ScopedBytesBlob out;
    int out_size;
    int r = gunzip->GunzipInflate(buf.data(), len, out, &out_size);

    Local<Value> outString = Encode(out.data(), out_size, enc);
    return scope.Close(outString);
  }

  static Handle<Value>
  GunzipEnd(const Arguments& args) {
    Gunzip *gunzip = ObjectWrap::Unwrap<Gunzip>(args.This());

    HandleScope scope;

    gunzip->GunzipEnd();

    return scope.Close(String::New(""));
  }

  Gunzip() 
    : EventEmitter(), state_(State::Idle)
  {}

  ~Gunzip()
  {
    this->GunzipEnd();
  }

 private:
  z_stream stream_;
  State::Value state_;
};

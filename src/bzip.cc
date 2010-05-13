#include <node.h>
#include <node_events.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#define BZ_NO_STDIO
#include <bzlib.h>
#undef BZ_NO_STDIO

#include "utils.h"
#include "zlib.h"

using namespace v8;
using namespace node;


class BzipUtils {
 public:
  typedef ScopedBlob Blob;


  static bool IsError(int bzipStatus) {
    return !(bzipStatus == BZ_OK ||
        bzipStatus == BZ_RUN_OK ||
        bzipStatus == BZ_FLUSH_OK ||
        bzipStatus == BZ_FINISH_OK ||
        bzipStatus == BZ_STREAM_END);
  }


  static Local<Value> GetException(int bzipStatus) {
    if (!IsError(bzipStatus)) {
      return Local<Value>::New(Undefined());
    } else {
      switch (bzipStatus) {
        case BZ_CONFIG_ERROR:
          return Exception::Error(String::New(ConfigError));
        case BZ_SEQUENCE_ERROR:
          return Exception::Error(String::New(SequenceError));
        case BZ_PARAM_ERROR:
          return Exception::Error(String::New(ParamError));
        case BZ_MEM_ERROR:
          return Exception::Error(String::New(MemError));
        case BZ_DATA_ERROR:
          return Exception::Error(String::New(DataError));
        case BZ_DATA_ERROR_MAGIC:
          return Exception::Error(String::New(DataErrorMagic));
        case BZ_IO_ERROR:
          return Exception::Error(String::New(IoError));
        case BZ_UNEXPECTED_EOF:
          return Exception::Error(String::New(UnexpectedEof));
        case BZ_OUTBUFF_FULL:
          return Exception::Error(String::New(OutbuffFull));

        default:
          return Exception::Error(String::New("Unknown error"));
      }
    }
  }

 private:
  static const char ConfigError[];
  static const char SequenceError[];
  static const char ParamError[];
  static const char MemError[];
  static const char DataError[];
  static const char DataErrorMagic[];
  static const char IoError[];
  static const char UnexpectedEof[];
  static const char OutbuffFull[];
};


const char BzipUtils::ConfigError[] = "Library configuration error.";
const char BzipUtils::SequenceError[] = "Call sequence error.";
const char BzipUtils::ParamError[] = "Invalid arguments.";
const char BzipUtils::MemError[] = "Out of memory.";
const char BzipUtils::DataError[] = "Data integrity error.";
const char BzipUtils::DataErrorMagic[] = "BZip magic not found.";
const char BzipUtils::IoError[] = "Input/output error.";
const char BzipUtils::UnexpectedEof[] = "Unexpected end of file.";
const char BzipUtils::OutbuffFull[] = "Output buffer full.";


class BzipImpl : public EventEmitter {
  friend class ZipLib<BzipImpl>;
  typedef BzipUtils Utils;
  typedef BzipUtils::Blob Blob;

  typedef ZipLib<BzipImpl> BzipLib;

 public:
  static const char Name[];

 private:
  int BzipInit(int blockSize100k, int workFactor) {
    COND_RETURN(state_ != BzipLib::Idle, BZ_SEQUENCE_ERROR);

    /* allocate deflate state */
    stream_.bzalloc = NULL;
    stream_.bzfree = NULL;
    stream_.opaque = NULL;

    int ret = BZ2_bzCompressInit(&stream_, blockSize100k, 0, workFactor);
    if (ret == BZ_OK) {
      state_ = BzipLib::Data;
    }
    return ret;
  }


  int Write(char *data, int data_len, Blob &out) {
    COND_RETURN(state_ != BzipLib::Data, BZ_SEQUENCE_ERROR);

    BzipLib::Transition t(state_, BzipLib::Error);

    int ret = BZ_OK;
    while (data_len > 0) {    
      COND_RETURN(!out.GrowBy(data_len + 1), BZ_MEM_ERROR);
      
      stream_.next_in = data;
      stream_.next_out = out.data() + out.length();
      stream_.avail_in = data_len;
      stream_.avail_out = data_len + 1;

      ret = BZ2_bzCompress(&stream_, BZ_RUN);
      assert(ret != BZ_SEQUENCE_ERROR);  /* state not clobbered */
      COND_RETURN(ret != BZ_RUN_OK, ret);

      out.IncreaseLengthBy(data_len + 1 - stream_.avail_out);
      data += data_len - stream_.avail_in;
      data_len = stream_.avail_in;
    }
    t.alter(BzipLib::Data);
    return BZ_OK;
  }


  int Close(Blob &out) {
    COND_RETURN(state_ == BzipLib::Idle, BZ_OK);
    assert(state_ == BzipLib::Data || state_ == BzipLib::Error);

    BzipLib::Transition t(state_, BzipLib::Error);

    int ret = BZ_OK;
    if (state_ == BzipLib::Data) {
      ret = BzipEndWithData(out);
    }

    t.abort();
    this->Destroy();
    return ret;
  }


  void Destroy() {
    if (state_ != BzipLib::Idle) {
      state_ = BzipLib::Idle;
      BZ2_bzCompressEnd(&stream_);
    }
  }


  int BzipEndWithData(Blob &out) {
    // Don't expect data to be large as output buffer for deflate is as large
    // as input.
    const int Chunk = 128;

    int ret;
    do {
      COND_RETURN(!out.GrowBy(Chunk), BZ_MEM_ERROR);
      
      stream_.avail_out = Chunk;
      stream_.next_out = out.data() + out.length();

      ret = BZ2_bzCompress(&stream_, BZ_FINISH);
      assert(ret != BZ_SEQUENCE_ERROR);  /* state not clobbered */
      COND_RETURN(ret != BZ_FINISH_OK && ret != BZ_STREAM_END, ret);

      out.IncreaseLengthBy(Chunk - stream_.avail_out);
    } while (ret != BZ_STREAM_END);
    return BZ_OK;
  }


 protected:
  static Handle<Value> New(const Arguments& args)
  {
    HandleScope scope;

    BzipImpl *bzip = new BzipImpl();
    bzip->Wrap(args.This());

    int blockSize100k = 1;
    int workFactor = 0;

    int length = args.Length();
    if (length >= 1 && !args[0]->IsUndefined()) {
      if (!args[0]->IsInt32()) {
        Local<Value> exception = Exception::TypeError(
            String::New("blockSize must be an integer"));
        return ThrowException(exception);
      }
      blockSize100k = args[0]->Int32Value();
    }
    if (length >= 2 && !args[1]->IsUndefined()) {
      if (!args[1]->IsInt32()) {
        Local<Value> exception = Exception::TypeError(
            String::New("workFactor must be an integer"));
        return ThrowException(exception);
      }
      workFactor = args[1]->Int32Value();
    }
    int r = bzip->BzipInit(blockSize100k, workFactor);
    return BzipLib::ReturnThisOrThrow(args, r);
  }


  BzipImpl()
    : EventEmitter(), state_(BzipLib::Idle)
  {}


  ~BzipImpl()
  {
    this->Destroy();
  }


 private:
  bz_stream stream_;
  BzipLib::State state_;

};
const char BzipImpl::Name[] = "Bzip";
typedef ZipLib<BzipImpl> Bzip;


class BunzipImpl : public EventEmitter {
  friend class ZipLib<BunzipImpl>;
  typedef BzipUtils Utils;
  typedef BzipUtils::Blob Blob;

  typedef ZipLib<BunzipImpl> BzipLib;

 public:
  static const char Name[];

 public:
  int BunzipInit(int small) {
    COND_RETURN(state_ != BzipLib::Idle, BZ_SEQUENCE_ERROR);

    /* allocate inflate state */
    stream_.bzalloc = NULL;
    stream_.bzfree = NULL;
    stream_.opaque = NULL;
    stream_.avail_in = 0;
    stream_.next_in = NULL;

    int ret = BZ2_bzDecompressInit(&stream_, 0, small);
    if (ret == BZ_OK) {
      state_ = BzipLib::Data;
    }
    return ret;
  }


  int Write(const char *data, int data_len, Blob &out) {
    COND_RETURN(state_ == BzipLib::Eos, BZ_OK);
    COND_RETURN(state_ != BzipLib::Data, BZ_SEQUENCE_ERROR);

    int ret = BZ_OK;

    BzipLib::Transition t(state_, BzipLib::Error);
    while (data_len > 0) { 
      COND_RETURN(!out.GrowBy(data_len), BZ_MEM_ERROR);

      stream_.next_in = (char*)data;
      stream_.next_out = out.data() + out.length();
      stream_.avail_in = data_len;
      stream_.avail_out = data_len;

      ret = BZ2_bzDecompress(&stream_);
      assert(ret != BZ_SEQUENCE_ERROR);  /* state not clobbered */
      COND_RETURN(ret != BZ_OK && ret != BZ_STREAM_END, ret);

      out.IncreaseLengthBy(data_len - stream_.avail_out);
      data += data_len - stream_.avail_in;
      data_len = stream_.avail_in;

      if (ret == BZ_STREAM_END) {
        t.alter(BzipLib::Eos);
        return ret;
      }
    }
    t.alter(BzipLib::Data);
    return ret;
  }


  int Close(Blob &out) {
    return BZ_OK;
  }


  void Destroy() {
    if (state_ != BzipLib::Idle) {
      state_ = BzipLib::Idle;
      BZ2_bzDecompressEnd(&stream_);
    }
  }

 protected:
  static Handle<Value> New(const Arguments& args) {
    HandleScope scope;

    BunzipImpl *bunzip = new BunzipImpl();
    bunzip->Wrap(args.This());

    int small = 0;
    if (args.Length() > 0 && !args[0]->IsUndefined()) {
      small = args[0]->BooleanValue() ? 1 : 0;
    }
    int r = bunzip->BunzipInit(small);

    return BzipLib::ReturnThisOrThrow(args, r);
  }


  BunzipImpl()
    : EventEmitter(), state_(BzipLib::Idle)
  {}


  ~BunzipImpl()
  {
    this->Destroy();
  }


 private:
  bz_stream stream_;
  BzipLib::State state_;

};
const char BunzipImpl::Name[] = "Bunzip";
typedef ZipLib<BunzipImpl> Bunzip;

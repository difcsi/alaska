#pragma once

#include "./miniz.h"

#include <zstd.h>

namespace alaska {

  // This class is used to compress objects in memory using zlib (miniz,
  // really). It is meant to be reused, and operates
  class ObjectCompressor {
   public:
    inline ObjectCompressor() {
      // initialize the stream for usage later.
      memset(&stream, 0, sizeof(stream));

      int window_bits = 15;
      int mem_level = 1;

      if (mz_deflateInit2(&stream, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED, window_bits, mem_level,
                          MZ_DEFAULT_STRATEGY) != MZ_OK) {
        fprintf(stderr, "Unable to initialize deflate stream\n");
        abort();
      }


      // zstd
      cctx = ZSTD_createCCtx();
      ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 9);
    }
    ~ObjectCompressor() { mz_deflateEnd(&stream); }

    // the interface for this is a little strange.
    // It operates with a callback function that is called with the compressed data, allocated from
    // the stack.
    template <typename Fn>
    inline void compress(const void *data, size_t size, Fn fn) {
      // mz_ulong dest_len = mz_compressBound(size);
      // void *buf = alloca(dest_len);

      // stream.next_in = (const unsigned char *)data;
      // stream.avail_in = (unsigned int)size;
      // stream.next_out = (unsigned char *)buf;
      // stream.avail_out = (unsigned int)dest_len;

      // mz_deflateReset(&stream);

      // if (mz_deflate(&stream, MZ_FINISH) != MZ_STREAM_END) {
      //   fprintf(stderr, "Deflate failed\n");
      //   abort();
      // }
      // size_t compressed_size = stream.total_out;


      size_t const cbuf_size = ZSTD_compressBound(size);
      void *buf = alloca(cbuf_size);

      size_t compressed_size = ZSTD_compress2(cctx, buf, cbuf_size, data, size);

      fn(buf, compressed_size);
    }

   private:
    ZSTD_CCtx *cctx;  // Compression Context

    mz_stream stream;
  };
}  // namespace alaska

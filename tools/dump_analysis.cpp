#include <iostream>
#include <omp.h>
#include <fstream>
#include <algorithm>
#include <random>
#include <vector>
#include <map>
#include <cstdint>
#include <string>
#include <zlib.h>
#include <string.h>
#include <set>
#include <lz4.h>
#include <math.h>
#if __has_include(<zstd.h>)
#include <zstd.h>
#define HAS_ZSTD 1
#endif
#include <functional>
#include <queue>
/**
 * @brief Represents a parsed object from the dump.
 */
struct Object {
  uint64_t hid;
  std::vector<uint8_t> data;
};

using Dump = std::map<uint64_t, std::vector<uint8_t>>;

/**
 * @brief Loads the dump file into a map of HID -> Object Data.
 *
 * @param filename The path to the dump file.
 * @return Dump The parsed objects.
 */
Dump load_dump(const std::string& filename,
               std::function<bool(uint64_t size, uint8_t* data)> filter) {
  Dump objects;
  std::ifstream file(filename, std::ios::binary);

  if (!file.is_open()) {
    std::cerr << "Error: File '" << filename << "' not found." << std::endl;
    return objects;
  }

  while (file.peek() != EOF) {
    uint64_t hid;
    uint64_t size;

    // Read HID (8 bytes)
    file.read(reinterpret_cast<char*>(&hid), sizeof(hid));

    // Read Size (8 bytes)
    file.read(reinterpret_cast<char*>(&size), sizeof(size));

    // Read Data (size bytes)
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);

    if (filter(size, data.data())) {
      objects[hid] = std::move(data);
    }
  }

  return objects;
}


class BlockArena {
 public:
  struct Block {
    Block* next;
    size_t used;
    size_t size;
    char data[0];

    void* allocate(size_t size) {
      if (used + size > this->size) {
        return nullptr;
      }
      void* ptr = &data[used];
      used += size;
      return ptr;
    }
    void clear() { used = 0; }
  };

  static const size_t DEFAULT_ARENA_SIZE = 4096;
  BlockArena(size_t arena_size = DEFAULT_ARENA_SIZE)
      : m_arena_size(arena_size) {
    new_block(m_arena_size);
  }

  ~BlockArena() {
    Block* block = m_current_block;
    while (block) {
      Block* next = block->next;
      free(block);
      block = next;
    }
  }


  void* push(size_t size, bool zero = false) {
    void* ptr = m_current_block->allocate(size);
    if (ptr == nullptr) {
      new_block(std::max(m_arena_size, size));
      ptr = m_current_block->allocate(size);
    }
    return ptr;
  }

  void clear(void) {
    Block* block = m_current_block;
    while (block) {
      block->clear();
      block = block->next;
    }
  }

  template <typename Fn>
  void eachBlock(Fn fn) {
    Block* block = m_current_block;
    while (block) {
      fn(block);
      block = block->next;
    }
  }

 private:
  Block* new_block(size_t required_size) {
    Block* block = (Block*)malloc(sizeof(Block) + required_size);
    block->next = m_current_block;
    block->used = 0;
    block->size = required_size;
    m_current_block = block;
    return block;
  }

  Block* m_current_block = nullptr;
  size_t m_arena_size = 0;
};

static uint64_t timestamp() {
  struct timespec spec;
  clock_gettime(1, &spec);
  return spec.tv_sec * (1000 * 1000 * 1000) + spec.tv_nsec;
}



using CompressorFn = size_t(void* data, size_t size);


// Returns entropy in bits per byte (0.0 - 8.0)
// 8.0 = perfectly random/incompressible
// 0.0 = all identical bytes
float entropy(const uint8_t* data, size_t size) {
  if (size == 0) return 0.0f;

  size_t counts[256] = {0};

  for (size_t i = 0; i < size; i++) {
    counts[data[i]]++;
  }

  float entropy = 0.0f;
  for (int i = 0; i < 256; i++) {
    if (counts[i] == 0) continue;
    float p = (float)counts[i] / size;
    entropy -= p * log2(p);
  }

  return entropy;
}

// The compressors namespace contains various compression functions, each of
// which returns the compressed size of the data. We are not concerned with
// the actual compressed data.
namespace compressors {
  size_t zlib(void* data, size_t size) {
    uLongf dest_len = compressBound(size);
    uint8_t* dest = (uint8_t*)alloca(dest_len);
    if (compress(dest, &dest_len, (const unsigned char*)data, size) == Z_OK) {
      return dest_len;
    }
    return size;
  }

  size_t lz4(void* data, size_t size) {
    size_t csize = LZ4_compressBound(size);
    uint8_t* dest = (uint8_t*)alloca(csize);
    int ret = LZ4_compress_default((const char*)data, (char*)dest, size, csize);
    if (ret > 0) {
      return ret;
    }
    return size;
  }
  size_t lz4_fast(void* data, size_t size) {
    size_t csize = LZ4_compressBound(size);
    uint8_t* dest = (uint8_t*)alloca(csize);
    int ret = LZ4_compress_fast((const char*)data, (char*)dest, size, csize, 4);
    if (ret > 0) {
      return ret;
    }
    return size;
  }



  size_t zero_removal(void* data, size_t size) {
    size_t csize = 0;
    csize += sizeof(uint16_t);  // store the size of the data
    // there should be a bit for each byte, set to 1 if the byte is non-zero
    csize += (size + 7) / 8;

    for (size_t i = 0; i < size; i++) {
      if (((uint8_t*)data)[i] != 0) {
        csize += sizeof(uint8_t);
      }
    }
    return csize;
  }


  // Zero removal, followed by lz4 compression of the nonzero values.
  size_t zero_removal_lz4(void* data, size_t size) {
    size_t nonzeroes = 0;

    for (size_t i = 0; i < size; i++) {
      if (((uint8_t*)data)[i] != 0) {
        nonzeroes++;
      }
    }


    size_t csize = 0;
    csize += sizeof(uint16_t);  // store the size of the data
    csize += (size + 7) / 8;

    uint8_t* nz = (uint8_t*)alloca(nonzeroes);
    size_t nz_idx = 0;
    for (size_t i = 0; i < size; i++) {
      if (((uint8_t*)data)[i] != 0) {
        nz[nz_idx++] = ((uint8_t*)data)[i];
      }
    }

    csize += lz4(nz, nonzeroes);

    return csize;
  }


  size_t adaptive(void* data, size_t size) {
    size_t zero_bytes = 0;
    size_t ascii_bytes = 0;

    for (size_t i = 0; i < size; i++) {
      if (((uint8_t*)data)[i] == 0) {
        zero_bytes++;
      }
      if (((uint8_t*)data)[i] >= 32 && ((uint8_t*)data)[i] <= 126) {
        ascii_bytes++;
      }
    }

    if (zero_bytes > ascii_bytes) {
      return zero_removal(data, size);
    }
    return lz4(data, size);
  }

  size_t rle(void* data, size_t size) {
    if (size == 0) return 0;
    size_t csize = 0;
    uint8_t* ptr = (uint8_t*)data;

    for (size_t i = 0; i < size;) {
      uint8_t val = ptr[i];
      uint8_t run = 1;
      while (i + run < size && ptr[i + run] == val && run < 255) {
        run++;
      }
      csize += 2;  // 1 byte for value, 1 byte for length
      i += run;
    }
    return csize;
  }

  size_t xor_delta_lz4(void* data, size_t size) {
    if (size <= 1) return lz4(data, size);

    uint8_t* tmp = (uint8_t*)alloca(size);
    uint8_t* src = (uint8_t*)data;

    tmp[0] = src[0];
    for (size_t i = 1; i < size; i++) {
      tmp[i] = src[i] ^ src[i - 1];
    }

    return lz4(tmp, size);
  }

  size_t stride_delta_lz4(void* data, size_t size) {
    if (size <= 8) return lz4(data, size);

    uint8_t* tmp = (uint8_t*)alloca(size);
    uint8_t* src = (uint8_t*)data;

    memcpy(tmp, src, 8);
    for (size_t i = 8; i < size; i++) {
      tmp[i] = src[i] ^ src[i - 8];
    }

    return lz4(tmp, size);
  }

  size_t huffman(void* data, size_t size) {
    if (size == 0) return 0;

    size_t counts[256] = {0};
    for (size_t i = 0; i < size; i++) {
      counts[((uint8_t*)data)[i]]++;
    }

    struct Node {
      size_t count;
      Node *left, *right;
    };

    auto cmp = [](Node* left, Node* right) {
      return left->count > right->count;
    };
    std::priority_queue<Node*, std::vector<Node*>, decltype(cmp)> pq(cmp);

    for (int i = 0; i < 256; i++) {
      if (counts[i] > 0) {
        pq.push(new Node{counts[i], nullptr, nullptr});
      }
    }

    if (pq.empty()) return 0;
    if (pq.size() == 1) {
      size_t bits = pq.top()->count;
      delete pq.top();
      return (bits + 7) / 8 + 32;
    }

    while (pq.size() > 1) {
      Node* left = pq.top();
      pq.pop();
      Node* right = pq.top();
      pq.pop();
      pq.push(new Node{left->count + right->count, left, right});
    }

    Node* root = pq.top();
    size_t total_bits = 0;

    std::function<void(Node*, int)> walk = [&](Node* n, int depth) {
      if (!n->left && !n->right) {
        total_bits += n->count * depth;
        return;
      }
      if (n->left) walk(n->left, depth + 1);
      if (n->right) walk(n->right, depth + 1);
    };

    walk(root, 0);

    // Cleanup tree
    std::function<void(Node*)> cleanup = [&](Node* n) {
      if (n->left) cleanup(n->left);
      if (n->right) cleanup(n->right);
      delete n;
    };
    cleanup(root);

    return (total_bits + 7) / 8 + 32;  // +32 bytes for header/table
  }


  size_t zero_removal_huffman(void* data, size_t size) {
    size_t nonzeroes = 0;

    for (size_t i = 0; i < size; i++) {
      if (((uint8_t*)data)[i] != 0) {
        nonzeroes++;
      }
    }


    size_t csize = 0;
    csize += sizeof(uint16_t);  // store the size of the data
    csize += (size + 7) / 8;

    uint8_t* nz = (uint8_t*)alloca(nonzeroes);
    size_t nz_idx = 0;
    for (size_t i = 0; i < size; i++) {
      if (((uint8_t*)data)[i] != 0) {
        nz[nz_idx++] = ((uint8_t*)data)[i];
      }
    }

    csize += huffman(nz, nonzeroes);
    return csize;
  }

#ifdef HAS_ZSTD
  size_t zstd(void* data, size_t size) {
    if (size == 0) return 0;
    size_t const cBound = ZSTD_compressBound(size);
    void* const cAddr = alloca(cBound);
    size_t const cSize = ZSTD_compress(cAddr, cBound, data, size, 3);
    if (ZSTD_isError(cSize)) return size;
    return cSize;
  }

  size_t zstd_fast(void* data, size_t size) {
    if (size == 0) return 0;
    size_t const cBound = ZSTD_compressBound(size);
    void* const cAddr = alloca(cBound);
    size_t const cSize = ZSTD_compress(cAddr, cBound, data, size, 1);
    if (ZSTD_isError(cSize)) return size;
    return cSize;
  }


  size_t zstd_slow(void* data, size_t size) {
    if (size == 0) return 0;
    size_t const cBound = ZSTD_compressBound(size);
    void* const cAddr = alloca(cBound);
    size_t const cSize = ZSTD_compress(cAddr, cBound, data, size, 19);
    if (ZSTD_isError(cSize)) return size;
    return cSize;
  }
#endif
}  // namespace compressors




float compress_naive(Dump& dump, CompressorFn compressor) {
  size_t total_bytes = 0;
  size_t compressed_bytes = 0;

  for (const auto& pair : dump) {
    total_bytes += pair.second.size();
    compressed_bytes += compressor((void*)pair.second.data(), pair.second.size());
  }

  return total_bytes > 0 ? static_cast<float>(compressed_bytes) / total_bytes : 0.0f;
}


float compress_chunked(Dump& dump, size_t chunk_size, CompressorFn compressor) {
  BlockArena arena(chunk_size);

  size_t total_bytes = 0;
  size_t compressed_bytes = 0;

  for (const auto& [hid, data] : dump) {
    total_bytes += data.size();
    void* blockDst = arena.push(data.size());
    memcpy(blockDst, data.data(), data.size());
  }

  arena.eachBlock([&](auto* block) {
    compressed_bytes += compressor(block->data, block->used);
  });

  return total_bytes > 0 ? static_cast<float>(compressed_bytes) / total_bytes : 0.0f;
}


float size_compress_chunked(Dump& dump, size_t chunk_size, bool shuffle, CompressorFn compressor) {
  static std::random_device rd;   // non-deterministic seed (if available)
  static std::mt19937 gen(rd());  // Mersenne Twister engine
  BlockArena arena(chunk_size);

  size_t total_bytes = 0;

  std::map<uint64_t, std::set<uint64_t>> sizes;
  for (auto [hid, data] : dump) {
    sizes[data.size()].insert(hid);
    total_bytes += data.size();
  }

  uint64_t hash = 0;

  for (auto [size, hids] : sizes) {
    std::vector<uint64_t> hids_vec(hids.begin(), hids.end());
    if (shuffle) {
      std::shuffle(hids_vec.begin(), hids_vec.end(), gen);
    }

    for (auto hid : hids_vec) {
      auto& data = dump[hid];
      void* blockDst = arena.push(data.size());
      memcpy(blockDst, data.data(), data.size());
    }
  }

  size_t compressed_bytes = 0;
  arena.eachBlock([&](auto* block) {
    compressed_bytes += compressor(block->data, block->used);
  });

  return compressed_bytes > 0 ? static_cast<float>(compressed_bytes) / total_bytes : 0.0f;
}


float compress_transposed(Dump& dump, size_t chunk_size, size_t stride, CompressorFn compressor) {
  size_t total_bytes = 0;
  size_t compressed_bytes = 0;

  if (stride == 0) stride = 1;

  std::map<uint64_t, std::vector<uint64_t>> sizes;
  for (auto const& [hid, data] : dump) {
    sizes[data.size()].push_back(hid);
    total_bytes += data.size();
  }

  for (auto const& [obj_size_bound, hids_bound] : sizes) {
    if (obj_size_bound == 0) continue;
    size_t obj_size = obj_size_bound;
    const std::vector<uint64_t>& hids = hids_bound;

    size_t num_in_batch = std::max((size_t)1, chunk_size / obj_size);
    std::vector<uint8_t> tmp_buf(num_in_batch * obj_size);

    for (size_t i = 0; i < hids.size(); i += num_in_batch) {
      size_t batch_count = std::min(num_in_batch, hids.size() - i);
      size_t batch_size = batch_count * obj_size;

      uint8_t* tmp = tmp_buf.data();

      // Transpose with configurable stride
      for (size_t s = 0; s < obj_size; s += stride) {
        size_t current_W = std::min(stride, obj_size - s);
        for (size_t n = 0; n < batch_count; n++) {
          memcpy(&tmp[s * batch_count + n * current_W], &dump.at(hids[i + n])[s], current_W);
        }
      }

      compressed_bytes += compressor(tmp, batch_size);
    }
  }

  return total_bytes > 0 ? (float)compressed_bytes / total_bytes : 0.0f;
}


float compress_centroid(Dump& dump, size_t chunk_size, CompressorFn compressor) {
  size_t total_bytes = 0;
  size_t compressed_bytes = 0;

  std::map<uint64_t, std::vector<uint64_t>> sizes;
  for (auto const& [hid, data] : dump) {
    sizes[data.size()].push_back(hid);
    total_bytes += data.size();
  }

  for (auto const& [obj_size_bound, hids_bound] : sizes) {
    if (obj_size_bound == 0) continue;
    size_t obj_size = obj_size_bound;
    const std::vector<uint64_t>& hids = hids_bound;

    size_t num_in_batch = std::max((size_t)1, chunk_size / obj_size);

    for (size_t i = 0; i < hids.size(); i += num_in_batch) {
      size_t batch_count = std::min(num_in_batch, hids.size() - i);
      size_t batch_size = batch_count * obj_size;

      std::vector<uint8_t> centroid(obj_size);
      for (size_t s = 0; s < obj_size; s++) {
        uint32_t freq[256] = {0};
        for (size_t n = 0; n < batch_count; n++) {
          freq[dump.at(hids[i + n])[s]]++;
        }
        uint8_t best_byte = 0;
        uint32_t max_freq = 0;
        for (int b = 0; b < 256; b++) {
          if (freq[b] > max_freq) {
            max_freq = freq[b];
            best_byte = b;
          }
        }
        centroid[s] = best_byte;
      }

      std::vector<uint8_t> tmp_buf;
      tmp_buf.reserve(obj_size + batch_size);
      tmp_buf.insert(tmp_buf.end(), centroid.begin(), centroid.end());

      for (size_t n = 0; n < batch_count; n++) {
        const auto& obj = dump.at(hids[i + n]);
        for (size_t s = 0; s < obj_size; s++) {
          tmp_buf.push_back(obj[s] ^ centroid[s]);
        }
      }

      compressed_bytes += compressor(tmp_buf.data(), tmp_buf.size());
    }
  }

  return total_bytes > 0 ? (float)compressed_bytes / total_bytes : 0.0f;
}


float compress_centroid_transposed(Dump& dump, size_t chunk_size, size_t stride,
                                   CompressorFn compressor) {
  size_t total_bytes = 0;
  size_t compressed_bytes = 0;

  if (stride == 0) stride = 1;

  std::map<uint64_t, std::vector<uint64_t>> sizes;
  for (auto const& [hid, data] : dump) {
    sizes[data.size()].push_back(hid);
    total_bytes += data.size();
  }

  for (auto const& [obj_size_bound, hids_bound] : sizes) {
    if (obj_size_bound == 0) continue;
    size_t obj_size = obj_size_bound;
    const std::vector<uint64_t>& hids = hids_bound;

    size_t num_in_batch = std::max((size_t)1, chunk_size / obj_size);

    for (size_t i = 0; i < hids.size(); i += num_in_batch) {
      size_t batch_count = std::min(num_in_batch, hids.size() - i);
      size_t batch_size = batch_count * obj_size;

      // 1. Calculate centroid
      std::vector<uint8_t> centroid(obj_size);
      for (size_t s = 0; s < obj_size; s++) {
        uint32_t freq[256] = {0};
        for (size_t n = 0; n < batch_count; n++) {
          freq[dump.at(hids[i + n])[s]]++;
        }
        uint8_t best_byte = 0;
        uint32_t max_freq = 0;
        for (int b = 0; b < 256; b++) {
          if (freq[b] > max_freq) {
            max_freq = freq[b];
            best_byte = (uint8_t)b;
          }
        }
        centroid[s] = best_byte;
      }

      // 2. Transpose deltas
      std::vector<uint8_t> tmp_buf(obj_size + batch_size);
      memcpy(tmp_buf.data(), centroid.data(), obj_size);
      uint8_t* transposed_deltas = tmp_buf.data() + obj_size;

      for (size_t s = 0; s < obj_size; s += stride) {
        size_t current_W = std::min(stride, obj_size - s);
        for (size_t n = 0; n < batch_count; n++) {
          const auto& obj = dump.at(hids[i + n]);
          for (size_t w = 0; w < current_W; w++) {
            transposed_deltas[(s + w) * batch_count + n * current_W + w] =
                obj[s + w] ^ centroid[s + w];
          }
        }
      }

      compressed_bytes += compressor(tmp_buf.data(), tmp_buf.size());
    }
  }

  return total_bytes > 0 ? (float)compressed_bytes / total_bytes : 0.0f;
}


static size_t rowIndex = 0;

void print_bar(float ratio, int width = 64) {
  int barWidth = std::min((float)width, ratio * width);
  int overWidth = std::max(0.0f, (ratio - 1.0f) * width);
  int leftover = std::max(0, width - barWidth);

  if (rowIndex % 2 == 1) {
    printf("\e[48;2;180;180;180m");
  } else {
    printf("\e[48;2;255;255;255m");
  }
  for (int i = 0; i < barWidth; i++) {
    printf(" ");
  }

  if (overWidth > 0) {
    printf("\e[48;2;255;50;50m");
    for (int i = 0; i < overWidth; i++) {
      printf(" ");
    }
  }

  printf("\e[48;2;50;50;50m\e[38;2;51;51;51m");
  for (int i = 0; i < leftover; i++) {
    if ((barWidth + i) % 2 == 1) {
      printf("·");
    } else {
      printf(" ");
    }
  }

  printf("\e[0m");
}

void print_ratio(const char* prefix, float ratio, int width = 64) {
  int barWidth = ratio * width;
  int leftover = width - barWidth;

  print_bar(ratio);
  printf(" %10.7f %s\n", ratio, prefix);
}

struct Result {
  std::string compressor_name;
  std::string run_name;
  float ratio;
};

std::vector<Result> allResults;

void test_compressor(Dump& dump, size_t total_size, const char* name, CompressorFn compressor) {
  struct TestWork {
    std::string label;
    std::function<float()> run;
    float ratio = 0.0f;
  };

  std::vector<TestWork> work;
  size_t chunk_min = 256;
  size_t chunk_max = 4096 * 8;

  for (size_t chunk_size = chunk_min; chunk_size <= chunk_max; chunk_size *= 2) {
    char buf[128];
    snprintf(buf, sizeof(buf), "chunk (%zu)", chunk_size);
    work.push_back({buf, [&dump, chunk_size, compressor]() {
                      return compress_chunked(dump, chunk_size, compressor);
                    }});
  }
  for (size_t chunk_size = chunk_min; chunk_size <= chunk_max; chunk_size *= 2) {
    char buf[128];
    snprintf(buf, sizeof(buf), "size seg chunk (%zu)", chunk_size);
    work.push_back({buf, [&dump, chunk_size, compressor]() {
                      return size_compress_chunked(dump, chunk_size, true, compressor);
                    }});
  }
  for (size_t stride = 1; stride <= 8; stride *= 2) {
    char buf[128];
    snprintf(buf, sizeof(buf), "transposed (%zu) chunk (4096)", stride);
    work.push_back({buf, [&dump, stride, compressor]() {
                      return compress_transposed(dump, 4096, stride, compressor);
                    }});
  }
  for (size_t chunk_size = chunk_min; chunk_size <= chunk_max; chunk_size *= 2) {
    char buf[128];
    snprintf(buf, sizeof(buf), "centroid delta chunk (%zu)", chunk_size);
    work.push_back({buf, [&dump, chunk_size, compressor]() {
                      return compress_centroid(dump, chunk_size, compressor);
                    }});
  }

  for (size_t chunk_size = chunk_min; chunk_size <= chunk_max; chunk_size *= 2) {
    char buf[128];
    snprintf(buf, sizeof(buf), "centroid + transposed (1) chunk (%zu)", chunk_size);
    work.push_back({buf, [&dump, chunk_size, compressor]() {
                      return compress_centroid_transposed(dump, chunk_size, 1, compressor);
                    }});
  }

  work.push_back({"naive", [&dump, compressor]() {
                    return compress_naive(dump, compressor);
                  }});

#pragma omp parallel for schedule(dynamic)
  for (size_t i = 0; i < work.size(); i++) {
    work[i].ratio = work[i].run();
  }

  printf("-- %s --\n", name);

  for (auto& w : work) {
    print_ratio(w.label.c_str(), w.ratio);
    allResults.push_back({name, w.label, w.ratio});
  }
  printf("\n");
}

bool is_compressible(const uint8_t* data, size_t len) {
  uint8_t seen[256] = {0};  // or use __uint256_t bitmap
  size_t unique = 0;

  for (size_t i = 0; i < len; i++) {
    if (!seen[data[i]]) {
      seen[data[i]] = 1;
      unique++;
    }
  }

  // Threshold tuning: <60% unique often compresses well
  return unique < (len * 3) / 5;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <dump_file>" << std::endl;
    return 1;
  }

  std::string filename = argv[1];
  auto objects = load_dump(filename, [](uint64_t size, uint8_t* data) {
    return true;
    return is_compressible(data, size);
  });

  std::cout << "Loaded " << objects.size() << " objects from " << filename << std::endl;

  size_t total_bytes = 0;
  size_t zero_bytes = 0;
  size_t ascii_bytes = 0;
  for (const auto& pair : objects) {
    total_bytes += pair.second.size();
    // count the number of zero bytes
    for (const auto& byte : pair.second) {
      if (byte == 0) {
        zero_bytes++;
      }
      if (byte >= 32 && byte <= 126) {
        ascii_bytes++;
      }
    }
  }


  printf("Total size: %zu bytes\n", total_bytes);
  printf("Zero bytes: %zu bytes (%f%%)\n", zero_bytes,
         static_cast<float>(zero_bytes) / total_bytes * 100.0);
  printf("Ascii bytes: %zu bytes (%f%%)\n", ascii_bytes,
         static_cast<float>(ascii_bytes) / total_bytes * 100.0);


  size_t maxEntropy = 8;
  size_t binSize = 10;
  size_t bins[maxEntropy * binSize];
  memset(bins, 0, sizeof(bins));

  for (auto& [hid, data] : objects) {
    float e = entropy(data.data(), data.size());
    size_t bin = e * binSize;
    if (bin >= maxEntropy * binSize) {
      bin = maxEntropy * binSize - 1;
    }
    bins[bin]++;
  }
  printf("Entropy distribution:\n");
  for (size_t i = 0; i < maxEntropy * binSize; i++) {
    printf("%f %8zu ", i / (float)binSize, bins[i]);
    float ratio = bins[i] / (float)objects.size();
    print_bar(sqrt(ratio));
    printf("\n");
  }


  // for (auto& [hid, data] : objects) {
  //   printf("%lx: ", hid);
  //   for (auto byte : data) {
  //     printf("%02x", byte);
  //   }
  //   printf("\n");
  // }

  // printf("[zlib] naive compression ratio: %f\n", compress_naive(objects, compressors::zlib));
  size_t chunkSize = 4096;

#ifdef HAS_ZSTD
  test_compressor(objects, total_bytes, "zstd", compressors::zstd);
  test_compressor(objects, total_bytes, "zstd fast", compressors::zstd_fast);
  test_compressor(objects, total_bytes, "zstd slow", compressors::zstd_slow);
  return 0;
#endif

  test_compressor(objects, total_bytes, "zlib", compressors::zlib);
  test_compressor(objects, total_bytes, "lz4", compressors::lz4);
  test_compressor(objects, total_bytes, "lz4 fast", compressors::lz4_fast);


  test_compressor(objects, total_bytes, "adaptive", compressors::adaptive);
  test_compressor(objects, total_bytes, "zero removal", compressors::zero_removal);
  test_compressor(objects, total_bytes, "zero removal w/ lz4", compressors::zero_removal_lz4);
  test_compressor(objects, total_bytes, "zero removal huffman", compressors::zero_removal_huffman);
  test_compressor(objects, total_bytes, "rle", compressors::rle);
  test_compressor(objects, total_bytes, "xor delta lz4", compressors::xor_delta_lz4);
  test_compressor(objects, total_bytes, "stride delta lz4", compressors::stride_delta_lz4);
  test_compressor(objects, total_bytes, "huffman", compressors::huffman);


  // dump all results as CSV
  // printf("compressor,run_name,ratio\n");
  // for (auto& result : allResults) {
  //   printf("%s,%s,%f\n", result.compressor_name.c_str(), result.run_name.c_str(),
  //   result.ratio);
  // }

  return 0;
}

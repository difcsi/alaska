#include <iostream>
#include <vector>
#include <list>

#include <stdint.h>

static inline uint64_t read_cycle_counter() {
#if defined(__x86_64__) || defined(__i386__)
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
#elif defined(__riscv)
  uint64_t cycles;
  __asm__ volatile("rdcycle %0" : "=r"(cycles));
  return cycles;
#else
#error "Unsupported architecture"
#endif
}

class ChainedHashTable {
 private:
  struct Entry {
    int key;
    int value;
    Entry(int k, int v)
        : key(k)
        , value(v) {}
  };

  std::vector<std::list<Entry>> table;
  size_t bucket_count;

 public:
  ChainedHashTable(size_t buckets)
      : bucket_count(buckets)
      , table(buckets) {}

  void insert(int hashed_key, int value) {
    size_t index = hashed_key % bucket_count;
    for (auto& entry : table[index]) {
      if (entry.key == hashed_key) {
        entry.value = value;  // Update existing key
        return;
      }
    }
    table[index].emplace_back(hashed_key, value);
  }

  bool lookup(int hashed_key, int& out_value) {
    size_t index = hashed_key % bucket_count;
    for (const auto& entry : table[index]) {
      if (entry.key == hashed_key) {
        out_value = entry.value;
        return true;
      }
    }
    return false;
  }

  void print_table() {
    for (size_t i = 0; i < bucket_count; ++i) {
      std::cout << "Bucket " << i << ": ";
      for (const auto& entry : table[i]) {
        std::cout << "(" << entry.key << " -> " << entry.value << ") ";
      }
      std::cout << "\n";
    }
  }
};

int main() {
  int buckets = 512;
  ChainedHashTable hash_table(buckets);

  printf("inserting... ");
  fflush(stdout);
  for (int i = 0; i < 200000; i++) {
    hash_table.insert(i, i);
  }
  printf("DONE\n");

  for (int trial = 0; trial < 10; trial++) {
    auto start = read_cycle_counter();
    for (int i = 0; i < 200000; i++) {
      int out;
      hash_table.lookup(i, out);
    }
    auto end = read_cycle_counter();
    printf("%d, %zu\n", buckets, end - start);
  }
  // hash_table.print_table();
  return 0;
}

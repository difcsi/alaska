#include <filesystem>
#include <new>
#include <alaska/Runtime.hpp>
#include <alaska/ThreadCache.hpp>
#include <alaska/sim/HTLB.hpp>
#include <stdlib.h>
#include <stdio.h>
#include <unordered_map>
#include <vector>
#include "alaska.h"
#include "alaska/ObjectHeader.hpp"
#include "alaska/RateCounter.hpp"
#include "alaska/alaska.hpp"
#include "alaska/utils.h"


enum class Event : uint8_t {
  ALLOC,
  FREE,
  ACCESS,
};

struct TraceEvent {
  uint64_t cycles : 56;  // cycle count at this event
  Event type : 8;
  uint64_t handle_id;
  // misc data. In ALLOC, this is the size. in ACCESS, this is the offset.
  uint32_t misc;
};


static void ensure_traceb(std::string path) {
  std::string binary_path = path + ".binary";
  // it already exists!
  if (std::filesystem::exists(binary_path)) {
    return;
  }

  // otherwise, we gotta make it.
  std::vector<TraceEvent> events;
  char command[256];
  char line[256];
  unsigned long start, end;

  start = alaska_timestamp();
  FILE *f = fopen(path.c_str(), "r");

  // now we need to process the events to determine the size of each allocation
  struct AllocationInfo {
    // maximum access offset in bytes.
    uint64_t rewritten_hid;
    uint32_t size;
    // the index in the events array where the allocation was made.
    uint64_t index;
  };
  uint64_t max_hid = 0;
  uint64_t next_hid = 1;
  std::unordered_map<uint32_t, AllocationInfo> allocs;




  auto file_size = std::filesystem::file_size(path);
  int line_num = 0;
  while (fgets(line, sizeof(line), f)) {
    line_num++;
    if (line[0] == '#') continue;  // skip comments

    if (line_num % 100000 == 0) {
      float byte_progress = (float)ftell(f) / (float)file_size;
      printf("processed %d lines (%f%%)\n", line_num, byte_progress * 100.0f);
    }


    unsigned long cycles;
    sscanf(line, "  %lu %s\n", &cycles, command);

    TraceEvent event;
    event.cycles = cycles;

    switch (command[0]) {
      case 'a': {
        unsigned long hid;
        sscanf(command, "a%lx", &hid);

        AllocationInfo info;
        info.rewritten_hid = next_hid++;
        info.size = 8;  // minimum size of 8 bytes for an object.
        info.index = events.size();
        allocs[hid] = info;

        // printf("%016lx alloc %zx\n", cycles, hid);

        event.type = Event::ALLOC;
        event.handle_id = info.rewritten_hid;
        event.misc = 0;  // we don't know the size yet!
        break;
      }

      case 'h': {
        unsigned long addr;
        sscanf(command, "h%lx", &addr);
        auto hid = (addr & ~(1UL << 63)) >> ALASKA_SIZE_BITS;
        int32_t offset = addr & ((1LU << ALASKA_SIZE_BITS) - 1);

        // validate the offset is valid (handles aren't bigger than 4k
        // on a yukon core.
        if (offset < 0 || offset > 0xffff) {
          continue;
        }

        auto it = allocs.find(hid);
        // if there isn't an alloc for this hid, ignore the access
        if (it == allocs.end()) continue;
        auto &info = it->second;

        event.type = Event::ACCESS;
        event.handle_id = info.rewritten_hid;
        // the size required to access this offset with a 8 byte load
        uint32_t needed_size = round_up(offset + 8, 8);
        event.misc = offset;
        info.size = std::max(info.size, needed_size);
        break;
      }

      case 'f': {
        unsigned long hid;
        sscanf(command, "f%lx", &hid);
        auto it = allocs.find(hid);
        // printf("%016lx free %zx\n", cycles, hid);
        // if there isn't an alloc for this hid, ignore the access
        ALASKA_ASSERT(it != allocs.end(), "All frees must have an alloc");
        auto &info = it->second;

        event.type = Event::FREE;
        event.handle_id = info.rewritten_hid;
        event.misc = 0;
        // rewrite the size in the allocation event
        auto &alloc_event = events[info.index];
        alloc_event.misc = info.size;
        // remove the alloc from the map
        allocs.erase(hid);
        break;
      }
      default: {
        printf("unknown command %s\n", command);
        exit(-1);
        break;
      }
    }
    events.push_back(event);
  }

  // now go through the remaining allocs in the set that were not freed
  for (auto &pair : allocs) {
    auto &info = pair.second;
    auto &event = events[info.index];
    event.misc = info.size;
  }

  fclose(f);
  end = alaska_timestamp();

  printf("read %zu events in %lums\n", events.size(), (end - start) / 1000 / 1000);


  FILE *fout = fopen(binary_path.c_str(), "w");
  for (auto &event : events) {
    fwrite(&event, sizeof(TraceEvent), 1, fout);
  }
  fclose(fout);
  return;
}



class TraceRunner {
 protected:
  alaska::Runtime rt;
  alaska::ThreadCache *tc;
  // a mapping from logical handle id to the mapping object in the simulated runtime.
  std::unordered_map<uint64_t, void *> mappings;
  alaska::RateCounter event_counter;
  alaska::RateCounter cycles_handled;
  alaska::RateCounter allocations;
  alaska::RateCounter accesses;

  bool has_timeout = false;
  int64_t timeout_cycles = 0;

  float time_seconds = 0;

 public:
  TraceRunner() { tc = rt.new_threadcache(); }
  virtual ~TraceRunner() = default;

  // called after allocation/free
  virtual void on_alloc(uint64_t cycle, alaska::Mapping *m) {}
  virtual void on_free(uint64_t cycle, alaska::Mapping *m) {}
  // called when an access is made.
  virtual void on_access(uint64_t cycle, alaska::Mapping *m, uint32_t offset) {}
  virtual void on_timer(uint64_t cycle) {}

  // set a timer for the next event in microseconds (assuming 1ghz sim like in firesim)
  void set_timer(uint64_t microseconds) {
    has_timeout = true;
    timeout_cycles = microseconds * 1000;
  }



  void process_event(TraceEvent &event) {
    switch (event.type) {
      case Event::ALLOC: {
        allocations++;
        size_t size = event.misc;
        if (size < 16) size = 16;
        auto p = tc->halloc(size);
        mappings[event.handle_id] = p;
        if (auto *m = alaska::Mapping::from_handle_safe(mappings[event.handle_id])) {
          on_alloc(event.cycles, m);
        }
        break;
      }
      case Event::FREE: {
        if (auto *m = alaska::Mapping::from_handle_safe(mappings[event.handle_id])) {
          on_free(event.cycles, m);
        }
        // tc->hfree(mappings[event.handle_id]);
        mappings.erase(event.handle_id);
        break;
      }
      case Event::ACCESS: {
        accesses++;
        if (auto *m = alaska::Mapping::from_handle_safe(mappings[event.handle_id])) {
          on_access(event.cycles, m, event.misc);
        }
        break;
      }
    }
  }


  void run(std::string path) {
    if (!path.ends_with(".binary")) {
      ensure_traceb(path);
      path = path + ".binary";
    }

    FILE *stream = fopen(path.c_str(), "r");
    size_t num_events = std::filesystem::file_size(path) / sizeof(TraceEvent);


    size_t batch_size = 4096 * 512;
    auto *events = new TraceEvent[batch_size];
    size_t batch_count = num_events / batch_size;
    uint64_t last_cycles = 0;
    uint64_t start_cycle = 0;

    for (size_t batch = 0; batch < batch_count; batch++) {
      size_t count = fread(events, sizeof(TraceEvent), batch_size, stream);
      if (unlikely(batch == 0)) {
        start_cycle = events[0].cycles;
      }
      uint64_t sim_time_ns = events[count - 1].cycles - start_cycle;
      float sim_time_seconds = (float)sim_time_ns / 1e9f;
      time_seconds = sim_time_seconds;

      for (size_t i = 0; i < count; i++) {
        event_counter++;

        uint64_t current_cycle = events[i].cycles - start_cycle;
        if (last_cycles == 0) last_cycles = current_cycle;

        auto cycles_passed = current_cycle - last_cycles;
        cycles_handled.track(cycles_passed);

        process_event(events[i]);
        last_cycles = current_cycle;

        if (has_timeout) {
          timeout_cycles -= cycles_passed;
          if (timeout_cycles <= 0) {
            has_timeout = false;
            on_timer(current_cycle);
          }
        }
      }

      if (true or batch % 8 == 0) {
        float progress = (float)batch / (float)batch_count;
        printf(
            "[%16.8fs %3.0f%%] processed %15zu events %15.0f/s, %12.0fcyc/s, alloc:%8.0f/s, "
            "acc:%8.0f/s\n",
            sim_time_seconds, progress * 100.0f, (batch + 1) * batch_size, event_counter.digest(),
            cycles_handled.digest(), allocations.digest(), accesses.digest());
      }
    }
    delete[] events;
    fclose(stream);
  }
};


float start_time = 8;

class HTLBTraceRunner : public TraceRunner {
 public:
  alaska::sim::HTLB htlb;
  FILE *hitrate_file;

  FILE *frag_file;
  HTLBTraceRunner()
      : TraceRunner() {
    htlb.thread_cache = tc;
    set_timer(50 * 1000);  // we start off w/ a 50ms timer
    hitrate_file = fopen("hitrate.csv", "w");
    fprintf(hitrate_file, "cycle,htlb1,htlb2,tlb1,tlb2,dcache1,dcache2\n");

    frag_file = fopen("frag.csv", "w");
    fprintf(frag_file, "cycle,id,frag\n");
  }

  virtual ~HTLBTraceRunner() { fclose(hitrate_file); }

  void on_access(uint64_t cycle, alaska::Mapping *m, uint32_t offset) override {
    // auto header = alaska::ObjectHeader::from(m);
    // if (time_seconds > start_time and !header->localized && header->object_size() != 0) {
    //   this->tc->localize(m, 12);
    // }
    htlb.access(*m, offset);
  }
  void on_free(uint64_t cycle, alaska::Mapping *m) override {
    // printf("invalidate %x\n", m->handle_id());
    htlb.invalidate(m->handle_id());
  }


  uint64_t us_since_reset = 0;
  void on_timer(uint64_t cycle) override {
    // htlb.print_state();
    if (time_seconds > start_time) htlb.localize();

    uint64_t dump_interval = 100;
    us_since_reset += dump_interval;
    if (us_since_reset > 10 * 1000) {
      fprintf(hitrate_file, "%zu,", cycle);
      fprintf(hitrate_file, "%5.1f,", htlb.htlb.l1.hitrate());
      fprintf(hitrate_file, "%5.1f,", htlb.htlb.l2.hitrate());
      fprintf(hitrate_file, "%5.1f,", htlb.tlb.l1.hitrate());
      fprintf(hitrate_file, "%5.1f,", htlb.tlb.l2.hitrate());
      fprintf(hitrate_file, "%5.1f,", htlb.dcache.l1.hitrate());
      fprintf(hitrate_file, "%5.1f", htlb.dcache.l2.hitrate());
      fprintf(hitrate_file, "\n");
      fflush(hitrate_file);
      htlb.reset();

      // dump fragmentation info!
      auto &table = rt.heap.pt.get_table();
      int id = 0;
      for (auto *heap : table) {
        fprintf(frag_file, "%zu,%s-%d,%f\n", cycle, heap->name, id, heap->fragmentation());
        id++;
      }
      fflush(frag_file);

      us_since_reset = 0;
    }

    set_timer(dump_interval);  // 100us
  }
};

int main(int argc, char **argv) {
  alaska::sim::HTLB htlb;

  if (argc != 2) {
    printf("Usage: %s <tracefile>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  unsigned long start, end;


  HTLBTraceRunner runner;
  runner.run(argv[1]);
  runner.htlb.print_state();




  return 0;
}

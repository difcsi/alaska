/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2025, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2025, The Constellation Project
 * All rights reserved.
 *
 */



#pragma once

#include <alaska/disk/Disk.hpp>
#include <alaska/disk/Structure.hpp>
#include <alaska/util/Logger.hpp>
#include <ck/template_lib.h>
#include <alaska/util/RateCounter.hpp>
#include <alaska/disk/impl/MurmurHash3.hpp>

namespace alaska::disk {


  /**
   * A base class for a chained hash table that can
   */
  template <typename K, typename V>
  class ChainedHashTable : public Structure {
    using HashValue = uint32_t;
    struct Entry {
      // We store the hash of the key to make scanning faster
      HashValue hash;
      K key;
      V value;
    } __attribute__((packed));




    // The chains are a doubly linked list of nodes, each of which contains a number of entries.
    struct ChainHeader {
      // the previous page in the chain. If 0, this is the first page
      uint64_t previous_page_id;
      // the next page in the chain. If 0, this is the last page
      uint64_t next_page_id;  // the next page in the chain (if this node doesn't have the entry.)
      // A latch to prevent concurrent access
      uint32_t latch;
      // How many entries are in this node
      uint32_t count;
    };
    using ChainNodeOverlay = alaska::disk::HeaderAndEntries<ChainHeader, Entry>;



    struct RootHeader {
      uint32_t cookie;  // should be 'CHSH' (chain hash)
      // number of buckets in the hashtable (for now, we don't resize, so it's
      // NUM_ENTRIES)
      uint32_t buckets;
      // latch to prevent concurrent access.
      uint32_t latch;
    };
    using RootNodeOverlay = alaska::disk::HeaderAndEntries<RootHeader, uint64_t>;

    alaska::RateCounter stat_collisions;
    alaska::RateCounter stat_insertions;
    alaska::RateCounter stat_lookups;
    alaska::RateCounter stat_chainwalks;

   public:
    ChainedHashTable(BufferPool &pool, const char *name)
        : Structure(pool, name) {
      auto root = pool.getOverlay<RootNodeOverlay>(root_page_id);
      if (root->header.cookie != 'CHSH') {
        auto rootMut = root.mut();
        rootMut->header.cookie = 'CHSH';
        rootMut->header.buckets = RootNodeOverlay::NUM_ENTRIES;
        rootMut->header.latch = 0;  // initial state for the latch is 'unlocked'

        // initialize the buckets to 0
        for (uint32_t i = 0; i < rootMut->header.buckets; i++) {
          rootMut->entries[i] = 0;
        }

        printf("Created new hash table %zu buckets\n", root->header.buckets);
      }
    }

    ~ChainedHashTable() {
      printf("Chained Hash Table Stats:\n");
      printf("  insertions:    %12zu (%.1f/s)\n", stat_insertions.read(), stat_insertions.digest());
      printf("  collisions:    %12zu (%.1f/s)\n", stat_collisions.read(), stat_collisions.digest());
      printf("     lookups:    %12zu (%.1f/s)\n", stat_lookups.read(), stat_lookups.digest());
      printf("  chainwalks:    %12zu (%.1f/s)\n", stat_chainwalks.read(), stat_chainwalks.digest());
    }




    /**
     * @brief Inserts a key-value pair into the hash table.
     *
     * @param key The key to insert.
     * @param value The value to insert.
     * @return true if the key was inserted or updated.
     */
    bool insert(K key, V value) {
      alaska::disk::FrameGuard chainNodeGuard;
      alaska::disk::Latch latch;

      stat_insertions++;

      auto search = getEntry(key, &chainNodeGuard, &latch, true);
      if (!search.has_value()) {
        fprintf(stderr, "Failed to get entry to insert key key %zu\n", key);
        return false;
      }

      uint32_t entry_index = search.take();
      auto chainNode = chainNodeGuard.getMut<ChainNodeOverlay>();
      chainNode->entries[entry_index].value = value;

      return false;
    }

    /**
     * @brief Removes a key-value pair from the hash table.
     *
     * @param key The key to remove.
     * @return true if the key was removed, false if it was not found.
     */
    bool remove(K key) {
      // Remove works in an interesting way. If we cannot find the key we want
      // to remove, we simply return false. If we do find the key, we simply
      // copy the entry from the end of the entries list to the entry we want to
      // remove and decrement the count. This removes the entry from the list
      // without needing to shuffle everything.

      alaska::disk::FrameGuard chainNodeGuard;
      alaska::disk::Latch latch;
      auto search = getEntry(key, &chainNodeGuard, &latch, false);
      if (!search.has_value()) {
        return false;
      }


      uint32_t entry_index = search.take();
      auto chainNode = chainNodeGuard.getMut<ChainNodeOverlay>();
      chainNode->header.count--;
      chainNode->entries[entry_index] = chainNode->entries[chainNode->header.count];
      return false;
    }

    /**
     * @brief Gets a value from the hash table.
     *
     * @param key The key to get.
     * @param value The value to get.
     * @return true if the key was found, false if it was not found.
     */
    bool get(K key, V *value) {
      stat_lookups++;
      alaska::disk::FrameGuard chainNodeGuard;
      alaska::disk::Latch latch;
      auto search = getEntry(key, &chainNodeGuard, &latch, false);
      if (!search.has_value()) {
        return false;
      }

      uint32_t entry_index = search.take();
      auto chainNode = chainNodeGuard.get<ChainNodeOverlay>();
      *value = chainNode->entries[entry_index].value;

      return true;
    }

    ck::opt<V> get(K key) {
      V value;
      if (!get(key, &value)) return None;
      return Some(value);
    }


   private:
    ck::opt<uint32_t> getEntry(const K &key, FrameGuard *chainNodeOut, Latch *chainNodeLatchOut,
                               bool create = false) {
      auto h = hash(key);
      uint32_t bucket;
      uint64_t chain_page_id = 0;

      {
        auto root = pool.getOverlay<RootNodeOverlay>(root_page_id);
        alaska::disk::Latch rootLatch(root.guard());

        h = hash(key);
        bucket = h % root->header.buckets;

        if (root->entries[bucket] == 0) {
          // the bucket is empty, and we aren't asked to create, don't.
          if (!create) return None;

          // we need to allocate one a new chain node.
          FrameGuard pg = pool.newPage();

          // The initial state of the chain is that it has no previous or next page.
          auto newChain = pg.getMut<ChainNodeOverlay>();
          newChain->header.previous_page_id = 0;
          newChain->header.next_page_id = 0;
          newChain->header.latch = 0;
          newChain->header.count = 0;
          // Update the root node
          root.mut()->entries[bucket] = pg.page_id();
        }

        chain_page_id = root->entries[bucket];
      }




      // Now, we can walk the chain
      while (chain_page_id != 0) {
        auto chain = pool.getOverlay<ChainNodeOverlay>(chain_page_id);
        alaska::disk::Latch chainLatch(chain.guard());
        // Check the entries in the ChainNode for the key. If we find it, we can
        // update the value and return.
        for (uint32_t i = 0; i < chain->header.count; i++) {
          // Quickly continue if the hash isn't equal.
          if (chain->entries[i].hash != h) continue;

          // Check the equality of the keys.
          if (equal(chain->entries[i].key, key)) {
            *chainNodeLatchOut = ck::move(chainLatch);
            *chainNodeOut = chain.guard();

            return Some(i);
          } else {
            stat_collisions++;
          }
        }


        // if we get here, we didn't find the key in the chain, so we need to
        // insert it in the current node if there's space, or allocate a new node.
        if (chain->header.count < ChainNodeOverlay::NUM_ENTRIES) {
          // we can insert the key in the current node.
          auto chainMut = chain.mut();
          uint32_t ind = chain->header.count;
          auto newEntry = &chainMut->entries[ind];

          *chainNodeLatchOut = ck::move(chainLatch);
          *chainNodeOut = chain.guard();

          newEntry->hash = h;
          newEntry->key = key;
          // newEntry->value = value;
          chainMut->header.count++;
          return Some(ind);
        } else if (chain->header.next_page_id == 0) {
          // we need to allocate a new node and insert it at the end of the chain.
          auto newChain = allocateChainNode();
          newChain->header.previous_page_id = chain_page_id;
          newChain->header.next_page_id = 0;
          newChain->header.latch = 0;
          newChain->header.count = 0;
          // insert the new node at the end of the chain
          chain.mut()->header.next_page_id = newChain.guard().page_id();
        }

        stat_chainwalks++;
        chain_page_id = chain->header.next_page_id;
      }

      return None;
    }

    GuardedMut<ChainNodeOverlay> allocateChainNode() {
      FrameGuard pg = pool.newPage();
      auto chain = pg.getMut<ChainNodeOverlay>();
      chain->header.previous_page_id = 0;
      chain->header.next_page_id = 0;
      chain->header.latch = 0;
      chain->header.count = 0;
      return pg;
    }

    HashValue hash(K key) {
      uint32_t h;
      uint32_t seed = 0xB0F57EE3;  // taken from smhasher. idk
      MurmurHash3_x86_32(&key, sizeof(K), seed, &h);
      // printf("hash %zx -> %zx\n", key, h);
      return h;
    }

    bool equal(const K &key1, const K &key2) { return Traits<K>::equal(key1, key2); }
  };



}  // namespace alaska::disk

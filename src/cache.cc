#include "cache.h"

#include <algorithm>
#include <iterator>

#include "champsim.h"
#include "champsim_constants.h"
#include "util.h"
#include "vmem.h"

#ifndef SANITY_CHECK
#define NDEBUG
#endif

extern VirtualMemory vmem;
extern uint8_t warmup_complete[NUM_CPUS];

void CACHE::handle_fill()
{
  /**
   * MSHR Fill
   *
   * Asynchronous cache request (Non-blocking)
   */
  while (writes_available_this_cycle > 0) {
    // Always Handle the first one in MSHR
    auto fill_mshr = MSHR.begin();

    // If MSHR is empty or the time is not correct, exit
    if (fill_mshr == std::end(MSHR) || fill_mshr->event_cycle > current_cycle)
      return;

    if (cache_type != NOT_CACHE && fill_mshr->mshr_return_data_invalid_count < fill_mshr->mshr_invalid_count) {
      bool is_read = prefetch_as_load || (fill_mshr->type != PREFETCH);

      // Allocate an MSHR
      // It will create NSHR in any level of cache once the cache see this request
      PACKET new_pkt;
      copy_packet(&(*fill_mshr), &new_pkt);

      // check to make sure the lower level queue has room for this read miss
      int queue_type = (is_read) ? 1 : 3;

      new_pkt.test_packet = true;
      int test_result = 0;
      if (cache_type != NOT_CACHE) {
        test_result = lower_level->add_rq(&new_pkt);
      }
      if (lower_level->get_occupancy(queue_type, new_pkt.address) == lower_level->get_size(queue_type, new_pkt.address) || (test_result == -2))
        return;
      new_pkt.test_packet = false;

      auto it = MSHR.insert(std::end(MSHR), new_pkt);
      it->event_cycle = std::numeric_limits<uint64_t>::max();

      if (new_pkt.fill_level <= fill_level)
        new_pkt.to_return = {this};
      else
        new_pkt.to_return.clear();

      if (is_read) {
        lower_level->add_rq(&new_pkt);
      } else {
        lower_level->add_pq(&new_pkt);
      }
      MSHR.erase(fill_mshr);
      return;
    }

    if (cache_type != EXCLUSIVE) {

      // find victim

      uint32_t set = get_set(fill_mshr->address);

      auto set_begin = std::next(std::begin(block), set * NUM_WAY);
      auto set_end = std::next(set_begin, NUM_WAY);

      // Find the invalid block (If there is no invalid block, it will be the last value)
      auto first_inv = std::find_if_not(set_begin, set_end, is_valid<BLOCK>());
      uint32_t way = std::distance(set_begin, first_inv);

      // If the distance is the number of way then no invalid block in the set
      if (way == NUM_WAY)
        // impl_replacement_find_victim is user-defined function
        way = impl_replacement_find_victim(fill_mshr->cpu, fill_mshr->instr_id, set, &block.data()[set * NUM_WAY], fill_mshr->ip, fill_mshr->address,
                                           fill_mshr->type);

      // Try to fetch the value and kick out the victim
      bool success = filllike_miss(set, way, *fill_mshr);
      if (!success) {
        return;
      }

      if (way != NUM_WAY) {
        // update processed packets
        fill_mshr->data = block[set * NUM_WAY + way].data;

        // Return the data to the related memory request producer
        for (auto ret : fill_mshr->to_return)
          ret->return_data(&(*fill_mshr));
      }
    } else {
      // Return the data to the related memory request producer
      for (auto ret : fill_mshr->to_return)
        ret->return_data(&(*fill_mshr));
    }

    MSHR.erase(fill_mshr);
    writes_available_this_cycle--;
  }
}

void CACHE::handle_writeback()
{
  while (writes_available_this_cycle > 0) {
    if (!WQ.has_ready())
      return;

    // handle the oldest entry
    PACKET& handle_pkt = WQ.front();

    DP(if (warmup_complete[handle_pkt.cpu] && cache_type == EXCLUSIVE) {
      std::cout << "[" << NAME << "_HANDLE_WB] " << __func__ << " instr_id: " << handle_pkt.instr_id << " address: " << std::hex
                << (handle_pkt.address >> OFFSET_BITS);
      std::cout << " full_addr: " << handle_pkt.address << " v_address: " << handle_pkt.v_address << std::dec << " type: " << +handle_pkt.type
                << " occupancy: " << WQ.occupancy() << " cycle: " << current_cycle << std::endl;
    })

    if (handle_pkt.type == NON_VALID) {
      sim_access[handle_pkt.cpu][handle_pkt.type]++;
      sim_hit[handle_pkt.cpu][handle_pkt.type]++;
      writes_available_this_cycle--;
      WQ.pop_front();
      return;
    }

    // access cache
    uint32_t set = get_set(handle_pkt.address);
    uint32_t way = get_way(handle_pkt.address, set);

    BLOCK& fill_block = block[set * NUM_WAY + way];

    if (way < NUM_WAY && fill_block.valid) // HIT
    {
      assert(cache_type != EXCLUSIVE);

      impl_replacement_update_state(handle_pkt.cpu, set, way, fill_block.address, handle_pkt.ip, 0, handle_pkt.type, 1);

      // COLLECT STATS
      sim_hit[handle_pkt.cpu][handle_pkt.type]++;
      sim_access[handle_pkt.cpu][handle_pkt.type]++;

      // mark dirty
      // No actual data in the simulator. Only request is considered
      fill_block.dirty = 1;
    } else {
      // MISS
      bool success;
      // Write request from CPU
      if (handle_pkt.type == RFO && handle_pkt.to_return.empty()) {
        success = readlike_miss(handle_pkt);
      } else {
        // find victim
        // Write back cache block miss in the lower level cache
        auto set_begin = std::next(std::begin(block), set * NUM_WAY);
        auto set_end = std::next(set_begin, NUM_WAY);
        auto first_inv = std::find_if_not(set_begin, set_end, is_valid<BLOCK>());
        way = std::distance(set_begin, first_inv);
        if (way == NUM_WAY)
          way = impl_replacement_find_victim(handle_pkt.cpu, handle_pkt.instr_id, set, &block.data()[set * NUM_WAY], handle_pkt.ip, handle_pkt.address,
                                             handle_pkt.type);
        success = filllike_miss(set, way, handle_pkt);
      }

      if (!success)
        return;
    }
    // remove this entry from WQ
    writes_available_this_cycle--;
    WQ.pop_front();
  }
}

void CACHE::handle_read()
{
  while (reads_available_this_cycle > 0) {

    if (!RQ.has_ready())
      return;

    // handle the oldest entry
    PACKET& handle_pkt = RQ.front();

    // A (hopefully temporary) hack to know whether to send the evicted paddr or
    // vaddr to the prefetcher
    ever_seen_data |= (handle_pkt.v_address != handle_pkt.ip);

    uint32_t set = get_set(handle_pkt.address);
    uint32_t way = get_way(handle_pkt.address, set);

    BLOCK& fill_block = block[set * NUM_WAY + way];

    if (way < NUM_WAY && fill_block.valid) // HIT
    {
      readlike_hit(set, way, handle_pkt);
    } else {
      bool success = readlike_miss(handle_pkt);
      if (!success) {
        return;
      }
    }

    // remove this entry from RQ
    RQ.pop_front();
    reads_available_this_cycle--;
  }
}

void CACHE::handle_prefetch()
{
  while (reads_available_this_cycle > 0) {
    if (!PQ.has_ready())
      return;

    // handle the oldest entry
    PACKET& handle_pkt = PQ.front();

    uint32_t set = get_set(handle_pkt.address);
    uint32_t way = get_way(handle_pkt.address, set);

    BLOCK& fill_block = block[set * NUM_WAY + way];

    if (way < NUM_WAY && fill_block.valid) // HIT
    {
      readlike_hit(set, way, handle_pkt);
    } else {
      bool success = readlike_miss(handle_pkt);
      if (!success)
        return;
    }

    // remove this entry from PQ
    PQ.pop_front();
    reads_available_this_cycle--;
  }
}

void CACHE::readlike_hit(std::size_t set, std::size_t way, PACKET& handle_pkt)
{
  DP(if (warmup_complete[handle_pkt.cpu]) {
    std::cout << "[" << NAME << "] " << __func__ << " hit";
    std::cout << " instr_id: " << handle_pkt.instr_id << " address: " << std::hex << (handle_pkt.address >> OFFSET_BITS);
    std::cout << " full_addr: " << handle_pkt.address;
    std::cout << " full_v_addr: " << handle_pkt.v_address << std::dec;
    std::cout << " type: " << +handle_pkt.type;
    std::cout << " cycle: " << current_cycle << std::endl;
  });

  BLOCK& hit_block = block[set * NUM_WAY + way];

  handle_pkt.data = hit_block.data;
  if (hit_block.dirty) {
    handle_pkt.data_valid = true;
  }

  // update prefetcher on load instruction
  if (should_activate_prefetcher(handle_pkt.type) && handle_pkt.pf_origin_level < fill_level) {
    cpu = handle_pkt.cpu;
    uint64_t pf_base_addr = (virtual_prefetch ? handle_pkt.v_address : handle_pkt.address) & ~bitmask(match_offset_bits ? 0 : OFFSET_BITS);
    handle_pkt.pf_metadata = impl_prefetcher_cache_operate(pf_base_addr, handle_pkt.ip, 1, handle_pkt.type, handle_pkt.pf_metadata);
  }

  if (cache_type == EXCLUSIVE) {
    // TODO: Exclusive read hit
    invalidate_entry(handle_pkt.address);
  }

  // update replacement policy
  impl_replacement_update_state(handle_pkt.cpu, set, way, hit_block.address, handle_pkt.ip, 0, handle_pkt.type, 1);

  // COLLECT STATS
  sim_hit[handle_pkt.cpu][handle_pkt.type]++;
  sim_access[handle_pkt.cpu][handle_pkt.type]++;

  for (auto ret : handle_pkt.to_return)
    ret->return_data(&handle_pkt);

  // update prefetch stats and reset prefetch bit
  if (hit_block.prefetch) {
    pf_useful++;
    hit_block.prefetch = 0;
  }
}

bool CACHE::readlike_miss(PACKET& handle_pkt)
{
  DP(if (warmup_complete[handle_pkt.cpu]) {
    std::cout << "[" << NAME << "] " << __func__ << " miss";
    std::cout << " instr_id: " << handle_pkt.instr_id << " address: " << std::hex << (handle_pkt.address >> OFFSET_BITS);
    std::cout << " full_addr: " << handle_pkt.address;
    std::cout << " full_v_addr: " << handle_pkt.v_address << std::dec;
    std::cout << " type: " << +handle_pkt.type;
    std::cout << " cycle: " << current_cycle << std::endl;
  });

  // check mshrd
  // Search related MSHR entry
  auto mshr_entry = std::find_if(MSHR.begin(), MSHR.end(), eq_addr<PACKET>(handle_pkt.address, OFFSET_BITS));
  bool mshr_full = (MSHR.size() == MSHR_SIZE);

  if (mshr_entry != MSHR.end()) // miss already inflight
  {
    // update fill location
    // Merge the dependencies between these requests
    mshr_entry->fill_level = std::min(mshr_entry->fill_level, handle_pkt.fill_level);

    packet_dep_merge(mshr_entry->lq_index_depend_on_me, handle_pkt.lq_index_depend_on_me);
    packet_dep_merge(mshr_entry->sq_index_depend_on_me, handle_pkt.sq_index_depend_on_me);
    packet_dep_merge(mshr_entry->instr_depend_on_me, handle_pkt.instr_depend_on_me);
    packet_dep_merge(mshr_entry->to_return, handle_pkt.to_return);

    if (mshr_entry->type == PREFETCH && handle_pkt.type != PREFETCH) {
      // Mark the prefetch as useful
      if (mshr_entry->pf_origin_level == fill_level)
        pf_useful++;

      uint64_t prior_event_cycle = mshr_entry->event_cycle;
      *mshr_entry = handle_pkt;

      // in case request is already returned, we should keep event_cycle
      mshr_entry->event_cycle = prior_event_cycle;
    }
  } else {
    if (mshr_full)  // not enough MSHR resource
      return false; // TODO should we allow prefetches anyway if they will not
                    // be filled to this level?

    bool is_read = prefetch_as_load || (handle_pkt.type != PREFETCH);

    // check to make sure the lower level queue has room for this read miss
    int queue_type = (is_read) ? 1 : 3;
    handle_pkt.test_packet = true;
    int test_result = 0;
    if (cache_type != NOT_CACHE) {
      test_result = lower_level->add_rq(&handle_pkt);
    }
    if (lower_level->get_occupancy(queue_type, handle_pkt.address) == lower_level->get_size(queue_type, handle_pkt.address) || (test_result == -2))
      return false;

    handle_pkt.test_packet = false;
    // Allocate an MSHR
    // It will create NSHR in any level of cache once the cache see this request
    if (handle_pkt.fill_level <= fill_level) {
      auto it = MSHR.insert(std::end(MSHR), handle_pkt);
      it->cycle_enqueued = current_cycle;
      it->event_cycle = std::numeric_limits<uint64_t>::max();
    }

    // Will Overwrite the final destination place
    if (handle_pkt.fill_level <= fill_level)
      handle_pkt.to_return = {this};
    else
      handle_pkt.to_return.clear();

    if (!is_read)
      lower_level->add_pq(&handle_pkt);
    else
      lower_level->add_rq(&handle_pkt);
  }

  // update prefetcher on load instructions and prefetches from upper levels
  if (should_activate_prefetcher(handle_pkt.type) && handle_pkt.pf_origin_level < fill_level) {
    cpu = handle_pkt.cpu;
    uint64_t pf_base_addr = (virtual_prefetch ? handle_pkt.v_address : handle_pkt.address) & ~bitmask(match_offset_bits ? 0 : OFFSET_BITS);
    handle_pkt.pf_metadata = impl_prefetcher_cache_operate(pf_base_addr, handle_pkt.ip, 0, handle_pkt.type, handle_pkt.pf_metadata);
  }

  return true;
}

bool CACHE::filllike_miss(std::size_t set, std::size_t way, PACKET& handle_pkt)
{
  DP(if (warmup_complete[handle_pkt.cpu]) {
    std::cout << "[" << NAME << "] " << __func__ << " miss";
    std::cout << " instr_id: " << handle_pkt.instr_id << " address: " << std::hex << (handle_pkt.address >> OFFSET_BITS);
    std::cout << " full_addr: " << handle_pkt.address;
    std::cout << " full_v_addr: " << handle_pkt.v_address << std::dec;
    std::cout << " type: " << +handle_pkt.type;
    std::cout << " cycle: " << current_cycle << std::endl;
  });

  // If way is still NUM_WAY, which is larger than the actual number of way. It means that the memory request will not be cached.
  bool bypass = (way == NUM_WAY);
#ifndef LLC_BYPASS
  assert(!bypass);
#endif
  assert(handle_pkt.type != WRITEBACK || !bypass);
  assert(handle_pkt.type != INVALIDATE);

  // Destination block
  BLOCK& fill_block = block[set * NUM_WAY + way];

  // Will not hit lower_level != NULL since the last level is DRAM (Not sure what lower_level in DRAM)
  bool evicting_dirty = !bypass && (lower_level != NULL) && fill_block.dirty;
  uint64_t evicting_address = 0;

  if (!bypass) {
    if (fill_block.valid) {

      if (send_wb_valid) {
        PACKET writeback_packet;
        writeback_packet.fill_level = lower_level->fill_level;
        writeback_packet.cpu = handle_pkt.cpu;
        writeback_packet.address = fill_block.address;
        writeback_packet.data = fill_block.data;
        writeback_packet.instr_id = handle_pkt.instr_id;
        writeback_packet.ip = 0;
        writeback_packet.inv_ongoing = (cache_type == INCLUSIVE) ? 2 : 0;

        if (cache_type == NOT_CACHE) {
          if (evicting_dirty) {
            writeback_packet.type = WRITEBACK;

            auto result = lower_level->add_wq(&writeback_packet);
            if (result == -2)
              return false;
          }
        } else {
          if (lower_level != NULL) {
            writeback_packet.type = evicting_dirty ? WRITEBACK : WRITEBACK_EXCLUSIVE;

            auto result = lower_level->add_wq(&writeback_packet);
            if (result == -2)
              return false;
          }
        }
        send_wb_valid = false;
      }

      if (cache_type == INCLUSIVE) {
        PACKET invalidation_packet;
        invalidation_packet.fill_level = lower_level->fill_level;
        invalidation_packet.cpu = handle_pkt.cpu;
        invalidation_packet.address = fill_block.address;
        invalidation_packet.instr_id = handle_pkt.instr_id;
        invalidation_packet.ip = 0;
        invalidation_packet.type = INVALIDATE;

        DP(if (warmup_complete[handle_pkt.cpu]) {
          std::cout << "[" << NAME << "_FILL_MISS] " << __func__ << " instr_id: " << invalidation_packet.instr_id << " address: " << std::hex
                    << (invalidation_packet.address >> OFFSET_BITS);
          std::cout << " full_addr: " << invalidation_packet.address << " v_address: " << invalidation_packet.v_address << std::dec
                    << " type: " << +invalidation_packet.type << " occupancy: " << lower_level->get_occupancy(2, 0) << " cycle: " << current_cycle;
        })

        bool skip = false;
        for (int i = 0; i < 2; i++) {
          if (upper_level[i] != nullptr && send_inv_valid[i]) {
            auto result = upper_level[i]->add_ivq(&invalidation_packet);
            if (result != -2) {
              send_inv_valid[i] = false;
            } else {
              skip = true;
            }
          }
        }
        if (skip) {
          DP(if (warmup_complete[handle_pkt.cpu]) { std::cout << " incomplete" << std::endl; })
          return false;
        }
        send_inv_valid[0] = true;
        send_inv_valid[1] = true;
        DP(if (warmup_complete[handle_pkt.cpu]) { std::cout << " complete" << std::endl; })
      }
      send_wb_valid = true;
    }

    if (ever_seen_data)
      evicting_address = fill_block.address & ~bitmask(match_offset_bits ? 0 : OFFSET_BITS);
    else
      evicting_address = fill_block.v_address & ~bitmask(match_offset_bits ? 0 : OFFSET_BITS);

    // If the destination block is prefetched, then it is more likely to be useless
    if (fill_block.prefetch && fill_block.valid)
      pf_useless++;

    // The memory request is from prefetcher
    if (handle_pkt.type == PREFETCH)
      pf_fill++;

    fill_block.valid = true;
    // Only the cache block that prefetcher wants to fill is actually the prefetch block
    fill_block.prefetch = (handle_pkt.type == PREFETCH && handle_pkt.pf_origin_level == fill_level);

    // TODO: Not sure what to_return mean
    // RFO = Read for ownership (equal to busRdx in cache coherence protocol)
    fill_block.dirty = (handle_pkt.type == WRITEBACK || (handle_pkt.type == RFO && handle_pkt.to_return.empty()) || handle_pkt.data_valid);
    fill_block.address = handle_pkt.address;
    fill_block.v_address = handle_pkt.v_address;
    fill_block.data = handle_pkt.data;
    fill_block.ip = handle_pkt.ip;
    fill_block.cpu = handle_pkt.cpu;
    fill_block.instr_id = handle_pkt.instr_id;
  }

  // Calculate the memory request miss latency (Fill cycle - Enqueue cycle)
  if (warmup_complete[handle_pkt.cpu] && (handle_pkt.cycle_enqueued != 0))
    total_miss_latency += current_cycle - handle_pkt.cycle_enqueued;

  // update prefetcher
  // TODO: ???? Shared cache?
  cpu = handle_pkt.cpu;
  // User-defined prefetcher
  handle_pkt.pf_metadata =
      impl_prefetcher_cache_fill((virtual_prefetch ? handle_pkt.v_address : handle_pkt.address) & ~bitmask(match_offset_bits ? 0 : OFFSET_BITS), set, way,
                                 handle_pkt.type == PREFETCH, evicting_address, handle_pkt.pf_metadata);

  // update replacement policy
  // User-defined cache replacement policy (Replacement status update)
  impl_replacement_update_state(handle_pkt.cpu, set, way, handle_pkt.address, handle_pkt.ip, 0, handle_pkt.type, 0);

  // COLLECT STATS
  sim_miss[handle_pkt.cpu][handle_pkt.type]++;
  sim_access[handle_pkt.cpu][handle_pkt.type]++;

  return true;
}

void CACHE::operate()
{
  operate_invalid();
  operate_writes();
  operate_reads();

  impl_prefetcher_cycle_operate();
}

void CACHE::operate_invalid()
{
  // perform all writes
  invalid_available_this_cycle = MAX_WRITE;
  handle_invalid();

  IVQ.operate();
}

void CACHE::operate_writes()
{
  // perform all writes
  writes_available_this_cycle = invalid_available_this_cycle;
  handle_fill();
  handle_writeback();

  WQ.operate();
}

void CACHE::operate_reads()
{
  // perform all reads
  reads_available_this_cycle = MAX_READ;
  handle_read();
  va_translate_prefetches();
  handle_prefetch();

  RQ.operate();
  PQ.operate();
  VAPQ.operate();
}

uint32_t CACHE::get_set(uint64_t address) { return ((address >> OFFSET_BITS) & bitmask(lg2(NUM_SET))); }

uint32_t CACHE::get_way(uint64_t address, uint32_t set)
{
  auto begin = std::next(block.begin(), set * NUM_WAY);
  auto end = std::next(begin, NUM_WAY);
  return std::distance(begin, std::find_if(begin, end, eq_addr<BLOCK>(address, OFFSET_BITS)));
}

int CACHE::invalidate_entry(uint64_t inval_addr)
{
  uint32_t set = get_set(inval_addr);
  uint32_t way = get_way(inval_addr, set);

  if (way < NUM_WAY)
    block[set * NUM_WAY + way].valid = false;

  return way;
}

int CACHE::add_rq(PACKET* packet)
{
  assert(packet->address != 0);

  if (packet->test_packet && cache_type != NOT_CACHE) {
    champsim::delay_queue<PACKET>::iterator found_ivq = std::find_if(IVQ.begin(), IVQ.end(), eq_addr<PACKET>(packet->address, OFFSET_BITS));

    if (found_ivq != IVQ.end()) {
      DP(if (warmup_complete[packet->cpu]) std::cout << " Address conflict" << std::endl;)
      return -2;
    }
    return RQ.occupancy();
  }

  RQ_ACCESS++;

  DP(if (warmup_complete[packet->cpu]) {
    std::cout << "[" << NAME << "_RQ] " << __func__ << " instr_id: " << packet->instr_id << " address: " << std::hex << (packet->address >> OFFSET_BITS);
    std::cout << " full_addr: " << packet->address << " v_address: " << packet->v_address << std::dec << " type: " << +packet->type
              << " occupancy: " << RQ.occupancy();
  })

  // check for the latest writebacks in the write queue
  champsim::delay_queue<PACKET>::iterator found_wq = std::find_if(WQ.begin(), WQ.end(), eq_addr<PACKET>(packet->address, match_offset_bits ? 0 : OFFSET_BITS));

  if (found_wq != WQ.end() && found_wq->inv_ongoing == 0) {

    DP(if (warmup_complete[packet->cpu]) std::cout << " MERGED_WQ" << std::endl;)

    packet->data = found_wq->data;
    for (auto ret : packet->to_return)
      ret->return_data(packet);

    WQ_FORWARD++;
    return -1;
  }

  // check for duplicates in the read queue
  auto found_rq = std::find_if(RQ.begin(), RQ.end(), eq_addr<PACKET>(packet->address, OFFSET_BITS));
  if (found_rq != RQ.end()) {

    DP(if (warmup_complete[packet->cpu]) std::cout << " MERGED_RQ" << std::endl;)

    packet_dep_merge(found_rq->lq_index_depend_on_me, packet->lq_index_depend_on_me);
    packet_dep_merge(found_rq->sq_index_depend_on_me, packet->sq_index_depend_on_me);
    packet_dep_merge(found_rq->instr_depend_on_me, packet->instr_depend_on_me);
    packet_dep_merge(found_rq->to_return, packet->to_return);

    RQ_MERGED++;

    return 0; // merged index
  }

  // check occupancy
  if (RQ.full()) {
    RQ_FULL++;

    DP(if (warmup_complete[packet->cpu]) std::cout << " FULL" << std::endl;)

    return -2; // cannot handle this request
  }

  // if there is no duplicate, add it to RQ
  if (warmup_complete[cpu])
    RQ.push_back(*packet);
  else
    RQ.push_back_ready(*packet);

  DP(if (warmup_complete[packet->cpu]) std::cout << " ADDED" << std::endl;)

  RQ_TO_CACHE++;
  return RQ.occupancy();
}

int CACHE::add_wq(PACKET* packet)
{
  if (cache_type != EXCLUSIVE && packet->type == WRITEBACK_EXCLUSIVE) {
    return 0;
  }

  WQ_ACCESS++;

  DP(if (warmup_complete[packet->cpu]) {
    std::cout << "[" << NAME << "_WQ] " << __func__ << " instr_id: " << packet->instr_id << " address: " << std::hex << (packet->address >> OFFSET_BITS);
    std::cout << " full_addr: " << packet->address << " v_address: " << packet->v_address << std::dec << " type: " << +packet->type
              << " occupancy: " << WQ.occupancy();
  })

  // check for duplicates in the write queue
  champsim::delay_queue<PACKET>::iterator found_wq = std::find_if(WQ.begin(), WQ.end(), eq_addr<PACKET>(packet->address, match_offset_bits ? 0 : OFFSET_BITS));

  if (found_wq != WQ.end()) {

    DP(if (warmup_complete[packet->cpu]) std::cout << " MERGED" << std::endl;)

    WQ_MERGED++;
    return 0; // merged index
  }

  // Check for room in the queue
  if (WQ.full()) {
    DP(if (warmup_complete[packet->cpu]) std::cout << " FULL" << std::endl;)

    ++WQ_FULL;
    return -2;
  }

  // if there is no duplicate, add it to the write queue
  if (warmup_complete[cpu])
    WQ.push_back(*packet);
  else
    WQ.push_back_ready(*packet);

  DP(if (warmup_complete[packet->cpu]) std::cout << " ADDED" << std::endl;)

  WQ_TO_CACHE++;

  // TODO: ?? Why two access count
  WQ_ACCESS++;

  return WQ.occupancy();
}

int CACHE::prefetch_line(uint64_t pf_addr, bool fill_this_level, uint32_t prefetch_metadata)
{
  pf_requested++;

  PACKET pf_packet;
  pf_packet.type = PREFETCH;
  pf_packet.fill_level = (fill_this_level ? fill_level : lower_level->fill_level);
  pf_packet.pf_origin_level = fill_level;
  pf_packet.pf_metadata = prefetch_metadata;
  pf_packet.cpu = cpu;
  pf_packet.address = pf_addr;
  pf_packet.v_address = virtual_prefetch ? pf_addr : 0;

  if (virtual_prefetch) {
    if (!VAPQ.full()) {
      VAPQ.push_back(pf_packet);
      return 1;
    }
  } else {
    int result = add_pq(&pf_packet);
    if (result != -2) {
      if (result > 0)
        pf_issued++;
      return 1;
    }
  }

  return 0;
}

int CACHE::prefetch_line(uint64_t ip, uint64_t base_addr, uint64_t pf_addr, bool fill_this_level, uint32_t prefetch_metadata)
{
  static bool deprecate_printed = false;
  if (!deprecate_printed) {
    std::cout << "WARNING: The extended signature CACHE::prefetch_line(ip, "
                 "base_addr, pf_addr, fill_this_level, prefetch_metadata) is "
                 "deprecated."
              << std::endl;
    std::cout << "WARNING: Use CACHE::prefetch_line(pf_addr, fill_this_level, "
                 "prefetch_metadata) instead."
              << std::endl;
    deprecate_printed = true;
  }
  return prefetch_line(pf_addr, fill_this_level, prefetch_metadata);
}

void CACHE::va_translate_prefetches()
{
  // TEMPORARY SOLUTION: mark prefetches as translated after a fixed latency
  if (VAPQ.has_ready()) {
    VAPQ.front().address = vmem.va_to_pa(cpu, VAPQ.front().v_address).first;

    // move the translated prefetch over to the regular PQ
    int result = add_pq(&VAPQ.front());

    // remove the prefetch from the VAPQ
    if (result != -2)
      VAPQ.pop_front();

    if (result > 0)
      pf_issued++;
  }
}

int CACHE::add_pq(PACKET* packet)
{
  assert(packet->address != 0);
  PQ_ACCESS++;

  DP(if (warmup_complete[packet->cpu]) {
    std::cout << "[" << NAME << "_WQ] " << __func__ << " instr_id: " << packet->instr_id << " address: " << std::hex << (packet->address >> OFFSET_BITS);
    std::cout << " full_addr: " << packet->address << " v_address: " << packet->v_address << std::dec << " type: " << +packet->type
              << " occupancy: " << RQ.occupancy();
  })

  champsim::delay_queue<PACKET>::iterator found_ivq =
      std::find_if(IVQ.begin(), IVQ.end(), eq_addr<PACKET>(packet->address, match_offset_bits ? 0 : OFFSET_BITS));

  if (found_ivq != IVQ.end()) {
    DP(if (warmup_complete[packet->cpu]) std::cout << " Address conflict" << std::endl;)
    return -2;
  }

  // check for the latest writebacks in the write queue
  champsim::delay_queue<PACKET>::iterator found_wq = std::find_if(WQ.begin(), WQ.end(), eq_addr<PACKET>(packet->address, match_offset_bits ? 0 : OFFSET_BITS));

  if (found_wq != WQ.end()) {

    DP(if (warmup_complete[packet->cpu]) std::cout << " MERGED_WQ" << std::endl;)

    packet->data = found_wq->data;
    for (auto ret : packet->to_return)
      ret->return_data(packet);

    WQ_FORWARD++;
    return -1;
  }

  // check for duplicates in the PQ
  auto found = std::find_if(PQ.begin(), PQ.end(), eq_addr<PACKET>(packet->address, OFFSET_BITS));
  if (found != PQ.end()) {
    DP(if (warmup_complete[packet->cpu]) std::cout << " MERGED_PQ" << std::endl;)

    found->fill_level = std::min(found->fill_level, packet->fill_level);
    packet_dep_merge(found->to_return, packet->to_return);

    PQ_MERGED++;
    return 0;
  }

  // check occupancy
  if (PQ.full()) {

    DP(if (warmup_complete[packet->cpu]) std::cout << " FULL" << std::endl;)

    PQ_FULL++;
    return -2; // cannot handle this request
  }

  // if there is no duplicate, add it to PQ
  if (warmup_complete[cpu])
    PQ.push_back(*packet);
  else
    PQ.push_back_ready(*packet);

  DP(if (warmup_complete[packet->cpu]) std::cout << " ADDED" << std::endl;)

  PQ_TO_CACHE++;
  return PQ.occupancy();
}

void CACHE::return_data(PACKET* packet)
{
  // check MSHR information
  auto mshr_entry = std::find_if(MSHR.begin(), MSHR.end(), eq_addr<PACKET>(packet->address, OFFSET_BITS));
  auto first_unreturned = std::find_if(MSHR.begin(), MSHR.end(), [](auto x) { return x.event_cycle == std::numeric_limits<uint64_t>::max(); });

  // sanity check
  if (mshr_entry == MSHR.end()) {
    std::cerr << "[" << NAME << "_MSHR] " << __func__ << " instr_id: " << packet->instr_id << " cannot find a matching entry!";
    std::cerr << " address: " << std::hex << packet->address;
    std::cerr << " v_address: " << packet->v_address;
    std::cerr << " address: " << (packet->address >> OFFSET_BITS) << std::dec;
    std::cerr << " event: " << packet->event_cycle << " current: " << current_cycle << std::endl;
    assert(0);
  }

  // MSHR holds the most updated information about this request
  mshr_entry->data = packet->data;
  mshr_entry->pf_metadata = packet->pf_metadata;
  mshr_entry->event_cycle = current_cycle + (warmup_complete[cpu] ? FILL_LATENCY : 0);
  mshr_entry->mshr_return_data_invalid_count = packet->mshr_invalid_count;

  DP(if (warmup_complete[packet->cpu]) {
    std::cout << "[" << NAME << "_MSHR] " << __func__ << " instr_id: " << mshr_entry->instr_id;
    std::cout << " address: " << std::hex << (mshr_entry->address >> OFFSET_BITS) << " full_addr: " << mshr_entry->address;
    std::cout << " data: " << mshr_entry->data << std::dec;
    std::cout << " index: " << std::distance(MSHR.begin(), mshr_entry) << " occupancy: " << get_occupancy(0, 0);
    std::cout << " event: " << mshr_entry->event_cycle << " current: " << current_cycle << std::endl;
  });

  // Order this entry after previously-returned entries, but before non-returned
  // entries
  // TODO: ???? What's the meaning of this?
  std::iter_swap(mshr_entry, first_unreturned);
}

uint32_t CACHE::get_occupancy(uint8_t queue_type, uint64_t address)
{
  if (queue_type == 0)
    return std::count_if(MSHR.begin(), MSHR.end(), is_valid<PACKET>());
  else if (queue_type == 1)
    return RQ.occupancy();
  else if (queue_type == 2)
    return WQ.occupancy();
  else if (queue_type == 3)
    return PQ.occupancy();
  else if (queue_type == 4)
    return IVQ.occupancy();

  return 0;
}

uint32_t CACHE::get_size(uint8_t queue_type, uint64_t address)
{
  if (queue_type == 0)
    return MSHR_SIZE;
  else if (queue_type == 1)
    return RQ.size();
  else if (queue_type == 2)
    return WQ.size();
  else if (queue_type == 3)
    return PQ.size();
  else if (queue_type == 4)
    return IVQ.size();
  return 0;
}

bool CACHE::should_activate_prefetcher(int type) { return (1 << static_cast<int>(type)) & pref_activate_mask; }

void CACHE::print_deadlock()
{
  if (!std::empty(MSHR)) {
    std::cout << NAME << " MSHR Entry" << std::endl;
    std::size_t j = 0;
    for (PACKET entry : MSHR) {
      std::cout << "[" << NAME << " MSHR] entry: " << j++ << " instr_id: " << entry.instr_id;
      std::cout << " address: " << std::hex << (entry.address >> LOG2_BLOCK_SIZE) << " full_addr: " << entry.address << std::dec << " type: " << +entry.type;
      std::cout << " fill_level: " << +entry.fill_level << " event_cycle: " << entry.event_cycle << std::endl;
    }
  } else {
    std::cout << NAME << " MSHR empty" << std::endl;
  }
}
void CACHE::handle_invalid()
{
  // TODO: Invalidation request
  while (invalid_available_this_cycle > 0) {

    if (!IVQ.has_ready())
      return;

    // handle the oldest entry
    PACKET& handle_pkt = IVQ.front();

    assert(handle_pkt.type == INVALIDATE);

    DP(if (warmup_complete[handle_pkt.cpu]) {
      std::cout << "[" << NAME << "_HANDLE_INV] " << __func__ << " instr_id: " << handle_pkt.instr_id << " address: " << std::hex
                << (handle_pkt.address >> OFFSET_BITS);
      std::cout << " full_addr: " << handle_pkt.address << " v_address: " << handle_pkt.v_address << std::dec << " type: " << +handle_pkt.type
                << " occupancy: " << IVQ.occupancy() << " cycle: " << current_cycle;
    })

    auto mshr_entry = std::find_if(MSHR.begin(), MSHR.end(), eq_addr<PACKET>(handle_pkt.address, OFFSET_BITS));
    if (mshr_entry != MSHR.end()) {
      mshr_entry->mshr_invalid_count++;
    }

    uint32_t set = get_set(handle_pkt.address);
    uint32_t way = get_way(handle_pkt.address, set);

    if (handle_pkt.fill_level > fill_level) {
      if (way < NUM_WAY) {
        BLOCK& fill_block = block[set * NUM_WAY + way];
        if (fill_block.valid) {
          // HIT
          if ((!handle_pkt.data_valid) && fill_block.dirty) {
            handle_pkt.data = fill_block.data;
            handle_pkt.data_valid = true;
          }
          DP(if (warmup_complete[handle_pkt.cpu]) { std::cout << " hit-inv"; })
        }
      }
      auto result = lower_level->add_ivq(&handle_pkt);
      if (result == -2) {
        return;
      } else if (result == -3) {
        DP(if (warmup_complete[handle_pkt.cpu]) { std::cout << " merge_to_wq"; })
      } else if (result == -4) {
        DP(if (warmup_complete[handle_pkt.cpu]) { std::cout << " merge_to_ivq"; })
      }
      if (way < NUM_WAY && block[set * NUM_WAY + way].valid) {
        sim_hit[handle_pkt.cpu][handle_pkt.type]++;
      } else {
        sim_miss[handle_pkt.cpu][handle_pkt.type]++;
      }
      invalidate_entry(handle_pkt.address);

    } else if (handle_pkt.fill_level == fill_level) {
      if (way < NUM_WAY) {
        BLOCK& fill_block = block[set * NUM_WAY + way];
        if (handle_pkt.data_valid) {
          fill_block.data = handle_pkt.data;
          fill_block.dirty = true;
        }
        sim_hit[handle_pkt.cpu][handle_pkt.type]++;
        DP(if (warmup_complete[handle_pkt.cpu]) {
          std::cout << " finish-invalidation "
                    << "inv_ongoing: " << fill_block.inv_ongoing << " merge_count: " << handle_pkt.merge_count;
        })
      } else {
        if (handle_pkt.data_valid) {
          handle_pkt.fill_level = lower_level->fill_level;
          auto result = lower_level->add_ivq(&handle_pkt);
          if (result == -2) {
            return;
          } else if (result == -3) {
            DP(if (warmup_complete[handle_pkt.cpu]) { std::cout << " merge_to_wq"; })
          } else if (result == -4) {
            DP(if (warmup_complete[handle_pkt.cpu]) { std::cout << " merge_to_ivq"; })
          }
        }
      }
    } else {
      assert(0);
    }

    // remove this entry from IVQ
    IVQ.pop_front();
    invalid_available_this_cycle--;

    DP(if (warmup_complete[handle_pkt.cpu]) { std::cout << " Inv_req-complete"; })
    sim_access[handle_pkt.cpu][handle_pkt.type]++;
    DP(if (warmup_complete[handle_pkt.cpu]) { std::cout << " Access: " << sim_access[handle_pkt.cpu][INVALIDATE] << std::endl; })
  }
}

int CACHE::add_ivq(PACKET* packet)
{
  // assert(packet->address != 0);
  assert(packet->type == INVALIDATE);
  IVQ_ACCESS++;

  DP(if (warmup_complete[packet->cpu]) {
    std::cout << "[" << NAME << "_IVQ] " << __func__ << " instr_id: " << packet->instr_id << " address: " << std::hex << (packet->address >> OFFSET_BITS);
    std::cout << " full_addr: " << packet->address << " v_address: " << packet->v_address << std::dec << " type: " << +packet->type
              << " occupancy: " << IVQ.occupancy() << " fill_level: " << packet->fill_level << " cycle: " << current_cycle << std::endl;
  })

  // check occupancy
  if (IVQ.full()) {
    IVQ_FULL++;

    DP(if (warmup_complete[packet->cpu]) std::cout << " FULL" << std::endl;)

    return -2; // cannot handle this request
  }

  // TODO: Check Write Queue
  auto found_wq = std::find_if(WQ.begin(), WQ.end(), eq_addr<PACKET>(packet->address, OFFSET_BITS));
  if (found_wq != WQ.end()) {
    if (found_wq->fill_level == packet->fill_level) {
      if (packet->data_valid) {
        found_wq->data = packet->data;
      }
      found_wq->inv_ongoing -= packet->merge_count;
      assert(found_wq->inv_ongoing >= 0);
      return -3;
    } else if (found_wq->fill_level < packet->fill_level) {
      if (!packet->data_valid && found_wq->type != WRITEBACK_EXCLUSIVE) {
        packet->data = found_wq->data;
        packet->data_valid = true;
      }
      found_wq->type = NON_VALID;
    } else {
      assert(0);
    }
    IVQ_MERGED++;
  }

  auto found = std::find_if(IVQ.begin(), IVQ.end(), eq_addr<PACKET>(packet->address, OFFSET_BITS));
  if (found != IVQ.end()) {
    if (found->fill_level == packet->fill_level) {
      found->merge_count++;
    }
    found->fill_level = std::max(found->fill_level, packet->fill_level);
    IVQ_MERGED++;
    return -4;
  }

  // if there is no duplicate, add it to PQ
  if (warmup_complete[cpu])
    IVQ.push_back(*packet);
  else
    IVQ.push_back_ready(*packet);

  DP(if (warmup_complete[packet->cpu]) std::cout << " ADDED" << std::endl;)

  IVQ_TO_CACHE++;
  return IVQ.occupancy();
}

void CACHE::copy_packet(PACKET* old_packet, PACKET* new_packet)
{
  new_packet->scheduled = old_packet->scheduled;
  new_packet->type = old_packet->type;
  new_packet->fill_level = old_packet->fill_level;
  new_packet->pf_origin_level = old_packet->pf_origin_level;
  new_packet->pf_metadata = old_packet->pf_metadata;
  new_packet->cpu = old_packet->cpu;
  new_packet->mshr_invalid_count = old_packet->mshr_invalid_count;
  new_packet->mshr_return_data_invalid_count = old_packet->mshr_return_data_invalid_count;
  new_packet->inv_ongoing = old_packet->inv_ongoing;
  new_packet->merge_count = old_packet->merge_count;
  new_packet->data_valid = old_packet->data_valid;
  new_packet->address = old_packet->address;
  new_packet->v_address = old_packet->address;
  new_packet->data = old_packet->data;
  new_packet->instr_id = old_packet->instr_id;
  new_packet->ip = old_packet->ip;
  new_packet->event_cycle = old_packet->event_cycle;
  new_packet->cycle_enqueued = old_packet->cycle_enqueued;
  new_packet->translation_level = old_packet->translation_level;
  new_packet->init_translation_level = old_packet->init_translation_level;
  new_packet->asid[0] = old_packet->asid[0];
  new_packet->asid[1] = old_packet->asid[1];
  for (auto& i : old_packet->lq_index_depend_on_me) {
    new_packet->lq_index_depend_on_me.push_back(i);
  }
  for (auto& i : old_packet->sq_index_depend_on_me) {
    new_packet->sq_index_depend_on_me.push_back(i);
  }
  for (auto& i : old_packet->instr_depend_on_me) {
    new_packet->instr_depend_on_me.push_back(i);
  }
  for (auto& i : old_packet->to_return) {
    new_packet->to_return.push_back(i);
  }
}

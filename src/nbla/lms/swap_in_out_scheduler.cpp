// Copyright (c) 2017 Sony Corporation. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include <limits>

#include <nbla/lms/swap_in_out_scheduler.hpp>
#include <nbla/device_synchronizer_registry.hpp>
#include <nbla/singleton_manager.hpp>


namespace nbla {


SwapInOutScheduler::SwapInOutScheduler(const Context &h_ctx,
                                       const Context &d_ctx,
                                       const size_t bytes)
  : host_ctx(h_ctx), device_ctx(d_ctx),
    // Balancing the maximum GPU memory size for swap in/out in half
    max_bytes_swap_in(bytes), max_bytes_swap_out(bytes / 2),
    // Set SyncedArrayCallback function for first iteration
    synced_array_callback(
      [&](SyncedArrayPtr saptr, const SyncedArrayCallbackTag func_name,
          const dtypes dtype, const Context &ctx, const bool write_only) {
       synced_array_callback_recorder(saptr, func_name, dtype, 
                                      ctx, write_only); }) {}


SwapInOutScheduler::~SwapInOutScheduler() {}



//-----------------------------------------------------------------
//  Interfaces
//-----------------------------------------------------------------

// Start point of the scheduler
void SwapInOutScheduler::start_scheduling() {
  init();
  set_synced_array_callback();
}


// End point of scheduler
void SwapInOutScheduler::end_scheduling() {
  unset_synced_array_callback(); 
  finalize(); // This must be called after unset_synced_array_callback().
}


void SwapInOutScheduler::reset() {
  init();
  order.clear();
  synced_array_callback = 
    [&](SyncedArrayPtr saptr, const SyncedArrayCallbackTag func_name,
        const dtypes dtype, const Context &ctx, const bool write_only) {
    synced_array_callback_recorder(saptr, func_name, dtype, 
                                   ctx, write_only); };
}


void SwapInOutScheduler::
use_dali(const vector<std::array<const NdArrayPtr, 2>>& data_batches) {
  /* Dali data iterator uses double buffering, meaning that
     it recycles two NdArray alternately.
   */
  if (data_batches.empty()) {
    NBLA_ERROR(error_code::unclassified, "Input data is empty.");
  }
  else if (data_batches.size() > 1) {
    NBLA_ERROR(error_code::unclassified, 
               "SwapInOutSchedule cannot deal with multi GPU.");
  }

  auto x = data_batches[0][0];
  auto t = data_batches[0][1];

  if (iter_count == 0) {
    // Record one SyncedArray
    dali_sawptrs[0] = { x->array(), t->array() };
  }
  else if (iter_count == 1) {
    // Record SyncedArray ID of 0-th iteration
    for (int i = 0; i < 2; i++) {
      auto saptr = dali_sawptrs[0][i].lock();

      if (!saptr) {
        NBLA_ERROR(error_code::unclassified,
          "Double buffered NdArray used by Dali data iterator "
          "was unexpectedly expired.");
      }

      for (const auto& r : order) {
        if (r.sawptr.lock() == saptr) {
          dali_saptr_idxs[i] = r.synced_array_id;
          break;
        }
      }
    }

    // Record another SyncedArray
    dali_sawptrs[1] = { x->array(), t->array() };

    // Switch the SyncedArray
    for (int i = 0; i < 2; i++) {
      const auto saptr_id = dali_saptr_idxs[i];

      for (const auto j : synced_array_id_to_order_idx[saptr_id]) {
        order[j].sawptr = dali_sawptrs[1][i];
      }
    }
  }
  else {
    // Switch the recorded SyncedArray
    auto current_data_batch = iter_count % 2;

    for (int i = 0; i < 2; i++) {
      const auto saptr_id = dali_saptr_idxs[i];

      for (const auto j : synced_array_id_to_order_idx[saptr_id]) {
        order[j].sawptr = dali_sawptrs[current_data_batch][i];
      }
    }
  }
}


//----------------------------------------------------------------
//  Pre/post hook of function and update
//----------------------------------------------------------------

void SwapInOutScheduler::pre_function_callback(const CgFunctionPtr &ptr) { 
  pre_callback(); 
}

void SwapInOutScheduler::post_function_callback(const CgFunctionPtr &ptr) {}

void SwapInOutScheduler::pre_update_callback() { 
  pre_callback(); 
}

void SwapInOutScheduler::post_update_callback() {}



//----------------------------------------------------------------
//  Initialization
//----------------------------------------------------------------

// Initializer of the scheduler for each training iteration
void SwapInOutScheduler::init() {
  tail = 0;
  used_bytes_swap_out = 0;
  order_idx = 0;
  func_idx = 0;
  wrong_ordered.clear();
  precleared.clear();
  synced_array_id_mapper.clear();
  swapped_out.clear();
}


//----------------------------------------------------------------
//  Finalization
//----------------------------------------------------------------

// Finalizer of the scheduler for each training iteration
void SwapInOutScheduler::finalize() {
  // The post process of the last function of a network.
  if (func_idx > 0) {
    swap_out_step();
  }

  // Swap out all arrays out of the recorded order.
  // In the first iteration, wrong_ordered.size() is always zero.
  swap_out_wrong_order();

  // Wait to swap out all arrays before the next iteration.
  wait_for_all_swap_out();

  // Schedule the timings
  if (first_iter) {
    init();
    schedule();
  }
  
  /* Host should wait for the all asynchronous processes in GPU managed
  by the scheduler.

  If host does not wait, it is unsafe to modify the values of CPU memory.
  Example:
  1. An asynchronous data transfer (cudaMemcpyAsync) are
  writing or reading a CPU memory.
  2. Host process went out from the management of this scheduler.
  3. A user could change the values of that CPU memory used by cudaMemcpyAsync.
  4. Finally, data used by cudaMemcpyAsync was broken.

  In particular, one of the modification of CPU memory is the writing of
  the input data for the next training iteration.
  */
  DeviceSynchronizer::synchronize(device_ctx);
  
  // After first itration, not record
  synced_array_callback = 
    [&](SyncedArrayPtr saptr, const SyncedArrayCallbackTag func_name,
        const dtypes dtype, const Context &ctx, const bool write_only) {
    synced_array_callback_tracer(saptr, func_name, dtype, ctx, write_only); };

  first_iter = false;
  iter_count++;
}


void SwapInOutScheduler::swap_out_wrong_order() {
  for (int i = 0; i < wrong_ordered.size(); i++) {
    if (wrong_ordered[i].tag == RecTag::CLEAR) {
      continue;
    }

    if (wrong_ordered[i].ctx.array_class == device_ctx.array_class) {
      auto p = wrong_ordered[i].sawptr.lock();

      if (p && p->get_num_arrays() > 0) {
        // The array not cleared yet. Swap out the array synchronously
        p->cast(wrong_ordered[i].dtype, host_ctx, false);
      }
    }
    else if (wrong_ordered[i].ctx.array_class != host_ctx.array_class) {
      // Function used an array on an uncertain device
      NBLA_ERROR(error_code::type,
        "Unsupported array class: " + wrong_ordered[i].ctx.array_class);
    }
  }
}


//----------------------------------------------------------------
//  Scheduler
//----------------------------------------------------------------

void SwapInOutScheduler::schedule() {
  schedule_preclear(); // This is used in the schedule of swap out.

  /* This counts the number of same arrays in the queue.
  If count of an array > 0, no need to fetch the same array again.
  If count of an array > 1, no need to swap out the array because it is
  planed to be used soon in the queue.
  */
  int head = 0;
  size_t used_bytes_swap_in = 0;
  SyncedArrayCountsInQueue synced_array_counts;
  auto last_function = func_block_ends.size() - 1;

  // Virtually iterate all layer functions and solver update
  for (int fid = 0; fid < last_function; fid++) {
    swap_in_schedule[fid] = schedule_swap_in(head, used_bytes_swap_in, 
                                              synced_array_counts);

    if (head < func_block_ends[fid]) {
      NBLA_ERROR(error_code::memory,
        "Some arrays were not prefetched probably due to out of GPU memory. ");
    }

    swap_out_schedule[fid] = schedule_swap_out(used_bytes_swap_in, 
                                               synced_array_counts, fid);
    wait_schedule[fid] = schedule_wait_for_swap_out();
  }

  wait_schedule[last_function - 1] = schedule_wait_for_all_swap_out();
}


int accumulate_counts(const unordered_map<dtypes, int>& count_map) {
  return accumulate(count_map.begin(), count_map.end(), 0,
    [](int value, const unordered_map<dtypes, int>::value_type& p)
    { return value + p.second; });
}


SwapInOutScheduler::ScheduleType
SwapInOutScheduler::
schedule_swap_in(int& head, size_t& used_bytes_swap_in,
                 SyncedArrayCountsInQueue& synced_array_counts) {
  // If a cast of an array to host is recorded, prefetch should stop
  // This flag prevents prefetching this array.
  unordered_map<unsigned int, bool> host_uses_this_synced_array;
  SwapInOutScheduler::ScheduleType schedule;

  while (head < order.size()) {
    RecType& r = order[head];

    if (r.tag == RecTag::CLEAR) {
      head++;
      continue;
    }
    
    if (r.ctx.array_class == device_ctx.array_class) {
      auto next_array_bytes = r.size * sizeof_dtype(r.dtype);

      if (used_bytes_swap_in + next_array_bytes
          > max_bytes_swap_in - max_bytes_swap_out) {
        break; // Out of memory. Stop fetching.
      }

      // Fetch
      if (synced_array_counts[r.synced_array_id][r.dtype] == 0) {
        if (!host_uses_this_synced_array[r.synced_array_id]) {
          // The array is firstly appeared in the queue.
          schedule.push_back(r);

          // If the array was previously swapped out,
          // the memcpy was waited for by swap in.
          if (swapped_out[r.synced_array_id]) {
            // the array will not be swapped out.
            swapped_out_r[r.synced_array_id]->no_need_swap_out = true;
            swapped_out[r.synced_array_id] = false; // reset flag
          }
        }
        // Increase memory usage
        used_bytes_swap_in += next_array_bytes;
      }

      // Increment the number of the same SyncedArray in the queue.
      synced_array_counts[r.synced_array_id][r.dtype]++;
      head++; // Move on the head of the queue
    }
    else if (r.ctx.array_class == host_ctx.array_class) {
      // No need swap-in (prefetch) to CPU. The array will be gotten/casted 
      // synchronously by the function itself.
      // Stop prefetch until the end of use of the array.
      host_uses_this_synced_array[r.synced_array_id] = true;
      head++;
    }
    else {
      // Get/cast of an array on an uncertain device
      NBLA_ERROR(error_code::type, 
                 "Unsupported array type: " + r.ctx.array_class);
    }
  }

  return schedule;
}


SwapInOutScheduler::ScheduleType
SwapInOutScheduler::
schedule_swap_out(size_t& used_bytes_swap_in, 
                  SyncedArrayCountsInQueue& synced_array_counts, 
                  const int fid) {
  SwapInOutScheduler::ScheduleType schedule;

  for (size_t i = func_block_ends[fid - 1]; i < func_block_ends[fid]; i++) {
    RecType& r = order[i];

    if (r.tag == RecTag::CLEAR) {
      continue;
    }

    if (r.ctx.array_class == device_ctx.array_class) { 
      if (accumulate_counts(synced_array_counts[r.synced_array_id]) == 1) {
        // An array is swapped out when the same array is no longer
        // in the queue.
        schedule.push_back(r);

        if (!r.preclear) {
          r.swapped_out = true;
          swapped_out[r.synced_array_id] = true;
          swapped_out_r[r.synced_array_id] = &order[i];
        
          // Transfer memory usage of all types
          r.swapped_out_bytes = 0;

          for (auto it : synced_array_counts[r.synced_array_id]) {
            auto array_bytes = r.size * sizeof_dtype(it.first);
            used_bytes_swap_out += array_bytes;
            r.swapped_out_bytes += array_bytes;
          }
        }

        // Transfer memory usage of all types
        for (auto it : synced_array_counts[r.synced_array_id]) {
          auto array_bytes = r.size * sizeof_dtype(it.first);
          used_bytes_swap_in -= array_bytes;
        }
      }

      // Decrease the counts of a used array in the queue.
      synced_array_counts[r.synced_array_id][r.dtype]--;
    }
    else if (r.ctx.array_class != host_ctx.array_class) {
      // Get/cast of an array on an uncertain device
      NBLA_ERROR(error_code::type, 
                 "Unsupported array type: " + r.ctx.array_class);
    }
  }

  return schedule;
}


SwapInOutScheduler::ScheduleType
SwapInOutScheduler::schedule_wait_for_swap_out() {
  SwapInOutScheduler::ScheduleType schedule;

  // When out of memory, wait to finish swap-out and release memory.
  while (used_bytes_swap_out > max_bytes_swap_out) {
    schedule_wait_for_swap_out_impl(schedule);
  }

  return schedule;
}


SwapInOutScheduler::ScheduleType
SwapInOutScheduler::schedule_wait_for_all_swap_out() {
  SwapInOutScheduler::ScheduleType schedule;

  // When out of memory, wait to finish swap-out and release memory.
  while (tail < order.size()) {
    schedule_wait_for_swap_out_impl(schedule);
  }

  return schedule;
}


void SwapInOutScheduler::schedule_wait_for_swap_out_impl(
  SwapInOutScheduler::ScheduleType& schedule) {
  RecType& r = order[tail++];

  if (r.swapped_out) {
    // Wait to finish swap-out and release the source array of memory copy.
    schedule.push_back(r);

    // Decrease memory usage
    r.swapped_out = false;
    used_bytes_swap_out -= r.swapped_out_bytes;
    r.swapped_out_bytes = 0;

    swapped_out[r.synced_array_id] = false;
  }
}

/* Determine preclear timing

For the same SyncedArray, get/cast just before the clear is
time to preclear it instead of swap out it.
*/
void SwapInOutScheduler::schedule_preclear() {
  unordered_map<unsigned int, bool> clear_flag;

  for (int i = order.size() - 1; i >= 0; i--) {
    if (order[i].tag == RecTag::CLEAR) {
      clear_flag[order[i].synced_array_id] = true;
    }
    else {
      order[i].preclear = clear_flag[order[i].synced_array_id];
      clear_flag[order[i].synced_array_id] = false;
    }
  }
}



//----------------------------------------------------------------
//  Execution
//----------------------------------------------------------------

// Common implementation of pre callback
void SwapInOutScheduler::pre_callback() {
  unset_synced_array_callback(); // Avoid unnecessary record and trace

  if (func_idx > 0) {
    swap_out_step(); // post process of the previous function
  }

  swap_in_step(); // pre process of the next function
  
  set_synced_array_callback(); // Restart record or trace
}


// The post-process of the previous function.
void SwapInOutScheduler::swap_out_step() {
  // Record the end of a function.
  if (first_iter) {
    func_block_ends.push_back(order_idx);
  }

  // Swap out and preclear the arrays used in the previous function.
  swap_out();

  if (order_idx < func_block_ends[func_idx]) {
    /* If the number of get/cast/clear in this iteration is less than
       recorded one, reset the index the recorded start of the next function 
       in order to compare between recorded and actually used order in this iteration. 

       See synced_array_callback_tracer about the comparison.
    */
    order_idx = func_block_ends[func_idx];
  }
}


// The pre-process of the next function.
void SwapInOutScheduler::swap_in_step() {
  func_idx++;

  if (!first_iter) {
    swap_in(); // Prefetch arrays as possible.
  }
}


// Prefetch (swap in)
void SwapInOutScheduler::swap_in() {
  for (auto r : swap_in_schedule[func_idx - 1]) {
    if (auto p = r.get().sawptr.lock()) {
      p->get(r.get().dtype, r.get().ctx, AsyncFlag::ASYNC | AsyncFlag::UNSAFE);
    }
  }
}

/** Swap out
*/
void SwapInOutScheduler::swap_out() {
  // Swap out the arrays used in the previous function
  if (first_iter) {
    swap_out_first_iter();
    wait_for_swap_out_first_iter();
  }
  else {
    swap_out_scheduled();
    wait_for_swap_out_scheduled();
  }
}


void SwapInOutScheduler::swap_out_first_iter() {
  for (int i = func_block_ends[func_idx - 1]; 
           i < func_block_ends[func_idx]; 
           i++) {
    if (order[i].tag == RecTag::CLEAR) {
      continue;
    }

    if (order[i].ctx.array_class == device_ctx.array_class) {
      auto p = order[i].sawptr.lock();

      if (p && p->get_num_arrays() > 0) {
        // In the first iteration, arrays used in a function are 
        // always swapped out.
        // The array is not cleared yet. Swap out the array
        p->cast(p->dtype(), host_ctx, false,
                AsyncFlag::ASYNC | AsyncFlag::UNSAFE);

        auto array_bytes = p->size() * sizeof_dtype(p->dtype());
        used_bytes_swap_out += array_bytes; // Increase memory usage
        order[i].swapped_out = true;
        order[i].swapped_out_bytes = array_bytes;
      }
    }
    else if (order[i].ctx.array_class != host_ctx.array_class) {
      // Get/cast of an array on an uncertain device
      NBLA_ERROR(error_code::type, 
                 "Unsupported array type: " + order[i].ctx.array_class);
    }
  }
}

void SwapInOutScheduler::wait_for_swap_out_first_iter() {
  while (used_bytes_swap_out > max_bytes_swap_out) {
    wait_for_swap_out_first_iter_impl();
  }
}


void SwapInOutScheduler::wait_for_all_swap_out() {
  while (tail < order.size()) {
    wait_for_swap_out_first_iter_impl();
  }
}


void SwapInOutScheduler::wait_for_swap_out_first_iter_impl() {
  RecType& r = order[tail++];

  if (r.tag == RecTag::CLEAR) {
    return;
  }

  auto p = r.sawptr.lock();

  if (r.swapped_out) {
    // Wait to finish swap-out and release the source array of memory copy.
    // Swap out the array
    if (p && p->head_array_class() == host_ctx.array_class &&
        p->get_num_arrays() > 0) { 
      // Not cleared yet, in first iteration, precleaer cannot be used.
      p->get(p->dtype(), host_ctx, AsyncFlag::UNSAFE);
    }

    // Decrease memory usage
    r.swapped_out = false;
    used_bytes_swap_out -= r.swapped_out_bytes;
    r.swapped_out_bytes = 0;
  }
}


void  SwapInOutScheduler::swap_out_scheduled() {
  for (auto r : swap_out_schedule[func_idx - 1]) {
    if (auto p = r.get().sawptr.lock()) {
      if (r.get().preclear) {
        p->clear();
        precleared[p] = true;
      }
      else if (!r.get().no_need_swap_out) {
        p->cast(p->dtype(), host_ctx, false,
          AsyncFlag::ASYNC | AsyncFlag::UNSAFE);
      }
    }
  }
}


void SwapInOutScheduler::wait_for_swap_out_scheduled() {
  for (auto r : wait_schedule[func_idx - 1]) {
    if (r.get().no_need_swap_out) {
      continue;
    }

    auto p = r.get().sawptr.lock();

    if (p && p->head_array_class() == host_ctx.array_class &&
        p->get_num_arrays() > 0) {
        p->get(p->dtype(), host_ctx, AsyncFlag::UNSAFE);
    }
  }
}


//----------------------------------------------------------------
//  SyncedArrayCallback function
//----------------------------------------------------------------

// Setter of SyncedArray callback
void SwapInOutScheduler::set_synced_array_callback() {
  SingletonManager::
    get<SyncedArrayCallback>()->set_callback_func(synced_array_callback);
}

// Remover of SyncedArray callback
void SwapInOutScheduler::unset_synced_array_callback() {
  SingletonManager::get<SyncedArrayCallback>()->set_callback_func(nullptr);
}

/// SyncedArray callback function to record get/cast/clear in the first iteration.
void SwapInOutScheduler::
synced_array_callback_recorder(SyncedArrayPtr saptr,
                               const SyncedArrayCallbackTag func_name,
                               const dtypes dtype,
                               const Context &ctx,
                               const bool write_only) {
  if (func_idx == 0) {
    // Do not record before forward propagation starts.
    return;
  }

  auto tag = get_tag(func_name, write_only);

  if (synced_array_id_mapper.size() 
      > std::numeric_limits<unsigned int>::max()) {
    NBLA_ERROR(error_code::unclassified, 
               "Too many SyncedArray in excess of the max of unsigned int. " 
               "Please contact the developer to expand the size of SyncedArray ID.");
  }

  if (synced_array_id_mapper.find(saptr) == synced_array_id_mapper.end()) {
    auto next_id = static_cast<unsigned int>(synced_array_id_mapper.size());
    synced_array_id_mapper[saptr] = next_id;
  }

  order.push_back(RecType{tag, synced_array_id_mapper.at(saptr), saptr, 
                          saptr->size(), dtype, ctx, false, false, 0, false});
  synced_array_id_to_order_idx[synced_array_id_mapper.at(saptr)].push_back(order_idx);
  order_idx++;
}


// SyncedArray callback function to trace get/cast/clear after the first iteration.
void SwapInOutScheduler::
synced_array_callback_tracer(SyncedArrayPtr saptr,
                             const SyncedArrayCallbackTag func_name,
                             const dtypes dtype,
                             const Context &ctx,
                             const bool write_only) {
  if (func_idx == 0) {
    // Do not trace before forward propagation starts.
    return;
  }

  auto tag = get_tag(func_name, write_only);

  // Return an error when encounting get/cast between preclear and actual clear.
  // It could happens due to unpredicted change from the recorded order.
  if (precleared[saptr]) {
    if (tag == RecTag::CLEAR) {
      precleared[saptr] = false;
    }
    else {
      NBLA_ERROR(error_code::target_specific_async, "Re-get/cast precleared array.");
    }
  }

  auto rec_saptr = order[order_idx].sawptr.lock();

  // Compare between the real and recorded order.
  if (order_idx < func_block_ends[func_idx] &&
      (tag == order[order_idx].tag &&
       saptr != rec_saptr &&
       dtype == order[order_idx].dtype &&
       get_array_key_from_context(ctx) ==
       get_array_key_from_context(order[order_idx].ctx))) {
    // The SyncedArray is replaced in the current iteration.
    // Replace all recorded SyncedArray
    for (const auto i : synced_array_id_to_order_idx[order[order_idx].synced_array_id]) {
        order[i].sawptr = saptr;
    }
  }
  else if (order_idx >= func_block_ends[func_idx] ||
           (order_idx < func_block_ends[func_idx] && 
            (tag != order[order_idx].tag || 
             saptr != rec_saptr || 
             dtype != order[order_idx].dtype ||
             get_array_key_from_context(ctx) != 
             get_array_key_from_context(order[order_idx].ctx)))) {
    // The number of real get/cast/clear is larger than that of the recorded order,
    // or the orders are different
    wrong_ordered.push_back({tag, 0, saptr, saptr->size(), dtype, ctx, 
                             false, false, 0, false});
  }

  order_idx++;
}


// Conversion of the recorded tag
SwapInOutScheduler::RecTag
SwapInOutScheduler::get_tag(const SyncedArrayCallbackTag func_name,
  const bool write_only) {
  switch (func_name) {
  case SyncedArrayCallbackTag::GET:
    return RecTag::GETCAST;
    break;
  case SyncedArrayCallbackTag::CAST:
    return RecTag::GETCAST;
    break;
  case SyncedArrayCallbackTag::CLEAR:
    return RecTag::CLEAR;
    break;
  default:
    NBLA_ERROR(error_code::type, "Unsupported SyncedArrayCallbackTag");
    break;
  }
}
}

// Copyright 2019 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/objects/backing-store.h"
#include "src/execution/isolate.h"
#include "src/handles/global-handles.h"
#include "src/logging/counters.h"
#include "src/wasm/wasm-engine.h"
#include "src/wasm/wasm-limits.h"
#include "src/wasm/wasm-objects-inl.h"

#define TRACE_BS(...) /* redefine for tracing output */

namespace v8 {
namespace internal {

namespace {
#if V8_TARGET_ARCH_64_BIT
constexpr bool kUseGuardRegions = true;
#else
constexpr bool kUseGuardRegions = false;
#endif

#if V8_TARGET_ARCH_MIPS64
// MIPS64 has a user space of 2^40 bytes on most processors,
// address space limits needs to be smaller.
constexpr size_t kAddressSpaceLimit = 0x4000000000L;  // 256 GiB
#elif V8_TARGET_ARCH_64_BIT
constexpr size_t kAddressSpaceLimit = 0x10100000000L;  // 1 TiB + 4 GiB
#else
constexpr size_t kAddressSpaceLimit = 0xC0000000;  // 3 GiB
#endif

constexpr uint64_t GB = 1024 * 1024 * 1024;
constexpr uint64_t kNegativeGuardSize = 2 * GB;
constexpr uint64_t kFullGuardSize = 10 * GB;

std::atomic<uint64_t> reserved_address_space_{0};

// Allocation results are reported to UMA
//
// See wasm_memory_allocation_result in counters.h
enum class AllocationStatus {
  kSuccess,  // Succeeded on the first try

  kSuccessAfterRetry,  // Succeeded after garbage collection

  kAddressSpaceLimitReachedFailure,  // Failed because Wasm is at its address
                                     // space limit

  kOtherFailure  // Failed for an unknown reason
};

base::AddressRegion GetGuardedRegion(void* buffer_start, size_t byte_length) {
  // Guard regions always look like this:
  // |xxx(2GiB)xxx|.......(4GiB)..xxxxx|xxxxxx(4GiB)xxxxxx|
  //              ^ buffer_start
  //                              ^ byte_length
  // ^ negative guard region           ^ positive guard region

  Address start = reinterpret_cast<Address>(buffer_start);
  DCHECK_EQ(8, sizeof(size_t));  // only use on 64-bit
  DCHECK_EQ(0, start % AllocatePageSize());
  return base::AddressRegion(start - (2 * GB),
                             static_cast<size_t>(kFullGuardSize));
}

void RecordStatus(Isolate* isolate, AllocationStatus status) {
  isolate->counters()->wasm_memory_allocation_result()->AddSample(
      static_cast<int>(status));
}

inline void DebugCheckZero(void* start, size_t byte_length) {
#if DEBUG
  // Double check memory is zero-initialized.
  const byte* bytes = reinterpret_cast<const byte*>(start);
  for (size_t i = 0; i < byte_length; i++) {
    DCHECK_EQ(0, bytes[i]);
  }
#endif
}
}  // namespace

bool BackingStore::ReserveAddressSpace(uint64_t num_bytes) {
  uint64_t reservation_limit = kAddressSpaceLimit;
  while (true) {
    uint64_t old_count = reserved_address_space_.load();
    if (old_count > reservation_limit) return false;
    if (reservation_limit - old_count < num_bytes) return false;
    if (reserved_address_space_.compare_exchange_weak(old_count,
                                                      old_count + num_bytes)) {
      return true;
    }
  }
}

void BackingStore::ReleaseReservation(uint64_t num_bytes) {
  uint64_t old_reserved = reserved_address_space_.fetch_sub(num_bytes);
  USE(old_reserved);
  DCHECK_LE(num_bytes, old_reserved);
}

// The backing store for a Wasm shared memory has a doubly linked list
// of weak global handles to the attached memory objects. The list
// is used to broadcast updates when a shared memory is grown.
struct SharedWasmMemoryData {
  SharedWasmMemoryData* next_;
  SharedWasmMemoryData* prev_;
  Isolate* isolate_;
  // A global (weak) handle to the memory object. Note that this handle
  // is destroyed by the finalizer of the memory object, so it need not
  // be manually destroyed here.
  Handle<WasmMemoryObject> memory_object_;

  SharedWasmMemoryData(Isolate* isolate, Handle<WasmMemoryObject> memory_object)
      : next_(nullptr),
        prev_(nullptr),
        isolate_(isolate),
        memory_object_(memory_object) {}

  SharedWasmMemoryData* unlink() {
    auto next = next_;
    if (prev_) prev_->next_ = next_;
    if (next_) next_->prev_ = prev_;
    return next;
  }
};

void BackingStore::Clear() {
  buffer_start_ = nullptr;
  byte_length_ = 0;
  has_guard_regions_ = false;
  type_specific_data_.v8_api_array_buffer_allocator = nullptr;
}

BackingStore::~BackingStore() {
  if (globally_registered_) {
    GlobalBackingStoreRegistry::Unregister(this);
    globally_registered_ = false;
  }

  if (buffer_start_ == nullptr) return;  // nothing to deallocate

  if (is_wasm_memory_) {
    if (is_shared_) {
      // Deallocate the list of attached memory objects.
      SharedWasmMemoryData* list = get_shared_wasm_memory_data();
      while (list) {
        auto old = list;
        list = list->next_;
        delete old;
      }
      type_specific_data_.shared_wasm_memory_data = nullptr;
    }

    // Wasm memories are always allocated through the page allocator.
    auto region =
        has_guard_regions_
            ? GetGuardedRegion(buffer_start_, byte_length_)
            : base::AddressRegion(reinterpret_cast<Address>(buffer_start_),
                                  byte_capacity_);
    bool pages_were_freed =
        region.size() == 0 /* no need to free any pages */ ||
        FreePages(GetPlatformPageAllocator(),
                  reinterpret_cast<void*>(region.begin()), region.size());
    CHECK(pages_were_freed);
    BackingStore::ReleaseReservation(has_guard_regions_ ? kFullGuardSize
                                                        : byte_capacity_);
    Clear();
    return;
  }
  if (free_on_destruct_) {
    // JSArrayBuffer backing store. Deallocate through the embedder's allocator.
    auto allocator = reinterpret_cast<v8::ArrayBuffer::Allocator*>(
        get_v8_api_array_buffer_allocator());
    TRACE_BS("BS:free bs=%p mem=%p (%zu bytes)\n", this, buffer_start_,
             byte_capacity_);
    allocator->Free(buffer_start_, byte_length_);
  }
  Clear();
}

// Allocate a backing store using the array buffer allocator from the embedder.
std::unique_ptr<BackingStore> BackingStore::Allocate(
    Isolate* isolate, size_t byte_length, SharedFlag shared,
    InitializedFlag initialized) {
  void* buffer_start = nullptr;
  auto allocator = isolate->array_buffer_allocator();
  CHECK_NOT_NULL(allocator);
  if (byte_length != 0) {
    auto counters = isolate->counters();
    int mb_length = static_cast<int>(byte_length / MB);
    if (mb_length > 0) {
      counters->array_buffer_big_allocations()->AddSample(mb_length);
    }
    if (shared == SharedFlag::kShared) {
      counters->shared_array_allocations()->AddSample(mb_length);
    }
    if (initialized == InitializedFlag::kZeroInitialized) {
      buffer_start = allocator->Allocate(byte_length);
      if (buffer_start) {
        // TODO(titzer): node does not implement the zero-initialization API!
        constexpr bool
            kDebugCheckZeroDisabledDueToNodeNotImplementingZeroInitAPI = true;
        if ((!(kDebugCheckZeroDisabledDueToNodeNotImplementingZeroInitAPI)) &&
            !FLAG_mock_arraybuffer_allocator) {
          DebugCheckZero(buffer_start, byte_length);
        }
      }
    } else {
      buffer_start = allocator->AllocateUninitialized(byte_length);
    }
    if (buffer_start == nullptr) {
      // Allocation failed.
      counters->array_buffer_new_size_failures()->AddSample(mb_length);
      return {};
    }
  }

  auto result = new BackingStore(buffer_start,  // start
                                 byte_length,   // length
                                 byte_length,   // capacity
                                 shared,        // shared
                                 false,         // is_wasm_memory
                                 true,          // free_on_destruct
                                 false);        // has_guard_regions

  TRACE_BS("BS:alloc bs=%p mem=%p (%zu bytes)\n", result,
           result->buffer_start(), byte_length);
  result->type_specific_data_.v8_api_array_buffer_allocator = allocator;
  return std::unique_ptr<BackingStore>(result);
}

// Allocate a backing store for a Wasm memory. Always use the page allocator
// and add guard regions.
std::unique_ptr<BackingStore> BackingStore::TryAllocateWasmMemory(
    Isolate* isolate, size_t initial_pages, size_t maximum_pages,
    SharedFlag shared) {
  bool guards = kUseGuardRegions;

  // For accounting purposes, whether a GC was necessary.
  bool did_retry = false;

  // A helper to try running a function up to 3 times, executing a GC
  // if the first and second attempts failed.
  auto gc_retry = [&](const std::function<bool()>& fn) {
    for (int i = 0; i < 3; i++) {
      if (fn()) return true;
      // Collect garbage and retry.
      did_retry = true;
      // TODO(wasm): try Heap::EagerlyFreeExternalMemory() first?
      isolate->heap()->MemoryPressureNotification(
          MemoryPressureLevel::kCritical, true);
    }
    return false;
  };

  // Compute size of reserved memory.
  size_t reservation_size = 0;
  size_t byte_capacity = 0;

  if (guards) {
    reservation_size = static_cast<size_t>(kFullGuardSize);
    byte_capacity =
        static_cast<size_t>(wasm::kV8MaxWasmMemoryPages * wasm::kWasmPageSize);
  } else {
    reservation_size = std::min(maximum_pages, wasm::kV8MaxWasmMemoryPages) *
                       wasm::kWasmPageSize;
    byte_capacity = reservation_size;
  }

  //--------------------------------------------------------------------------
  // 1. Enforce maximum address space reservation per engine.
  //--------------------------------------------------------------------------
  auto reserve_memory_space = [&] {
    return BackingStore::ReserveAddressSpace(reservation_size);
  };

  if (!gc_retry(reserve_memory_space)) {
    // Crash on out-of-memory if the correctness fuzzer is running.
    if (FLAG_correctness_fuzzer_suppressions) {
      FATAL("could not allocate wasm memory backing store");
    }
    RecordStatus(isolate, AllocationStatus::kAddressSpaceLimitReachedFailure);
    return {};
  }

  //--------------------------------------------------------------------------
  // 2. Allocate pages (inaccessible by default).
  //--------------------------------------------------------------------------
  void* allocation_base = nullptr;
  auto allocate_pages = [&] {
    allocation_base =
        AllocatePages(GetPlatformPageAllocator(), nullptr, reservation_size,
                      wasm::kWasmPageSize, PageAllocator::kNoAccess);
    return allocation_base != nullptr;
  };
  if (!gc_retry(allocate_pages)) {
    // Page allocator could not reserve enough pages.
    BackingStore::ReleaseReservation(reservation_size);
    RecordStatus(isolate, AllocationStatus::kOtherFailure);
    return {};
  }

  // Get a pointer to the start of the buffer, skipping negative guard region
  // if necessary.
  byte* buffer_start = reinterpret_cast<byte*>(allocation_base) +
                       (guards ? kNegativeGuardSize : 0);

  //--------------------------------------------------------------------------
  // 3. Commit the initial pages (allow read/write).
  //--------------------------------------------------------------------------
  size_t byte_length = initial_pages * wasm::kWasmPageSize;
  auto commit_memory = [&] {
    return byte_length == 0 ||
           SetPermissions(GetPlatformPageAllocator(), buffer_start, byte_length,
                          PageAllocator::kReadWrite);
  };
  if (!gc_retry(commit_memory)) {
    // SetPermissions put us over the process memory limit.
    V8::FatalProcessOutOfMemory(nullptr, "BackingStore::AllocateWasmMemory()");
  }

  DebugCheckZero(buffer_start, byte_length);  // touch the bytes.

  RecordStatus(isolate, did_retry ? AllocationStatus::kSuccessAfterRetry
                                  : AllocationStatus::kSuccess);

  auto result = new BackingStore(buffer_start,   // start
                                 byte_length,    // length
                                 byte_capacity,  // capacity
                                 shared,         // shared
                                 true,           // is_wasm_memory
                                 true,           // free_on_destruct
                                 guards);        // has_guard_regions

  // Shared Wasm memories need an anchor for the memory object list.
  if (shared == SharedFlag::kShared) {
    result->type_specific_data_.shared_wasm_memory_data =
        new SharedWasmMemoryData(nullptr, Handle<WasmMemoryObject>());
  }

  return std::unique_ptr<BackingStore>(result);
}

// Allocate a backing store for a Wasm memory. Always use the page allocator
// and add guard regions.
std::unique_ptr<BackingStore> BackingStore::AllocateWasmMemory(
    Isolate* isolate, size_t initial_pages, size_t maximum_pages,
    SharedFlag shared) {
  // Wasm pages must be a multiple of the allocation page size.
  DCHECK_EQ(0, wasm::kWasmPageSize % AllocatePageSize());

  // Enforce engine limitation on the maximum number of pages.
  if (initial_pages > wasm::kV8MaxWasmMemoryPages) return nullptr;

  auto backing_store =
      TryAllocateWasmMemory(isolate, initial_pages, maximum_pages, shared);
  if (!backing_store && maximum_pages > initial_pages) {
    // If allocating the maximum failed, try allocating with maximum set to
    // initial
    backing_store =
        TryAllocateWasmMemory(isolate, initial_pages, initial_pages, shared);
  }
  return backing_store;
}

std::unique_ptr<BackingStore> BackingStore::CopyWasmMemory(
    Isolate* isolate, std::shared_ptr<BackingStore> old,
    size_t new_byte_length) {
  DCHECK_GE(new_byte_length, old->byte_length());
  // Note that we could allocate uninitialized to save initialization cost here,
  // but since Wasm memories are allocated by the page allocator, the zeroing
  // cost is already built-in.
  // TODO(titzer): should we use a suitable maximum here?
  auto new_backing_store = BackingStore::AllocateWasmMemory(
      isolate, new_byte_length / wasm::kWasmPageSize,
      new_byte_length / wasm::kWasmPageSize,
      old->is_shared() ? SharedFlag::kShared : SharedFlag::kNotShared);

  if (!new_backing_store ||
      new_backing_store->has_guard_regions() != old->has_guard_regions()) {
    return {};
  }

  size_t old_size = old->byte_length();
  memcpy(new_backing_store->buffer_start(), old->buffer_start(), old_size);

  return new_backing_store;
}

// Try to grow the size of a wasm memory in place, without realloc + copy.
bool BackingStore::GrowWasmMemoryInPlace(Isolate* isolate,
                                         size_t new_byte_length) {
  DCHECK(is_wasm_memory_);
  DCHECK_EQ(0, new_byte_length % wasm::kWasmPageSize);
  if (new_byte_length <= byte_length_) {
    return true;  // already big enough.
  }
  if (byte_capacity_ < new_byte_length) {
    return false;  // not enough capacity.
  }
  // Try to adjust the guard regions.
  DCHECK_NOT_NULL(buffer_start_);
  // If adjusting permissions fails, propagate error back to return
  // failure to grow.
  if (!i::SetPermissions(GetPlatformPageAllocator(), buffer_start_,
                         new_byte_length, PageAllocator::kReadWrite)) {
    return false;
  }
  reinterpret_cast<v8::Isolate*>(isolate)
      ->AdjustAmountOfExternalAllocatedMemory(new_byte_length - byte_length_);
  byte_length_ = new_byte_length;
  return true;
}

void BackingStore::AttachSharedWasmMemoryObject(
    Isolate* isolate, Handle<WasmMemoryObject> memory_object) {
  DCHECK(is_wasm_memory_);
  DCHECK(is_shared_);
  // We need to take the global registry lock for this operation.
  GlobalBackingStoreRegistry::AddSharedWasmMemoryObject(isolate, this,
                                                        memory_object);
}

void BackingStore::BroadcastSharedWasmMemoryGrow(
    Isolate* isolate, std::shared_ptr<BackingStore> backing_store,
    size_t new_size) {
  // requires the global registry lock.
  GlobalBackingStoreRegistry::BroadcastSharedWasmMemoryGrow(
      isolate, backing_store, new_size);
}

void BackingStore::RemoveSharedWasmMemoryObjects(Isolate* isolate) {
  // requires the global registry lock.
  GlobalBackingStoreRegistry::Purge(isolate);
}

void BackingStore::UpdateSharedWasmMemoryObjects(Isolate* isolate) {
  // requires the global registry lock.
  GlobalBackingStoreRegistry::UpdateSharedWasmMemoryObjects(isolate);
}

std::unique_ptr<BackingStore> BackingStore::WrapAllocation(
    Isolate* isolate, void* allocation_base, size_t allocation_length,
    SharedFlag shared, bool free_on_destruct) {
  auto result =
      new BackingStore(allocation_base, allocation_length, allocation_length,
                       shared, false, free_on_destruct, false);
  result->type_specific_data_.v8_api_array_buffer_allocator =
      isolate->array_buffer_allocator();
  TRACE_BS("BS:wrap bs=%p mem=%p (%zu bytes)\n", result, result->buffer_start(),
           result->byte_length());
  return std::unique_ptr<BackingStore>(result);
}

void* BackingStore::get_v8_api_array_buffer_allocator() {
  CHECK(!is_wasm_memory_);
  auto array_buffer_allocator =
      type_specific_data_.v8_api_array_buffer_allocator;
  CHECK_NOT_NULL(array_buffer_allocator);
  return array_buffer_allocator;
}

SharedWasmMemoryData* BackingStore::get_shared_wasm_memory_data() {
  CHECK(is_wasm_memory_ && is_shared_);
  auto shared_wasm_memory_data = type_specific_data_.shared_wasm_memory_data;
  CHECK(shared_wasm_memory_data);
  return shared_wasm_memory_data;
}

namespace {
// Implementation details of GlobalBackingStoreRegistry.
struct GlobalBackingStoreRegistryImpl {
  GlobalBackingStoreRegistryImpl() {}
  base::Mutex mutex_;
  std::unordered_map<const void*, std::weak_ptr<BackingStore>> map_;
};
base::LazyInstance<GlobalBackingStoreRegistryImpl>::type global_registry_impl_ =
    LAZY_INSTANCE_INITIALIZER;
inline GlobalBackingStoreRegistryImpl* impl() {
  return global_registry_impl_.Pointer();
}

void NopFinalizer(const v8::WeakCallbackInfo<void>& data) {
  Address* global_handle_location =
      reinterpret_cast<Address*>(data.GetParameter());
  GlobalHandles::Destroy(global_handle_location);
}
}  // namespace

void GlobalBackingStoreRegistry::Register(
    std::shared_ptr<BackingStore> backing_store) {
  if (!backing_store) return;

  base::MutexGuard scope_lock(&impl()->mutex_);
  if (backing_store->globally_registered_) return;
  TRACE_BS("BS:reg bs=%p mem=%p (%zu bytes)\n", backing_store.get(),
           backing_store->buffer_start(), backing_store->byte_length());
  std::weak_ptr<BackingStore> weak = backing_store;
  auto result = impl()->map_.insert({backing_store->buffer_start(), weak});
  CHECK(result.second);
  backing_store->globally_registered_ = true;
}

void GlobalBackingStoreRegistry::Unregister(BackingStore* backing_store) {
  if (!backing_store->globally_registered_) return;

  base::MutexGuard scope_lock(&impl()->mutex_);
  const auto& result = impl()->map_.find(backing_store->buffer_start());
  if (result != impl()->map_.end()) {
    auto shared = result->second.lock();
    if (shared) {
      DCHECK_EQ(backing_store, shared.get());
    }
    impl()->map_.erase(result);
  }
  backing_store->globally_registered_ = false;
}

std::shared_ptr<BackingStore> GlobalBackingStoreRegistry::Lookup(
    void* buffer_start, size_t length) {
  base::MutexGuard scope_lock(&impl()->mutex_);
  TRACE_BS("bs:lookup mem=%p (%zu bytes)\n", buffer_start, length);
  const auto& result = impl()->map_.find(buffer_start);
  if (result == impl()->map_.end()) {
    return std::shared_ptr<BackingStore>();
  }
  auto backing_store = result->second.lock();
  DCHECK_EQ(buffer_start, backing_store->buffer_start());
  DCHECK_EQ(length, backing_store->byte_length());
  return backing_store;
}

void GlobalBackingStoreRegistry::Purge(Isolate* isolate) {
  base::MutexGuard scope_lock(&impl()->mutex_);
  // Purge all entries in the map that refer to the given isolate.
  for (auto& entry : impl()->map_) {
    auto backing_store = entry.second.lock();
    if (!backing_store) continue;  // skip entries where weak ptr is null
    if (!backing_store->is_wasm_memory()) continue;  // skip non-wasm memory
    SharedWasmMemoryData* list = backing_store->get_shared_wasm_memory_data();
    while (list) {
      if (list->isolate_ == isolate) {
        // Unlink and delete the entry.
        auto old = list;
        list = list->unlink();
        delete old;
        continue;
      }
      list = list->next_;
    }
  }
}

void GlobalBackingStoreRegistry::AddSharedWasmMemoryObject(
    Isolate* isolate, BackingStore* backing_store,
    Handle<WasmMemoryObject> memory_object) {
  // Create a weak global handle to the memory object.
  Handle<WasmMemoryObject> weak_memory =
      isolate->global_handles()->Create<WasmMemoryObject>(*memory_object);
  Address* global_handle_location = weak_memory.location();
  GlobalHandles::MakeWeak(global_handle_location, global_handle_location,
                          &NopFinalizer, v8::WeakCallbackType::kParameter);
  SharedWasmMemoryData* entry = new SharedWasmMemoryData(isolate, weak_memory);

  base::MutexGuard scope_lock(&impl()->mutex_);
  SharedWasmMemoryData* list = backing_store->get_shared_wasm_memory_data();
  SharedWasmMemoryData* next = list->next_;

  if (next) {
    next->prev_ = entry;
    entry->next_ = next;
  }
  list->next_ = entry;
  entry->prev_ = list;
}

void GlobalBackingStoreRegistry::BroadcastSharedWasmMemoryGrow(
    Isolate* isolate, std::shared_ptr<BackingStore> backing_store,
    size_t new_size) {
  HandleScope scope(isolate);
  std::vector<Handle<WasmMemoryObject>> memory_objects;
  {
    // We must gather the memory objects to update while holding the
    // the lock, but we cannot allocate while we hold the lock, because
    // a GC may cause another backing store to be deleted and unregistered,
    // which also tries to take the lock.
    base::MutexGuard scope_lock(&impl()->mutex_);
    SharedWasmMemoryData* list =
        backing_store->get_shared_wasm_memory_data()->next_;
    while (list) {
      if (list->isolate_ == isolate && !list->memory_object_.is_null()) {
        memory_objects.push_back(list->memory_object_);
      } else {
        list->isolate_->stack_guard()->RequestGrowSharedMemory();
      }
      list = list->next_;
    }
  }
  // Update memory objects without the lock held (GC may free backing stores).
  // Note that we only gathered memory objects from this isolate, in which
  // we are currently running. Therefore there cannot be any new (relevant)
  // memory objects which are constructed, and none of the gathered memory
  // objects can die.
  for (auto memory_object : memory_objects) {
    Handle<JSArrayBuffer> new_buffer =
        isolate->factory()->NewJSSharedArrayBuffer();
    new_buffer->Attach(backing_store);
    memory_object->update_instances(isolate, new_buffer);
  }
}

void GlobalBackingStoreRegistry::UpdateSharedWasmMemoryObjects(
    Isolate* isolate) {
  HandleScope scope(isolate);
  std::vector<std::shared_ptr<BackingStore>> backing_stores;
  std::vector<Handle<WasmMemoryObject>> memory_objects;
  {
    // We must gather the memory objects to update while holding the
    // the lock, but we cannot allocate while we hold the lock, because
    // a GC may cause another backing store to be deleted and unregistered,
    // which also tries to take the lock.
    base::MutexGuard scope_lock(&impl()->mutex_);
    for (auto& entry : impl()->map_) {
      auto backing_store = entry.second.lock();
      if (!backing_store) continue;  // skip entries where weak ptr is null
      if (!backing_store->is_wasm_memory()) continue;  // skip non-wasm memory
      SharedWasmMemoryData* list =
          backing_store->get_shared_wasm_memory_data()->next_;
      while (list) {
        Handle<WasmMemoryObject> memory_object = list->memory_object_;
        if (list->isolate_ == isolate && !memory_object.is_null()) {
          backing_stores.push_back(backing_store);
          memory_objects.push_back(memory_object);
        }
        list = list->next_;
      }
    }
  }
  // Update memory objects without the lock held (GC may free backing stores).
  // Note that we only gathered memory objects from this isolate, in which
  // we are currently running. Therefore there cannot be any new (relevant)
  // memory objects which are constructed, and none of the gathered memory
  // objects can die.
  for (size_t i = 0; i < backing_stores.size(); i++) {
    auto backing_store = backing_stores[i];
    auto memory_object = memory_objects[i];
    Handle<JSArrayBuffer> old_buffer(memory_object->array_buffer(), isolate);
    if (old_buffer->byte_length() != backing_store->byte_length()) {
      Handle<JSArrayBuffer> new_buffer =
          isolate->factory()->NewJSSharedArrayBuffer();
      new_buffer->Attach(backing_store);
      memory_object->update_instances(isolate, new_buffer);
    }
  }
}

}  // namespace internal
}  // namespace v8

#undef TRACE_BS
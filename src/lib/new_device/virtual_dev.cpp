/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <sisl/fds/buffer.hpp>
#include <sisl/metrics/metrics.hpp>
#include <sisl/logging/logging.h>
#include <sisl/utility/atomic_counter.hpp>
#include <iomgr/iomgr_flip.hpp>

// #include <homestore/homestore.hpp>
#include "new_device/physical_dev.hpp"
#include "new_device/new_device.h"
#include "new_device/virtual_dev.hpp"
#include "new_device/chunk.h"
#include "common/error.h"
#include "common/homestore_assert.hpp"
#include "common/homestore_utils.hpp"
#include "blkalloc/varsize_blk_allocator.h"

SISL_LOGGING_DECL(device)

namespace homestore {

static std::shared_ptr< BlkAllocator > create_blk_allocator(blk_allocator_type_t btype, uint32_t vblock_size,
                                                            uint32_t ppage_sz, uint32_t align_sz, uint64_t size,
                                                            bool is_auto_recovery, uint32_t unique_id, bool is_init) {
#ifdef ENABLE_LATER
    switch (btype) {
    case blk_allocator_type_t::fixed: {
        BlkAllocConfig cfg{vblock_size, align_sz, size, std::string{"fixed_chunk_"} + std::to_string(unique_id)};
        cfg.set_auto_recovery(is_auto_recovery);
        return std::make_shared< FixedBlkAllocator >(cfg, is_init, unique_id);
    }
    case blk_allocator_type_t::varsize: {
        VarsizeBlkAllocConfig cfg{vblock_size,
                                  ppage_sz,
                                  align_sz,
                                  size,
                                  std::string("varsize_chunk_") + std::to_string(unique_id),
                                  true /* realtime_bitmap */,
                                  is_data_drive_hdd() ? false : true /* use_slabs */};
        HS_DBG_ASSERT_EQ((size % MIN_DATA_CHUNK_SIZE(ppage_sz)), 0);
        cfg.set_auto_recovery(is_auto_recovery);
        return std::make_shared< VarsizeBlkAllocator >(cfg, is_init, unique_id);
    }
    case blk_allocator_type_t::none:
    default:
        return nullptr;
    }
#else
    return nullptr;
#endif
}

VirtualDev::VirtualDev(DeviceManager& dmgr, const vdev_info& vinfo, blk_allocator_type_t allocator_type,
                       chunk_selector_type_t chunk_selector, bool is_auto_recovery) :
        m_vdev_info{vinfo},
        m_dmgr{dmgr},
        m_name{vinfo.name},
        m_metrics{vinfo.name},
        m_allocator_type{allocator_type},
        m_chunk_selector_type{chunk_selector},
        m_auto_recovery{is_auto_recovery} {
    m_chunk_selector = std::make_unique< RoundRobinChunkSelector >(false /* dynamically add chunk */);
}

void VirtualDev::add_chunk(cshared< Chunk >& chunk, bool is_fresh_chunk) {
    std::unique_lock lg{m_mgmt_mutex};
    auto ba = create_blk_allocator(m_allocator_type, block_size(), chunk->physical_dev()->optimal_page_size(),
                                   chunk->physical_dev()->align_size(), chunk->size(), m_auto_recovery,
                                   chunk->chunk_id(), is_fresh_chunk);
    chunk->set_block_allocator(std::move(ba));
    m_pdevs.insert(chunk->physical_dev_mutable());
    m_chunk_selector->add_chunk(chunk);
}

folly::Future< bool > VirtualDev::async_format() {
    static thread_local std::vector< folly::Future< bool > > s_futs;
    s_futs.clear();

    m_chunk_selector->foreach_chunks([&](cshared< Chunk >& chunk) {
        auto* pdev = chunk->physical_dev_mutable();
        LOGINFO("writing zero for chunk: {}, size: {}, offset: {}", chunk->chunk_id(), in_bytes(chunk->size()),
                chunk->start_offset());
        s_futs.emplace_back(pdev->async_write_zero(chunk->size(), chunk->start_offset()));
    });
    return folly::collectAllUnsafe(s_futs).thenTry([](auto&&) { return folly::makeFuture< bool >(true); });
}

/*std::shared_ptr< blkalloc_cp > VirtualDev::attach_prepare_cp(const std::shared_ptr< blkalloc_cp >& cur_ba_cp) {
    return (Chunk::attach_prepare_cp(cur_ba_cp));
}*/

bool VirtualDev::is_blk_alloced(const BlkId& blkid) const {
    return m_dmgr.get_chunk(blkid.get_chunk_num())->blk_allocator()->is_blk_alloced(blkid);
}

#ifdef ENABLE_LATER
BlkAllocStatus VirtualDev::commit_blk(const BlkId& blkid) {
    Chunk* chunk = m_dmgr.get_chunk_mutable(blkid.get_chunk_num());
    HS_LOG(DEBUG, device, "commit_blk: bid {}", blkid.to_string());
    return chunk->blk_allocator_mutable()->alloc_on_disk(blkid);
}
#endif

BlkAllocStatus VirtualDev::alloc_contiguous_blk(blk_count_t nblks, const blk_alloc_hints& hints, BlkId* out_blkid) {
    BlkAllocStatus ret;
    try {
        static thread_local std::vector< BlkId > blkid{};
        blkid.clear();
        HS_DBG_ASSERT_EQ(hints.is_contiguous, true);
        ret = alloc_blk(nblks, hints, blkid);
        if (ret == BlkAllocStatus::SUCCESS) {
            HS_REL_ASSERT_EQ(blkid.size(), 1, "out blkid more than 1 entries({}) will lead to blk leak!", blkid.size());
            *out_blkid = std::move(blkid.front());
        } else {
            HS_DBG_ASSERT_EQ(blkid.size(), 0);
        }
    } catch (const std::exception& e) {
        ret = BlkAllocStatus::FAILED;
        HS_DBG_ASSERT(0, "{}", e.what());
    }
    return ret;
}

BlkAllocStatus VirtualDev::alloc_blk(uint32_t nblks, const blk_alloc_hints& hints, std::vector< BlkId >& out_blkid) {
    size_t start_idx = out_blkid.size();
    while (nblks != 0) {
        const blk_count_t nblks_op = std::min(BlkId::max_blks_in_op(), s_cast< blk_count_t >(nblks));
        const auto ret = do_alloc_blk(nblks_op, hints, out_blkid);
        if (ret != BlkAllocStatus::SUCCESS) {
            for (auto i = start_idx; i < out_blkid.size(); ++i) {
                free_blk(out_blkid[i]);
                out_blkid.erase(out_blkid.begin() + start_idx, out_blkid.end());
            }
            return ret;
        }
        nblks -= nblks_op;
    }
    return BlkAllocStatus::SUCCESS;
}

BlkAllocStatus VirtualDev::do_alloc_blk(blk_count_t nblks, const blk_alloc_hints& hints,
                                        std::vector< BlkId >& out_blkid) {
    try {
        Chunk* first_failed_chunk = nullptr;

        // First select a chunk to allocate it from
        BlkAllocStatus status;
        Chunk* chunk;
        do {
            chunk = m_chunk_selector->select(hints);
            status = alloc_blk_from_chunk(nblks, hints, out_blkid, chunk);
            if (status == BlkAllocStatus::SUCCESS || !hints.can_look_for_other_chunk) { break; }
        } while (chunk != first_failed_chunk);

        if (status != BlkAllocStatus::SUCCESS) {
            LOGERROR("nblks={} failed to alloc after trying to allo on every chunks {} and devices {}.", nblks);
            COUNTER_INCREMENT(m_metrics, vdev_num_alloc_failure, 1);
        }

        return status;
    } catch (const std::exception& e) {
        LOGERROR("exception happened {}", e.what());
        assert(false);
        return BlkAllocStatus::FAILED;
    }
}

BlkAllocStatus VirtualDev::alloc_blk_from_chunk(blk_count_t nblks, const blk_alloc_hints& hints,
                                                std::vector< BlkId >& out_blkid, Chunk* chunk) {
#ifdef _PRERELEASE
    if (auto const fake_status =
            iomgr_flip::instance()->get_test_flip< uint32_t >("blk_allocation_flip", nblks, chunk->vdev_id())) {
        return static_cast< BlkAllocStatus >(fake_status.get());
    }
#endif
    static thread_local std::vector< BlkId > chunk_blkid{};
    chunk_blkid.clear();
    auto status = chunk->blk_allocator_mutable()->alloc(nblks, hints, chunk_blkid);
    if (status == BlkAllocStatus::PARTIAL) {
        // free partial result
#ifdef ENABLE_LATER
        for (auto const b : chunk_blkid) {
            auto const ret = chunk->blk_allocator_mutable()->free_on_realtime(b);
            HS_REL_ASSERT(ret, "failed to free on realtime");
        }
#endif
        chunk->blk_allocator_mutable()->free(chunk_blkid);
        status = BlkAllocStatus::FAILED;
    } else if (status == BlkAllocStatus::SUCCESS) {
        // append chunk blocks to out blocks
        out_blkid.insert(std::end(out_blkid), std::make_move_iterator(std::begin(chunk_blkid)),
                         std::make_move_iterator(std::end(chunk_blkid)));
    }
    return status;
}

/*bool VirtualDev::free_on_realtime(const BlkId& b) {
    Chunk* chunk = m_dmgr.get_chunk_mutable(b.get_chunk_num());
    return chunk->blk_allocator_mutable()->free_on_realtime(b);
}*/

void VirtualDev::free_blk(const BlkId& b) {
    Chunk* chunk = m_dmgr.get_chunk_mutable(b.get_chunk_num());
    chunk->blk_allocator_mutable()->free(b);
}

void VirtualDev::recovery_done() {
    DEBUG_ASSERT_EQ(m_auto_recovery, false, "recovery done (manual recovery completion) called on auto recovery vdev");
    m_chunk_selector->foreach_chunks([](cshared< Chunk >& chunk) { chunk->blk_allocator_mutable()->inited(); });
}

uint64_t VirtualDev::get_len(const iovec* iov, const int iovcnt) {
    uint64_t len{0};
    for (int i{0}; i < iovcnt; ++i) {
        len += iov[i].iov_len;
    }
    return len;
}

////////////////////////// async write section //////////////////////////////////
folly::Future< bool > VirtualDev::async_write(const char* buf, uint32_t size, const BlkId& bid, bool part_of_batch) {
    Chunk* chunk;
    uint64_t const dev_offset = to_dev_offset(bid, &chunk);
    auto* pdev = chunk->physical_dev_mutable();

    HS_LOG(TRACE, device, "Writing in device: {}, offset = {}", pdev->pdev_id(), dev_offset);
    COUNTER_INCREMENT(m_metrics, vdev_write_count, 1);
    if (sisl_unlikely(!hs_utils::mod_aligned_sz(dev_offset, pdev->align_size()))) {
        COUNTER_INCREMENT(m_metrics, unalign_writes, 1);
    }
    return pdev->async_write(buf, size, dev_offset, part_of_batch);
}

folly::Future< bool > VirtualDev::async_writev(const iovec* iov, const int iovcnt, const BlkId& bid,
                                               bool part_of_batch) {
    Chunk* chunk;
    uint64_t const dev_offset = to_dev_offset(bid, &chunk);
    auto const size = get_len(iov, iovcnt);
    auto* pdev = chunk->physical_dev_mutable();

    HS_LOG(TRACE, device, "Writing in device: {}, offset = {}", pdev->pdev_id(), dev_offset);
    COUNTER_INCREMENT(m_metrics, vdev_write_count, 1);
    if (sisl_unlikely(!hs_utils::mod_aligned_sz(dev_offset, pdev->align_size()))) {
        COUNTER_INCREMENT(m_metrics, unalign_writes, 1);
    }
    return pdev->async_writev(iov, iovcnt, size, dev_offset, part_of_batch);
}

////////////////////////// sync write section //////////////////////////////////
void VirtualDev::sync_write(const char* buf, uint32_t size, const BlkId& bid) {
    Chunk* chunk;
    uint64_t const dev_offset = to_dev_offset(bid, &chunk);
    chunk->physical_dev_mutable()->sync_write(buf, size, dev_offset);
}

void VirtualDev::sync_writev(const iovec* iov, int iovcnt, const BlkId& bid) {
    Chunk* chunk;
    uint64_t const dev_offset = to_dev_offset(bid, &chunk);
    auto const size = get_len(iov, iovcnt);
    auto* pdev = chunk->physical_dev_mutable();

    COUNTER_INCREMENT(m_metrics, vdev_write_count, 1);
    if (sisl_unlikely(!hs_utils::mod_aligned_sz(dev_offset, pdev->align_size()))) {
        COUNTER_INCREMENT(m_metrics, unalign_writes, 1);
    }

    pdev->sync_writev(iov, iovcnt, size, dev_offset);
}

////////////////////////////////// async read section ///////////////////////////////////////////////
folly::Future< bool > VirtualDev::async_read(char* buf, uint64_t size, const BlkId& bid, bool part_of_batch) {
    Chunk* pchunk;
    uint64_t const dev_offset = to_dev_offset(bid, &pchunk);
    return pchunk->physical_dev_mutable()->async_read(buf, size, dev_offset, part_of_batch);
}

folly::Future< bool > VirtualDev::async_readv(iovec* iovs, int iovcnt, uint64_t size, const BlkId& bid,
                                              bool part_of_batch) {
    Chunk* pchunk;
    uint64_t const dev_offset = to_dev_offset(bid, &pchunk);
    return pchunk->physical_dev_mutable()->async_readv(iovs, iovcnt, size, dev_offset, part_of_batch);
}

////////////////////////////////////////// sync read section ////////////////////////////////////////////
void VirtualDev::sync_read(char* buf, uint32_t size, const BlkId& bid) {
    Chunk* chunk;
    uint64_t const dev_offset = to_dev_offset(bid, &chunk);
    chunk->physical_dev_mutable()->sync_read(buf, size, dev_offset);
}

void VirtualDev::sync_readv(iovec* iov, int iovcnt, const BlkId& bid) {
    Chunk* chunk;
    uint64_t const dev_offset = to_dev_offset(bid, &chunk);
    auto const size = get_len(iov, iovcnt);
    auto* pdev = chunk->physical_dev_mutable();

    COUNTER_INCREMENT(m_metrics, vdev_write_count, 1);
    if (sisl_unlikely(!hs_utils::mod_aligned_sz(dev_offset, pdev->align_size()))) {
        COUNTER_INCREMENT(m_metrics, unalign_writes, 1);
    }

    pdev->sync_readv(iov, iovcnt, size, dev_offset);
}

folly::Future< bool > VirtualDev::queue_fsync_pdevs() {
    HS_DBG_ASSERT_EQ(HS_DYNAMIC_CONFIG(device->direct_io_mode), false, "Not expect to do fsync in DIRECT_IO_MODE.");

    assert(m_pdevs.size() > 0);
    if (m_pdevs.size() == 1) {
        auto* pdev = *(m_pdevs.begin());
        HS_LOG(TRACE, device, "Flushing pdev {}", pdev->get_devname());
        return pdev->queue_fsync();
    } else {
        static thread_local std::vector< folly::Future< bool > > s_futs;
        s_futs.clear();
        for (auto* pdev : m_pdevs) {
            HS_LOG(TRACE, device, "Flushing pdev {}", pdev->get_devname());
            s_futs.emplace_back(pdev->queue_fsync());
        }
        return folly::collectAllUnsafe(s_futs).thenTry([](auto&&) { return folly::makeFuture< bool >(true); });
    }
}

void VirtualDev::submit_batch() {
    // It is enough to submit batch on first pdev, since all pdevs are expected to be under same drive interfaces
    auto* pdev = *(m_pdevs.begin());
    return pdev->submit_batch();
}

uint64_t VirtualDev::available_blks() const {
    uint64_t avl_blks{0};
    m_chunk_selector->foreach_chunks(
        [this, &avl_blks](cshared< Chunk >& chunk) { avl_blks += chunk->blk_allocator()->available_blks(); });
    return avl_blks;
}

uint64_t VirtualDev::used_size() const {
    uint64_t alloc_cnt{0};
    m_chunk_selector->foreach_chunks(
        [this, &alloc_cnt](cshared< Chunk >& chunk) { alloc_cnt += chunk->blk_allocator()->get_used_blks(); });
    return (alloc_cnt * block_size());
}

void VirtualDev::cp_flush() {
    m_chunk_selector->foreach_chunks([this](cshared< Chunk >& chunk) { chunk->cp_flush(); });
}

std::vector< shared< Chunk > > VirtualDev::get_chunks() const {
    std::vector< shared< Chunk > > ret;
    m_chunk_selector->foreach_chunks([&ret](cshared< Chunk >& chunk) { ret.push_back(chunk); });
    return ret;
}

/*void VirtualDev::blkalloc_cp_start(const std::shared_ptr< blkalloc_cp >& ba_cp) {
    for (size_t i{0}; i < m_primary_pdev_chunks_list.size(); ++i) {
        for (size_t chunk_indx{0}; chunk_indx < m_primary_pdev_chunks_list[i].chunks_in_pdev.size(); ++chunk_indx) {
            auto* chunk = m_primary_pdev_chunks_list[i].chunks_in_pdev[chunk_indx];
            chunk->cp_start(ba_cp);
        }
    }
}*/

/* Get status for all chunks */
nlohmann::json VirtualDev::get_status(int log_level) const {
    nlohmann::json j;

#ifdef ENABLE_LATER
    try {
        m_chunk_selector->foreach_chunks([this, &j, log_level](cshared< Chunk >& chunk) {
            nlohmann::json chunk_j;
            chunk_j["ChunkInfo"] = chunk->get_status(log_level);
            if (chunk->blk_allocator() != nullptr) {
                chunk_j["BlkallocInfo"] = chunk->blk_allocator()->get_status(log_level);
            }
            j[std::to_string(chunk->chunk_id())] = chunk_j;
        });
    } catch (const std::exception& e) { LOGERROR("exception happened {}", e.what()); }
#endif
    return j;
}

uint32_t VirtualDev::align_size() const {
    auto* pdev = *(m_pdevs.begin());
    return pdev->align_size();
}
uint32_t VirtualDev::optimal_page_size() const {
    auto* pdev = *(m_pdevs.begin());
    return pdev->optimal_page_size();
}
uint32_t VirtualDev::atomic_page_size() const {
    auto* pdev = *(m_pdevs.begin());
    return pdev->atomic_page_size();
}

std::string VirtualDev::to_string() const { return ""; }

///////////////////////// VirtualDev Private Methods /////////////////////////////
uint64_t VirtualDev::to_dev_offset(const BlkId& b, Chunk** chunk) const {
    *chunk = m_dmgr.get_chunk_mutable(b.get_chunk_num());
    return uint64_cast(b.get_blk_num()) * block_size() + uint64_cast((*chunk)->start_offset());
}

} // namespace homestore
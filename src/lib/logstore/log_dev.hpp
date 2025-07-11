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
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <sisl/fds/id_reserver.hpp>
#include <sisl/fds/stream_tracker.hpp>
#include <sisl/fds/buffer.hpp>
#include <folly/futures/SharedPromise.h>
#include <fmt/format.h>
#include <sisl/logging/logging.h>

#include <homestore/logstore/log_store_internal.hpp>
#include <homestore/superblk_handler.hpp>
#include "common/homestore_config.hpp"
#include "device/chunk.h"
#include "device/journal_vdev.hpp"

namespace homestore {

static constexpr uint32_t LOG_GROUP_HDR_MAGIC{0xF00D1E};
static constexpr uint32_t LOG_GROUP_FOOTER_MAGIC{0xB00D1E};
static constexpr uint32_t dma_address_boundary{512}; // Mininum size the dma/writes to be aligned with
static constexpr uint32_t initial_read_size{4096};
// logdev doesn't support concurrent writes. There can only be 2 active log groups.
static constexpr uint32_t max_log_group{2};

// clang-format off
/*
 * LogGroup Layout:
 *
 *   <----        Log Group Header         ---> <--   Record 1   --> <--   Record 2   -->        <-- -  Inline data area  -->
 *  |----------------------------------------- |--------------------|--------------------|      |----------------|-----------|----------------|
 *  |#records|...| oob area   | inline area    | Size | data offset | Size | data offset | ...  | Record #1 data |     ...   |   OOB Record 1 |
 *  |----------------------------------------- |--------------------|--------------------|      |----------------|-----------|----------------|
 *                      |             |                     |                                     ^                            ^
 *                      |             |                     |                                     |                            |
 *                      |             |                      -------------------------------------|                            |
 *                      |             ------------------------------------------------------------|                            |
 *                      |------------------------------------------------------------------------------------------------------|
 */
// clang-format on

/************************************* Log Record Section ************************************/
/* Each log record which is serialized to the persistent store in the following format */
#pragma pack(1)
struct serialized_log_record {
    uint32_t size;                    // Size of this log record
    uint32_t offset : 31;             // Offset within the log_group where data is residing
    uint32_t is_inlined : 1;          // Is the log data is inlined or out-of-band area
    logstore_seq_num_t store_seq_num; // Seqnum by the log store
    logstore_id_t store_id;           // ID of the store this log is associated with

    void set_inlined(bool inlined) { is_inlined = static_cast< uint32_t >(inlined ? 0x1 : 0x0); }
    bool get_inlined() const { return ((is_inlined == static_cast< uint32_t >(0x1)) ? true : false); }

    serialized_log_record() = default;
    serialized_log_record(uint32_t s, uint32_t o, bool inlined, logstore_seq_num_t sq, logstore_id_t id) :
            size{s}, offset{o}, store_seq_num{sq}, store_id{id} {
        set_inlined(inlined);
    }
    serialized_log_record(const serialized_log_record&) = default;
    serialized_log_record& operator=(const serialized_log_record&) = default;
    serialized_log_record(serialized_log_record&&) noexcept = default;
    serialized_log_record& operator=(serialized_log_record&&) noexcept = default;
    ~serialized_log_record() = default;
};
#pragma pack()

/* This structure represents the in-memory representation of a log record */
struct log_record {
    sisl::io_blob data;
    void* context;
    logstore_id_t store_id;
    logstore_seq_num_t seq_num;

    log_record(const logstore_id_t& sid, const logstore_seq_num_t snum, const sisl::io_blob& d, void* const ctx) :
            data{d}, context{ctx}, store_id{sid}, seq_num{snum} {}
    log_record(const log_record&) = delete;
    log_record& operator=(const log_record&) = delete;
    log_record(log_record&&) noexcept = delete;
    log_record& operator=(log_record&&) noexcept = delete;
    ~log_record() = default;

    size_t serialized_size() const { return sizeof(serialized_log_record) + data.size(); }
    bool is_inlineable(const uint64_t flush_size_multiple) const {
        // Need inlining if size is smaller or size/buffer is not in dma'ble boundary.
        return (is_size_inlineable(data.size(), flush_size_multiple) ||
                ((r_cast< const uintptr_t >(data.cbytes()) % flush_size_multiple) != 0) || !data.is_aligned());
    }

    static bool is_size_inlineable(const size_t sz, const uint64_t flush_size_multiple) {
        return ((sz < HS_DYNAMIC_CONFIG(logstore.optimal_inline_data_size)) || ((sz % flush_size_multiple) != 0));
    }

    static size_t serialized_size(const uint32_t sz) { return sizeof(serialized_log_record) + sz; }
};

/************************************* Log Group Section ************************************/
/* This structure represents a group commit log header */
#pragma pack(1)
struct log_group_header {
    static constexpr uint8_t header_version{0};

    uint32_t magic;
    uint32_t version;
    uint32_t n_log_records;      // Total number of log records
    logid_t start_log_idx;       // log id of the first log record
    uint32_t group_size;         // Total size of this group including this header
    uint32_t inline_data_offset; // Offset of inlined area of data
    uint32_t oob_data_offset;    // Offset of where the data which are not inlined starts
    uint32_t footer_offset;      // offset of where footer starts
    crc32_t prev_grp_crc;        // Checksum of the previous group that was written
    crc32_t cur_grp_crc;         // Checksum of the current group record
    logdev_id_t logdev_id;       // Logdev id

    log_group_header() : magic{LOG_GROUP_HDR_MAGIC}, version{header_version} {}
    log_group_header(const log_group_header&) = delete;
    log_group_header& operator=(const log_group_header&) = delete;
    log_group_header(log_group_header&&) noexcept = delete;
    log_group_header& operator=(log_group_header&&) noexcept = delete;
    ~log_group_header() = default;

    uint32_t inline_data_size() const {
        return oob_data_offset ? (oob_data_offset - inline_data_offset) : (group_size - inline_data_offset);
    }

    const uint8_t* inline_area() const { return (reinterpret_cast< const uint8_t* >(this) + inline_data_offset); }
    const uint8_t* oob_area() const { return (reinterpret_cast< const uint8_t* >(this) + oob_data_offset); }
    const uint8_t* record_area() const { return (reinterpret_cast< const uint8_t* >(this) + sizeof(log_group_header)); }

    const serialized_log_record* nth_record(const uint32_t n) const {
        return reinterpret_cast< const serialized_log_record* >(record_area() + (sizeof(serialized_log_record) * n));
    }

    sisl::blob data(const logid_t idx) const {
        assert(idx >= start_log_idx);
        assert(idx - start_log_idx < n_log_records);

        const serialized_log_record* const lr{nth_record(start_log_idx - idx)};
        return sisl::blob{(lr->get_inlined() ? inline_area() : oob_area() + lr->offset), lr->size};
    }

    uint32_t magic_word() const { return magic; }
    uint8_t get_version() const { return static_cast< uint8_t >(version); }
    logid_t start_idx() const { return start_log_idx; }
    uint32_t nrecords() const { return n_log_records; }
    uint32_t total_size() const { return group_size; }
    crc32_t this_group_crc() const { return cur_grp_crc; }
    crc32_t prev_group_crc() const { return prev_grp_crc; }
    uint32_t _inline_data_offset() const { return inline_data_offset; }
};
#pragma pack()

#pragma pack(1)
struct log_group_footer {
    static constexpr uint8_t footer_version{0};

    log_group_footer() : magic{LOG_GROUP_FOOTER_MAGIC}, version{footer_version} {}
    uint32_t magic : 24;
    uint32_t version : 8;
    logid_t start_log_idx;
    uint8_t padding[12];
};
#pragma pack()
} // namespace homestore

template <>
struct fmt::formatter< homestore::log_group_header > {
    constexpr auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    auto format(const homestore::log_group_header& header, format_context& ctx) const -> format_context::iterator {
        return fmt::format_to(
            ctx.out(),
            "magic = {} version={} n_log_records = {} start_log_idx = {} group_size = {} inline_data_offset = {} "
            "oob_data_offset = {} prev_grp_crc = {} cur_grp_crc = {} logdev = {}",
            header.magic, header.version, header.n_log_records, header.start_log_idx, header.group_size,
            header.inline_data_offset, header.oob_data_offset, header.prev_grp_crc, header.cur_grp_crc,
            header.logdev_id);
    }
};

namespace homestore {
template < typename charT, typename traits >
std::basic_ostream< charT, traits >& operator<<(std::basic_ostream< charT, traits >& out_stream,
                                                const log_group_header& header) {
    // copy the stream formatting
    std::basic_ostringstream< charT, traits > out_string_stream;
    out_string_stream.copyfmt(out_stream);

    // print the stream
    out_string_stream << fmt::format("{}", header);
    out_stream << out_string_stream.str();

    return out_stream;
}

struct iovec_wrapper : public iovec {
    iovec_wrapper(void* const base, const size_t len) : iovec{base, len} {}
    iovec_wrapper(const iovec_wrapper&) = default;
    iovec_wrapper& operator=(const iovec_wrapper&) = default;
    iovec_wrapper(iovec_wrapper&&) noexcept = default;
    iovec_wrapper& operator=(iovec_wrapper&&) noexcept = default;
    ~iovec_wrapper() = default;
};
typedef std::vector< iovec_wrapper > iovec_array;

/* In memory representation of a log group which will be written as a group together */
class LogGroup {
public:
    /* These are going to be compile time constants to build the inline array, so they are not using dynamic
     * settings to change them */
    static constexpr uint32_t optimal_num_records{16};
    static constexpr uint32_t estimated_iovs{10};
    static constexpr size_t inline_log_buf_size{512 * optimal_num_records};
    static constexpr uint32_t max_records_in_a_batch{(initial_read_size - sizeof(log_group_header)) /
                                                     sizeof(serialized_log_record)};

    friend class LogDev;

    LogGroup();
    LogGroup(const LogGroup&) = delete;
    LogGroup& operator=(const LogGroup&) = delete;
    LogGroup(LogGroup&&) noexcept = delete;
    LogGroup& operator=(LogGroup&&) noexcept = delete;
    ~LogGroup() = default;

    void start(const uint64_t flush_size_multiple, const uint32_t align_size);
    void stop();
    void reset(const uint32_t max_records);
    void create_overflow_buf(const uint32_t min_needed);
    bool add_record(log_record& record, const int64_t log_idx);
    bool can_accomodate(const log_record& record) const { return (m_nrecords <= m_max_records); }

    const iovec_array& finish(logdev_id_t logdev_id, const crc32_t prev_crc);
    crc32_t compute_crc();

    log_group_header* header() { return reinterpret_cast< log_group_header* >(m_cur_log_buf); }
    const log_group_header* header() const { return reinterpret_cast< const log_group_header* >(m_cur_log_buf); }
    iovec_array const& iovecs() const { return m_iovecs; }
    uint32_t actual_data_size() const { return m_actual_data_size; }
    uint32_t nrecords() const { return m_nrecords; }

    auto max_records() const { return m_max_records; }
    auto flush_log_idx_from() const { return m_flush_log_idx_from; }
    auto flush_log_idx_upto() const { return m_flush_log_idx_upto; }
    auto log_dev_offset() const { return m_log_dev_offset; }

    sisl::aligned_unique_ptr< uint8_t, sisl::buftag::logwrite > m_log_buf;
    sisl::aligned_unique_ptr< uint8_t, sisl::buftag::logwrite > m_footer_buf;
    sisl::aligned_unique_ptr< uint8_t, sisl::buftag::logwrite > m_overflow_log_buf;

    uint8_t* m_cur_log_buf;
    uint32_t m_cur_buf_len;
    uint32_t m_footer_buf_len;

    serialized_log_record* m_record_slots;
    uint32_t m_inline_data_pos;
    uint32_t m_oob_data_pos;

    uint32_t m_nrecords{0};
    uint32_t m_max_records{0};
    uint32_t m_actual_data_size{0};

    // Info about the final data
    iovec_array m_iovecs;
    int64_t m_flush_log_idx_from;
    int64_t m_flush_log_idx_upto;
    off_t m_log_dev_offset;

    uint64_t m_flush_multiple_size{0};

private:
    log_group_footer* add_and_get_footer();
    bool new_iovec_for_footer() const;
};
} // namespace homestore

template <>
struct fmt::formatter< homestore::LogGroup > {
    constexpr auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    auto format(const homestore::LogGroup& lg, format_context& ctx) const -> format_context::iterator {
        const auto* const header{lg.header()};
        return fmt::format_to(ctx.out(),
                              "Header:[{}]\nLog_idx_range: [{} - {}] DevOffset: {} Max_Records: {} IOVecSize: {}\n"
                              "-----------------------------------------------------------------\n",
                              *header, lg.flush_log_idx_from(), lg.flush_log_idx_upto(), lg.log_dev_offset(),
                              lg.max_records(), lg.iovecs().size());
    }
};

namespace homestore {
template < typename charT, typename traits >
std::basic_ostream< charT, traits >& operator<<(std::basic_ostream< charT, traits >& out_stream, const LogGroup& lg) {
    // copy the stream formatting
    std::basic_ostringstream< charT, traits > out_string_stream;
    out_string_stream.copyfmt(out_stream);

    // print the stream
    out_string_stream << fmt::format("{}", lg);
    out_stream << out_string_stream.str();

    return out_stream;
}

#if 0
/************************************* LogDev Request to BlkStore Section ************************************/
struct logdev_req;
typedef boost::intrusive_ptr< logdev_req > logdev_req_ptr;

struct logdev_req : public virtualdev_req {
public:
    static boost::intrusive_ptr< logdev_req > make_request() {
        return boost::intrusive_ptr< logdev_req >{sisl::ObjectAllocator< logdev_req >::make_object()};
    }

    virtual void free_yourself() override { sisl::ObjectAllocator< logdev_req >::deallocate(this); }

    logdev_req(const logdev_req&) = delete;
    logdev_req& operator=(const logdev_req&) = delete;
    logdev_req(logdev_req&&) noexcept = delete;
    logdev_req& operator=(logdev_req&&) noexcept = delete;
    virtual ~logdev_req() override = default;

    // virtual size_t get_your_size() const override { return sizeof(ssd_loadgen_req); }

    static logdev_req_ptr to_logdev_req(const boost::intrusive_ptr< virtualdev_req >& vd_req) {
#ifdef NDEBUG
        return boost::intrusive_ptr< logdev_req >(reinterpret_cast< logdev_req* >(vd_req.get()));
#else
        return boost::dynamic_pointer_cast< logdev_req >(vd_req);
#endif
    }

    LogGroup* m_log_group;

protected:
    friend class sisl::ObjectAllocator< logdev_req >;

private:
    logdev_req() = default;
};
#endif

template < typename charT, typename traits >
std::basic_ostream< charT, traits >& operator<<(std::basic_ostream< charT, traits >& out_stream,
                                                const logdev_key& key) {

    // copy the stream formatting
    std::basic_ostringstream< charT, traits > out_string_stream;
    out_string_stream.copyfmt(out_stream);

    // print the stream
    out_string_stream << "[idx=" << key.idx << " dev_offset=" << key.dev_offset << "]";
    out_stream << out_string_stream.str();

    return out_stream;
}

} // namespace homestore

namespace homestore {
using log_buffer = sisl::byte_view;

struct truncation_request_t {
    logstore_id_t store_id;
    logstore_seq_num_t upto_seq_num;
};

/* This structure represents the logdevice super block which will be loaded upto start of the homestore */
struct logstore_superblk;

#pragma pack(1)
struct logdev_superblk {
    static constexpr uint32_t LOGDEV_SB_MAGIC{0xDABAF00D};
    static constexpr uint32_t LOGDEV_SB_VERSION{1};

    uint32_t magic{LOGDEV_SB_MAGIC};
    uint32_t version{LOGDEV_SB_VERSION};
    logdev_id_t logdev_id{0};
    uint32_t num_stores{0};
    uint64_t start_dev_offset{0};
    logid_t key_idx{0};
    flush_mode_t flush_mode;

    // The meta data starts immediately after the super block
    // Equivalent of:
    // logstore_superblk meta[0];

    uint32_t get_magic() const { return magic; }
    uint32_t get_version() const { return version; }
    off_t start_offset() const { return static_cast< off_t >(start_dev_offset); }
    uint32_t num_stores_reserved() const { return num_stores; }

    void set_start_offset(const off_t offset) { start_dev_offset = static_cast< uint64_t >(offset); }

    logstore_superblk* get_logstore_superblk() {
        return reinterpret_cast< logstore_superblk* >(reinterpret_cast< uint8_t* >(this) + sizeof(logdev_superblk));
    }
    const logstore_superblk* get_logstore_superblk() const {
        return reinterpret_cast< const logstore_superblk* >(reinterpret_cast< const uint8_t* >(this) +
                                                            sizeof(logdev_superblk));
    }
};
#pragma pack()

#pragma pack(1)
typedef std::pair< logid_t, logid_t > logid_range_t;

struct rollback_record {
    logstore_id_t store_id;
    logid_range_t idx_range;
};

struct rollback_superblk {
    static constexpr uint32_t ROLLBACK_SB_MAGIC{0xDABAF00D};
    static constexpr uint32_t ROLLBACK_SB_VERSION{1};
    static constexpr uint32_t num_record_increment{8};

    uint32_t magic{ROLLBACK_SB_MAGIC};
    uint32_t version{ROLLBACK_SB_VERSION};
    logdev_id_t logdev_id{0};
    uint32_t num_records{0};

    uint32_t get_magic() const { return magic; }
    uint32_t get_version() const { return version; }

    static uint32_t size_needed(uint32_t nrecords) {
        return sizeof(rollback_superblk) + (sisl::round_up(nrecords, num_record_increment) * sizeof(rollback_record));
    }

    rollback_record& at(uint32_t idx) {
        auto r = r_cast< rollback_record* >(uintptr_cast(this) + sizeof(rollback_superblk));
        return r[idx];
    }

    void remove_ith_record(uint32_t i) {
        uint8_t* rmem = uintptr_cast(&at(i));
        std::memmove(rmem, rmem + sizeof(rollback_record), sizeof(rollback_record) * (num_records - i - 1));
        --num_records;
    }

    void add_record(logstore_id_t store_id, logid_range_t idx_range) {
        rollback_record& r = at(num_records++);
        r.store_id = store_id;
        r.idx_range = idx_range;
    }
};
#pragma pack()

// This class represents the metadata of logdev providing methods to change/access log dev super block.
class LogDevMetadata {
    friend class LogDev;

public:
    LogDevMetadata();
    LogDevMetadata(const LogDevMetadata&) = delete;
    LogDevMetadata& operator=(const LogDevMetadata&) = delete;
    LogDevMetadata(LogDevMetadata&&) noexcept = delete;
    LogDevMetadata& operator=(LogDevMetadata&&) noexcept = delete;
    ~LogDevMetadata() = default;

    logdev_superblk* create(logdev_id_t id, flush_mode_t);
    void reset();
    std::vector< std::pair< logstore_id_t, logstore_superblk > > load();
    void persist();

    bool is_empty() const { return m_sb.is_empty(); }

    inline off_t get_start_dev_offset() const { return (m_sb->start_offset()); }
    void set_start_dev_offset(off_t offset, logid_t key_idx, bool persist_now);
    logid_t get_start_log_idx() const;

    logstore_id_t reserve_store(bool persist_now);
    void unreserve_store(logstore_id_t idx, bool persist_now);
    const std::set< logstore_id_t >& reserved_store_ids() const { return m_store_info; }

    void update_store_superblk(logstore_id_t idx, const logstore_superblk& meta, bool persist_now);
    const logstore_superblk& store_superblk(logstore_id_t idx) const;
    logstore_superblk& mutable_store_superblk(logstore_id_t idx);

    auto num_stores_reserved() const { return m_sb->num_stores_reserved(); }

    void add_rollback_record(logstore_id_t store_id, logid_range_t id_range, bool persist_now);
    void remove_rollback_record_upto(logid_t upto_id, bool persist_now);
    void remove_all_rollback_records(logstore_id_t store_id, bool persist_now);
    uint32_t num_rollback_records(logstore_id_t store_id) const;
    bool is_rolled_back(logstore_id_t store_id, logid_t logid) const;

    void logdev_super_blk_found(const sisl::byte_view& buf, void* meta_cookie);
    void rollback_super_blk_found(const sisl::byte_view& buf, void* meta_cookie);
    void destroy();

private:
    bool resize_logdev_sb_if_needed();
    bool resize_rollback_sb_if_needed();

    uint32_t logdev_sb_size_needed(uint32_t nstores) const {
        return sizeof(logdev_superblk) + (nstores * sizeof(logstore_superblk));
    }

    uint32_t store_capacity() const;

private:
    superblk< logdev_superblk > m_sb;
    superblk< rollback_superblk > m_rollback_sb;
    std::unique_ptr< sisl::IDReserver > m_id_reserver;
    std::set< logstore_id_t > m_store_info;
    std::multimap< logstore_id_t, logid_range_t > m_rollback_info;
    bool m_rollback_info_dirty{false};
};

class HomeStore;
typedef std::shared_ptr< HomeStore > HomeStoreSafePtr;
class JournalVirtualDev;

class log_stream_reader {
public:
    log_stream_reader(off_t device_cursor, std::shared_ptr< JournalVirtualDev > vdev,
                      shared< JournalVirtualDev::Descriptor > vdev_jd, uint64_t min_read_size);
    log_stream_reader(const log_stream_reader&) = delete;
    log_stream_reader& operator=(const log_stream_reader&) = delete;
    log_stream_reader(log_stream_reader&&) noexcept = delete;
    log_stream_reader& operator=(log_stream_reader&&) noexcept = delete;
    ~log_stream_reader() = default;

    sisl::byte_view next_group(off_t* out_dev_offset);
    sisl::byte_view group_in_next_page();

private:
    sisl::byte_view read_next_bytes(uint64_t nbytes, bool& end_of_stream);

private:
    std::shared_ptr< JournalVirtualDev > m_vdev;
    shared< JournalVirtualDev::Descriptor > m_vdev_jd; // Journal descriptor.
    sisl::byte_view m_cur_log_buf;
    off_t m_first_group_cursor;
    off_t m_cur_read_bytes{0};
    crc32_t m_prev_crc{0};
    uint64_t m_read_size_multiple;
};

struct logstore_info {
    std::shared_ptr< HomeLogStore > log_store;
    bool append_mode;
    log_found_cb_t log_found_cb{nullptr};
    log_replay_done_cb_t log_replay_done_cb{nullptr};
    folly::SharedPromise< std::shared_ptr< HomeLogStore > > promise{};
};

static std::string const logdev_sb_meta_name{"Logdev_sb"};
static std::string const logdev_rollback_sb_meta_name{"Logdev_rollback_sb"};

class LogDev : public std::enable_shared_from_this< LogDev > {
    friend class HomeLogStore;

public:
    static inline int64_t flush_data_threshold_size() {
        return HS_DYNAMIC_CONFIG(logstore.flush_threshold_size) - sizeof(log_group_header);
    }

    LogDev(logdev_id_t logdev_id,
           flush_mode_t flush_mode = static_cast< flush_mode_t >(HS_DYNAMIC_CONFIG(logstore.flush_mode)));
    LogDev(const LogDev&) = delete;
    LogDev& operator=(const LogDev&) = delete;
    LogDev(LogDev&&) noexcept = delete;
    LogDev& operator=(LogDev&&) noexcept = delete;
    ~LogDev();

    /**
     * @brief Start the logdev. This method reads the log virtual dev info block, loads all of the store and prepares
     * to the recovery. It is expected that all callbacks are registered before calling the start.
     *
     * @param format: Do we need to format the logdev or not.
     * @param blk_store: The blk_store associated to this logdev
     */
    void start(bool format, std::shared_ptr< JournalVirtualDev > vdev);

    /**
     * @brief Stop the logdev. it waits for all the pending writes to be completed and reject new api calls.
     *
     */
    void stop();

    /**
     * @brief Destroy the logdev metablks.
     *
     */
    void destroy();

    /**
     * @brief Append the data to the log device asynchronously. The buffer that is passed is expected to be valid, till
     * the append callback is done.
     *
     * @param store_id: The upper layer store id for this log record
     * @param seq_num: Upper layer store seq_num
     * @param data : Pointer to the data to be appended with its size
     * structure which could be 8K
     * @param cb_context Context to put upon a callback once append is. Upon completion the registered callback is
     * called.
     *
     * @return logid_t : log_idx of the log of the data.
     */
    logid_t append_async(logstore_id_t store_id, logstore_seq_num_t seq_num, const sisl::io_blob& data,
                         void* cb_context);

    /**
     * @brief Read the log id from the device offset
     *
     * @param logdev_key : log_id and dev_offset pair to read
     *
     * @return log_buffer : Opaque structure which contains the data blob and its size. It is safe buffer and hence it
     * need not be freed and can be cheaply passed it around.
     */
    log_buffer read(const logdev_key& key);

    /**
     * @brief Read the log id from the device offset
     *
     * @param logdev_key : log_id and dev_offset pair to read
     *
     * @param record_header Pass the pointer to the header of the read record
     */
    void read_record_header(const logdev_key& key, serialized_log_record& record_header);

    /// @brief Flush the log device in case if pending data size is at least the threshold size. This is a blocking call
    /// and hence it is required to run on thread/fiber which can run blocking io. If not run on such thread, it will
    /// redirect the flush to a flush thread and run there.
    ///
    /// @param threshold_size [Optional]: Size in bytes after which it will flush, if set to -1, will use default size
    ///
    /// @return bool : True if it has flushed the data, false otherwise
    bool flush_if_necessary(int64_t threshold_size = -1);

    /// @brief : Look at all logstore and find out the safest point upto which it can truncate and truncate them.
    ///
    /// @return number of log records it has truncated
    uint64_t truncate();

    /**
     * @brief Rollback the logid range specific to the given store id. This method persists the information
     * synchronously to the underlying storage. Once rolledback those logids in this range are ignored (only for
     * this logstore) during load.
     *
     * @param store_id : Store id whose logids are to be rolled back or invalidated
     * @param id_range : Log id range to rollback/invalidate
     */
    bool rollback(logstore_id_t store_id, logid_range_t id_range);

    /**
     * @brief This method get all the store ids that are registered already and out of them which are being garbaged
     * and waiting to be garbage collected. Predominant use of this method is for validation and testing
     *
     * @param registered out - Reference to the vector where all registered ids are pushed
     * @param garbage out - Reference to the vector where all garbage ids
     */
    bool get_registered_store_ids(std::vector< logstore_id_t >& registered, std::vector< logstore_id_t >& garbage);

    nlohmann::json dump_log_store(const log_dump_req& dum_req);
    nlohmann::json get_status(int verbosity) const;

    //////////////////// Logstore management ///////////////////////
    /// @brief Create a new log store under this log device
    /// @param append_mode Is this log store is append mode or not. If append mode, write_async call fails and only
    /// append_async calls succeed.
    ///
    /// @return shared< HomeLogStore > : The newly created log store
    shared< HomeLogStore > create_new_log_store(bool append_mode = false);

    /// @brief Open the log store which was created under this log device. It expects that log store id is already
    /// created. Behavior of opening a log store which was never created is unknown. One can create log store in
    /// non-append mode, but upon restart, it can be opened in append_mode. Log store is not usable until the future is
    /// armed with logstore. It is expected that caller calls this method before LogDev::start() is called, otherwise
    /// unopened log devs and log stores are removed.
    ///
    /// @param store_id Store id to open the log store
    /// @param append_mode Is this log store is append mode or not. If append mode, write_async call fails and only
    /// append_async calls succeed.
    /// @return future< shared< HomeLogStore > > : Future which will be set with the log store once it is opened
    folly::Future< shared< HomeLogStore > > open_log_store(logstore_id_t store_id, bool append_mode,
                                                           log_found_cb_t log_found_cb = nullptr,
                                                           log_replay_done_cb_t log_replay_done_cb = nullptr);

    /// @brief Remove the log store and its associated resources
    /// @param store_id Store id that was created/opened
    bool remove_log_store(logstore_id_t store_id);

    /// @return externally visible lock to avoid flush concurrently
    auto flush_guard() { return std::unique_lock(m_flush_mtx); }

    /// @brief do flush under the protection of flush guard
    bool flush_under_guard();

    ///////////////// Getters ///////////////////////
    LogDevMetadata& log_dev_meta() { return m_logdev_meta; }
    logdev_id_t get_id() const { return m_logdev_id; }
    uint64_t get_flush_size_multiple() const { return m_flush_size_multiple; }

private:
    void start_timer();
    folly::Future< int > stop_timer();

    bool allow_inline_flush() const { return uint32_cast(m_flush_mode) & uint32_cast(flush_mode_t::INLINE); }
    bool allow_timer_flush() const { return uint32_cast(m_flush_mode) & uint32_cast(flush_mode_t::TIMER); }
    bool allow_explicit_flush() const { return uint32_cast(m_flush_mode) & uint32_cast(flush_mode_t::EXPLICIT); }

    void verify_log_group_header(const logid_t idx, const log_group_header* header);

    /**
     * @brief Reserve logstore id and persist if needed. It persists the entire map about the logstore id inside the
     *
     * @return uint32_t : Return the reserved id
     */
    logstore_id_t reserve_store_id();

    /**
     * @brief Unreserve the logstore id. It does not immediately unregisters and persist the unregistered map, but it
     * will add to the waiting list (garbage list) and then during truncation, it actually unreserves and persits map.
     *
     * @param store_id
     */
    void unreserve_store_id(logstore_id_t store_id);

    void on_flush_completion(LogGroup* lg);
    void on_log_store_found(logstore_id_t store_id, const logstore_superblk& sb);
    void handle_unopened_log_stores(bool format);
    void on_logfound(logstore_id_t id, logstore_seq_num_t seq_num, logdev_key ld_key, logdev_key flush_ld_key,
                     log_buffer buf, uint32_t nremaining_in_batch);

    LogGroup* make_log_group(uint32_t estimated_records) {
        m_log_group_pool[m_log_group_idx].reset(estimated_records);
        return &m_log_group_pool[m_log_group_idx];
    }

    void free_log_group(LogGroup* lg) { m_log_group_idx = !m_log_group_idx; }

    LogGroup* prepare_flush(int32_t estimated_record);
    void do_load(off_t offset);
    void assert_next_pages(log_stream_reader& lstream);

    /// @brief force to flush the log device
    /// @return whether real flush is done
    bool flush();

    bool can_flush_in_this_thread();

private:
    std::unique_ptr< sisl::StreamTracker< log_record > > m_log_records; // Container stores all in-memory log records
    std::atomic< logid_t > m_log_idx{0};                                // Generator of log idx
    std::atomic< int64_t > m_pending_flush_size{0};                     // How much flushable logs are pending
    logdev_id_t m_logdev_id;
    std::shared_ptr< JournalVirtualDev > m_vdev;
    shared< JournalVirtualDev::Descriptor > m_vdev_jd; // Journal descriptor.
    HomeStoreSafePtr m_hs;                             // Back pointer to homestore
    flush_mode_t m_flush_mode;

    folly::SharedMutexWritePriority m_store_map_mtx;
    std::unordered_map< logstore_id_t, logstore_info > m_id_logstore_map;
    std::unordered_map< logstore_id_t, uint64_t > m_unopened_store_io;
    std::unordered_set< logstore_id_t > m_unopened_store_id;
    std::multimap< logid_t, logstore_id_t > m_garbage_store_ids;
    Clock::time_point m_last_flush_time;

    logid_t m_last_flush_idx{-1};         // Track last flushed, last device offset and truncated log idx
    logdev_key m_last_flush_ld_key{0, 0}; // Left interval of the last flush, 0 indicates the very beginning of logdev
    logid_t m_last_truncate_idx{-1};      // Logdev truncate up to this idx
    crc32_t m_last_crc{INVALID_CRC32_VALUE};

    // LogDev Info block related fields
    std::mutex m_meta_mutex;
    LogDevMetadata m_logdev_meta;
    uint64_t m_flush_size_multiple{0};

    // Pool for creating log group
    LogGroup m_log_group_pool[max_log_group];
    uint32_t m_log_group_idx{0};
    // Timer handle
    iomgr::timer_handle_t m_flush_timer_hdl{iomgr::null_timer_handle};

    // if we support inline flush mode, we might schedule flush operation in the same thread(for exampel, in the
    // callback of the append_async we schedule another flush.), so we need the lock to be locked for multitimes in the
    // same thread.
    iomgr::FiberManagerLib::mutex m_flush_mtx;
    std::atomic_uint64_t m_pending_callback{0};
    folly::SharedMutexWritePriority m_stream_tracker_mtx;

private:
    // graceful shutdown related fields
    std::atomic_bool m_stopping{false};
    mutable std::atomic_uint64_t pending_request_num{0};

    bool is_stopping() const { return m_stopping.load(); }
    void start_stopping() { m_stopping = true; }

    uint64_t get_pending_request_num() const { return pending_request_num.load(); }

    void incr_pending_request_num() const { pending_request_num++; }
    void decr_pending_request_num() const { pending_request_num--; }
}; // LogDev

} // namespace homestore

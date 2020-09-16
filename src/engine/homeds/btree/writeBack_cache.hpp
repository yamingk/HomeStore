#ifndef WRITEBACK_CACHE_HPP
#define WRITEBACK_CACHE_HPP

#include <vector>
#include <engine/common/error.h>
#include <engine/blkstore/blkstore.hpp>
#include <engine/homeds/btree/btree_internal.h>
#include <utility/thread_factory.hpp>
#include "engine/homestore.hpp"
#include "engine/index/resource_mgr.hpp"

#define wb_cache_buffer_t WriteBackCacheBuffer< K, V, InteriorNodeType, LeafNodeType >

namespace homeds {
namespace btree {
#define SSDBtreeNode BtreeNode< btree_store_type::SSD_BTREE, K, V, InteriorNodeType, LeafNodeType >

enum writeback_req_state {
    WB_REQ_INIT = 0, /* init */
    WB_REQ_WAITING,  /* waiting for cp */
    WB_REQ_SENT,     /* send to blksore to write */
    WB_REQ_COMPL,    /* completed */
};

#define btree_blkstore_t homestore::BlkStore< homestore::VdevFixedBlkAllocatorPolicy, wb_cache_buffer_t >
#define writeback_req_t wb_cache_buffer_t::writeback_req
#define writeback_req_ptr boost::intrusive_ptr< typename writeback_req_t >
#define to_wb_req(req) boost::static_pointer_cast< typename writeback_req_t >(req)
#define wb_cache_t WriteBackCache< K, V, InteriorNodeType, LeafNodeType >
#define MAX_CP_CNT 2

/* The class BtreeBuffer represents the buffer type that is used to interact with the BlkStore. It will have
 * all the SSD Btree Node declarative type. Hence in-memory representation of this buffer is as follows
 *
 *   ****************Cache Buffer************************
 *   *    ****************Cache Record***************   *
 *   *    *   ************Hash Node**************   *   *
 *   *    *   * Singly Linked list of hash node *   *   *
 *   *    *   ***********************************   *   *
 *   *    *******************************************   *
 *   * BlkId                                            *
 *   * Memvector of actual buffer                       *
 *   * Usage Reference counter                          *
 *   ****************************************************
 *   ************** Transient Header ********************
 *   * Upgraders count                                  *
 *   * Reader Write Lock                                *
 *   ****************************************************
 */
template < typename K, typename V, btree_node_type InteriorNodeType, btree_node_type LeafNodeType >
struct WriteBackCacheBuffer : public CacheBuffer< homestore::BlkId > {

    /******************************************** writeback_req *****************************/
    struct writeback_req : public homestore::blkstore_req< wb_cache_buffer_t > {

        typedef std::function< void(const writeback_req_ptr& req, std::error_condition status) > blkstore_callback;

        mutex mtx;
        writeback_req_state state;
        BlkId bid;
        btree_cp_ptr bcp;
        void* wb_cache;
        boost::intrusive_ptr< SSDBtreeNode > bn;

        /* Queue of the requests which should be written only after this req is written.
         * Reason of using stl over boost intrusive is that one request can be
         * shared with multiple queues.
         */
        std::deque< writeback_req_ptr > req_q;

        /* issue this request when the cnt become zero */
        sisl::atomic_counter< int > dependent_cnt;

        boost::intrusive_ptr< homeds::MemVector > m_mem;
        Clock::time_point cache_start_time; // Start time to put the wb cache to the request

        static boost::intrusive_ptr< writeback_req > make_request() {
            return boost::intrusive_ptr< writeback_req >(sisl::ObjectAllocator< writeback_req >::make_object());
        }

        virtual void free_yourself() override { sisl::ObjectAllocator< writeback_req >::deallocate(this); }

        virtual ~writeback_req() {
            HS_ASSERT(DEBUG, (state == WB_REQ_COMPL || state == WB_REQ_INIT), "state {}", state);
        }

    protected:
        friend class sisl::ObjectAllocator< writeback_req >;
        writeback_req() : state(WB_REQ_INIT), bid(0), bcp(nullptr), req_q(), dependent_cnt(1), m_mem(nullptr){};
    };

    /*************************************************** WriteBackCacheBuffer *******************************/

    btree_cp_ptr bcp = nullptr;
    writeback_req_ptr req[MAX_CP_CNT];

    static wb_cache_buffer_t* make_object() { return sisl::ObjectAllocator< SSDBtreeNode >::make_object(); }
    virtual void free_yourself() override { sisl::ObjectAllocator< SSDBtreeNode >::deallocate((SSDBtreeNode*)this); }

    WriteBackCacheBuffer(){};
    virtual ~WriteBackCacheBuffer() = default;

    virtual void init() override {
        /* Note : it is called under cache lock to prevent multiple threads to call init. And init function
         * internally also try to take the cache lock to access cache to update in memory structure. So
         * we have to be careful in taking any lock inside this function.
         */
        ((SSDBtreeNode*)this)->init();
    }

    friend void intrusive_ptr_add_ref(wb_cache_buffer_t* buf) {
        intrusive_ptr_add_ref((CacheBuffer< homestore::BlkId >*)buf);
    }
    friend void intrusive_ptr_release(wb_cache_buffer_t* buf) {
        intrusive_ptr_release((CacheBuffer< homestore::BlkId >*)buf);
    }
};

template < typename K, typename V, btree_node_type InteriorNodeType, btree_node_type LeafNodeType >
class WriteBackCache {
private:
    /* TODO :- need to have concurrent list */
    std::unique_ptr< sisl::ThreadVector< writeback_req_ptr > > m_req_list[MAX_CP_CNT];
    homestore::blkid_list_ptr m_free_list[MAX_CP_CNT];
    sisl::atomic_counter< uint64_t > m_dirty_buf_cnt[MAX_CP_CNT];
    cp_comp_callback m_cp_comp_cb;
    trigger_cp_callback m_trigger_cp_cb;
    uint64_t m_free_list_cnt = 0;
    static btree_blkstore_t* m_blkstore;
    static std::vector< iomgr::io_thread_t > m_thread_ids;

public:
    WriteBackCache(){};
    WriteBackCache(void* blkstore, uint64_t align_size, cp_comp_callback cb, trigger_cp_callback trigger_cp_cb) {
        for (int i = 0; i < MAX_CP_CNT; ++i) {
            m_free_list[i] = std::make_shared< sisl::ThreadVector< BlkId > >();
            m_req_list[i] = std::make_unique< sisl::ThreadVector< writeback_req_ptr > >();
            m_dirty_buf_cnt[i] = 0;
        }
        m_cp_comp_cb = cb;
        m_blkstore = (btree_blkstore_t*)blkstore;
        m_blkstore->attach_compl(wb_cache_t::writeBack_completion);
        m_trigger_cp_cb = trigger_cp_cb;
        static std::once_flag flag1;
        std::call_once(flag1, ([]() {
                           for (int i = 0; i < HS_DYNAMIC_CONFIG(generic.cache_flush_threads); ++i) {
                               bool thread_inited = false;
                               std::condition_variable cv;
                               std::mutex cv_m;
                               /* XXX : there can be race condition when message is sent before run_io_loop is called */
                               auto sthread = sisl::named_thread("wbcache_flusher", [i, &cv, &cv_m, &thread_inited]() {
                                   iomanager.run_io_loop(
                                       false, nullptr, ([i, &cv, &cv_m, &thread_inited](bool is_started) {
                                           if (is_started) {
                                               wb_cache_t::m_thread_ids.push_back(iomanager.iothread_self());
                                               std::unique_lock< std::mutex > lk(cv_m);
                                               thread_inited = true;
                                               cv.notify_all();
                                           }
                                       }));
                               });
                               std::unique_lock< std::mutex > lk(cv_m);
                               if (!thread_inited) { cv.wait(lk); }
                               sthread.detach();
                           }
                       }));
    }

    ~WriteBackCache() {
        for (uint32_t i = 0; i < MAX_CP_CNT; ++i) {
#ifndef NDEBUG
            HS_ASSERT_CMP(DEBUG, m_dirty_buf_cnt[i].get(), ==, 0);
            HS_ASSERT_CMP(DEBUG, m_req_list[i]->size(), ==, 0);
            HS_ASSERT_CMP(DEBUG, m_free_list[i]->size(), ==, 0);
#endif
        }
    }

    void prepare_cp(const btree_cp_ptr& new_bcp, const btree_cp_ptr& cur_bcp, bool blkalloc_checkpoint) {
        if (new_bcp) {
            int cp_id = (new_bcp->cp_id) % MAX_CP_CNT;
            HS_ASSERT_CMP(DEBUG, m_dirty_buf_cnt[cp_id].get(), ==, 0);
            /* decrement it by all cache threads at the end after writing all pending requests */
            HS_ASSERT_CMP(DEBUG, m_req_list[cp_id]->size(), ==, 0);
            blkid_list_ptr free_list;
            if (blkalloc_checkpoint || !cur_bcp) {
                free_list = m_free_list[++m_free_list_cnt % MAX_CP_CNT];
                HS_ASSERT_CMP(DEBUG, free_list->size(), ==, 0);
            } else {
                /* we keep accumulating the free blks until blk checkpoint is not taken */
                free_list = cur_bcp->free_blkid_list;
            }
            new_bcp->free_blkid_list = free_list;
        }
    }

    void write(const boost::intrusive_ptr< SSDBtreeNode >& bn, const boost::intrusive_ptr< SSDBtreeNode >& dependent_bn,
               const btree_cp_ptr& bcp) {
        int cp_id = bcp->cp_id % MAX_CP_CNT;
        HS_ASSERT(DEBUG, (!dependent_bn || dependent_bn->req[cp_id] != nullptr), "");
        writeback_req_ptr wbd_req = dependent_bn ? dependent_bn->req[cp_id] : nullptr;
        if (!bn->req[cp_id]) {
            /* create wb request */
            auto wb_req = writeback_req_t::make_request();
            wb_req->bcp = bcp;
            wb_req->m_mem = bn->get_memvec_intrusive();
            wb_req->bn = bn;
            wb_req->bid.set(bn->get_node_id());
            /* we can assume that btree is not destroyed until cp is not completed */
            wb_req->wb_cache = this;
            HS_ASSERT_CMP(DEBUG, wb_req->state, ==, WB_REQ_INIT);
            wb_req->state = WB_REQ_WAITING;

            /* update buffer */
            bn->req[cp_id] = wb_req;
            bn->bcp = bcp;

            /* add it to the list */
            m_req_list[cp_id]->push_back(wb_req);

            /* check for dirty buffers cnt */
            m_dirty_buf_cnt[cp_id].increment(1);
            ResourceMgr::inc_dirty_buf_cnt();
        } else {
            HS_ASSERT_CMP(DEBUG, bn->req[cp_id]->bid.to_integer(), ==, bn->get_node_id());
            if (bn->req[cp_id]->m_mem != bn->get_memvec_intrusive()) {
                bn->req[cp_id]->m_mem = bn->get_memvec_intrusive();
                HS_ASSERT_NOTNULL(DEBUG, bn->req[cp_id]->m_mem.get());
            }
        }

        auto wb_req = bn->req[cp_id];
        HS_ASSERT_CMP(DEBUG, wb_req->state, ==, WB_REQ_WAITING);

        if (wbd_req) {
            std::unique_lock< std::mutex > req_mtx(wbd_req->mtx);
            wbd_req->req_q.push_back(wb_req);
            wb_req->dependent_cnt.increment(1);
        }
    }

    /* We don't want to free the blocks until cp is persisted. Because we use these blocks
     * to recover btree.
     */
    void free_blk(bnodeid_t node_id, const blkid_list_ptr& free_blkid_list, uint64_t size) {
        HS_ASSERT_CMP(DEBUG, node_id, !=, empty_bnodeid);
        BlkId bid(node_id);

        /*  if bcp is null then free it only from the cache. */
        m_blkstore->free_blk(bid, boost::none, boost::none, free_blkid_list ? true : false);
        if (free_blkid_list) {
            ResourceMgr::inc_free_blk(size);
            free_blkid_list->push_back(bid);
        }
    }

    btree_status_t refresh_buf(const boost::intrusive_ptr< SSDBtreeNode >& bn, bool is_write_modifiable,
                               const btree_cp_ptr& bcp) {
        if (!bcp || !bn->bcp) { return btree_status_t::success; }

        if (!is_write_modifiable) {
            if (bn->bcp->cp_id > bcp->cp_id) { return btree_status_t::cp_mismatch; }
            return btree_status_t::success;
        }

        if (bn->bcp->cp_id == bcp->cp_id) {
            /* modifying the buffer multiple times in a same cp */
            return btree_status_t::success;
        }

        if (bn->bcp->cp_id > bcp->cp_id) { return btree_status_t::cp_mismatch; }

        int prev_cp_id = (bcp->cp_id - 1) % MAX_CP_CNT;
        auto req = bn->req[prev_cp_id];
        if (!req || req->state == WB_REQ_COMPL) {
            /* req on last cp is already completed. No need to make copy */
            return btree_status_t::success;
        }

        /* make a copy */
        auto mem = hs_iobuf_alloc(bn->get_cache_size());
        sisl::blob outb;
        (bn->get_memvec()).get(&outb);
        memcpy(mem, outb.bytes, outb.size);

        /* create a new mem vec */
        boost::intrusive_ptr< homeds::MemVector > mvec(new homeds::MemVector());
        mvec->set((uint8_t*)mem, bn->get_cache_size(), 0);

        /* assign new memvec to buffer */
        bn->set_memvec(mvec, 0, bn->get_cache_size());
        return btree_status_t::success;
    }

    /* We free the block only upto the end seqid of this cp. We might have persisted the data of sequence id
     * greater then this seq_id. But we are going to replay the entry from
     */
    void flush_free_blks(const btree_cp_ptr& bcp, std::shared_ptr< homestore::blkalloc_cp >& ba_cp) {
        ba_cp->free_blks(bcp->free_blkid_list);
    }

    void cp_start(const btree_cp_ptr& bcp) {
        static int thread_cnt = 0;
        int cp_id = bcp->cp_id % MAX_CP_CNT;
        iomanager.run_on(m_thread_ids[thread_cnt++ % HS_DYNAMIC_CONFIG(generic.cache_flush_threads)],
                         [this, bcp](io_thread_addr_t addr) { this->flush_buffers(bcp); });
    }

    void flush_buffers(const btree_cp_ptr& bcp) {
        int cp_id = bcp->cp_id % MAX_CP_CNT;
        m_dirty_buf_cnt[cp_id].increment(1);
        auto list = m_req_list[cp_id].get();
        typename sisl::ThreadVector< writeback_req_ptr >::thread_vector_iterator it;
        auto wb_req_ptr_ref = list->begin(it);
        while (wb_req_ptr_ref) {
            auto wb_req = *wb_req_ptr_ref;
            if (wb_req->dependent_cnt.decrement_testz(1)) {
                wb_req->state = WB_REQ_SENT;
                m_blkstore->write(wb_req->bid, wb_req->m_mem, 0, wb_req, false);
            }
            wb_req_ptr_ref = list->next(it);
        }
        list->clear();
        if (m_dirty_buf_cnt[cp_id].decrement_testz(1)) { m_cp_comp_cb(bcp); };
    }

    static void writeBack_completion(boost::intrusive_ptr< blkstore_req< wb_cache_buffer_t > > bs_req) {
        auto wb_req = to_wb_req(bs_req);
        wb_cache_t* wb_cache_instance = (wb_cache_t*)(wb_req->wb_cache);
        wb_cache_instance->writeBack_completion_internal(bs_req);
    }

    void writeBack_completion_internal(boost::intrusive_ptr< blkstore_req< wb_cache_buffer_t > >& bs_req) {
        auto wb_req = to_wb_req(bs_req);
        int cp_id = wb_req->bcp->cp_id % MAX_CP_CNT;
        wb_req->state = WB_REQ_COMPL;

        /* Scan if it has any req depending on this req */
        std::unique_lock< std::mutex > req_mtx(wb_req->mtx);
        while (!wb_req->req_q.empty()) {
            auto depend_req = wb_req->req_q.back();
            wb_req->req_q.pop_back();
            if (depend_req->dependent_cnt.decrement_testz(1)) {
                depend_req->state = WB_REQ_SENT;
                m_blkstore->write(depend_req->bid, depend_req->m_mem, 0, depend_req, false);
            }
        }
        wb_req->bn->req[cp_id] = nullptr;
        ResourceMgr::dec_dirty_buf_cnt();

        if (m_dirty_buf_cnt[cp_id].decrement_testz(1)) { m_cp_comp_cb(wb_req->bcp); };
    }
};
template < typename K, typename V, btree_node_type InteriorNodeType, btree_node_type LeafNodeType >
btree_blkstore_t* wb_cache_t::m_blkstore;
template < typename K, typename V, btree_node_type InteriorNodeType, btree_node_type LeafNodeType >
std::vector< iomgr::io_thread_t > wb_cache_t::m_thread_ids;

} // namespace btree
} // namespace homeds

#endif

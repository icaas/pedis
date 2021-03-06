/*
* Pedis is free software: you can redistribute it and/or modify
* it under the terms of the GNU Affero General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* You may obtain a copy of the License at
*
*     http://www.gnu.org/licenses
*
* Unless required by applicable law or agreed to in writing,
* software distributed under the License is distributed on an
* "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
* KIND, either express or implied.  See the License for the
* specific language governing permissions and limitations
* under the License.
*
*  Copyright (c) 2016-2026, Peng Jian, pstack@163.com. All rights reserved.
*
*/
#pragma once
#include <boost/intrusive/unordered_set.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/optional.hpp>
#include "common.hh"
#include "utils/bytes.hh"
#include "utils/managed_ref.hh"
#include "utils/managed_bytes.hh"
#include "utils/allocation_strategy.hh"
#include "utils/logalloc.hh"
#include "list_lsa.hh"
#include "dict_lsa.hh"
#include "sset_lsa.hh"
#include "core/sstring.hh"
#include "core/timer-set.hh"
#include "hll.hh"
#include "util/log.hh"
using logger =  seastar::logger;
static logger logc ("cache");

namespace redis {
namespace bi = boost::intrusive;
using clock_type = lowres_clock;
class cache;

struct expiration {
    using time_point = clock_type::time_point;
    using duration   = time_point::duration;
    time_point _time = never_expire_timepoint;

    expiration() {}

    expiration(long s) {
        using namespace std::chrono;
        static_assert(sizeof(clock_type::duration::rep) >= 8, "clock_type::duration::rep must be at least 8 bytes wide");

        if (s == 0U) {
            return; // means never expire.
        } else {
            _time = clock_type::now() + milliseconds(s);
        }
    }

    inline const bool ever_expires() const {
        return _time != never_expire_timepoint;
    }

    inline const time_point to_time_point() const {
        return _time;
    }

    inline void set_never_expired() {
        _time = never_expire_timepoint;
    }
};

// cache_entry should be allocated by LSA.
enum class entry_type {
    ENTRY_FLOAT = 0,
    ENTRY_INT64 = 1,
    ENTRY_BYTES = 2,
    ENTRY_LIST  = 3,
    ENTRY_MAP   = 4,
    ENTRY_SET   = 5,
    ENTRY_SSET  = 6,
    ENTRY_HLL   = 7,
};
class cache_entry
{
protected:
    friend class cache;
    using hook_type = boost::intrusive::unordered_set_member_hook<>;
    hook_type _cache_link;
    entry_type _type;
    managed_ref<managed_bytes> _key;
    size_t _key_hash;
    union storage {
        double _float_number;
        int64_t _integer_number;
        managed_ref<managed_bytes> _bytes;
        managed_ref<list_lsa> _list;
        managed_ref<dict_lsa> _dict;
        managed_ref<sset_lsa> _sset;
        storage() {}
        ~storage() {}
    } _storage;
    bi::list_member_hook<> _timer_link;
    expiration _expiry;
public:
    using time_point = expiration::time_point;
    using duration = expiration::duration;
    cache_entry(const sstring& key, size_t hash, entry_type type) noexcept
        : _cache_link()
        , _type(type)
        , _key_hash(hash)
    {
        _key = make_managed<managed_bytes>(bytes_view{reinterpret_cast<const signed char*>(key.data()), key.size()});
    }

    cache_entry(const sstring& key, size_t hash, double data) noexcept
        : cache_entry(key, hash, entry_type::ENTRY_FLOAT)
    {
        _storage._float_number = data;
    }

    cache_entry(const sstring key, size_t hash, int64_t data) noexcept
        : cache_entry(key, hash, entry_type::ENTRY_INT64)
    {
        _storage._integer_number = data;
    }

    cache_entry(const sstring key, size_t hash, size_t origin_size) noexcept
        : cache_entry(key, hash, entry_type::ENTRY_BYTES)
    {
        _storage._bytes = make_managed<managed_bytes>(origin_size, 0);
    }

    cache_entry(const sstring& key, size_t hash, const sstring& data) noexcept
        : cache_entry(key, hash, entry_type::ENTRY_BYTES)
    {
        _storage._bytes = make_managed<managed_bytes>(bytes_view{reinterpret_cast<const signed char*>(data.data()), data.size()});
    }
    struct list_initializer {};
    cache_entry(const sstring& key, size_t hash, list_initializer) noexcept
        : cache_entry(key, hash, entry_type::ENTRY_LIST)
    {
        _storage._list = make_managed<list_lsa>();
    }

    struct dict_initializer {};
    cache_entry(const sstring& key, size_t hash, dict_initializer) noexcept
        : cache_entry(key, hash, entry_type::ENTRY_MAP)
    {
        _storage._dict = make_managed<dict_lsa>();
    }

    struct set_initializer {};
    cache_entry(const sstring& key, size_t hash, set_initializer) noexcept
        : cache_entry(key, hash, entry_type::ENTRY_SET)
    {
        _storage._dict = make_managed<dict_lsa>();
    }

    struct sset_initializer {};
    cache_entry(const sstring& key, size_t hash, sset_initializer) noexcept
        : cache_entry(key, hash, entry_type::ENTRY_SSET)
    {
        _storage._sset = make_managed<sset_lsa>();
    }

    struct hll_initializer {};
    cache_entry(const sstring& key, size_t hash, hll_initializer) noexcept
        : cache_entry(key, hash, entry_type::ENTRY_HLL)
    {
        _storage._bytes = make_managed<managed_bytes>(HLL_BYTES_SIZE, 0);
    }

    cache_entry(cache_entry&& o) noexcept
        : _cache_link(std::move(o._cache_link))
        , _type(o._type)
        , _key(std::move(_key))
        , _key_hash(std::move(o._key_hash))
    {
        switch (_type) {
            case entry_type::ENTRY_FLOAT:
                _storage._float_number = std::move(o._storage._float_number);
                break;
            case entry_type::ENTRY_INT64:
                _storage._integer_number = std::move(o._storage._integer_number);
                break;
            case entry_type::ENTRY_BYTES:
            case entry_type::ENTRY_HLL:
                _storage._bytes = std::move(o._storage._bytes);
                break;
            case entry_type::ENTRY_LIST:
                _storage._list = std::move(o._storage._list);
                break;
            case entry_type::ENTRY_MAP:
            case entry_type::ENTRY_SET:
                _storage._dict = std::move(o._storage._dict);
                break;
            case entry_type::ENTRY_SSET:
                _storage._sset = std::move(o._storage._sset);
                break;
        }
    }

    virtual ~cache_entry()
    {
        switch (_type) {
            case entry_type::ENTRY_FLOAT:
            case entry_type::ENTRY_INT64:
                break;
            case entry_type::ENTRY_BYTES:
            case entry_type::ENTRY_HLL:
                _storage._bytes.~managed_ref<managed_bytes>();
                break;
            case entry_type::ENTRY_LIST:
                _storage._list.~managed_ref<list_lsa>();
                break;
            case entry_type::ENTRY_MAP:
            case entry_type::ENTRY_SET:
                _storage._dict.~managed_ref<dict_lsa>();
                break;
            case entry_type::ENTRY_SSET:
                _storage._sset.~managed_ref<sset_lsa>();
                break;
        }
    }

    const sstring type_name() const
    {
        switch (_type) {
            case entry_type::ENTRY_FLOAT:
            case entry_type::ENTRY_INT64:
            case entry_type::ENTRY_BYTES:
                return msg_type_string;
            case entry_type::ENTRY_HLL:
                return msg_type_hll;
            case entry_type::ENTRY_LIST:
                return msg_type_list;
            case entry_type::ENTRY_MAP:
                return msg_type_hash;
            case entry_type::ENTRY_SET:
                return msg_type_set;
            case entry_type::ENTRY_SSET:
                return msg_type_zset;
        }
        return msg_type_none;
    }
    friend inline bool operator == (const cache_entry &l, const cache_entry &r) {
        return (l._key_hash == r._key_hash) && (*(l._key) == *(r._key));
    }

    friend inline std::size_t hash_value(const cache_entry& e) {
        return e._key_hash;
    }

    struct compare {
    public:
        inline bool operator () (const cache_entry& l, const cache_entry& r) const {
            const auto& lk = *(l._key);
            const auto& rk = *(r._key);
            return (l.key_hash() == r.key_hash()) && (lk == rk);
        }
        inline bool operator () (const redis_key& k, const cache_entry& e) const {
            return (k.hash() == e.key_hash()) && (k.size() == e.key_size()) && (memcmp(k.data(), e.key_data(), k.size()) == 0);
        }
        inline bool operator () (const cache_entry& e, const redis_key& k) const {
            return (k.hash() == e.key_hash()) && (k.size() == e.key_size()) && (memcmp(k.data(), e.key_data(), k.size()) == 0);
        }
    };
public:
    inline const clock_type::time_point get_timeout() const
    {
        return _expiry.to_time_point();
    }

    inline const bool ever_expires() const
    {
        return _expiry.ever_expires();
    }

    inline void set_never_expired()
    {
        return _expiry.set_never_expired();
    }

    inline void set_expiry(const expiration& expiry)
    {
        _expiry = expiry;
    }

    inline const size_t time_of_live() const
    {
        auto dur = get_timeout() - clock_type::now();
        return static_cast<size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(dur).count());
    }

    inline bool cancel() const
    {
        return false;
    }

    inline size_t key_hash() const
    {
        return _key_hash;
    }

    inline size_t key_size() const
    {
        return _key->size();
    }

    inline const bytes_view key() const
    {
        return { _key->data(), _key->size() };
    }

    inline const char* key_data() const
    {
        return reinterpret_cast<const char*>(_key->data());
    }
    inline size_t value_bytes_size() const
    {
        return _storage._bytes->size();
    }
    inline const char* value_bytes_data() const
    {
        return reinterpret_cast<const char*>(_storage._bytes->data());
    }
    inline entry_type type() const
    {
        return _type;
    }
    inline bool type_of_float() const
    {
        return _type == entry_type::ENTRY_FLOAT;
    }
    inline bool type_of_integer() const
    {
        return _type == entry_type::ENTRY_INT64;
    }
    inline bool type_of_bytes() const
    {
        return _type == entry_type::ENTRY_BYTES;
    }
    inline bool type_of_list() const
    {
        return _type == entry_type::ENTRY_LIST;
    }
    inline bool type_of_map() const {
        return _type == entry_type::ENTRY_MAP;
    }
    inline bool type_of_set() const {
        return _type == entry_type::ENTRY_SET;
    }
    inline bool type_of_sset() const {
        return _type == entry_type::ENTRY_SSET;
    }
    inline bool type_of_hll() const {
        return _type == entry_type::ENTRY_HLL;
    }
    inline int64_t value_integer() const
    {
        return _storage._integer_number;
    }
    inline void value_integer_incr(int64_t step)
    {
        _storage._integer_number += step;
    }

    inline double value_float() const
    {
        return _storage._float_number;
    }

    inline void value_float_incr(double step)
    {
        _storage._float_number += step;
    }
    inline managed_bytes& value_bytes() {
        return *(_storage._bytes);
    }
    inline const managed_bytes& value_bytes() const {
        return *(_storage._bytes);
    }
    inline list_lsa& value_list() {
        return *(_storage._list);
    }
    inline const list_lsa& value_list() const {
        return *(_storage._list);
    }
    inline dict_lsa& value_map() {
        return *(_storage._dict);
    }
    inline const dict_lsa& value_map() const {
        return *(_storage._dict);
    }
    inline dict_lsa& value_set() {
        return *(_storage._dict);
    }
    inline const dict_lsa& value_set() const {
        return *(_storage._dict);
    }
    inline sset_lsa& value_sset() {
        return *(_storage._sset);
    }
    inline const sset_lsa& value_sset() const {
        return *(_storage._sset);
    }
};

static constexpr const size_t DEFAULT_INITIAL_SIZE = 1 << 20;

class cache {
    using cache_type = boost::intrusive::unordered_set<cache_entry,
        boost::intrusive::member_hook<cache_entry, cache_entry::hook_type, &cache_entry::_cache_link>,
        boost::intrusive::power_2_buckets<true>,
        boost::intrusive::constant_time_size<true>>;
    using cache_iterator = typename cache_type::iterator;
    using const_cache_iterator = typename cache_type::const_iterator;
    static constexpr size_t initial_bucket_count = DEFAULT_INITIAL_SIZE;
    static constexpr float load_factor = 0.75f;
    size_t _resize_up_threshold = load_factor * initial_bucket_count;
    cache_type::bucket_type* _buckets;
    cache_type _store;
    seastar::timer_set<cache_entry, &cache_entry::_timer_link> _alive;
    timer<clock_type> _timer;
    clock_type::duration _wc_to_clock_type_delta;
    allocation_strategy* alloc;
    using expired_entry_releaser_type = std::function<void(cache_entry& e)>;
    expired_entry_releaser_type _expired_entry_releaser;
public:
    cache ()
        : _buckets(new cache_type::bucket_type[initial_bucket_count])
        , _store(cache_type::bucket_traits(_buckets, initial_bucket_count))
    {
        _timer.set_callback([this] { erase_expired_entries(); });
    }
    ~cache ()
    {
    }

    inline size_t expiring_size() const
    {
        return _alive.size();
    }


    void set_expired_entry_releaser(expired_entry_releaser_type&& releaser)
    {
        _alive.clear();
        _expired_entry_releaser = std::move(releaser);
    }

    void flush_all()
    {
        for (auto it = _store.begin(); it != _store.end(); ++it) {
            if (it->ever_expires()) {
                _alive.remove(*it);
            }
        }
        _store.erase_and_dispose(_store.begin(), _store.end(), current_deleter<cache_entry>());
    }

    inline bool erase(const redis_key& key)
    {
        static auto hash_fn = [] (const redis_key& k) -> size_t { return k.hash(); };
        auto it = _store.find(key, hash_fn, cache_entry::compare());
        if (it != _store.end()) {
            if (it->ever_expires()) {
                _alive.remove(*it);
            }
            _store.erase_and_dispose(it, current_deleter<cache_entry>());
            return true;
        }
        return false;
    }

    inline bool erase(cache_entry& e)
    {
        auto it = _store.iterator_to(e);
        _store.erase_and_dispose(it, current_deleter<cache_entry>());
        return true;
    }

    inline bool replace(cache_entry* entry)
    {
        bool res = true;
        if (entry) {
            static auto hash_fn = [] (const cache_entry& e) -> size_t { return e.key_hash(); };
            auto it = _store.find(*entry, hash_fn, cache_entry::compare());
            if (it != _store.end()) {
                if (it->ever_expires()) {
                    _alive.remove(*it);
                }
                _store.erase_and_dispose(it, current_deleter<cache_entry>());
                res = false;
            }
        }
        insert(entry);
        return res;
    }

    inline bool replace(cache_entry* entry, long expired)
    {
        bool res = true;
        if (entry) {
            static auto hash_fn = [] (const cache_entry& e) -> size_t { return e.key_hash(); };
            auto it = _store.find(*entry, hash_fn, cache_entry::compare());
            if (it != _store.end()) {
                if (it->ever_expires()) {
                    _alive.remove(*it);
                }
                _store.erase_and_dispose(it, current_deleter<cache_entry>());
                res = false;
            }
        }
        insert(entry);
        return res;
    }

    // return value: true if the entry was inserted, otherwise false.
    inline bool insert_if(cache_entry* entry, long expired, bool nx, bool xx)
    {
        if (!entry) {
            return false;
        }
        static auto hash_fn = [] (const cache_entry& e) -> size_t { return e.key_hash(); };
        auto it = _store.find(*entry, hash_fn, cache_entry::compare());
        if (it != _store.end() && (xx || (!xx && !nx))) {
            if (it->ever_expires()) {
                _alive.remove(*it);
            }
            _store.erase_and_dispose(it, current_deleter<cache_entry>());
        }
        bool should_insert = (xx && it != _store.end()) || (nx && it == _store.end()) || (!nx && !xx);
        if (should_insert) {
            if (expired > 0) {
                auto expiry = expiration(expired);
                entry->set_expiry(expiry);
                if (_alive.insert(*entry)) {
                    _timer.rearm(entry->get_timeout());
                }
            }
            _store.insert(*entry);
            maybe_rehash();
            return true;
        }
        return false;
    }

    inline void insert(cache_entry* entry)
    {
        auto& etnry_reference = *entry;
        _store.insert(etnry_reference);
        // maybe cache will be rehashed.
        maybe_rehash();
    }

    template <typename Func>
    inline std::result_of_t<Func(const cache_entry* e)> with_entry_run(const redis_key& rk, Func&& func) const {
        static auto hash_fn = [] (const redis_key& k) -> size_t { return k.hash(); };
        auto it = _store.find(rk, hash_fn, cache_entry::compare());
        if (it != _store.end()) {
            const auto& e = *it;
            return func(&e);
        }
        else {
            return func(nullptr);
        }
    }

    template <typename Func>
    inline std::result_of_t<Func(cache_entry* e)> with_entry_run(const redis_key& rk, Func&& func) {
        static auto hash_fn = [] (const redis_key& k) -> size_t { return k.hash(); };
        auto it = _store.find(rk, hash_fn, cache_entry::compare());
        if (it != _store.end()) {
            auto& e = *it;
            return func(&e);
        }
        else {
            return func(nullptr);
        }
    }


    inline bool exists(const redis_key& rk)
    {
        static auto hash_fn = [] (const redis_key& k) -> size_t { return k.hash(); };
        auto it = _store.find(rk, hash_fn, cache_entry::compare());
        return it != _store.end();
    }

    void maybe_rehash()
    {
        if (_store.size() >= _resize_up_threshold) {
            auto new_size = _store.bucket_count() * 2;
            auto old_buckets = _buckets;
            try {
                _buckets = new cache_type::bucket_type[new_size];
            } catch (const std::bad_alloc& e) {
                return;
            }
            _store.rehash(typename cache_type::bucket_traits(_buckets, new_size));
            delete[] old_buckets;
            _resize_up_threshold = _store.bucket_count() * load_factor;
        }
    }

    inline size_t size() const
    {
        return _store.size();
    }

    inline bool empty() const
    {
        return _store.empty();
    }

    bool expire(const redis_key& rk, long expired)
    {
        bool result = false;
        static auto hash_fn = [] (const redis_key& k) -> size_t { return k.hash(); };
        auto it = _store.find(rk, hash_fn, cache_entry::compare());
        if (it != _store.end()) {
            auto expiry = expiration(expired);
            it->set_expiry(expiry);
            auto& ref = *it;
            if (_alive.insert(ref)) {
                _timer.rearm(it->get_timeout());
                result = true;
            }
        }
        return result;
    }

    void erase_expired_entries()
    {
        assert(_expired_entry_releaser);

        auto expired_entries = _alive.expire(clock_type::now());
        while (!expired_entries.empty()) {
            auto entry = &*expired_entries.begin();
            expired_entries.pop_front();
            _expired_entry_releaser(*entry);
        }
        _timer.arm(_alive.get_next_timeout());
    }

    bool never_expired(const redis_key& rk)
    {
        bool result = false;
        static auto hash_fn = [] (const redis_key& k) -> size_t { return k.hash(); };
        auto it = _store.find(rk, hash_fn, cache_entry::compare());
        if (it != _store.end() && it->ever_expires()) {
            it->set_never_expired();
            auto& ref = *it;
            _alive.remove(ref);
            result = true;
        }
        return result;
    }
};
}

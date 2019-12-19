#pragma once
#include <memory>
#include <map>

namespace derecho {
namespace cascade {

#define debug_enter_func_with_args(format,...) \
    dbg_default_debug("Entering {} with parameter:" #format ".", __func__, __VA_ARGS__)
#define debug_leave_func_with_value(format,...) \
    dbg_default_debug("Leaving {} with " #format "." , __func__, __VA_ARGS__)
#define debug_enter_func() dbg_default_debug("Entering {}.")
#define debug_leave_func() dbg_default_debug("Leaving {}.")

///////////////////////////////////////////////////////////////////////////////
// 1 - Volatile Cascade Store Implementation
///////////////////////////////////////////////////////////////////////////////

template<typename KT, typename VT, KT* IK, VT* IV>
std::tuple<persistent::version_t,uint64_t> VolatileCascadeStore<KT,VT,IK,IV>::put(const VT& value) {
    debug_enter_func_with_args("value.key={}",value.key);
    derecho::Replicated<VolatileCascadeStore>& subgroup_handle = group->template get_subgroup<VolatileCascadeStore>(this->subgroup_id);
    auto results = subgroup_handle.template ordered_send<RPC_NAME(ordered_put)>(value);
    auto& replies = results.get();
    std::tuple<persistent::version_t,uint64_t> ret(INVALID_VERSION,0);
    // TODO: verfiy consistency ?
    for (auto& reply_pair : replies) {
        ret = reply_pair.second.get();
    }
    debug_leave_func_with_value("version=0x{:x},timestamp={}",std::get<0>(ret),std::get<1>(ret));
    return ret;
}

template<typename KT, typename VT, KT* IK, VT* IV>
std::tuple<persistent::version_t,uint64_t> VolatileCascadeStore<KT,VT,IK,IV>::remove(const KT& key) {
    debug_enter_func_with_args("key={}",key);
    derecho::Replicated<VolatileCascadeStore>& subgroup_handle = group->template get_subgroup<VolatileCascadeStore>(this->subgroup_id);
    auto results = subgroup_handle.template ordered_send<RPC_NAME(ordered_remove)>(key);
    auto& replies = results.get();
    std::tuple<persistent::version_t,uint64_t> ret(INVALID_VERSION,0);
    // TODO: verify consistency ?
    for (auto& reply_pair : replies) {
        ret = reply_pair.second.get();
    }
    debug_leave_func_with_value("version=0x{:x},timestamp={}",std::get<0>(ret),std::get<1>(ret));
    return ret;
}

template<typename KT, typename VT, KT* IK, VT* IV>
const VT VolatileCascadeStore<KT,VT,IK,IV>::get(const KT& key, const persistent::version_t& ver) {
    debug_enter_func_with_args("key={},ver=0x{:x}",key,ver);
    if (ver != INVALID_VERSION) {
        debug_leave_func_with_value("Cannot support versioned get, ver=0x{:x}", ver);
        return *IV;
    }
    derecho::Replicated<VolatileCascadeStore>& subgroup_handle = group->template get_subgroup<VolatileCascadeStore>(this->subgroup_id);
    auto results = subgroup_handle.template ordered_send<RPC_NAME(ordered_get)>(key);
    auto& replies = results.get();
    // TODO: verify consistency ?
    // for (auto& reply_pair : replies) {
    //     ret = reply_pair.second.get();
    // }
    debug_leave_func();
    return replies.begin()->second.get();
}

template<typename KT, typename VT, KT* IK, VT* IV>
const VT VolatileCascadeStore<KT,VT,IK,IV>::get_by_time(const KT& key, const uint64_t& ts_us) {
    // VolatileCascadeStore does not support that.
    debug_enter_func();
    debug_leave_func();

    return *IV;
}

template<typename KT, typename VT, KT* IK, VT* IV>
std::tuple<persistent::version_t,uint64_t> VolatileCascadeStore<KT,VT,IK,IV>::ordered_put(const VT& value) {
    debug_enter_func_with_args("key={}",value.key);

    std::tuple<persistent::version_t,uint64_t> version = group->template get_subgroup<VolatileCascadeStore>(this->subgroup_id).get_next_version();
    this->kv_map.erase(value.key);
    value.ver = version;
    this->kv_map.emplace(value.key, value); // copy constructor
    if (cascade_watcher) {
        cascade_watcher(this->subgroup_id,
            group->template get_subgroup<VolatileCascadeStore>(this->subgroup_id).get_shard_num(),
            value.key, value);
    }

    debug_leave_func_with_value("version=0x{:x},timestamp={}",std::get<0>(version), std::get<1>(version));

    return version;
}

template<typename KT, typename VT, KT* IK, VT* IV>
std::tuple<persistent::version_t,uint64_t> VolatileCascadeStore<KT,VT,IK,IV>::ordered_remove(const KT& key) {
    debug_enter_func_with_args("key={}",key);

    std::tuple<persistent::version_t,uint64_t> version = group->template get_subgroup<VolatileCascadeStore>(this->subgroup_id).get_next_version();
    if(this->kv_map.erase(key)) {
        if (cascade_watcher) {
            cascade_watcher(this->subgroup_id,
                group->template get_subgroup<VolatileCascadeStore>(this->subgroup_id).get_shard_num(),
                key, *IV);
        }
    }

    debug_leave_func_with_value("version=0x{:x},timestamp={}",std::get<0>(version), std::get<1>(version));
    
    return version;
}

template<typename KT, typename VT, KT* IK, VT* IV>
const VT VolatileCascadeStore<KT,VT,IK,IV>::ordered_get(const KT& key) {
    debug_enter_func_with_args("key={}",key);

    if (this->kv_map.find(key) != this->kv_map.end()) {
        debug_leave_func_with_value("key={}",key);
        return this->kv_map.at(key);
    } else {
        debug_leave_func();
        return *IV;
    }
}

template<typename KT, typename VT, KT* IK, VT* IV>
std::unique_ptr<VolatileCascadeStore<KT,VT,IK,IV>> VolatileCascadeStore<KT,VT,IK,IV>::from_bytes(
    mutils::DeserializationManager* dsm, 
    char const* buf) {
    auto subgroup_id_ptr = mutils::from_bytes<subgroup_id_t>(dsm,buf);
    auto kv_map_ptr = mutils::from_bytes<std::map<KT,VT>>(dsm,buf+mutils::bytes_size(*subgroup_id_ptr));
    auto volatile_cascade_store_ptr =
        std::make_unique<VolatileCascadeStore>(*subgroup_id_ptr,std::move(*kv_map_ptr),dsm->mgr<CascadeWatcher<KT,VT,IK,IV>>());
    return volatile_cascade_store_ptr;
}

template<typename KT, typename VT, KT* IK, VT* IV>
VolatileCascadeStore<KT,VT,IK,IV>::VolatileCascadeStore(subgroup_id_t sid,
    const CascadeWatcher<KT,VT,IK,IV>& cw):
    subgroup_id(sid),
    cascade_watcher(cw) {
    debug_enter_func_with_args("sid={}",sid);
    debug_leave_func();
}

template<typename KT, typename VT, KT* IK, VT* IV>
VolatileCascadeStore<KT,VT,IK,IV>::VolatileCascadeStore(subgroup_id_t sid,
    const std::map<KT,VT>& _kvm, const CascadeWatcher<KT,VT,IK,IV>& cw):
    subgroup_id(sid),
    kv_map(_kvm),
    cascade_watcher(cw) {
    debug_enter_func_with_args("sid={}, copy to kv_map, size={}",sid,kv_map.size());
    debug_leave_func();
}

template<typename KT, typename VT, KT* IK, VT* IV>
VolatileCascadeStore<KT,VT,IK,IV>::VolatileCascadeStore(subgroup_id_t sid,
    std::map<KT,VT>&& _kvm, const CascadeWatcher<KT,VT,IK,IV>& cw):
    subgroup_id(sid),
    kv_map(std::move(_kvm)),
    cascade_watcher(cw) {
    debug_enter_func_with_args("sid={}, move to kv_map, size={}",sid,kv_map.size());
    debug_leave_func();
}

///////////////////////////////////////////////////////////////////////////////
// 2 - Persistent Cascade Store Implementation
///////////////////////////////////////////////////////////////////////////////
template <typename KT, typename VT, KT* IK, VT* IV>
void DeltaCascadeStoreCore<KT,VT,IK,IV>::_Delta::set_opid(DeltaCascadeStoreCore<KT,VT,IK,IV>::_OPID opid) {
    assert(buffer != nullptr);
    assert(capacity >= sizeof(uint32_t));
    *(DeltaCascadeStoreCore::_OPID*)buffer = opid;
}

template <typename KT, typename VT, KT* IK, VT* IV>
void DeltaCascadeStoreCore<KT,VT,IK,IV>::_Delta::set_data_len(const size_t& dlen) {
    assert(capacity >= (dlen + sizeof(uint32_t)));
    this->len = dlen + sizeof(uint32_t);
}

template <typename KT, typename VT, KT* IK, VT* IV>
char* DeltaCascadeStoreCore<KT,VT,IK,IV>::_Delta::data_ptr() {
    assert(buffer != nullptr);
    assert(capacity > sizeof(uint32_t));
    return buffer + sizeof(uint32_t);
}

template <typename KT, typename VT, KT* IK, VT *IV>
void DeltaCascadeStoreCore<KT,VT,IK,IV>::_Delta::calibrate(const size_t& dlen) {
    size_t new_cap = dlen + sizeof(uint32_t);
    if(this->capacity >= new_cap) {
        return;
    }
    // calculate new capacity
    int width = sizeof(size_t) << 3;
    int right_shift_bits = 1;
    new_cap--;
    while(right_shift_bits < width) {
        new_cap |= new_cap >> right_shift_bits;
        right_shift_bits = right_shift_bits << 1;
    }
    new_cap++;
    // resize
    this->buffer = (char*)realloc(buffer, new_cap);
    if(this->buffer == nullptr) {
        dbg_default_crit("{}:{} Failed to allocate delta buffer. errno={}", __FILE__, __LINE__, errno);
        throw derecho::derecho_exception("Failed to allocate delta buffer.");
    } else {
        this->capacity = new_cap;
    }
}

template <typename KT, typename VT, KT* IK, VT *IV>
bool DeltaCascadeStoreCore<KT,VT,IK,IV>::_Delta::is_empty() {
    return (this->len == 0);
}

template <typename KT, typename VT, KT* IK, VT *IV>
void DeltaCascadeStoreCore<KT,VT,IK,IV>::_Delta::clean() {
    this->len = 0;
}

template <typename KT, typename VT, KT* IK, VT *IV>
void DeltaCascadeStoreCore<KT,VT,IK,IV>::_Delta::destroy() {
    if(this->capacity > 0) {
        free(this->buffer);
    }
}

template <typename KT, typename VT, KT* IK, VT *IV>
void DeltaCascadeStoreCore<KT,VT,IK,IV>::initialize_delta() {
    delta.buffer = (char*)malloc(DEFAULT_DELTA_BUFFER_CAPACITY);
    if (delta.buffer == nullptr) {
        dbg_default_crit("{}:{} Failed to allocate delta buffer. errno={}", __FILE__, __LINE__, errno);
        throw derecho::derecho_exception("Failed to allocate delta buffer.");
    }
    delta.capacity = DEFAULT_DELTA_BUFFER_CAPACITY;
    delta.len = 0;
}

template <typename KT, typename VT, KT* IK, VT *IV>
void DeltaCascadeStoreCore<KT,VT,IK,IV>::finalize_current_delta(const persistent::DeltaFinalizer& df) {
    df(this->delta.buffer, this->delta.len);
    this->delta.clean();
}

template <typename KT, typename VT, KT* IK, VT *IV>
void DeltaCascadeStoreCore<KT,VT,IK,IV>::apply_delta(char const* const delta) {
    const char* data = (delta + sizeof(const uint32_t));
    switch(*static_cast<const uint32_t*>(delta)) {
        case PUT:
            apply_ordered_put(*mutils::from_bytes<VT>(nullptr,data));
            break;
        case REMOVE:
            apply_ordered_remove(*mutils::from_bytes<KT>(nullptr,data));
            break;
        default:
            std::cerr << __FILE__ << ":" << __LINE__ << ":" << __func__ << " " << std::endl;
    };
}

template <typename KT, typename VT, KT* IK, VT *IV>
std::unique_ptr<DeltaCascadeStoreCore<KT,VT,IK,IV>> DeltaCascadeStoreCore<KT,VT,IK,IV>::create(mutils::DeserializationManager* dm) {
    if (dm != nullptr) {
        try {
            return std::make_unique<DeltaCascadeStoreCore<KT,VT,IK,IV>>();
        } catch (...) {
        }
    }
    return std::make_unique<DeltaCascadeStoreCore<KT,VT,IK,IV>>();
}

template <typename KT, typename VT, KT* IK, VT *IV>
void DeltaCascadeStoreCore<KT,VT,IK,IV>::apply_ordered_put(const VT& value) {
    // put
    this->kv_map.erase(value.key);
    this->kv_map.emplace(value.key,value);
}

template <typename KT, typename VT, KT* IK, VT *IV>
bool DeltaCascadeStoreCore<KT,VT,IK,IV>::apply_ordered_remove(const KT& key) {
    bool ret = false;
    // remove
    if (this->kv_map.erase(key)) {
        ret = true;
    }
    return ret;
}

template <typename KT, typename VT, KT* IK, VT *IV>
bool DeltaCascadeStoreCore<KT,VT,IK,IV>::ordered_put(const VT& value) {
    // create delta.
    assert(this->delta.is_empty());
    this->delta.calibrate(mutils::bytes_size(value));
    mutils::to_bytes(value,this->delta.data_ptr());
    this->delta.set_data_len(mutils::bytes_size(value));
    this->delta.set_opid(DeltaCascadeStoreCore<KT,VT,IK,IV>::PUT);
    // apply ordered_put
    apply_ordered_put(value);
    return true;
}

template <typename KT, typename VT, KT* IK, VT *IV>
bool DeltaCascadeStoreCore<KT,VT,IK,IV>::ordered_remove(const KT& key) {
    // create delta.
    assert(this->delta.is_empty());
    this->delta.calibrate(mutils::bytes_size(key));
    mutils::to_bytes(key,this->delta.data_ptr());
    this->delta.set_data_len(mutils::bytes_size(key));
    this->delta.set_opid(DeltaCascadeStoreCore<KT,VT,IK,IV>::REMOVE);
    // remove
    return apply_ordered_remove(key);
}

template <typename KT, typename VT, KT* IK, VT* IV>
const VT DeltaCascadeStoreCore<KT,VT,IK,IV>::ordered_get(const KT& key) {
    if (kv_map.find(key) != kv_map.end()) {
        return kv_map.at(key);
    } else {
        return *IV;
    }
}

template <typename KT, typename VT, KT* IK, VT* IV>
DeltaCascadeStoreCore<KT,VT,IK,IV>::DeltaCascadeStoreCore() {
    initialize_delta();
}

template <typename KT, typename VT, KT* IK, VT* IV>
DeltaCascadeStoreCore<KT,VT,IK,IV>::DeltaCascadeStoreCore(const std::map<KT,VT>& _kv_map): kv_map(_kv_map) {
    initialize_delta();
}

template <typename KT, typename VT, KT* IK, VT* IV>
DeltaCascadeStoreCore<KT,VT,IK,IV>::DeltaCascadeStoreCore(std::map<KT,VT>&& _kv_map): kv_map(_kv_map) {
    initialize_delta();
}

template<typename KT, typename VT, KT* IK, VT* IV>
DeltaCascadeStoreCore<KT,VT,IK,IV>::~DeltaCascadeStoreCore() {
    if (this->delta.buffer != nullptr) {
        free(this->delta.buffer);
    }
}

template<typename KT, typename VT, KT* IK, VT* IV, persistent::StorageType ST>
std::tuple<persistent::version_t,uint64_t> PersistentCascadeStore<KT,VT,IK,IV,ST>::put(const VT& value) {
    debug_enter_func_with_args("value.key={}",value.key);
    derecho::Replicated<PersistentCascadeStore>& subgroup_handle = group->template get_subgroup<PersistentCascadeStore>(this->subgroup_id);
    auto results = subgroup_handle.template ordered_send<RPC_NAME(ordered_put)>(value);
    auto& replies = results.get();
    std::tuple<persistent::version_t,uint64_t> ret(INVALID_VERSION,0);
    // TODO: verfiy consistency ?
    for (auto& reply_pair : replies) {
        ret = reply_pair.second.get();
    }
    debug_leave_func_with_value("version=0x{:x},timestamp={}",std::get<0>(ret),std::get<1>(ret));
    return ret;
}

template<typename KT, typename VT, KT* IK, VT* IV, persistent::StorageType ST>
std::tuple<persistent::version_t,uint64_t> PersistentCascadeStore<KT,VT,IK,IV,ST>::remove(const KT& key) {
    debug_enter_func_with_args("key={}",key);
    derecho::Replicated<PersistentCascadeStore>& subgroup_handle = group->template get_subgroup<PersistentCascadeStore>(this->subgroup_id);
    auto results = subgroup_handle.template ordered_send<RPC_NAME(ordered_remove)>(key);
    auto& replies = results.get();
    std::tuple<persistent::version_t,uint64_t> ret(INVALID_VERSION,0);
    // TODO: verify consistency ?
    for (auto& reply_pair : replies) {
        ret = reply_pair.second.get();
    }
    debug_leave_func_with_value("version=0x{:x},timestamp={}",std::get<0>(ret),std::get<1>(ret));
    return ret;
}

template<typename KT, typename VT, KT* IK, VT* IV, persistent::StorageType ST>
const VT PersistentCascadeStore<KT,VT,IK,IV,ST>::get(const KT& key, const persistent::version_t& ver) {
    debug_enter_func_with_args("key={},ver=0x{:x}",key,ver);
    if (ver != INVALID_VERSION) {
        debug_leave_func_with_value("Cannot support versioned get, ver=0x{:x}", ver);
        return *IV;
    }
    derecho::Replicated<PersistentCascadeStore>& subgroup_handle = group->template get_subgroup<PersistentCascadeStore>(this->subgroup_id);
    auto results = subgroup_handle.template ordered_send<RPC_NAME(ordered_get)>(key);
    auto& replies = results.get();
    // TODO: verify consistency ?
    // for (auto& reply_pair : replies) {
    //     ret = reply_pair.second.get();
    // }
    debug_leave_func();
    return replies.begin()->second.get();
}

template<typename KT, typename VT, KT* IK, VT* IV, persistent::StorageType ST>
const VT PersistentCascadeStore<KT,VT,IK,IV,ST>::get_by_time(const KT& key, const uint64_t& ts_us) {
    debug_enter_func_with_args("key={},ts_us={}",key,ts_us);
    const HLC hlc(ts_us,0ull);
    try {
        debug_leave_func();
        return persistent_cascade_store.get(hlc)->kv_map.at(key);
    } catch (const int64_t &ex) {
        dbg_default_warn("temporal query throws exception:0x{:x}. key={}, ts={}", ex, key, ts_us);
    } catch (...) {
        dbg_default_warn("temporal query throws unknown exception. key={}, ts={}", key, ts_us);
    }
    debug_leave_func();
    return *IV;
}

template<typename KT, typename VT, KT* IK, VT* IV, persistent::StorageType ST>
std::tuple<persistent::version_t,uint64_t> PersistentCascadeStore<KT,VT,IK,IV,ST>::ordered_put(const VT& value) {
    debug_enter_func_with_args("key={}",value.key);

    std::tuple<persistent::version_t,uint64_t> version = group->template get_subgroup<PersistentCascadeStore>(this->subgroup_id).get_next_version();
    this->persistent_cascade_store.ordered_put(value);
    value.ver = version;
    if (cascade_watcher) {
        cascade_watcher(this->subgroup_id,
            group->template get_subgroup<PersistentCascadeStore>(this->subgroup_id).get_shard_num(),
            value.key, value);
    }

    debug_leave_func_with_value("version=0x{:x},timestamp={}",std::get<0>(version), std::get<1>(version));

    return version;
}

template<typename KT, typename VT, KT* IK, VT* IV, persistent::StorageType ST>
std::tuple<persistent::version_t,uint64_t> PersistentCascadeStore<KT,VT,IK,IV,ST>::ordered_remove(const KT& key) {
    debug_enter_func_with_args("key={}",key);

    std::tuple<persistent::version_t,uint64_t> version = group->template get_subgroup<PersistentCascadeStore>(this->subgroup_id).get_next_version();
    if(this->persistent_cascade_store.ordered_remove.erase(key)) {
        if (cascade_watcher) {
            cascade_watcher(this->subgroup_id,
                group->template get_subgroup<PersistentCascadeStore>(this->subgroup_id).get_shard_num(),
                key, *IV);
        }
    }

    debug_leave_func_with_value("version=0x{:x},timestamp={}",std::get<0>(version), std::get<1>(version));
    
    return version;
}

template<typename KT, typename VT, KT* IK, VT* IV, persistent::StorageType ST>
const VT PersistentCascadeStore<KT,VT,IK,IV,ST>::ordered_get(const KT& key) {
    debug_enter_func_with_args("key={}",key);

    debug_leave_func();

    return this->persistent_cascade_store.ordered_get(key);
}

template<typename KT, typename VT, KT* IK, VT* IV, persistent::StorageType ST>
PersistentCascadeStore<KT,VT,IK,IV,ST>::PersistentCascadeStore(subgroup_id_t sid,
                                               const CascadeWatcher<KT,VT,IK,IV>& cw):
                                               subgroup_id(sid),
                                               cascade_watcher(cw) {}


template<typename KT, typename VT, KT* IK, VT* IV, persistent::StorageType ST>
PersistentCascadeStore<KT,VT,IK,IV,ST>::PersistentCascadeStore(subgroup_id_t sid,
                                               persistent::Persistent<DeltaCascadeStoreCore<KT,VT,IK,IV>>&&
                                               _persistent_cascade_store,
                                               const CascadeWatcher<KT,VT,IK,IV>& cw):
                                               subgroup_id(sid),
                                               persistent_cascade_store(std::move(_persistent_cascade_store)),
                                               cascade_watcher(cw) {}


}//namespace cascade
}//namespace derecho

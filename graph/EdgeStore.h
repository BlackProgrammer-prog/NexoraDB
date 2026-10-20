#pragma once
#include "Graphtypes.h"
#include <stdexcept>
#include <type_traits>

namespace nexora::graph {
// Dense slot IDs do not need hash nodes or a second stored key. Sparse outliers
// stay in a fallback map instead of forcing an enormous vector allocation.
class EdgeStore {
    std::vector<EdgeRecord> slots_;
    std::unordered_map<EdgeId,EdgeRecord> sparse_;
    size_t count_=0;
public:
    template<bool Const> class Iterator {
        using Owner=std::conditional_t<Const,const EdgeStore,EdgeStore>;
        using Sparse=std::conditional_t<Const,decltype(sparse_)::const_iterator,decltype(sparse_)::iterator>;
        Owner* owner_=nullptr;
        size_t index_=0;
        Sparse sparse_it_{};
        friend class EdgeStore;
        void skip() {
            while (index_<owner_->slots_.size() && !owner_->slots_[index_].isActive()) ++index_;
        }
        Iterator(Owner* owner,size_t index,Sparse sparse):owner_(owner),index_(index),sparse_it_(sparse) { skip(); }
    public:
        using Record=std::conditional_t<Const,const EdgeRecord,EdgeRecord>;
        using Reference=std::pair<EdgeId,Record&>;
        struct Arrow { Reference value; const Reference* operator->() const { return &value; } };
        Reference operator*() const {
            if (index_<owner_->slots_.size()) return {index_,owner_->slots_[index_]};
            return {sparse_it_->first,sparse_it_->second};
        }
        Arrow operator->() const { return {operator*()}; }
        Iterator& operator++() {
            if (index_<owner_->slots_.size()) { ++index_; skip(); }
            else ++sparse_it_;
            return *this;
        }
        bool operator==(const Iterator& rhs) const {
            return owner_==rhs.owner_ && index_==rhs.index_ && sparse_it_==rhs.sparse_it_;
        }
    };
    using iterator=Iterator<false>;
    using const_iterator=Iterator<true>;
    iterator begin() { return {this,0,sparse_.begin()}; }
    iterator end() { return {this,slots_.size(),sparse_.end()}; }
    const_iterator begin() const { return {this,0,sparse_.begin()}; }
    const_iterator end() const { return {this,slots_.size(),sparse_.end()}; }
    iterator find(EdgeId id) {
        if (id<slots_.size()) return slots_[id].isActive() ? iterator(this,id,sparse_.begin()) : end();
        auto it=sparse_.find(id); return it==sparse_.end() ? end() : iterator(this,slots_.size(),it);
    }
    const_iterator find(EdgeId id) const {
        if (id<slots_.size()) return slots_[id].isActive() ? const_iterator(this,id,sparse_.begin()) : end();
        auto it=sparse_.find(id); return it==sparse_.end() ? end() : const_iterator(this,slots_.size(),it);
    }
    EdgeRecord& operator[](EdgeId id) {
        if (id<slots_.size()) {
            if (!slots_[id].isActive()) { ++count_; slots_[id].edge_id=id; slots_[id].flags=FLAG_ACTIVE; }
            return slots_[id];
        }
        auto found=sparse_.find(id);
        if (found!=sparse_.end()) return found->second;
        // Grow only adjacent slots. A high sparse ID never becomes an allocation size.
        if (id==slots_.size()) {
            slots_.push_back({}); auto& record=slots_.back(); record.edge_id=id; record.flags=FLAG_ACTIVE;
            ++count_; return record;
        }
        auto [it,inserted]=sparse_.try_emplace(id);
        if (inserted) { ++count_; it->second.edge_id=id; it->second.flags=FLAG_ACTIVE; }
        return it->second;
    }
    const EdgeRecord& at(EdgeId id) const {
        const auto it=find(id); if (it==end()) throw std::out_of_range("inactive edge slot"); return it->second;
    }
    iterator erase(iterator it) {
        if (it==end()) return it;
        if (it.index_<slots_.size()) {
            slots_[it.index_].flags=FLAG_DELETED; --count_; ++it; return it;
        }
        it.sparse_it_=sparse_.erase(it.sparse_it_); --count_; return it;
    }
    size_t size() const noexcept { return count_; }
    void clear() { slots_.clear(); sparse_.clear(); count_=0; }
    size_t slotCapacity() const noexcept { return slots_.capacity(); }
    size_t sparseSize() const noexcept { return sparse_.size(); }
    void optimizeDense() {
        EdgeId high=slots_.empty() ? 0 : slots_.size()-1;
        for (const auto& [id,record]:sparse_) high=std::max(high,id);
        if (high==UINT64_MAX || high>=std::max<size_t>(4096,count_*2)) return;
        if (!count_) return;
        slots_.resize(high+1);
        for (const auto& [id,record]:sparse_) slots_[id]=record;
        sparse_.clear(); sparse_.rehash(0);
    }
    size_t allocatedBytesEstimate() const noexcept {
        return slots_.capacity()*sizeof(EdgeRecord)+sparse_.bucket_count()*sizeof(void*)+
               sparse_.size()*(sizeof(decltype(sparse_)::value_type)+2*sizeof(void*));
    }
};
}

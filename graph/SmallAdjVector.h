#pragma once
#include <algorithm>
#include <variant>
#include <vector>

namespace nexora::graph {
// One entry inline, no heap allocation for degree 0 or 1. Only the operations
// needed by adjacency building are exposed; no custom allocator dependencies.
template<class T> class SmallAdjVector {
    std::variant<std::monostate,T,std::vector<T>> value_;
    static T* emptyData() { static T sentinel{}; return &sentinel; }
public:
    size_t size() const noexcept {
        return value_.index()==0 ? 0 : value_.index()==1 ? 1 : std::get<2>(value_).size();
    }
    size_t capacity() const noexcept { return value_.index()==2 ? std::get<2>(value_).capacity() : 1; }
    size_t heapBytes() const noexcept { return value_.index()==2 ? capacity()*sizeof(T) : 0; }
    bool empty() const noexcept { return size()==0; }
    T* begin() { return empty() ? emptyData() : value_.index()==1 ? &std::get<1>(value_) : std::get<2>(value_).data(); }
    const T* begin() const { return empty() ? emptyData() : value_.index()==1 ? &std::get<1>(value_) : std::get<2>(value_).data(); }
    T* end() { return size() ? begin()+size() : begin(); }
    const T* end() const { return size() ? begin()+size() : begin(); }
    void reserve(size_t n) {
        if (n<=capacity()) return;
        if (value_.index()==2) { std::get<2>(value_).reserve(n); return; }
        std::vector<T> values; values.reserve(n);
        if (value_.index()==1) values.push_back(std::get<1>(value_));
        value_=std::move(values);
    }
    void push_back(const T& value) { insert(end(),value); }
    void insert(const T* position,const T& value) {
        const size_t offset=empty() ? 0 : size_t(position-begin());
        T copy=value;
        if (empty() && value_.index()!=2) { value_=copy; return; }
        if (value_.index()!=2) reserve(2);
        auto& values=std::get<2>(value_); values.insert(values.begin()+offset,copy);
    }
    void erase(const T* position) {
        const auto offset=position-begin();
        if (value_.index()==1) { value_=std::monostate{}; return; }
        auto& values=std::get<2>(value_); values.erase(values.begin()+offset);
        if (values.size()<=1) shrink_to_fit();
    }
    void clear() { value_=std::monostate{}; }
    void shrink_to_fit() {
        if (value_.index()!=2) return;
        auto& values=std::get<2>(value_);
        if (values.empty()) value_=std::monostate{};
        else if (values.size()==1) { T copy=values.front(); value_=copy; }
        else values.shrink_to_fit();
    }
    SmallAdjVector& operator=(std::vector<T>&& values) {
        value_=std::move(values); shrink_to_fit(); return *this;
    }
};
}

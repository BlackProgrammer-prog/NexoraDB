#pragma once
#include "Graphtypes.h"
#include <string>
#include <string_view>
#include <stdexcept>

namespace nexora::graph::storage_codec {
inline constexpr size_t header_bytes = 32, record_bytes = 40;
inline void put(std::string& bytes, size_t offset, uint64_t value, size_t width) {
    for (size_t i=0;i<width;++i) bytes[offset+i]=char(value>>(8*i));
}
inline uint64_t get(std::string_view bytes, size_t offset, size_t width) {
    if (offset+width>bytes.size()) throw std::runtime_error("truncated graph encoding");
    uint64_t value=0;
    for (size_t i=0;i<width;++i) value|=uint64_t(static_cast<unsigned char>(bytes[offset+i]))<<(8*i);
    return value;
}
inline uint32_t crc(std::string_view bytes) {
    uint32_t value=~uint32_t{0};
    for (unsigned char byte:bytes) {
        value^=byte;
        for (int bit=0;bit<8;++bit) value=(value>>1)^(0xedb88320U & (0U-(value&1U)));
    }
    return ~value;
}
inline void seal(std::string& bytes, size_t offset) {
    put(bytes,offset,0,4); put(bytes,offset,crc(bytes),4);
}
inline void check(std::string bytes, size_t offset) {
    const auto expected=get(bytes,offset,4); put(bytes,offset,0,4);
    if (crc(bytes)!=expected) throw std::runtime_error("graph checksum mismatch");
}
inline std::string header(uint32_t kind,uint64_t generation) {
    std::string bytes(header_bytes,'\0'); bytes.replace(0,4,"NXG2");
    put(bytes,4,2,4); put(bytes,8,kind,4);
    put(bytes,12,kind==3?0:record_bytes,4); put(bytes,16,1,4);
    put(bytes,20,generation,8); seal(bytes,28); return bytes;
}
inline void checkHeader(std::string_view bytes,uint32_t kind,uint64_t generation) {
    if (bytes.size()!=header_bytes || bytes.substr(0,4)!="NXG2" || get(bytes,4,4)!=2 ||
        get(bytes,8,4)!=kind || get(bytes,12,4)!=(kind==3?0:record_bytes) ||
        get(bytes,16,4)!=1 || get(bytes,20,8)!=generation)
        throw std::runtime_error("invalid graph header/version/kind/generation/checksum policy");
    check(std::string(bytes),28);
}
inline std::string node(const NodeRecord& record) {
    std::string bytes(record_bytes,'\0'); put(bytes,0,record.dense_id,8);
    put(bytes,8,record.type_id,4); put(bytes,12,record.flags,4);
    put(bytes,16,record.out_degree,8); put(bytes,24,record.in_degree,8);
    seal(bytes,32); return bytes;
}
inline std::string edge(const EdgeRecord& record) {
    std::string bytes(record_bytes,'\0'); put(bytes,0,record.edge_id,8);
    put(bytes,8,record.src,8); put(bytes,16,record.dst,8);
    put(bytes,24,record.type_id,4); put(bytes,28,record.flags,4);
    seal(bytes,32); return bytes;
}
inline void flags(uint32_t value,uint32_t allowed) {
    if ((value & ~allowed) || ((value&FLAG_ACTIVE) && (value&FLAG_DELETED)))
        throw std::runtime_error("invalid graph record flags");
}
inline NodeRecord decodeNode(std::string_view bytes,uint64_t id) {
    if (bytes.size()!=record_bytes) throw std::runtime_error("truncated graph node");
    check(std::string(bytes),32);
    if (get(bytes,36,4) || get(bytes,0,8)!=id) throw std::runtime_error("invalid graph node slot");
    NodeRecord result{}; result.dense_id=id; result.type_id=get(bytes,8,4);
    result.flags=get(bytes,12,4); result.out_degree=get(bytes,16,8); result.in_degree=get(bytes,24,8);
    flags(result.flags,FLAG_ACTIVE|FLAG_DELETED|FLAG_IMPLICIT); return result;
}
inline EdgeRecord decodeEdge(std::string_view bytes,uint64_t id) {
    if (bytes.size()!=record_bytes) throw std::runtime_error("truncated graph edge");
    check(std::string(bytes),32);
    if (get(bytes,36,4) || get(bytes,0,8)!=id) throw std::runtime_error("invalid graph edge slot");
    EdgeRecord result{}; result.edge_id=id; result.src=get(bytes,8,8); result.dst=get(bytes,16,8);
    result.type_id=get(bytes,24,4); result.flags=get(bytes,28,4);
    flags(result.flags,FLAG_ACTIVE|FLAG_DELETED|FLAG_UNDIRECTED); return result;
}
} // namespace nexora::graph::storage_codec

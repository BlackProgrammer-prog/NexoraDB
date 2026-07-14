//
// Created by HOME on 6/19/2026.
//

#ifndef GITIGNORE_GRAPHTYPES_H
#define GITIGNORE_GRAPHTYPES_H
#pragma once

/**
 * @file graph/GraphTypes.h
 * @brief تمام type aliasها، enumها، Fixed-size Recordها و ساختارهای مشترک GraphEngine
 *
 * @architecture
 * ┌────────────────────────────────────────────────────────────────┐
 * │                Python / Cython / Admin API                     │
 * ├────────────────────────────────────────────────────────────────┤
 * │              GraphEngine  (این ماژول — C++)                    │
 * │  ┌─────────────────┐  ┌──────────────┐  ┌──────────────────┐  │
 * │  │  LiveGraph(RAM) │  │ StaticGraph  │  │   GraphManager   │  │
 * │  │ AdjacencyList   │  │ (disk→RAM    │  │ (orchestrator)   │  │
 * │  │ Dense-ID        │  │  on-demand)  │  │                  │  │
 * │  └─────────────────┘  └──────────────┘  └──────────────────┘  │
 * │  ┌─────────────────┐  ┌──────────────┐  ┌──────────────────┐  │
 * │  │  GraphStorage   │  │  GraphWAL    │  │  DenseIdMap      │  │
 * │  │(.nex/.nexr/.nexl│  │(graph.wal)   │  │(externalId→DenseId│ │
 * │  └─────────────────┘  └──────────────┘  └──────────────────┘  │
 * ├────────────────────────────────────────────────────────────────┤
 * │            DocEngine / RocksDB (source of truth)               │
 * └────────────────────────────────────────────────────────────────┘
 *
 * قانون کلیدی:
 *   - LiveGraph همواره در RAM است و تغییرات لحظه‌ای دارد
 *   - StaticGraph فقط هنگام اجرای job سنگین از disk به RAM load می‌شود
 *   - فایل‌های .nex/.nexr/.nexl منبع rebuild هستند، نه RocksDB مستقیم
 *   - پس از پایان job، RAM و snapshot آزاد می‌شوند
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace nexora {
    namespace graph {

// ══════════════════════════════════════════════════════════════
// §1  ID Aliases
// ══════════════════════════════════════════════════════════════

/// Dense-ID — شناسه داخلی پیوسته (index در vector/array)
        using DenseId  = uint64_t;
/// External string ID — همان _id سند در RocksDB
        using ExtId    = std::string;
/// TypeId — ثبت‌شده در TypeRegistry
        using TypeId   = uint32_t;
/// Edge ID — شناسه منحصر‌به‌فرد یال
        using EdgeId   = uint64_t;

        constexpr DenseId kInvalidDenseId = UINT64_MAX;
        constexpr EdgeId  kInvalidEdgeId  = UINT64_MAX;
        constexpr TypeId  kInvalidTypeId  = UINT32_MAX;

// ══════════════════════════════════════════════════════════════
// §2  Fixed-size Disk Records
//     دسترسی O(1) از طریق offset = id * sizeof(Record)
// ══════════════════════════════════════════════════════════════

/// پرچم‌های وضعیت رکورد
        enum RecordFlags : uint32_t {
            FLAG_ACTIVE   = 0x00000001,  ///< رکورد فعال
            FLAG_DELETED  = 0x00000002,  ///< حذف منطقی — فضا قابل reuse
            FLAG_IMPLICIT = 0x00000004,  ///< node توسط edge ساخته شده، نه مستقیم
        };

/**
 * @struct NodeRecord
 * @brief رکورد ثابت‌طول برای node در فایل .nex
 *
 * اندازه: 32 بایت — سازگار با cache line alignment
 * دسترسی: offset = dense_id * sizeof(NodeRecord) → O(1)
 */
#pragma pack(push, 1)
        struct NodeRecord {
            uint64_t dense_id;    ///< Dense-ID (index)
            uint32_t type_id;     ///< نوع node (از TypeRegistry)
            uint32_t flags;       ///< FLAG_ACTIVE / FLAG_DELETED / FLAG_IMPLICIT
            uint64_t out_degree;  ///< تعداد یال‌های خروجی
            uint64_t in_degree;   ///< تعداد یال‌های ورودی

            static constexpr size_t SIZE = sizeof(uint64_t) * 3 + sizeof(uint32_t) * 2;

            bool isActive()   const noexcept { return (flags & FLAG_ACTIVE)   != 0; }
            bool isDeleted()  const noexcept { return (flags & FLAG_DELETED)  != 0; }
            bool isImplicit() const noexcept { return (flags & FLAG_IMPLICIT) != 0; }
        };
#pragma pack(pop)
        static_assert(sizeof(NodeRecord) == 32, "NodeRecord must be 32 bytes");

/**
 * @struct EdgeRecord
 * @brief رکورد ثابت‌طول برای edge در فایل .nexr
 *
 * اندازه: 32 بایت
 * دسترسی: offset = edge_id * sizeof(EdgeRecord) → O(1)
 */
#pragma pack(push, 1)
        struct EdgeRecord {
            uint64_t edge_id;   ///< Edge-ID
            uint64_t src;       ///< Dense-ID مبدا
            uint64_t dst;       ///< Dense-ID مقصد
            uint32_t type_id;   ///< نوع رابطه (از TypeRegistry)
            uint32_t flags;     ///< FLAG_ACTIVE / FLAG_DELETED

            static constexpr size_t SIZE = sizeof(uint64_t) * 3 + sizeof(uint32_t) * 2;

            bool isActive()  const noexcept { return (flags & FLAG_ACTIVE)  != 0; }
            bool isDeleted() const noexcept { return (flags & FLAG_DELETED) != 0; }
        };
#pragma pack(pop)
        static_assert(sizeof(EdgeRecord) == 32, "EdgeRecord must be 32 bytes");

/**
 * @struct GraphMeta
 * @brief متادیتا گراف — ذخیره در فایل graph_meta.nexl
 *
 * این فایل در startup بررسی می‌شود. اگر وضعیت clean نباشد، rebuild انجام می‌شود.
 */
        enum class GraphState : uint32_t {
            Clean      = 0xC1EA01,  ///< آخرین عملیات موفق بود
            Dirty      = 0xD1A001,  ///< عملیات‌ناتمام — باید rebuild یا rollback شود
            Building   = 0xBD0001,  ///< در حال build
            Corrupted  = 0xFF0001,  ///< خرابی احتمالی
        };

        enum class GraphKind : uint8_t {
            Live   = 0,  ///< همواره در RAM، تغییر لحظه‌ای
            Static = 1,  ///< روی disk، فقط هنگام job در RAM
        };

#pragma pack(push, 1)
        struct GraphMeta {
            uint32_t  magic         = 0x4E455847;  ///< "NEXG" — magic number
            uint32_t  version       = 1;
            GraphKind kind          = GraphKind::Live;
            GraphState state        = GraphState::Dirty;
            uint64_t  node_count    = 0;           ///< تعداد کل رکوردها (شامل deleted)
            uint64_t  edge_count    = 0;
            uint64_t  active_nodes  = 0;           ///< فعال‌ها
            uint64_t  active_edges  = 0;
            uint64_t  last_node_id  = 0;           ///< بزرگ‌ترین dense_id اختصاص‌یافته
            uint64_t  last_edge_id  = 0;
            int64_t   created_at    = 0;           ///< unix timestamp
            int64_t   updated_at    = 0;
            int64_t   wal_retention_secs = 86400;  ///< نگهداری WAL: 24 ساعت پیش‌فرض
            char      graph_name[128] = {};
            char      filter_expr[256] = {};       ///< شرط WHERE در صورت وجود
            uint8_t   _reserved[80]  = {};         ///< آینده‌نگر
        };
#pragma pack(pop)

        static_assert(sizeof(GraphMeta) == 549, "GraphMeta must be 512 bytes");

// ══════════════════════════════════════════════════════════════
// §3  WAL Record
// ══════════════════════════════════════════════════════════════

        enum class WalOpType : uint8_t {
            AddNode    = 1,
            RemoveNode = 2,
            AddEdge    = 3,
            RemoveEdge = 4,
            Begin      = 10,
            Commit     = 11,
            Rollback   = 12,
        };

#pragma pack(push, 1)
        struct WalRecord {
            uint64_t  seq;            ///< شماره ترتیبی یکتا
            int64_t   timestamp_ms;   ///< زمان عملیات (unix ms)
            WalOpType op;
            uint8_t   _pad[3];
            uint64_t  node_or_edge_id;
            uint64_t  src_id;         ///< فقط برای AddEdge/RemoveEdge
            uint64_t  dst_id;
            uint32_t  type_id;
            uint32_t  flags;
            bool      applied;        ///< آیا روی in-memory graph اعمال شده؟
            uint8_t   _pad2[7];
        };
#pragma pack(pop)
        static_assert(sizeof(WalRecord) == 60, "WalRecord must be 56 bytes");

// ══════════════════════════════════════════════════════════════
// §4  RAM Adjacency Structures
// ══════════════════════════════════════════════════════════════

/// حد آستانه: اگر degree از این مقدار بیشتر شد، chunked mode فعال می‌شود
        constexpr size_t kHeavyNodeThreshold = 1024;
/// اندازه هر chunk در Chunked Sorted Vector
        constexpr size_t kChunkSize = 512;

/**
 * @struct AdjEntry
 * @brief یک item در adjacency list
 * اندازه: 12 بایت — compact برای cache efficiency
 */
        struct AdjEntry {
            DenseId neighbor;   ///< Dense-ID همسایه
            EdgeId  edge_id;    ///< id یال (برای حذف سریع)
            TypeId  type_id;    ///< نوع یال

            bool operator<(const AdjEntry& o) const noexcept {
                return neighbor < o.neighbor;
            }
            bool operator==(const AdjEntry& o) const noexcept {
                return neighbor == o.neighbor && type_id == o.type_id;
            }
        };

/**
 * @struct ChunkedSortedVector
 * @brief Chunked Sorted Vector برای heavy nodes
 *
 * @details
 * برای nodes با degree بالا (heavy nodes):
 * به‌جای یک vector بزرگ که درج/حذف O(n) دارد،
 * از بلوک‌های chunk_size استفاده می‌کنیم.
 * هر chunk مرتب است.
 * درج: chunk مناسب پیدا می‌شود → O(chunk_size) shift
 * جستجو: binary search روی chunk heads → O(log(n/chunk_size)) + O(chunk_size)
 */
        struct ChunkedSortedVector {
            static constexpr size_t CHUNK = kChunkSize;
            std::vector<std::vector<AdjEntry>> chunks;

            void insert(const AdjEntry& e);
            bool remove(DenseId neighbor, TypeId type_id);
            bool contains(DenseId neighbor, TypeId type_id) const;
            size_t size() const noexcept;
            void forEach(const std::function<bool(const AdjEntry&)>& fn) const;
        };

/**
 * @struct NodeAdj
 * @brief adjacency برای یک node
 *
 * @details
 * برای nodes با degree کم: از sorted vector استفاده می‌کنیم.
 * برای heavy nodes (degree > kHeavyNodeThreshold): از ChunkedSortedVector.
 *
 * heavy_out / heavy_in فقط یکی از آن‌ها در هر زمان فعال است.
 */
        struct NodeAdj {
            // mode: اگر heavy باشد از chunked استفاده می‌شود
            bool out_heavy = false;
            bool in_heavy  = false;

            // normal mode
            std::vector<AdjEntry> out_edges;  ///< یال‌های خروجی (sorted by neighbor)
            std::vector<AdjEntry> in_edges;   ///< یال‌های ورودی

            // heavy mode
            ChunkedSortedVector heavy_out;
            ChunkedSortedVector heavy_in;

            size_t out_degree() const noexcept {
                return out_heavy ? heavy_out.size() : out_edges.size();
            }
            size_t in_degree() const noexcept {
                return in_heavy ? heavy_in.size() : in_edges.size();
            }

            void addOut(const AdjEntry& e);
            void addIn(const AdjEntry& e);
            bool removeOut(DenseId neighbor, TypeId type_id);
            bool removeIn(DenseId neighbor, TypeId type_id);
            void forEachOut(const std::function<bool(const AdjEntry&)>& fn) const;
            void forEachIn(const std::function<bool(const AdjEntry&)>& fn) const;

        private:
            void checkAndUpgrade();  ///< بررسی heavy threshold و upgrade در صورت نیاز
        };

// ══════════════════════════════════════════════════════════════
// §5  Type Registry
// ══════════════════════════════════════════════════════════════

/**
 * @struct TypeRegistry
 * @brief رجیستری نوع‌های node و edge
 * مثال: "User"→0, "Post"→1 / "FOLLOWS"→0, "LIKES"→1
 */
        struct TypeRegistry {
            std::unordered_map<std::string, TypeId> name_to_id;
            std::unordered_map<TypeId, std::string> id_to_name;
            uint32_t next_id = 0;

            TypeId getOrCreate(const std::string& name) {
                auto it = name_to_id.find(name);
                if (it != name_to_id.end()) return it->second;
                TypeId id = next_id++;
                name_to_id[name] = id;
                id_to_name[id]   = name;
                return id;
            }

            std::optional<TypeId> get(const std::string& name) const {
                auto it = name_to_id.find(name);
                if (it == name_to_id.end()) return std::nullopt;
                return it->second;
            }

            std::string getName(TypeId id) const {
                auto it = id_to_name.find(id);
                return (it != id_to_name.end()) ? it->second : "";
            }
        };

// ══════════════════════════════════════════════════════════════
// §6  Graph Statistics
// ══════════════════════════════════════════════════════════════

        struct GraphStats {
            uint64_t active_nodes    = 0;
            uint64_t active_edges    = 0;
            uint64_t deleted_nodes   = 0;  ///< در free stack
            uint64_t deleted_edges   = 0;
            uint64_t implicit_nodes  = 0;
            uint64_t heavy_nodes     = 0;  ///< nodes با Chunked mode
            uint64_t version         = 0;  ///< شماره نسخه RAM graph
        };

// ══════════════════════════════════════════════════════════════
// §7  Graph Filter (برای WHERE در CREATE GRAPH)
// ══════════════════════════════════════════════════════════════

/**
 * @brief تابع فیلتر node — پارسر Python آن را می‌سازد و به C++ می‌دهد
 * ورودی: externalId سند، محتوای BSON
 * خروجی: true = این node را include کن
 */
        using NodeFilterFn = std::function<bool(const ExtId&, const std::string& bson)>;

/**
 * @brief تابع فیلتر edge
 */
        using EdgeFilterFn = std::function<bool(const ExtId& src, const ExtId& dst,
                                                const std::string& src_bson,
                                                const std::string& dst_bson)>;

/**
 * @struct GraphFilter
 * @brief شرط‌های WHERE برای ساخت گراف
 */
        struct GraphFilter {
            NodeFilterFn node_filter;  ///< اگر null = همه nodes
            EdgeFilterFn edge_filter;  ///< اگر null = همه edges
            std::string  expr_str;     ///< عبارت رشته‌ای (برای ذخیره در meta)
        };

// ══════════════════════════════════════════════════════════════
// §8  Node/Edge View (public output برای algorithm team و Python)
// ══════════════════════════════════════════════════════════════

        struct NodeView {
            DenseId     dense_id;
            ExtId       external_id;
            std::string type_name;
            uint32_t    flags;
            uint64_t    out_degree;
            uint64_t    in_degree;
        };

        struct EdgeView {
            EdgeId      edge_id;
            DenseId     src;
            DenseId     dst;
            ExtId       src_ext;
            ExtId       dst_ext;
            std::string type_name;
            uint32_t    flags;
        };

// ══════════════════════════════════════════════════════════════
// §9  Job / Static Graph Result
// ══════════════════════════════════════════════════════════════

/**
 * @brief نتیجه‌ای که تیم الگوریتم بعد از اجرای job برمی‌گرداند
 */
        struct JobResult {
            bool        success = false;
            std::string error_msg;
            std::string result_json;  ///< خروجی قابل ارسال به Python
        };

/// callback که تیم الگوریتم پیاده‌سازی می‌کند
/// ورودی: snapshot گراف ثابت در RAM
/// این callback در background thread اجرا می‌شود
        class StaticGraph;  // forward declaration
        using AlgorithmJobFn = std::function<JobResult(const StaticGraph&)>;

// ══════════════════════════════════════════════════════════════
// §13  Direction (اضافه شد — قبلاً در این فایل تعریف نشده بود)
// ══════════════════════════════════════════════════════════════

/**
 * @enum Direction
 * @brief جهت traversal یا edge
 *
 * Out  → یال‌های خروجی از یک node (node می‌رود به ...)
 * In   → یال‌های ورودی به یک node (... می‌آیند به node)
 * Both → هر دو جهت
 */
enum class Direction : uint8_t {
    Out  = 0,
    In   = 1,
    Both = 2
};

// ══════════════════════════════════════════════════════════════
// §14  GraphMode — Live vs Static
// ══════════════════════════════════════════════════════════════

/**
 * @enum GraphMode
 * Live:   همواره در RAM، تغییر لحظه‌ای با DocEngine
 * Static: فقط هنگام job در RAM، بعد آزاد می‌شود
 */
enum class GraphMode : uint8_t {
    Live   = 0,
    Static = 1
};

// ══════════════════════════════════════════════════════════════
// §15  NodeMapping و EdgeMapping — تعریف ساختار گراف از collections
//       این‌ها را GraphManager ذخیره می‌کند تا بداند چطور گراف را بسازد
// ══════════════════════════════════════════════════════════════

/**
 * @struct UnwindConfig
 * @brief پیکربندی UNWIND برای ساخت edge از array درون document
 *
 * مثال: {"_id":"u1","following":["u2","u3"]}
 * UnwindConfig { array_path="following", alias="followed_id" }
 * → یک edge به ازای هر عضو آرایه ساخته می‌شود
 */
struct UnwindConfig {
    std::string array_path;  ///< dot-path به array ("following" یا "engagement.likedBy")
    std::string alias;       ///< اسم متغیر که به source یا target داده می‌شود
};

/**
 * @struct NodeMappingDef
 * @brief تعریف اینکه کدام collection → کدام node type
 *
 * مثال NexoraQL:
 *   MAP NODE User FROM users KEY _id PROPERTIES username, avatar;
 */
struct NodeMappingDef {
    std::string              node_type;    ///< نام node type ("User","Post",...)
    std::string              collection;   ///< نام collection در DocEngine
    std::string              key_path;     ///< فیلدی که DenseId از آن ساخته می‌شود ("_id")
    std::vector<std::string> properties;   ///< فیلدهایی که در node ذخیره می‌شوند
    std::string              filter_expr;  ///< شرط WHERE (اختیاری)
};

/**
 * @struct EdgeMappingDef
 * @brief تعریف اینکه کدام collection/field → کدام edge type
 *
 * سه روش:
 *  1. collection جداگانه: follows.from_id → users, follows.to_id → users
 *  2. FK در سند: posts.author_id → users._id
 *  3. UNWIND array: users.following[] → User nodes
 */
struct EdgeMappingDef {
    std::string              edge_type;         ///< نام edge type ("FOLLOWS","LIKES",...)
    std::string              collection;         ///< collection مبدا
    std::optional<UnwindConfig> unwind;          ///< اگر set شود: روش ۳ (UNWIND)
    std::string              source_path;        ///< dot-path به source id
    std::string              source_node_type;   ///< node type مبدا
    std::string              target_path;        ///< dot-path به target id
    std::string              target_node_type;   ///< node type مقصد
    bool                     directed = true;    ///< آیا جهت‌دار است؟
    std::vector<std::string> properties;         ///< فیلدهایی که در edge ذخیره می‌شوند
};

/**
 * @struct GraphDefinition
 * @brief تعریف کامل یک گراف — ذخیره در RocksDB (از طریق GraphIdStore)
 *
 * این struct تعریف می‌کند:
 *  - چه collections به node تبدیل می‌شوند
 *  - چه fields/collections به edge تبدیل می‌شوند
 * GraphManager از این برای BuildGraph() و live update استفاده می‌کند.
 */
struct GraphDefinition {
    std::string                    name;
    GraphMode                      mode = GraphMode::Live;
    bool                           directed = true;
    bool                           heterogeneous = true;
    bool                           auto_build_on_startup = true;
    std::vector<NodeMappingDef>    node_mappings;
    std::vector<EdgeMappingDef>    edge_mappings;
};


// ══════════════════════════════════════════════════════════════
// §16  GraphStats کامل (جایگزین تعریف §6 — آن struct حذف شود)
//      فیلدهای nodeCount/edgeCount/nodeTypeCount/edgeTypeCount اضافه شد
// ══════════════════════════════════════════════════════════════

/**
 * @struct GraphStatsEx
 * @brief نسخه کامل آمار گراف — جایگزین GraphStats برای StaticGraph
 *
 * @note GraphStats موجود (§6) برای LiveGraph باقی می‌ماند.
 *       StaticGraph از این struct استفاده می‌کند.
 */
struct GraphStatsEx {
    // ── سازگار با GraphStats ──
    uint64_t active_nodes    = 0;
    uint64_t active_edges    = 0;
    uint64_t deleted_nodes   = 0;
    uint64_t deleted_edges   = 0;
    uint64_t implicit_nodes  = 0;
    uint64_t heavy_nodes     = 0;
    uint64_t version         = 0;

    // ── فیلدهای اضافه برای StaticGraph ──
    uint64_t nodeCount       = 0;   ///< alias برای active_nodes
    uint64_t edgeCount       = 0;   ///< alias برای active_edges
    uint64_t nodeTypeCount   = 0;   ///< تعداد انواع node
    uint64_t edgeTypeCount   = 0;   ///< تعداد انواع edge
};

// ══════════════════════════════════════════════════════════════
// §17  Export Structures — COO / CSR / GPU
// ══════════════════════════════════════════════════════════════

/**
 * @enum GraphExportFormat
 * @brief فرمت خروجی گراف برای ML/GNN/CUDA
 */
enum class GraphExportFormat : uint8_t {
    COO        = 0,   ///< Coordinate format — (src, dst) pairs
    CSR        = 1,   ///< Compressed Sparse Row
    GNNPackage = 2    ///< بسته کامل برای GNN (آینده)
};

/**
 * @enum GraphStorageBackend
 * @brief مقصد ذخیره snapshot
 */
enum class GraphStorageBackend : uint8_t {
    RAM  = 0,   ///< primary mutable graph — همیشه اینجاست
    VRAM = 1    ///< compressed snapshot برای GPU/CUDA
};

/**
 * @struct GraphExportOptions
 * @brief گزینه‌های export گراف برای ML/GNN
 *
 * @example
 * ```cpp
 * GraphExportOptions opts;
 * opts.nodeTypes = {"User", "Post"};
 * opts.edgeTypes = {"FOLLOWS"};
 * opts.remapNodeIdsToContiguous = true;
 * auto coo = snapshot.exportCOO(opts);
 * ```
 */
struct GraphExportOptions {
    GraphExportFormat        format                   = GraphExportFormat::COO;
    std::vector<std::string> nodeTypes;               ///< خالی = همه node types
    std::vector<std::string> edgeTypes;               ///< خالی = همه edge types
    bool                     includeNodeProperties    = true;
    bool                     includeEdgeProperties    = false;
    bool                     includeNumericFeatures   = true;
    bool                     remapNodeIdsToContiguous = true;  ///< index از 0
    GraphStorageBackend      targetBackend            = GraphStorageBackend::RAM;
};

/**
 * @struct CooGraph
 * @brief خروجی COO (Coordinate) format
 *
 * @details
 * COO = لیست جفت‌های (src, dst):
 *   src[i] → dst[i]  یک edge
 *   edgeTypeIds[i]    نوع آن edge
 *   originalNodeIds[remapped] = original DenseId
 *
 * این فرمت مستقیماً برای PyTorch Geometric و DGL قابل استفاده است.
 */
struct CooGraph {
    std::vector<uint64_t> src;              ///< مبدا هر edge (contiguous remapped)
    std::vector<uint64_t> dst;              ///< مقصد هر edge
    std::vector<uint32_t> edgeTypeIds;      ///< نوع هر edge
    std::vector<uint64_t> originalNodeIds;  ///< originalNodeIds[remapped] = DenseId اصلی
};

/**
 * @struct CsrGraph
 * @brief خروجی CSR (Compressed Sparse Row) format
 *
 * @details
 * CSR برای traversal سریع روی GPU:
 *   node i → همسایه‌هایش در colIdx[ rowPtr[i] .. rowPtr[i+1] )
 *
 * این فرمت برای cuGraph و GPU BFS مستقیماً قابل استفاده است.
 */
struct CsrGraph {
    std::vector<uint64_t> rowPtr;           ///< شروع هر node در colIdx
    std::vector<uint64_t> colIdx;           ///< id همسایه‌ها
    std::vector<uint32_t> edgeTypeIds;      ///< نوع هر edge
    std::vector<uint64_t> originalNodeIds;  ///< mapping از row index به DenseId
};

    } // namespace graph
} // namespace nexora
#endif //GITIGNORE_GRAPHTYPES_H

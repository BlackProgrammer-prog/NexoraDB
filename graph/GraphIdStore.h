//
// Created by HOME on 6/19/2026.
//

#ifndef GITIGNORE_GRAPHIDSTORE_H
#define GITIGNORE_GRAPHIDSTORE_H


#pragma once

/**
 * @file graph/GraphIdStore.h
 * @brief ذخیره‌سازی دائمی DenseId ↔ ExtId در RocksDB
 *
 * @details
 * ═══════════════════════════════════════════════════════════════
 * مشکلی که این کلاس حل می‌کند:
 * ═══════════════════════════════════════════════════════════════
 *
 * NodeRecord در فایل .nex فقط این را ذخیره می‌کند:
 *   dense_id | type_id | flags | out_degree | in_degree
 *
 * اما ExtId (همان _id سند در RocksDB) در NodeRecord نیست!
 * بنابراین اگر برق قطع شود:
 *   - فایل .nex موجود است → dense_id‌ها درست است
 *   - اما نمی‌دانیم dense_id=42 مربوط به کدام _id کاربر است
 *   → گراف بازیابی‌ناپذیر می‌شود!
 *
 * راه‌حل:
 *   یک فضای مخفی در RocksDB برای ذخیره این مپ:
 *
 *   graph:idmap:{graph_name}:ext:{ext_id}   → dense_id (little-endian uint64)
 *   graph:idmap:{graph_name}:dns:{dense_id} → ext_id (string)
 *   graph:idmap:{graph_name}:seq            → next_dense_id (uint64)
 *   graph:idmap:{graph_name}:etype:{name}   → type_id (uint32)
 *   graph:idmap:{graph_name}:ntype:{name}   → type_id (uint32)
 *
 * این داده‌ها در RocksDB هستند (نه فایل‌های .nex) → ACID → بعد از هر crash
 * قابل بازیابی.
 *
 * ═══════════════════════════════════════════════════════════════
 * استفاده:
 * ═══════════════════════════════════════════════════════════════
 * - GraphManager در addNode و removeNode این را update می‌کند
 * - در startup، loadFromDisk این را می‌خواند و DenseIdMap را بازمی‌سازد
 * - کاملاً شفاف برای تیم الگوریتم
 */

#include "Graphtypes.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nexora {
    namespace graph {

/**
 * @class GraphIdStore
 * @brief لایه persistence برای DenseId ↔ ExtId در RocksDB
 *
 * این کلاس تضمین می‌کند که حتی بعد از crash/power-off،
 * رابطه DenseId ↔ ExtId از دست نمی‌رود.
 */
        class GraphIdStore {
        public:
            /**
             * @param db         اشاره‌گر به RocksDB (از DocEngine یا مستقل)
             * @param graph_name نام گراف (برای namespace جداسازی)
             */
            explicit GraphIdStore(rocksdb::DB* db, const std::string& graph_name);
            ~GraphIdStore() = default;

            // ──────────────────────────────────────────────────────────
            // §1  DenseId ↔ ExtId
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک ExtId→DenseId جدید را persistent می‌کند
             * @param ext_id  شناسه خارجی (_id سند)
             * @param dense_id DenseId اختصاص‌یافته
             * @return true اگر موفق
             *
             * @note اتمیک: WriteBatch هر دو جهت را با هم می‌نویسد
             */
            bool putMapping(const ExtId& ext_id, DenseId dense_id);

            /**
             * @brief یک mapping را حذف می‌کند (هنگام removeNode)
             */
            bool deleteMapping(const ExtId& ext_id, DenseId dense_id);

            /**
             * @brief DenseId از ExtId
             * @return nullopt اگر وجود نداشته باشد
             */
            std::optional<DenseId> getDenseId(const ExtId& ext_id) const;

            /**
             * @brief ExtId از DenseId
             * @return "" اگر وجود نداشته باشد
             */
            ExtId getExtId(DenseId dense_id) const;

            /**
             * @brief تمام mapping‌ها را بارگذاری می‌کند (در startup)
             * @return map از ExtId → DenseId
             */
            std::unordered_map<ExtId, DenseId> loadAllMappings() const;

            // ──────────────────────────────────────────────────────────
            // §2  next_dense_id (sequence counter)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief شمارنده بعدی DenseId را ذخیره می‌کند
             * @details بعد از هر addNode یا compaction باید صدا شود
             */
            bool saveNextDenseId(DenseId next_id);

            /**
             * @brief شمارنده بعدی DenseId را می‌خواند
             * @return 0 اگر هنوز ذخیره نشده
             */
            DenseId loadNextDenseId() const;

            /**
             * @brief شمارنده بعدی EdgeId را ذخیره می‌کند
             */
            bool saveNextEdgeId(EdgeId next_id);
            EdgeId loadNextEdgeId() const;

            // ──────────────────────────────────────────────────────────
            // §3  Type Registry persistence
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک node type را ذخیره می‌کند
             * @param type_name نام نوع ("User", "Post", ...)
             * @param type_id   TypeId اختصاص‌یافته
             */
            bool putNodeType(const std::string& type_name, TypeId type_id);

            /**
             * @brief یک edge type را ذخیره می‌کند
             */
            bool putEdgeType(const std::string& type_name, TypeId type_id);

            /**
             * @brief تمام node types را بارگذاری می‌کند
             */
            std::unordered_map<std::string, TypeId> loadNodeTypes() const;

            /**
             * @brief تمام edge types را بارگذاری می‌کند
             */
            std::unordered_map<std::string, TypeId> loadEdgeTypes() const;

            // ──────────────────────────────────────────────────────────
            // §4  Graph Definition persistence (بدون نیاز به فایل جداگانه)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief GraphMeta را در RocksDB ذخیره می‌کند
             * @note علاوه بر فایل .nexl، یک backup در RocksDB هم نگه می‌دارد
             */
            bool saveGraphMeta(const GraphMeta& meta);

            /**
             * @brief GraphMeta را از RocksDB می‌خواند
             */
            std::optional<GraphMeta> loadGraphMeta() const;

            // ──────────────────────────────────────────────────────────
            // §5  Cleanup
            // ──────────────────────────────────────────────────────────

            /**
             * @brief تمام داده‌های این گراف را از RocksDB حذف می‌کند
             * @note هنگام DROP GRAPH فراخوانی می‌شود
             */
            bool clearAll();

            /**
             * @brief تعداد mapping‌های ذخیره‌شده
             */
            size_t mappingCount() const;

        private:
            rocksdb::DB*  db_;
            std::string   graph_name_;

            rocksdb::ReadOptions  read_opts_;
            rocksdb::WriteOptions write_opts_;

            // ── Key builders ──
            std::string keyExtToDense(const ExtId& ext_id) const;
            std::string keyDenseToExt(DenseId dense_id)    const;
            std::string keySeqNode()                        const;
            std::string keySeqEdge()                        const;
            std::string keyNodeType(const std::string& name) const;
            std::string keyEdgeType(const std::string& name) const;
            std::string keyMeta()                           const;
            std::string keyPrefix()                         const;

            // ── Serialization ──
            static std::string encodeDenseId(DenseId id);
            static DenseId     decodeDenseId(const std::string& s);
            static std::string encodeTypeId(TypeId id);
            static TypeId      decodeTypeId(const std::string& s);
        };

    } // namespace graph
} // namespace nexora


#endif //GITIGNORE_GRAPHIDSTORE_H

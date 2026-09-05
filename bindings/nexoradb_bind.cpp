/**
 * @file bindings/nexoradb_bind.cpp
 * @brief pybind11 binding — DocEngine + GraphManager → Python
 *
 * @details
 * این فایل تمام کلاس‌ها و توابع C++ را برای Python expose می‌کند.
 *
 * استفاده در Python:
 * ```python
 * import nexoradb
 *
 * # DocEngine
 * db = nexoradb.DocEngine("/var/data/nexoradb")
 * result = db.insert_one("users", '{"username":"ali"}')
 * print(result.success, result.data)
 *
 * # GraphManager
 * gm = nexoradb.GraphManager(db, "./graph_data")
 * gm.startup()
 * gm.build_graph("social")
 * nbrs = gm.neighbors("social", "u1", "out", "FOLLOWS", 50)
 * ```
 *
 * @note
 * تمام منطق در C++ است.
 * Python فقط wrapper است — هیچ business logic اینجا نیست.
 *
 * @section GIL
 * عملیات‌های سنگین (buildGraph, IterateCollection, createSnapshot)
 * با py::gil_scoped_release اجرا می‌شوند تا Python thread-safe بماند.
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>
#include <pybind11/functional.h>

// ── DocEngine ──
#include "core/DocEngine.h"
#include "query/Condition.h"
#include "query/UpdateSpec.h"

// ── GraphEngine (اگر build شده) ──
#ifdef NEXORA_BUILD_GRAPH
#  include "graph/GraphManager.h"
#  include "graph/StaticGraph.h"
#  include "graph/Graphtypes.h"
#  include "graph/algorithms/BuiltinAlgorithms.h"
#endif

namespace py = pybind11;

using namespace nexora::core;
using namespace nexora::query;

#ifdef NEXORA_BUILD_GRAPH
using namespace nexora::graph;
#endif

#ifdef NEXORA_BUILD_GRAPH
PYBIND11_MAKE_OPAQUE(std::vector<nexora::graph::NodeMappingDef>);
PYBIND11_MAKE_OPAQUE(std::vector<nexora::graph::EdgeMappingDef>);
#endif

// ══════════════════════════════════════════════════════════════
// §0  Helper — تبدیل DBResult به Python dict
// ══════════════════════════════════════════════════════════════

static py::dict result_to_dict(const DBResult& r) {
    py::dict d;
    d["success"]   = r.success;
    d["data"]      = r.data;
    d["error_msg"] = r.error_msg;
    return d;
}

static py::dict build_info_dict() {
    py::dict info;
    info["project"]          = "NexoraDB";
    info["project_version"]  = NEXORA_PROJECT_VERSION;
    info["build_type"]       = NEXORA_BUILD_TYPE;
    info["cmake_version"]    = NEXORA_CMAKE_VERSION;
    info["compiler_id"]      = NEXORA_COMPILER_ID;
    info["compiler_version"] = NEXORA_COMPILER_VERSION;
    info["system"]           = NEXORA_SYSTEM_NAME;
    info["processor"]        = NEXORA_SYSTEM_PROCESSOR;
    info["cpp_standard"]     = 20;
    info["rocksdb_version"]  = NEXORA_ROCKSDB_VERSION;
    info["fmt_version"]      = NEXORA_FMT_VERSION;
    info["pybind11_version"] = NEXORA_PYBIND11_VERSION;
    info["python_version"]   = NEXORA_PYTHON_VERSION;
    info["lto_enabled"]      = static_cast<bool>(NEXORA_LTO_ENABLED);
    info["asan_enabled"]     = static_cast<bool>(NEXORA_ASAN_ENABLED);
    info["ubsan_enabled"]    = static_cast<bool>(NEXORA_UBSAN_ENABLED);
    info["tsan_enabled"]     = static_cast<bool>(NEXORA_TSAN_ENABLED);
#ifdef NEXORA_BUILD_GRAPH
    info["graph_enabled"]    = true;
#else
    info["graph_enabled"]    = false;
#endif
    return info;
}

#ifdef NEXORA_BUILD_GRAPH
static const char* wal_op_to_string(WalOpType op) {
    switch (op) {
        case WalOpType::AddNode:    return "AddNode";
        case WalOpType::RemoveNode: return "RemoveNode";
        case WalOpType::AddEdge:    return "AddEdge";
        case WalOpType::RemoveEdge: return "RemoveEdge";
        case WalOpType::Begin:      return "Begin";
        case WalOpType::Commit:     return "Commit";
        case WalOpType::Rollback:   return "Rollback";
        default:                    return "Unknown";
    }
}
#endif

// ══════════════════════════════════════════════════════════════
// PYBIND11_MODULE
// ══════════════════════════════════════════════════════════════

PYBIND11_MODULE(nexoradb, m) {
    m.def("build_info", &build_info_dict,
          "Return compiler, dependency, sanitizer and build metadata for this C++ core.");
    m.doc() = "NexoraDB — High-performance Document + Graph Database (C++ core)";

#ifdef NEXORA_BUILD_GRAPH
    // Graph mapping vectors are opaque and therefore need explicit bindings.
    py::bind_vector<std::vector<NodeMappingDef>>(m, "NodeMappingList");
    py::bind_vector<std::vector<EdgeMappingDef>>(m, "EdgeMappingList");
#endif

    // ──────────────────────────────────────────────────────────
    // §1  DBResult
    // ──────────────────────────────────────────────────────────

    py::class_<DBResult>(m, "DBResult",
                         R"doc(
        خروجی استاندارد تمام عملیات DocEngine.

        Attributes:
            success  (bool):  آیا عملیات موفق بود؟
            data     (str):   مقدار بازگشتی (doc_id / BSON JSON / count)
            error_msg(str):   پیام خطا در صورت failure

        Example:
            result = db.insert_one("users", '{"username":"ali"}')
            if result.success:
                print("id:", result.data)
            else:
                print("error:", result.error_msg)
        )doc")
            .def_readonly("success",   &DBResult::success)
            .def_readonly("data",      &DBResult::data)
            .def_readonly("error_msg", &DBResult::error_msg)
            .def("to_dict", &result_to_dict,
                 "تبدیل به Python dict")
            .def("__bool__", [](const DBResult& r) { return r.success; })
            .def("__repr__", [](const DBResult& r) {
                return std::string("<DBResult success=") +
                       (r.success ? "True" : "False") +
                       " data='" + r.data.substr(0, 80) + "'>";
            });

    py::class_<PageResult>(m, "PageResult")
            .def_readonly("success", &PageResult::success)
            .def_readonly("data", &PageResult::data)
            .def_readonly("continuation_token",
                          &PageResult::continuation_token)
            .def_readonly("error_msg", &PageResult::error_msg)
            .def("__bool__", [](const PageResult& result) {
                return result.success;
            });

    py::enum_<BulkWriteMode>(m, "BulkWriteMode")
            .value("Atomic", BulkWriteMode::Atomic)
            .value("OrderedChunks", BulkWriteMode::OrderedChunks);

    py::class_<BulkWriteOptions>(m, "BulkWriteOptions")
            .def(py::init<>())
            .def_readwrite("mode", &BulkWriteOptions::mode)
            .def_readwrite("max_operations_per_chunk",
                           &BulkWriteOptions::max_operations_per_chunk)
            .def_readwrite("max_bytes_per_chunk",
                           &BulkWriteOptions::max_bytes_per_chunk);

    py::class_<BulkWriteResult>(m, "BulkWriteResult")
            .def_readonly("success", &BulkWriteResult::success)
            .def_readonly("processed", &BulkWriteResult::processed)
            .def_readonly("modified", &BulkWriteResult::modified)
            .def_readonly("committed_chunks",
                          &BulkWriteResult::committed_chunks)
            .def_readonly("last_error", &BulkWriteResult::last_error)
            .def("__bool__", [](const BulkWriteResult& result) {
                return result.success;
            });

    py::class_<TransactionSettings>(m, "TransactionSettings")
            .def(py::init<>())
            .def_readwrite("lock_timeout_ms",
                           &TransactionSettings::lock_timeout_ms)
            .def_readwrite("expiration_ms",
                           &TransactionSettings::expiration_ms)
            .def_readwrite("deadlock_detect",
                           &TransactionSettings::deadlock_detect)
            .def_readwrite("deadlock_detect_depth",
                           &TransactionSettings::deadlock_detect_depth);

    // ──────────────────────────────────────────────────────────
    // §2  Enums (Query)
    // ──────────────────────────────────────────────────────────

    py::enum_<FieldType>(m, "FieldType")
            .value("String",  FieldType::String)
            .value("Int32",   FieldType::Int32)
            .value("Int64",   FieldType::Int64)
            .value("Float64", FieldType::Float64)
            .value("Bool",    FieldType::Bool)
            .value("Array",   FieldType::Array)
            .value("Object",  FieldType::Object)
            .value("Binary",  FieldType::Binary)
            .value("Null",    FieldType::Null)
            .export_values();

    py::enum_<IndexType>(m, "IndexType")
            .value("SingleField", IndexType::SingleField)
            .value("Compound",    IndexType::Compound)
            .value("Unique",      IndexType::Unique)
            .export_values();

    py::enum_<IndexState>(m, "IndexState")
            .value("Building", IndexState::Building)
            .value("Ready", IndexState::Ready)
            .value("Failed", IndexState::Failed);

    py::enum_<Op>(m, "Op",
                  "عملگرهای مقایسه‌ای برای Condition")
            .value("EQ",       Op::EQ)
            .value("NEQ",      Op::NEQ)
            .value("GT",       Op::GT)
            .value("GTE",      Op::GTE)
            .value("LT",       Op::LT)
            .value("LTE",      Op::LTE)
            .value("IN",       Op::IN)
            .value("NIN",      Op::NIN)
            .value("EXISTS",   Op::EXISTS)
            .value("REGEX",    Op::REGEX)
            .value("STARTS",   Op::STARTS)
            .value("CONTAINS", Op::CONTAINS)
            .export_values();

    py::enum_<ValueType>(m, "ValueType")
            .value("String",  ValueType::String)
            .value("Int64",   ValueType::Int64)
            .value("Float64", ValueType::Float64)
            .value("Bool",    ValueType::Bool)
            .value("Null",    ValueType::Null)
            .export_values();

    py::enum_<LogicOp>(m, "LogicOp")
            .value("AND", LogicOp::AND)
            .value("OR",  LogicOp::OR)
            .value("NOR", LogicOp::NOR)
            .value("NOT", LogicOp::NOT)
            .export_values();

    py::enum_<UpdateOp>(m, "UpdateOp")
            .value("Set",         UpdateOp::Set)
            .value("Unset",       UpdateOp::Unset)
            .value("Inc",         UpdateOp::Inc)
            .value("Mul",         UpdateOp::Mul)
            .value("Min",         UpdateOp::Min)
            .value("Max",         UpdateOp::Max)
            .value("Rename",      UpdateOp::Rename)
            .value("CurrentDate", UpdateOp::CurrentDate)
            .value("Push",        UpdateOp::Push)
            .value("PushAll",     UpdateOp::PushAll)
            .value("Pull",        UpdateOp::Pull)
            .value("PullAll",     UpdateOp::PullAll)
            .value("AddToSet",    UpdateOp::AddToSet)
            .value("Pop",         UpdateOp::Pop)
            .export_values();

    py::enum_<UpdateValueType>(m, "UpdateValueType")
            .value("String",  UpdateValueType::String)
            .value("Int64",   UpdateValueType::Int64)
            .value("Float64", UpdateValueType::Float64)
            .value("Bool",    UpdateValueType::Bool)
            .value("Null",    UpdateValueType::Null)
            .value("Array",   UpdateValueType::Array)
            .export_values();

    // ──────────────────────────────────────────────────────────
    // §3  SchemaField / SchemaDefinition
    // ──────────────────────────────────────────────────────────

    py::class_<SchemaField>(m, "SchemaField",
                            "تعریف یک فیلد در Schema")
            .def(py::init<>())
            .def_readwrite("name",        &SchemaField::name)
            .def_readwrite("type",        &SchemaField::type)
            .def_readwrite("required",    &SchemaField::required)
            .def_readwrite("unique",      &SchemaField::unique)
            .def("__repr__", [](const SchemaField& f) {
                return "<SchemaField name='" + f.name +
                       "' required=" + (f.required ? "True" : "False") + ">";
            });

    py::class_<SchemaDefinition>(m, "SchemaDefinition",
                                 R"doc(
        تعریف Schema برای یک Collection.

        Example:
            schema = nexoradb.SchemaDefinition()
            f = nexoradb.SchemaField()
            f.name = "username"
            f.type = nexoradb.FieldType.String
            f.required = True
            f.unique   = True
            schema.fields.append(f)
            db.create_collection("users", schema)
        )doc")
            .def(py::init<>())
            .def_readwrite("fields", &SchemaDefinition::fields)
            .def_readwrite("strict", &SchemaDefinition::strict);

    // ──────────────────────────────────────────────────────────
    // §4  IndexDefinition / ForeignKeyDefinition
    // ──────────────────────────────────────────────────────────

    py::class_<IndexDefinition>(m, "IndexDefinition",
                                R"doc(
        تعریف Index.

        Example:
            idx = nexoradb.IndexDefinition()
            idx.index_name = "idx_email"
            idx.fields     = ["email"]
            idx.type       = nexoradb.IndexType.Unique
            db.create_index("users", idx)
        )doc")
            .def(py::init<>())
            .def_readwrite("index_name", &IndexDefinition::index_name)
            .def_readwrite("fields",     &IndexDefinition::fields)
            .def_readwrite("type",       &IndexDefinition::type)
            .def_readonly("index_id",    &IndexDefinition::index_id)
            .def_readonly("format_version",
                          &IndexDefinition::format_version)
            .def_readonly("state", &IndexDefinition::state)
            .def_readonly("build_cursor", &IndexDefinition::build_cursor)
            .def_readonly("last_error", &IndexDefinition::last_error);

    py::class_<ForeignKeyDefinition>(m, "ForeignKeyDefinition",
                                     R"doc(
        تعریف Foreign Key.

        Example:
            fk = nexoradb.ForeignKeyDefinition()
            fk.fk_name        = "fk_author"
            fk.local_field    = "author_id"
            fk.ref_collection = "users"
            fk.ref_field      = "_id"
            db.add_foreign_key("posts", fk)
        )doc")
            .def(py::init<>())
            .def_readwrite("fk_name",        &ForeignKeyDefinition::fk_name)
            .def_readwrite("local_field",    &ForeignKeyDefinition::local_field)
            .def_readwrite("ref_collection", &ForeignKeyDefinition::ref_collection)
            .def_readwrite("ref_field",      &ForeignKeyDefinition::ref_field);

    // ──────────────────────────────────────────────────────────
    // §5  Condition
    // ──────────────────────────────────────────────────────────

    py::class_<Condition>(m, "Condition",
                          R"doc(
        شرط جستجو — می‌تواند ساده (leaf) یا ترکیبی (AND/OR) باشد.

        Example:
            # شرط ساده
            c = nexoradb.Condition.leaf("age", nexoradb.Op.GT, "18", nexoradb.ValueType.Int64)

            # AND ترکیبی
            c = nexoradb.Condition.and_([
                nexoradb.Condition.leaf("age", nexoradb.Op.GTE, "18", nexoradb.ValueType.Int64),
                nexoradb.Condition.leaf("active", nexoradb.Op.EQ, "1", nexoradb.ValueType.Bool)
            ])

            # IN
            c = nexoradb.Condition.in_("status", ["active", "verified"])

            # شرط خالی (match همه)
            c = nexoradb.Condition()
        )doc")
            .def(py::init<>(),
                 "شرط خالی — همه اسناد match می‌کنند")
            .def_readwrite("field",          &Condition::field)
            .def_readwrite("op",             &Condition::op)
            .def_readwrite("value",          &Condition::value)
            .def_readwrite("value_type",     &Condition::value_type)
            .def_readwrite("values",         &Condition::values)
            .def_readwrite("logic",          &Condition::logic)
            .def_readwrite("sub_conditions", &Condition::sub_conditions)
            .def("is_empty",     &Condition::IsEmpty)
            .def("is_leaf",      &Condition::IsLeaf)
            .def("is_composite", &Condition::IsComposite)
                    // factory methods (اسم Python-friendly)
            .def_static("leaf", &Condition::Leaf,
                        py::arg("field"), py::arg("op"), py::arg("value"),
                        py::arg("value_type") = ValueType::String,
                        "ساخت شرط ساده: field op value")
            .def_static("and_",
                        [](std::vector<Condition> subs) {
                            return Condition::And(std::move(subs));
                        }, py::arg("conditions"),
                        "ساخت شرط AND از لیست شروط")
            .def_static("or_",
                        [](std::vector<Condition> subs) {
                            return Condition::Or(std::move(subs));
                        }, py::arg("conditions"),
                        "ساخت شرط OR")
            .def_static("nor_",
                        [](std::vector<Condition> subs) {
                            return Condition::Nor(std::move(subs));
                        }, py::arg("conditions"))
            .def_static("in_", &Condition::In,
                        py::arg("field"), py::arg("values"), py::arg("negate") = false,
                        "شرط IN یا NOT IN");

    // ──────────────────────────────────────────────────────────
    // §6  UpdateSpec
    // ──────────────────────────────────────────────────────────

    py::class_<UpdateOperation>(m, "UpdateOperation")
            .def(py::init<>())
            .def_readwrite("op",         &UpdateOperation::op)
            .def_readwrite("field",      &UpdateOperation::field)
            .def_readwrite("value",      &UpdateOperation::value)
            .def_readwrite("values",     &UpdateOperation::values)
            .def_readwrite("value_type", &UpdateOperation::value_type);

    py::class_<UpdateSpec>(m, "UpdateSpec",
                           R"doc(
        مجموعه عملیات به‌روزرسانی — Builder Pattern.

        Example:
            spec = nexoradb.UpdateSpec()
            spec.set("bio", "Hello World")
            spec.inc("likes", "1", nexoradb.UpdateValueType.Int64)
            spec.push("tags", "sport")
            spec.touch_date("updated_at")
            db.update_by_id("posts", "p1", spec)
        )doc")
            .def(py::init<>())
            .def_readwrite("operations", &UpdateSpec::operations)
            .def_readwrite("upsert",     &UpdateSpec::upsert)
                    // builder methods
            .def("set", [](UpdateSpec& s, const std::string& f, const std::string& v,
                           UpdateValueType vt) -> UpdateSpec& {
                     return s.Set(f, v, vt);
                 }, py::arg("field"), py::arg("value"),
                 py::arg("value_type") = UpdateValueType::String,
                 py::return_value_policy::reference_internal)
            .def("unset", [](UpdateSpec& s, const std::string& f) -> UpdateSpec& {
                return s.Unset(f);
            }, py::return_value_policy::reference_internal)
            .def("inc", [](UpdateSpec& s, const std::string& f, const std::string& v,
                           UpdateValueType vt) -> UpdateSpec& {
                     return s.Inc(f, v, vt);
                 }, py::arg("field"), py::arg("delta"),
                 py::arg("value_type") = UpdateValueType::Int64,
                 py::return_value_policy::reference_internal)
            .def("push", [](UpdateSpec& s, const std::string& f, const std::string& v,
                            UpdateValueType vt) -> UpdateSpec& {
                     return s.Push(f, v, vt);
                 }, py::arg("field"), py::arg("element"),
                 py::arg("value_type") = UpdateValueType::String,
                 py::return_value_policy::reference_internal)
            .def("pull", [](UpdateSpec& s, const std::string& f, const std::string& v,
                            UpdateValueType vt) -> UpdateSpec& {
                     return s.Pull(f, v, vt);
                 }, py::arg("field"), py::arg("element"),
                 py::arg("value_type") = UpdateValueType::String,
                 py::return_value_policy::reference_internal)
            .def("touch_date", [](UpdateSpec& s, const std::string& f) -> UpdateSpec& {
                return s.TouchDate(f);
            }, py::return_value_policy::reference_internal);

    // ──────────────────────────────────────────────────────────
    // §7  DocEngine
    // ──────────────────────────────────────────────────────────

    py::class_<DocEngine>(m, "DocEngine",
                          R"doc(
        موتور اصلی پایگاه داده اسناد NexoraDB.

        Args:
            db_path (str): مسیر پوشه RocksDB

        Raises:
            RuntimeError: اگر RocksDB باز نشود

        Example:
            db = nexoradb.DocEngine("/var/data/nexoradb")
            assert db.is_healthy()
        )doc")
            .def(py::init<const std::string&>(),
                 py::arg("db_path"),
                 "باز کردن / ایجاد دیتابیس در db_path")
            .def(py::init<const std::string&, const TransactionSettings&>(),
                 py::arg("db_path"), py::arg("transaction_settings"),
                 "باز کردن دیتابیس با timeout و deadlock policy سفارشی")

                    // ── Lifecycle ──
            .def("is_healthy", &DocEngine::IsHealthy,
                 "بررسی سلامت اتصال RocksDB")

                    // ── Collection Management ──
            .def("create_collection",
                 [](DocEngine& e, const std::string& name,
                    std::optional<SchemaDefinition> schema) {
                     return e.CreateCollection(name, schema);
                 },
                 py::arg("collection"), py::arg("schema") = py::none(),
                 R"doc(
            ایجاد Collection جدید.

            Args:
                collection (str): نام Collection
                schema (SchemaDefinition, optional): Schema اختیاری

            Returns:
                DBResult: success=True اگر موفق

            Example:
                result = db.create_collection("users")
            )doc")

            .def("drop_collection", &DocEngine::DropCollection,
                 py::arg("collection"),
                 "حذف Collection (برگشت‌ناپذیر)")

            .def("collection_exists", &DocEngine::CollectionExists,
                 py::arg("collection"),
                 "بررسی وجود Collection → bool")

            .def("list_collections", &DocEngine::ListCollections,
                 "لیست همه Collections → list[str]")

            .def("set_schema", &DocEngine::SetSchemaValidation,
                 py::arg("collection"), py::arg("schema"),
                 "تنظیم/به‌روزرسانی Schema")

                    // ── Index ──
            .def("create_index", &DocEngine::CreateIndex,
                 py::arg("collection"), py::arg("index_def"),
                 "ایجاد Index")

            .def("drop_index", &DocEngine::DropIndex,
                 py::arg("collection"), py::arg("index_name"),
                 "حذف Index")

            .def("rebuild_indexes", &DocEngine::RebuildIndexes,
                 py::arg("collection"),
                 "بازسازی indexها و migration فرمت legacy به v2")

            .def("resume_index_build", &DocEngine::ResumeIndexBuild,
                 py::arg("collection"), py::arg("index_name"))

            .def("cleanup_index_build", &DocEngine::CleanupIndexBuild,
                 py::arg("collection"), py::arg("index_name"))

            .def("get_indexes",
                 [](DocEngine& e, const std::string& col) {
                     return e.GetIndexes(col);
                 },
                 py::arg("collection"),
                 "لیست Index های یک Collection")

                    // ── Foreign Key ──
            .def("add_foreign_key", &DocEngine::AddForeignKey,
                 py::arg("collection"), py::arg("fk_def"),
                 "تعریف Foreign Key")

            .def("drop_foreign_key", &DocEngine::DropForeignKey,
                 py::arg("collection"), py::arg("fk_name"),
                 "حذف Foreign Key")

            .def("get_foreign_keys",
                 [](DocEngine& e, const std::string& col) {
                     return e.GetForeignKeys(col);
                 },
                 py::arg("collection"),
                 "لیست Foreign Key های یک Collection")

                    // ── CRUD: Insert ──
            .def("insert_one",
                 [](DocEngine& e, const std::string& col, const std::string& bson) {
                     py::gil_scoped_release rel;
                     return e.InsertOne(col, bson);
                 },
                 py::arg("collection"), py::arg("bson_json"),
                 R"doc(
            درج یک سند.

            Args:
                collection (str): نام Collection
                bson_json  (str): محتوای سند به فرمت JSON string

            Returns:
                DBResult: data = doc_id (UUID یا _id دستی)

            Example:
                r = db.insert_one("users", '{"username":"ali","email":"ali@ex.com"}')
                print(r.data)  # "550e8400-..."
            )doc")

            .def("insert_many",
                 [](DocEngine& e, const std::string& col,
                    const std::vector<std::string>& docs) {
                     py::gil_scoped_release rel;
                     return e.InsertMany(col, docs);
                 },
                 py::arg("collection"), py::arg("bson_list"),
                 R"doc(
            درج اتمیک چند سند.

            Args:
                collection (str):       نام Collection
                bson_list  (list[str]): لیست اسناد JSON

            Returns:
                DBResult: data = '["id1","id2","id3"]'
            )doc")

            .def("insert_many_bulk",
                 [](DocEngine& e, const std::string& col,
                    const std::vector<std::string>& docs,
                    const BulkWriteOptions& options) {
                     py::gil_scoped_release rel;
                     return e.InsertManyBulk(col, docs, options);
                 },
                 py::arg("collection"), py::arg("bson_list"),
                 py::arg("options") = BulkWriteOptions{})

                    // ── CRUD: Find ──
            .def("find_by_id",
                 [](DocEngine& e, const std::string& col, const std::string& id) {
                     py::gil_scoped_release rel;
                     return e.FindById(col, id);
                 },
                 py::arg("collection"), py::arg("doc_id"),
                 "پیدا کردن سند با ID → DBResult(data=JSON str)")

            .def("find_many",
                 [](DocEngine& e, const std::string& col,
                    const Condition& cond, uint32_t limit, uint32_t skip) {
                     py::gil_scoped_release rel;
                     return e.FindMany(col, cond, limit, skip);
                 },
                 py::arg("collection"),
                 py::arg("condition") = Condition{},
                 py::arg("limit")     = 0,
                 py::arg("skip")      = 0,
                 R"doc(
            جستجو با شرط.

            Args:
                collection (str):       نام Collection
                condition  (Condition): شرط جستجو (پیش‌فرض: همه)
                limit      (int):       حداکثر نتایج (0=بدون محدودیت)
                skip       (int):       برای pagination

            Returns:
                DBResult: data = '[{doc1},{doc2}]'

            Example:
                c = nexoradb.Condition.leaf("age", nexoradb.Op.GT, "18",
                                             nexoradb.ValueType.Int64)
                r = db.find_many("users", c, limit=10)
            )doc")

            .def("find_page",
                 [](DocEngine& e, const std::string& collection,
                    const Condition& condition, std::uint32_t limit,
                    const std::string& token) {
                     py::gil_scoped_release release;
                     return e.FindPage(collection, condition, limit, token);
                 },
                 py::arg("collection"),
                 py::arg("condition") = Condition{},
                 py::arg("limit") = 100,
                 py::arg("continuation_token") = "")

            .def("explain_plan", &DocEngine::ExplainPlan,
                 py::arg("collection"), py::arg("condition"),
                 py::arg("force_full_scan") = false,
                 "نمایش scan type، index و تعداد candidateهای بررسی‌شده")

                    // ── CRUD: Update ──
            .def("update_by_id",
                 [](DocEngine& e, const std::string& col,
                    const std::string& id, const UpdateSpec& spec) {
                     py::gil_scoped_release rel;
                     return e.UpdateById(col, id, spec);
                 },
                 py::arg("collection"), py::arg("doc_id"), py::arg("update_spec"),
                 "به‌روزرسانی یک سند با ID")

            .def("update_many",
                 [](DocEngine& e, const std::string& col,
                    const Condition& cond, const UpdateSpec& spec) {
                     py::gil_scoped_release rel;
                     return e.UpdateMany(col, cond, spec);
                 },
                 py::arg("collection"), py::arg("condition"), py::arg("update_spec"),
                 "به‌روزرسانی همه اسناد منطبق با شرط")

            .def("update_many_bulk",
                 [](DocEngine& e, const std::string& col,
                    const Condition& cond, const UpdateSpec& spec,
                    const BulkWriteOptions& options) {
                     py::gil_scoped_release rel;
                     return e.UpdateManyBulk(col, cond, spec, options);
                 },
                 py::arg("collection"), py::arg("condition"),
                 py::arg("update_spec"),
                 py::arg("options") = BulkWriteOptions{})

                    // ── CRUD: Delete ──
            .def("delete_by_id",
                 [](DocEngine& e, const std::string& col, const std::string& id) {
                     py::gil_scoped_release rel;
                     return e.DeleteById(col, id);
                 },
                 py::arg("collection"), py::arg("doc_id"),
                 "حذف یک سند با ID → DBResult(data='1' یا '0')")

            .def("delete_many",
                 [](DocEngine& e, const std::string& col, const Condition& cond) {
                     py::gil_scoped_release rel;
                     return e.DeleteMany(col, cond);
                 },
                 py::arg("collection"), py::arg("condition"),
                 "حذف اسناد منطبق با شرط → DBResult(data=count)")

            .def("delete_many_bulk",
                 [](DocEngine& e, const std::string& col,
                    const Condition& cond,
                    const BulkWriteOptions& options) {
                     py::gil_scoped_release rel;
                     return e.DeleteManyBulk(col, cond, options);
                 },
                 py::arg("collection"), py::arg("condition"),
                 py::arg("options") = BulkWriteOptions{})

                    // ── Utility ──
            .def("count",
                 [](DocEngine& e, const std::string& col, const Condition& cond) {
                     py::gil_scoped_release rel;
                     return e.Count(col, cond);
                 },
                 py::arg("collection"), py::arg("condition") = Condition{},
                 "تعداد اسناد → DBResult(data='42')")

            .def("exists",
                 [](DocEngine& e, const std::string& col, const Condition& cond) {
                     py::gil_scoped_release rel;
                     return e.Exists(col, cond);
                 },
                 py::arg("collection"), py::arg("condition"),
                 "بررسی وجود → DBResult(data='true'/'false')")

            .def("get_collection_size", &DocEngine::GetCollectionSize,
                 py::arg("collection"),
                 "تعداد اسناد → int (O(1))")

            .def("reconcile_collection_counter",
                 [](DocEngine& e, const std::string& col) {
                     py::gil_scoped_release rel;
                     return e.ReconcileCollectionCounter(col);
                 },
                 py::arg("collection"),
                 "بازسازی اتمیک شمارنده collection از روی اسناد واقعی")

            .def("get_ram_usage_bytes", &DocEngine::GetRamUsageBytes,
                 "RAM فعلی مصرف‌شده توسط process دیتابیس، بر حسب byte")

            .def("get_disk_usage_bytes", &DocEngine::GetDiskUsageBytes,
                 "فضای دیسک اشغال‌شده توسط دایرکتوری دیتابیس، بر حسب byte")

            .def("create_internal_user", &DocEngine::CreateInternalUser,
                 py::arg("user_json"),
                 "ساخت کاربر داخلی دیتابیس در system collection مخفی")

            .def("get_internal_user", &DocEngine::GetInternalUser,
                 py::arg("username"),
                 "دریافت کاربر داخلی دیتابیس بر اساس username")

            .def("update_internal_user", &DocEngine::UpdateInternalUser,
                 py::arg("username"), py::arg("user_json"),
                 "جایگزینی کامل سند کاربر داخلی دیتابیس")

            .def("delete_internal_user", &DocEngine::DeleteInternalUser,
                 py::arg("username"),
                 "حذف منطقی کاربر داخلی دیتابیس با status='deleted'")

            .def("create_internal_app_token", &DocEngine::CreateInternalAppToken,
                 py::arg("token_id"), py::arg("token_json"),
                 "ذخیره امن metadata توکن برنامه در system collection")

            .def("list_internal_app_tokens", &DocEngine::ListInternalAppTokens,
                 "لیست توکن‌های برنامه از system collection")

            .def("delete_internal_app_token", &DocEngine::DeleteInternalAppToken,
                 py::arg("token_id"), "حذف و revoke توکن برنامه")

            .def("is_internal_app_token_active", &DocEngine::IsInternalAppTokenActive,
                 py::arg("token_id"), "بررسی فعال بودن توکن مدیریت‌شده")

            .def("get_schema",
                 [](DocEngine& e, const std::string& col) -> py::object {
                     auto s = e.GetSchema(col);
                     if (!s) return py::none();
                     return py::cast(*s);
                 },
                 py::arg("collection"),
                 "Schema یک Collection → SchemaDefinition یا None")

            .def("lookup_join",
                 [](DocEngine& e,
                    const std::string& from_col, const std::string& from_field,
                    const std::string& to_col,   const std::string& to_field,
                    const Condition& cond, uint32_t limit) {
                     // GIL را فقط در طول C++ call آزاد می‌کنیم
                     // قبل از ساخت py::dict باید GIL برگردد
                     JoinResult jr;
                     {
                         py::gil_scoped_release rel;
                         jr = e.LookupJoin(from_col, from_field,
                                           to_col,   to_field, cond, limit);
                     }
                     // GIL حالا مجدداً گرفته شده — safe است
                     py::dict d;
                     d["success"]   = jr.success;
                     d["records"]   = jr.records;
                     d["error_msg"] = jr.error_msg;
                     return d;
                 },
                 py::arg("from_collection"), py::arg("from_field"),
                 py::arg("to_collection"),   py::arg("to_field"),
                 py::arg("condition")        = Condition{},
                 py::arg("limit")            = 0u,
                 R"doc(
            LEFT LOOKUP JOIN دو Collection.

            Returns:
                dict: {"success": bool, "records": list[str], "error_msg": str}
                هر record یک JSON string است که شامل __joined__ فیلد است.

            Example:
                r = db.lookup_join("posts", "author_id", "users", "_id",
                                   limit=10)
                for doc_str in r["records"]:
                    doc = json.loads(doc_str)
            )doc")

                    // ── Graph Internal API (برای GraphManager) ──
            .def("iterate_collection",
                 [](DocEngine& e, const std::string& col,
                    const std::function<bool(std::string, std::string)>& cb,
                    uint32_t batch) {
                     // ⚠️ GIL را اینجا آزاد نمی‌کنیم!
                     // چون callback یک Python function است و برای اجرا به GIL نیاز دارد.
                     // اگر GIL آزاد شود و callback صدا زده شود → Segfault
                     e.IterateCollection(col, cb, batch);
                 },
                 py::arg("collection"),
                 py::arg("callback"),
                 py::arg("batch_size") = 100u,
                 R"doc(
            iterate روی همه اسناد یک Collection (برای GraphEngine).

            Args:
                collection (str):  نام Collection
                callback   (callable): fn(doc_id: str, bson: str) → bool
                batch_size (int):  اندازه batch (پیش‌فرض: 100)

            Example:
                def on_doc(doc_id, bson):
                    print(doc_id, bson[:50])
                    return True  # False = توقف زودهنگام
                db.iterate_collection("users", on_doc)
            )doc")

                    // ── Transaction ──
            .def("begin_transaction",
                 [](DocEngine& e) -> py::object {
                     auto tx = e.BeginTransaction();
                     if (!tx) return py::none();
                     return py::cast(tx.release(),
                                     py::return_value_policy::take_ownership);
                 },
                 "شروع تراکنش → TxHandle یا None")

            .def("commit_transaction",
                 [](DocEngine& e, TxHandle* tx) -> DBResult {
                     if (!tx) return DBResult::Err("null transaction");
                     return e.CommitTransaction(*tx);
                 }, py::arg("tx"))

            .def("rollback_transaction",
                 [](DocEngine& e, TxHandle* tx) -> DBResult {
                     if (!tx) return DBResult::Err("null transaction");
                     return e.RollbackTransaction(*tx);
                 }, py::arg("tx"))

            .def("insert_one_tx",
                 [](DocEngine& e, TxHandle* tx,
                    const std::string& col, const std::string& doc) -> DBResult {
                     if (!tx) return DBResult::Err("null transaction");
                     py::gil_scoped_release rel;
                     return e.InsertOneTx(*tx, col, doc);
                 },
                 py::arg("tx"), py::arg("collection"), py::arg("document"),
                 "InsertOne داخل transaction")

            .def("update_by_id_tx",
                 [](DocEngine& e, TxHandle* tx,
                    const std::string& col, const std::string& id,
                    const UpdateSpec& spec) -> DBResult {
                     if (!tx) return DBResult::Err("null transaction");
                     py::gil_scoped_release rel;
                     return e.UpdateByIdTx(*tx, col, id, spec);
                 },
                 py::arg("tx"), py::arg("collection"), py::arg("doc_id"),
                 py::arg("update_spec"),
                 "UpdateById داخل transaction")

            .def("delete_by_id_tx",
                 [](DocEngine& e, TxHandle* tx,
                    const std::string& col, const std::string& id) -> DBResult {
                     if (!tx) return DBResult::Err("null transaction");
                     py::gil_scoped_release rel;
                     return e.DeleteByIdTx(*tx, col, id);
                 },
                 py::arg("tx"), py::arg("collection"), py::arg("doc_id"),
                 "DeleteById داخل transaction")

            .def("find_by_id_tx",
                 [](DocEngine& e, TxHandle* tx,
                    const std::string& col, const std::string& id) -> DBResult {
                     if (!tx) return DBResult::Err("null transaction");
                     py::gil_scoped_release rel;
                     return e.FindByIdTx(*tx, col, id);
                 },
                 py::arg("tx"), py::arg("collection"), py::arg("doc_id"),
                 "FindById داخل transaction");

    // ──────────────────────────────────────────────────────────
    // §8  TxHandle
    // ──────────────────────────────────────────────────────────

    py::class_<TxHandle>(m, "TxHandle",
                         "هندل تراکنش — از begin_transaction() گرفته می‌شود")
            .def("is_valid", &TxHandle::IsValid);

    // ──────────────────────────────────────────────────────────
    // §9  GraphEngine (فقط اگر NEXORA_BUILD_GRAPH)
    // ──────────────────────────────────────────────────────────

#ifdef NEXORA_BUILD_GRAPH

    // ── Enums ──

    py::enum_<GraphMode>(m, "GraphMode")
            .value("Live",   GraphMode::Live)
            .value("Static", GraphMode::Static)
            .export_values();

    py::enum_<Direction>(m, "Direction")
            .value("Out",  Direction::Out)
            .value("In",   Direction::In)
            .value("Both", Direction::Both)
            .export_values();

    // ── NodeMappingDef ──

    py::class_<NodeMappingDef>(m, "NodeMappingDef",
                               R"doc(
        تعریف mapping از یک Collection به node type.

        Example:
            nm = nexoradb.NodeMappingDef()
            nm.node_type  = "User"
            nm.collection = "users"
            nm.key_path   = "_id"
            nm.properties = ["username", "age"]
        )doc")
            .def(py::init<>())
            .def_readwrite("node_type",   &NodeMappingDef::node_type)
            .def_readwrite("collection",  &NodeMappingDef::collection)
            .def_readwrite("key_path",    &NodeMappingDef::key_path)
            .def_readwrite("properties",  &NodeMappingDef::properties)
            .def_readwrite("filter_expr", &NodeMappingDef::filter_expr);

    // ── UnwindConfig ──

    py::class_<UnwindConfig>(m, "UnwindConfig",
                             R"doc(
        پیکربندی UNWIND برای ساخت edge از array.

        Example:
            # سند: {"_id":"u1","following":["u2","u3"]}
            uw = nexoradb.UnwindConfig()
            uw.array_path = "following"
            uw.alias      = "followed_id"
        )doc")
            .def(py::init<>())
            .def_readwrite("array_path", &UnwindConfig::array_path)
            .def_readwrite("alias",      &UnwindConfig::alias);

    // ── EdgeMappingDef ──

    py::class_<EdgeMappingDef>(m, "EdgeMappingDef",
                               R"doc(
        تعریف mapping از یک Collection به edge type.

        سه روش:
          1. collection جداگانه (follows.from_id → users)
          2. FK در سند (posts.author_id → users._id)
          3. UNWIND array (users.following[] → User)

        Example روش ۱:
            em = nexoradb.EdgeMappingDef()
            em.edge_type        = "FOLLOWS"
            em.collection       = "follows"
            em.source_path      = "from_id"
            em.source_node_type = "User"
            em.target_path      = "to_id"
            em.target_node_type = "User"
            em.directed         = True

        Example روش ۳ (UNWIND):
            em = nexoradb.EdgeMappingDef()
            em.edge_type        = "LIKES"
            em.collection       = "posts"
            em.source_path      = "liker_id"
            em.source_node_type = "User"
            em.target_path      = "_id"
            em.target_node_type = "Post"
            uw = nexoradb.UnwindConfig()
            uw.array_path = "liked_by"
            uw.alias      = "liker_id"
            em.unwind = uw
        )doc")
            .def(py::init<>())
            .def_readwrite("edge_type",        &EdgeMappingDef::edge_type)
            .def_readwrite("collection",       &EdgeMappingDef::collection)
            .def_readwrite("source_path",      &EdgeMappingDef::source_path)
            .def_readwrite("source_node_type", &EdgeMappingDef::source_node_type)
            .def_readwrite("target_path",      &EdgeMappingDef::target_path)
            .def_readwrite("target_node_type", &EdgeMappingDef::target_node_type)
            .def_readwrite("directed",         &EdgeMappingDef::directed)
            .def_readwrite("properties",       &EdgeMappingDef::properties)
            .def_property("unwind",
                          [](const EdgeMappingDef& e) -> py::object {
                              if (!e.unwind) return py::none();
                              return py::cast(*e.unwind);
                          },
                          [](EdgeMappingDef& e, py::object obj) {
                              if (obj.is_none()) e.unwind = std::nullopt;
                              else e.unwind = obj.cast<UnwindConfig>();
                          });

    // ── GraphDefinition ──

    py::class_<GraphDefinition>(m, "GraphDefinition",
                                R"doc(
        تعریف کامل یک گراف.

        Example:
            gdef = nexoradb.GraphDefinition()
            gdef.name = "social"
            gdef.mode = nexoradb.GraphMode.Live
            gdef.directed = True
            gdef.node_mappings.append(user_nm)
            gdef.edge_mappings.append(follows_em)
        )doc")
            .def(py::init<>())
            .def_readwrite("name",                  &GraphDefinition::name)
            .def_readwrite("mode",                  &GraphDefinition::mode)
            .def_readwrite("directed",              &GraphDefinition::directed)
            .def_readwrite("heterogeneous",         &GraphDefinition::heterogeneous)
            .def_readwrite("auto_build_on_startup", &GraphDefinition::auto_build_on_startup)
            .def_readwrite("node_mappings",         &GraphDefinition::node_mappings)
            .def_readwrite("edge_mappings",         &GraphDefinition::edge_mappings);

    // ── BuildResult ──

    py::class_<BuildResult>(m, "BuildResult",
                            "نتیجه build گراف")
            .def_readonly("success",      &BuildResult::success)
            .def_readonly("error_msg",    &BuildResult::error_msg)
            .def_readonly("nodes_built",  &BuildResult::nodes_built)
            .def_readonly("edges_built",  &BuildResult::edges_built)
            .def_readonly("elapsed_ms",   &BuildResult::elapsed_ms)
            .def("__repr__", [](const BuildResult& r) {
                return "<BuildResult success=" + std::string(r.success ? "True":"False") +
                       " nodes=" + std::to_string(r.nodes_built) +
                       " edges=" + std::to_string(r.edges_built) + ">";
            });

    // ── AlgoResult ──

    py::class_<algorithms::AlgoResult>(m, "AlgoResult",
                                       "نتیجه استاندارد الگوریتم‌های گراف")
            .def_readonly("success",     &algorithms::AlgoResult::success)
            .def_readonly("error_msg",   &algorithms::AlgoResult::error_msg)
            .def_readonly("result_json", &algorithms::AlgoResult::result_json)
            .def_readonly("elapsed_ms",  &algorithms::AlgoResult::elapsed_ms)
            .def("__repr__", [](const algorithms::AlgoResult& r) {
                return "<AlgoResult success=" + std::string(r.success ? "True":"False") +
                       " elapsed_ms=" + std::to_string(r.elapsed_ms) + ">";
            });

    // ── GraphStats ──

    py::class_<GraphStats>(m, "GraphStats")
            .def_readonly("active_nodes",   &GraphStats::active_nodes)
            .def_readonly("active_edges",   &GraphStats::active_edges)
            .def_readonly("deleted_nodes",  &GraphStats::deleted_nodes)
            .def_readonly("deleted_edges",  &GraphStats::deleted_edges)
            .def_readonly("heavy_nodes",    &GraphStats::heavy_nodes)
            .def_readonly("version",        &GraphStats::version);

    // ── GraphStatsEx (برای StaticGraph) ──

    py::class_<GraphStatsEx>(m, "GraphStatsEx")
            .def_readonly("active_nodes",   &GraphStatsEx::active_nodes)
            .def_readonly("active_edges",   &GraphStatsEx::active_edges)
            .def_readonly("node_count",     &GraphStatsEx::nodeCount)
            .def_readonly("edge_count",     &GraphStatsEx::edgeCount)
            .def_readonly("node_type_count",&GraphStatsEx::nodeTypeCount)
            .def_readonly("edge_type_count",&GraphStatsEx::edgeTypeCount)
            .def_readonly("version",        &GraphStatsEx::version);

    // ── CooGraph ──

    py::class_<CooGraph>(m, "CooGraph",
                         "خروجی COO format برای PyTorch Geometric / DGL")
            .def_readonly("src",              &CooGraph::src)
            .def_readonly("dst",              &CooGraph::dst)
            .def_readonly("edge_type_ids",    &CooGraph::edgeTypeIds)
            .def_readonly("original_node_ids",&CooGraph::originalNodeIds)
            .def("num_edges", [](const CooGraph& c) { return c.src.size(); });

    // ── CsrGraph ──

    py::class_<CsrGraph>(m, "CsrGraph",
                         "خروجی CSR format برای GPU BFS / cuGraph")
            .def_readonly("row_ptr",          &CsrGraph::rowPtr)
            .def_readonly("col_idx",          &CsrGraph::colIdx)
            .def_readonly("edge_type_ids",    &CsrGraph::edgeTypeIds)
            .def_readonly("original_node_ids",&CsrGraph::originalNodeIds);

    // ── GraphExportOptions ──

    py::class_<GraphExportOptions>(m, "GraphExportOptions")
            .def(py::init<>())
            .def_readwrite("node_types",              &GraphExportOptions::nodeTypes)
            .def_readwrite("edge_types",              &GraphExportOptions::edgeTypes)
            .def_readwrite("include_node_properties", &GraphExportOptions::includeNodeProperties)
            .def_readwrite("remap_contiguous",        &GraphExportOptions::remapNodeIdsToContiguous);

    // ── StaticGraph ──

    py::class_<StaticGraph>(m, "StaticGraph",
                            R"doc(
        Snapshot read-only از LiveGraph برای الگوریتم‌های سنگین.
        از GraphManager.create_snapshot() گرفته می‌شود.

        Example:
            snap = gm.create_snapshot("social")
            print(snap.node_count, snap.edge_count)

            nbrs = snap.neighbors(0, nexoradb.Direction.Out)
            snap.for_each_node(lambda id, tid: print(id, snap.node_type_name(tid)) or True)

            coo = snap.export_coo()
            # coo.src, coo.dst → PyTorch Geometric edge_index
        )doc")
            .def("node_count",      &StaticGraph::nodeCount)
            .def("edge_count",      &StaticGraph::edgeCount)
            .def("snapshot_version",&StaticGraph::snapshotVersion)
            .def("has_node",        &StaticGraph::hasNode,       py::arg("dense_id"))
            .def("node_type",       &StaticGraph::nodeType,      py::arg("dense_id"))
            .def("out_degree",      &StaticGraph::outDegree,     py::arg("dense_id"))
            .def("in_degree",       &StaticGraph::inDegree,      py::arg("dense_id"))
            .def("ext_id",          &StaticGraph::extId,         py::arg("dense_id"))
            .def("dense_id",        &StaticGraph::denseId,       py::arg("ext_id"))
            .def("node_type_name",  &StaticGraph::nodeTypeName,  py::arg("type_id"))
            .def("edge_type_name",  &StaticGraph::edgeTypeName,  py::arg("type_id"))
            .def("neighbors",
                 [](const StaticGraph& sg, DenseId id, Direction dir, TypeId tid) {
                     return sg.neighbors(id, dir, tid);
                 },
                 py::arg("dense_id"), py::arg("direction"),
                 py::arg("type_id") = kInvalidTypeId)
            .def("has_edge",        &StaticGraph::hasEdge,
                 py::arg("src"), py::arg("dst"), py::arg("type_id") = kInvalidTypeId)
            .def("for_each_node",
                 [](const StaticGraph& sg,
                    const std::function<bool(DenseId, TypeId)>& fn) {
                     sg.forEachNode(fn);
                 }, py::arg("callback"))
            .def("for_each_edge",
                 [](const StaticGraph& sg,
                    const std::function<bool(EdgeId, DenseId, DenseId, TypeId)>& fn) {
                     sg.forEachEdge(fn);
                 }, py::arg("callback"))
            .def("stats",      &StaticGraph::stats)
            .def("export_coo", &StaticGraph::exportCOO,
                 py::arg("opts") = GraphExportOptions{})
            .def("export_csr", &StaticGraph::exportCSR,
                 py::arg("opts") = GraphExportOptions{})
            .def("node_count_by_type", &StaticGraph::nodeCountByType)
            .def("edge_count_by_type", &StaticGraph::edgeCountByType);

    // ── GraphManager ──

    py::class_<GraphManager>(m, "GraphManager",
                             R"doc(
        Orchestrator اصلی — اتصال DocEngine به LiveGraph.

        Args:
            doc_engine (DocEngine): موتور اسناد
            graph_dir  (str):      پوشه فایل‌های گراف (پیش‌فرض: ./graph_data)

        Example:
            db = nexoradb.DocEngine("/var/data/nexoradb")
            gm = nexoradb.GraphManager(db, "./graph_data")
            gm.startup()

            gdef = nexoradb.GraphDefinition()
            gdef.name = "social"
            gdef.mode = nexoradb.GraphMode.Live
            nm = nexoradb.NodeMappingDef()
            nm.node_type  = "User"
            nm.collection = "users"
            nm.key_path   = "_id"
            gdef.node_mappings.append(nm)
            gm.create_graph(gdef)

            result = gm.build_graph("social")
            print(result.nodes_built, result.edges_built)
        )doc")
            .def(py::init<DocEngine*, const std::string&>(),
                 py::arg("doc_engine"), py::arg("graph_dir") = "./graph_data",
                 py::keep_alive<1, 2>())  // GraphManager نباید بیشتر از DocEngine زنده بماند

                    // Lifecycle
            .def("startup",  [](GraphManager& gm) {
                py::gil_scoped_release rel;
                return gm.startup();
            }, "بارگذاری گراف‌ها در startup")
            .def("shutdown", &GraphManager::shutdown, "بستن همه گراف‌ها")

                    // Graph Definition
            .def("create_graph",    &GraphManager::createGraph,    py::arg("definition"))
            .def("add_node_mapping",&GraphManager::addNodeMapping,
                 py::arg("graph_name"), py::arg("mapping"))
            .def("add_edge_mapping",&GraphManager::addEdgeMapping,
                 py::arg("graph_name"), py::arg("mapping"))
            .def("drop_graph",      &GraphManager::dropGraph,      py::arg("graph_name"))
            .def("list_graphs",     &GraphManager::listGraphs)
            .def("get_definition",  &GraphManager::getDefinition, py::arg("graph_name"))
            .def("is_ready",        &GraphManager::isReady,        py::arg("graph_name"))

                    // Build / Render
            .def("build_graph",
                 [](GraphManager& gm, const std::string& name) {
                     py::gil_scoped_release rel;
                     return gm.buildGraph(name);
                 }, py::arg("graph_name"),
                 "Build گراف از DocEngine → فایل‌های .nex/.nexr")

            .def("render_graph",
                 [](GraphManager& gm, const std::string& name) {
                     py::gil_scoped_release rel;
                     return gm.renderGraph(name);
                 }, py::arg("graph_name"),
                 "بارگذاری گراف از دیسک به RAM")

            .def("refresh_graph",
                 [](GraphManager& gm, const std::string& name) {
                     py::gil_scoped_release rel;
                     return gm.refreshGraph(name);
                 }, py::arg("graph_name"),
                 "Rebuild کامل گراف")

            .def("compact_graph",   &GraphManager::compactGraph,   py::arg("graph_name"))

                    // Traversal روی LiveGraph
            .def("neighbors",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::string& ext_id, const std::string& direction_str,
                    const std::string& edge_type, size_t limit) -> std::vector<std::string> {

                     Direction dir = Direction::Out;
                     if (direction_str == "in"   || direction_str == "IN")   dir = Direction::In;
                     if (direction_str == "both" || direction_str == "BOTH") dir = Direction::Both;

                     py::gil_scoped_release rel;
                     auto [g, lk] = gm.acquireReadLock(graph_name);
                     if (!g) return {};
                     return g->neighborsExt(ext_id, dir, edge_type, limit);
                 },
                 py::arg("graph_name"), py::arg("ext_id"),
                 py::arg("direction")  = "out",
                 py::arg("edge_type")  = "",
                 py::arg("limit")      = 100,
                 R"doc(
            همسایه‌های یک node در LiveGraph.

            Args:
                graph_name (str): نام گراف
                ext_id     (str): شناسه خارجی node (همان _id سند)
                direction  (str): "out" | "in" | "both"
                edge_type  (str): فیلتر نوع edge (پیش‌فرض: همه)
                limit      (int): حداکثر نتایج

            Returns:
                list[str]: لیست ext_id های همسایه

            Example:
                following = gm.neighbors("social", "u1", "out", "FOLLOWS", 50)
                followers = gm.neighbors("social", "u1", "in",  "FOLLOWS", 50)
            )doc")

            .def("has_edge",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::string& src, const std::string& dst,
                    const std::string& edge_type) -> bool {
                     auto [g, lk] = gm.acquireReadLock(graph_name);
                     if (!g) return false;
                     return g->hasEdge(src, dst, edge_type);
                 },
                 py::arg("graph_name"), py::arg("src_ext_id"),
                 py::arg("dst_ext_id"), py::arg("edge_type") = "")

            .def("run_mutual_friends",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::vector<std::string>& params) {
                     py::gil_scoped_release rel;
                     return algorithms::runMutualFriends(gm, graph_name, params);
                 },
                 py::arg("graph_name"), py::arg("params"),
                 "اجرای MutualFriends به صورت LockAlgorithm")

            .def("run_connected_components",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::vector<std::string>& params) {
                     py::gil_scoped_release rel;
                     return algorithms::runConnectedComponents(gm, graph_name, params);
                 },
                 py::arg("graph_name"), py::arg("params") = std::vector<std::string>{},
                 "اجرای ConnectedComponents به صورت JobAlgorithm")

            .def("run_most_connected",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::vector<std::string>& params) {
                     py::gil_scoped_release rel;
                     return algorithms::runMostConnected(gm, graph_name, params);
                 },
                 py::arg("graph_name"), py::arg("params") = std::vector<std::string>{},
                 "اجرای MostConnected به صورت LockAlgorithm")

            .def("run_network_stats",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::vector<std::string>& params) {
                     py::gil_scoped_release rel;
                     return algorithms::runNetworkStats(gm, graph_name, params);
                 },
                 py::arg("graph_name"), py::arg("params") = std::vector<std::string>{},
                 "اجرای NetworkStats به صورت LockAlgorithm")

            .def("run_community_detection",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::vector<std::string>& params) {
                     py::gil_scoped_release rel;
                     return algorithms::runCommunityDetection(gm, graph_name, params);
                 },
                 py::arg("graph_name"), py::arg("params") = std::vector<std::string>{},
                 "اجرای CommunityDetection به صورت JobAlgorithm")

            .def("run_all_distances",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::vector<std::string>& params) {
                     py::gil_scoped_release rel;
                     return algorithms::runAllDistances(gm, graph_name, params);
                 },
                 py::arg("graph_name"), py::arg("params") = std::vector<std::string>{},
                 "اجرای AllDistances به صورت JobAlgorithm")

            .def("run_get_friends",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::vector<std::string>& params) {
                     py::gil_scoped_release rel;
                     return algorithms::runGetFriends(gm, graph_name, params);
                 },
                 py::arg("graph_name"), py::arg("params"),
                 "اجرای GetFriends به صورت LockAlgorithm")

            .def("run_are_connected",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::vector<std::string>& params) {
                     py::gil_scoped_release rel;
                     return algorithms::runAreConnected(gm, graph_name, params);
                 },
                 py::arg("graph_name"), py::arg("params"),
                 "اجرای AreConnected به صورت LockAlgorithm")

            .def("run_shortest_path",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::vector<std::string>& params) {
                     py::gil_scoped_release rel;
                     return algorithms::runShortestPath(gm, graph_name, params);
                 },
                 py::arg("graph_name"), py::arg("params"),
                 "اجرای ShortestPath به صورت LockAlgorithm")

            .def("run_friend_suggestion",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::vector<std::string>& params) {
                     py::gil_scoped_release rel;
                     return algorithms::runFriendSuggestion(gm, graph_name, params);
                 },
                 py::arg("graph_name"), py::arg("params"),
                 "اجرای FriendSuggestion به صورت LockAlgorithm")

            .def("run_betweenness_centrality",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::vector<std::string>& params) {
                     py::gil_scoped_release rel;
                     return algorithms::runBetweennessCentrality(gm, graph_name, params);
                 },
                 py::arg("graph_name"), py::arg("params") = std::vector<std::string>{},
                 "اجرای BetweennessCentrality به صورت JobAlgorithm")

            .def("run_influence_maximization",
                 [](GraphManager& gm, const std::string& graph_name,
                    const std::vector<std::string>& params) {
                     py::gil_scoped_release rel;
                     return algorithms::runInfluenceMaximization(gm, graph_name, params);
                 },
                 py::arg("graph_name"), py::arg("params") = std::vector<std::string>{},
                 "اجرای InfluenceMaximization به صورت JobAlgorithm")

                    // Snapshot برای الگوریتم سنگین
            .def("create_snapshot",
                 [](GraphManager& gm, const std::string& name,
                    const std::vector<std::string>& nt,
                    const std::vector<std::string>& et) -> py::object {
                     // GIL فقط در طول C++ snapshot گرفته می‌شود
                     std::unique_ptr<StaticGraph> snap;
                     {
                         py::gil_scoped_release rel;
                         snap = gm.createSnapshot(name, nt, et);
                     }
                     // GIL برگشته — py::cast و py::none ایمن هستند
                     if (!snap) return py::none();
                     return py::cast(snap.release(),
                                     py::return_value_policy::take_ownership);
                 },
                 py::arg("graph_name"),
                 py::arg("node_types") = std::vector<std::string>{},
                 py::arg("edge_types") = std::vector<std::string>{},
                 R"doc(
            گرفتن snapshot برای الگوریتم سنگین (JobAlgorithm).

            Returns:
                StaticGraph یا None

            Example:
                snap = gm.create_snapshot("social")
                # الگوریتم روی snap اجرا کن (snapshot از LiveGraph جداست)
                coo = snap.export_coo()
            )doc")

                    // Live Update hooks (از NexoraDB wrapper صدا زده می‌شوند)
            .def("on_document_inserted",
                 [](GraphManager& gm, const std::string& col, const std::string& bson) {
                     py::gil_scoped_release rel;
                     gm.onDocumentInserted(col, bson);
                 },
                 py::arg("collection"), py::arg("bson_json"),
                 "notify به LiveGraph بعد از insert در DocEngine")

            .def("on_document_updated",
                 [](GraphManager& gm, const std::string& col,
                    const std::string& old_bson, const std::string& new_bson) {
                     py::gil_scoped_release rel;
                     gm.onDocumentUpdated(col, old_bson, new_bson);
                 },
                 py::arg("collection"), py::arg("old_bson"), py::arg("new_bson"))

            .def("on_document_deleted",
                 [](GraphManager& gm, const std::string& col, const std::string& bson) {
                     py::gil_scoped_release rel;
                     gm.onDocumentDeleted(col, bson);
                 },
                 py::arg("collection"), py::arg("bson_json"))

                    // Stats / WAL
            .def("get_stats",    &GraphManager::getStats,    py::arg("graph_name"))
            .def("get_wal_status",
                 [](GraphManager& gm, const std::string& name) {
                     // GIL نگه می‌داریم چون py::dict می‌سازیم
                     auto ws = gm.getWalStatus(name);
                     py::dict d;
                     d["total_entries"]   = ws.total_entries;
                     d["pending_entries"] = ws.pending_entries;
                     d["has_pending"]     = ws.has_pending;
                     return d;
                 }, py::arg("graph_name"))

            .def("get_recent_wal_entries",
                 [](GraphManager& gm, const std::string& name, size_t limit) {
                     auto entries = gm.getRecentWalEntries(name, limit);
                     py::list out;
                     for (const auto& rec : entries) {
                         py::dict d;
                         d["seq"]             = rec.seq;
                         d["timestamp_ms"]    = rec.timestamp_ms;
                         d["op"]              = wal_op_to_string(rec.op);
                         d["op_code"]         = static_cast<unsigned int>(rec.op);
                         d["node_or_edge_id"] = rec.node_or_edge_id;
                         d["src_id"]          = rec.src_id;
                         d["dst_id"]          = rec.dst_id;
                         d["type_id"]         = rec.type_id;
                         d["flags"]           = rec.flags;
                         d["applied"]         = rec.applied;
                         out.append(d);
                     }
                     return out;
                 },
                 py::arg("graph_name"), py::arg("limit") = 20,
                 "آخرین رکوردهای WAL گراف → list[dict]")
            .def("purge_wal",    &GraphManager::purgeWAL,    py::arg("graph_name"));

#endif // NEXORA_BUILD_GRAPH

    // ──────────────────────────────────────────────────────────
    // §10  Version info
    // ──────────────────────────────────────────────────────────

    m.attr("__version__")    = "0.1.1";
    m.attr("GRAPH_ENABLED")  =
#ifdef NEXORA_BUILD_GRAPH
            true;
#else
    false;
#endif

} // PYBIND11_MODULE

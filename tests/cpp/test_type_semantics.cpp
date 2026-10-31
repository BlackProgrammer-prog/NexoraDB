#include "TestTempDir.h"
#include "core/DocEngine.h"
#include "core/DocumentCodec.h"
#include "core/IndexCodec.h"
#include "query/Evaluator.h"
#include "query/NumericCompare.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <limits>
#include <set>
#include <tuple>
#include <vector>
#include <charconv>
#include <cmath>
using namespace nexora::core;
using namespace nexora::query;
using Json=nlohmann::json;

namespace {
std::vector<std::string> Corpus() {
    std::vector<Json> values={nullptr,false,true,"","null","1","a",std::string("a\0b",3),"فارسی",
        int64_t(0),int64_t(1),int64_t(-1),int64_t(9007199254740992LL),int64_t(9007199254740993LL),
        std::numeric_limits<int64_t>::min(),std::numeric_limits<int64_t>::max(),
        0.0,-0.0,1.0,-1.5,9007199254740992.0,0x1p63,
        std::numeric_limits<double>::max(),Json::array({1,2}),Json::object({{"a",1}})};
    std::vector<std::string> docs;
    for(size_t i=0;i<values.size();++i)
        docs.push_back(Json({{"_id",std::to_string(i)},{"tenant","t"},{"v",values[i]}}).dump());
    docs.push_back(R"({"_id":"missing","tenant":"t"})");
    return docs;
}
std::set<std::string> Ids(const DBResult& result) {
    EXPECT_TRUE(result.success)<<result.error_msg;
    std::set<std::string> ids;
    if(result.success) for(const auto& row:Json::parse(result.data)) ids.insert(row.at("_id"));
    return ids;
}
}

TEST(TypeSemantics, CorpusScanPlannerAndEvaluatorAgree) {
    TestTempDir temp("type_semantics_corpus");DocEngine engine(temp.path().string());
    ASSERT_TRUE(engine.CreateCollection("items").success);
    const auto corpus=Corpus();ASSERT_TRUE(engine.InsertMany("items",corpus).success);
    IndexDefinition single;single.index_name="v_idx";single.fields={"v"};
    ASSERT_TRUE(engine.CreateIndex("items",single).success);
    IndexDefinition compound;compound.index_name="tenant_v_idx";compound.fields={"tenant","v"};compound.type=IndexType::Compound;
    ASSERT_TRUE(engine.CreateIndex("items",compound).success);
    Evaluator evaluator;
    for(const auto op:{Op::EQ,Op::NEQ,Op::GT,Op::GTE,Op::LT,Op::LTE,Op::IN,Op::NIN}) {
        for(const auto& [raw,type]:std::vector<std::pair<std::string,ValueType>>{
                {"",ValueType::String},{"null",ValueType::String},{"a",ValueType::String},
                {std::string("a\0b",3),ValueType::String},{"true",ValueType::Bool},
                {"null",ValueType::Null},{"0",ValueType::Int64},{"1",ValueType::Int64},
                {"9007199254740993",ValueType::Int64},{"9223372036854775807",ValueType::Int64},
                {"-9223372036854775808",ValueType::Int64},{"0.0",ValueType::Float64},
                {"-0.0",ValueType::Float64},{"1.0",ValueType::Float64},{"-1.5",ValueType::Float64},
                {"9223372036854775808.0",ValueType::Float64}}) {
            auto condition=Condition::Leaf("v",op,raw,type);
            if(op==Op::IN || op==Op::NIN) {condition.values={raw};condition.value_types={type};}
            SCOPED_TRACE(raw+" op="+std::to_string(int(op)));
            std::set<std::string> expected;
            for(const auto& doc:corpus) if(evaluator.Match(doc,condition)) expected.insert(Json::parse(doc)["_id"]);
            EXPECT_EQ(Ids(engine.FindManyFullScanForTesting("items",condition,1000,0)),expected);
            EXPECT_EQ(Ids(engine.FindMany("items",condition,1000,0)),expected);
        }
    }
    // Compound sparse prefix must include null/missing/container v values.
    const auto prefix=Condition::Leaf("tenant",Op::EQ,"t");
    EXPECT_EQ(Ids(engine.FindMany("items",prefix,1000,0)).size(),corpus.size());
    EXPECT_NE(engine.ExplainPlan("items",prefix).data.find("full_scan"),std::string::npos);
    for(const auto type:{ValueType::Int64,ValueType::Float64,ValueType::Null}) {
        const auto condition=Condition::Leaf("v",Op::EQ,type==ValueType::Null?"null":"0",type);
        EXPECT_NE(engine.ExplainPlan("items",condition).data.find(type==ValueType::Null?"full_scan":"index_equality"),std::string::npos);
    }
}

TEST(TypeSemantics, IndependentExpectedNumericNullMissingAndContainers) {
    Evaluator evaluator;
    EXPECT_TRUE(evaluator.Match(R"({"v":1})",Condition::Leaf("v",Op::EQ,"1.0",ValueType::Float64)));
    EXPECT_FALSE(evaluator.Match(R"({"v":true})",Condition::Leaf("v",Op::EQ,"1",ValueType::Int64)));
    EXPECT_FALSE(evaluator.Match(R"({"v":"1"})",Condition::Leaf("v",Op::EQ,"1",ValueType::Int64)));
    EXPECT_FALSE(evaluator.Match(R"({"v":9007199254740993})",Condition::Leaf("v",Op::EQ,"9007199254740992.0",ValueType::Float64)));
    EXPECT_TRUE(evaluator.Match(R"({"v":9007199254740993})",Condition::Leaf("v",Op::GT,"9007199254740992.0",ValueType::Float64)));
    EXPECT_TRUE(evaluator.Match(R"({"v":-0.0})",Condition::Leaf("v",Op::EQ,"0.0",ValueType::Float64)));
    EXPECT_TRUE(evaluator.Match(R"({"v":null})",Condition::Leaf("v",Op::EQ,"null",ValueType::Null)));
    EXPECT_FALSE(evaluator.Match("{}",Condition::Leaf("v",Op::EQ,"null",ValueType::Null)));
    EXPECT_FALSE(evaluator.Match("{}",Condition::Leaf("v",Op::NEQ,"null",ValueType::Null)));
    EXPECT_FALSE(evaluator.Match(R"({"v":[1,2]})",Condition::Leaf("v",Op::EQ,"[1,2]",ValueType::String)));
    EXPECT_EQ(CompareIntDouble(std::numeric_limits<int64_t>::max(),0x1p63),-1);
}

TEST(TypeSemantics, IndexConstantsRejectInvalidTypesAndHaveV3GoldenBytes) {
    EXPECT_EQ(indexv2::kFormatVersion,3U);
    const auto hex=[](const std::string& bytes) {
        std::string out;const char* alphabet="0123456789abcdef";
        for(unsigned char byte:bytes) {out+=alphabet[byte>>4];out+=alphabet[byte&15];}
        return out;
    };
    for(const auto& [raw,type,golden]:std::vector<std::tuple<std::string,ValueType,std::string>>{
            {"",ValueType::String,"000000"},{std::string("a\0b",3),ValueType::String,"006100ff620000"},
            {"1",ValueType::Int64,"018000000000000001"},{"1.0",ValueType::Float64,"018000000000000001"},
            {"0.0",ValueType::Float64,"018000000000000000"},{"-0.0",ValueType::Float64,"018000000000000000"},
            {"true",ValueType::Bool,"0301"}}) {
        const auto encoded=indexv2::EncodeValues({FieldValue{raw,type,true}});
        ASSERT_TRUE(encoded);EXPECT_EQ(hex(*encoded),golden);
    }
    for(const auto type:{ValueType::Int64,ValueType::Float64,ValueType::Bool})
        EXPECT_FALSE(indexv2::EncodeValues({FieldValue{"",type,true}}));
    EXPECT_FALSE(indexv2::EncodeValues({FieldValue{"maybe",ValueType::Bool,true}}));
    EXPECT_FALSE(indexv2::EncodeValues({FieldValue{"nan",ValueType::Float64,true}}));
    EXPECT_FALSE(indexv2::EncodeValues({FieldValue{"inf",ValueType::Float64,true}}));
    EXPECT_FALSE(indexv2::EncodeValues({FieldValue{"9223372036854775808",ValueType::Int64,true}}));
    EXPECT_TRUE(indexv2::EncodeValues({FieldValue{"",ValueType::String,true}}));
    for(const auto& doc:Corpus()) {
        const auto json=Json::parse(doc);
        if(!json.contains("v") || json["v"].is_null() || json["v"].is_structured()) {
            EXPECT_FALSE(indexv2::EncodeTuple(doc,{"v"}));continue;
        }
        const auto& value=json["v"];
        const auto type=value.is_string()?ValueType::String:value.is_boolean()?ValueType::Bool:
            value.is_number_float()?ValueType::Float64:ValueType::Int64;
        const auto raw=value.is_string()?value.get<std::string>():value.dump();
        EXPECT_EQ(indexv2::EncodeTuple(doc,{"v"}),indexv2::EncodeValues({FieldValue{raw,type,true}}));
    }
}

TEST(TypeSemantics, InvalidNumbersRejectedAndEmbeddedNullRoundTrips) {
    EXPECT_FALSE(DocumentCodec::EncodeJson(R"({"v":9223372036854775808})").success);
    EXPECT_FALSE(DocumentCodec::EncodeJson(R"({"v":-9223372036854775809})").success);
    EXPECT_FALSE(DocumentCodec::EncodeJson(R"({"v":1e999})").success);
    EXPECT_FALSE(DocumentCodec::EncodeJson(R"({"v":{"$numberDouble":"NaN"}})").success);
    const auto encoded=DocumentCodec::EncodeJson(R"({"v":"a\u0000b"})");
    ASSERT_TRUE(encoded.success)<<encoded.error;
    const auto decoded=DocumentCodec::DecodeToJson(encoded.value);
    ASSERT_TRUE(decoded.success)<<decoded.error;
    EXPECT_EQ(Json::parse(decoded.value)["v"],std::string("a\0b",3));
    EXPECT_TRUE(DocumentCodec::EncodeBson(std::string_view(encoded.value).substr(DocumentCodec::kEnvelopeSize)).success);
}

TEST(TypeSemantics, V3UniqueBehaviorMatchesCanonicalCorpus) {
    TestTempDir temp("v2_unique_type_corpus");DocEngine engine(temp.path().string());
    ASSERT_TRUE(engine.CreateCollection("items").success);
    IndexDefinition index;index.index_name="unique_v";index.fields={"v"};index.type=IndexType::Unique;
    ASSERT_TRUE(engine.CreateIndex("items",index).success);
    std::set<std::string> owned;
    size_t sequence=0;
    for(int repeat=0;repeat<2;++repeat) for(const auto& document:Corpus()) {
        auto value=Json::parse(document);value["_id"]=std::to_string(sequence++);
        const auto doc=value.dump();const auto key=indexv2::EncodeTuple(doc,{"v"});
        const bool expected=!key || owned.insert(*key).second;
        const auto result=engine.InsertOne("items",doc);
        EXPECT_EQ(result.success,expected)<<doc<<" error="<<result.error_msg;
    }
    // Numeric aliases share a key; absent/null/container values remain sparse.
}

TEST(TypeSemantics, CanonicalNumericKeysAgreeAtPrecisionBoundsAndSubnormals) {
    struct Number {bool integer;int64_t i;double d;};
    std::vector<Number> values;
    for(const auto value:{std::numeric_limits<int64_t>::min(),int64_t(-1),int64_t(0),int64_t(1),
                          int64_t(9007199254740992LL),int64_t(9007199254740993LL),std::numeric_limits<int64_t>::max()})
        values.push_back({true,value,0});
    for(const auto value:{-0x1p63,std::nextafter(-0x1p63,-INFINITY),-1.5,-1.0,-0.0,0.0,1.0,1.5,
                          std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::min(),
                          std::nextafter(0x1p63,0.0),0x1p63,std::numeric_limits<double>::max()})
        values.push_back({false,0,value});
    const auto key=[](const Number& value) {
        char buffer[64];
        const auto text=value.integer ? std::to_chars(buffer,buffer+64,value.i) : std::to_chars(buffer,buffer+64,value.d);
        return indexv2::EncodeValues({FieldValue{std::string(buffer,text.ptr),value.integer?ValueType::Int64:ValueType::Float64,true}});
    };
    for(const auto& left:values) for(const auto& right:values) {
        const bool equal=left.integer ? (right.integer ? left.i==right.i : CompareIntDouble(left.i,right.d)==0) :
            (right.integer ? CompareIntDouble(right.i,left.d)==0 : left.d==right.d);
        const auto a=key(left),b=key(right);ASSERT_TRUE(a);ASSERT_TRUE(b);
        EXPECT_EQ(*a==*b,equal);
    }
}

// Copyright 2026 atframework
// etcd_packer unit tests (no etcd service required)

#include <atframe/etcdcli/etcd_packer.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "frame/test_macros.h"

// H.1.1 etcd_key_value pack/unpack round-trip
CASE_TEST(atapp_etcd_packer, packer_key_value_pack_unpack) {
  atapp::etcd_key_value original;
  original.key = "test-key-hello";
  original.value = "test-value-world";
  original.create_revision = 100;
  original.mod_revision = 200;
  original.version = 3;
  original.lease = 9876543210LL;

  rapidjson::Document doc;
  doc.SetObject();
  rapidjson::Value json_val(rapidjson::kObjectType);
  atapp::etcd_packer::pack(original, json_val, doc);

  // Packed JSON should have base64-encoded key and value
  CASE_EXPECT_TRUE(json_val.HasMember("key"));
  CASE_EXPECT_TRUE(json_val.HasMember("value"));

  atapp::etcd_key_value unpacked;
  atapp::etcd_packer::unpack(unpacked, json_val);

  CASE_EXPECT_EQ(original.key, unpacked.key);
  CASE_EXPECT_EQ(original.value, unpacked.value);
  CASE_EXPECT_EQ(original.create_revision, unpacked.create_revision);
  CASE_EXPECT_EQ(original.mod_revision, unpacked.mod_revision);
  CASE_EXPECT_EQ(original.version, unpacked.version);
  CASE_EXPECT_EQ(original.lease, unpacked.lease);
}

// H.1.2 etcd_response_header pack/unpack round-trip
CASE_TEST(atapp_etcd_packer, packer_response_header_pack_unpack) {
  atapp::etcd_response_header original;
  original.cluster_id = 12345678ULL;
  original.member_id = 87654321ULL;
  original.revision = 42;
  original.raft_term = 7;

  rapidjson::Document doc;
  doc.SetObject();
  rapidjson::Value json_val(rapidjson::kObjectType);
  atapp::etcd_packer::pack(original, json_val, doc);

  atapp::etcd_response_header unpacked;
  memset(&unpacked, 0, sizeof(unpacked));
  atapp::etcd_packer::unpack(unpacked, json_val);

  CASE_EXPECT_EQ(original.cluster_id, unpacked.cluster_id);
  CASE_EXPECT_EQ(original.member_id, unpacked.member_id);
  CASE_EXPECT_EQ(original.revision, unpacked.revision);
  CASE_EXPECT_EQ(original.raft_term, unpacked.raft_term);
}

// H.1.3 pack_key_range with prefix match ("+1")
CASE_TEST(atapp_etcd_packer, packer_key_range_prefix) {
  {
    // "abc" with "+1" → range_end should be "abd" (after base64 decode)
    rapidjson::Document doc;
    doc.SetObject();
    rapidjson::Value json_val(rapidjson::kObjectType);
    atapp::etcd_packer::pack_key_range(json_val, "abc", "+1", doc);

    std::string decoded_key;
    std::string decoded_range_end;
    atapp::etcd_packer::unpack_base64(json_val, "key", decoded_key);
    atapp::etcd_packer::unpack_base64(json_val, "range_end", decoded_range_end);

    CASE_EXPECT_EQ("abc", decoded_key);
    CASE_EXPECT_EQ("abd", decoded_range_end);
  }

  {
    // Key ending with 0xff: "a\xff" → trim last byte, increment → "b"
    std::string key_with_ff = "a";
    key_with_ff.push_back(static_cast<char>(0xff));

    rapidjson::Document doc;
    doc.SetObject();
    rapidjson::Value json_val(rapidjson::kObjectType);
    atapp::etcd_packer::pack_key_range(json_val, key_with_ff, "+1", doc);

    std::string decoded_range_end;
    atapp::etcd_packer::unpack_base64(json_val, "range_end", decoded_range_end);
    CASE_EXPECT_EQ("b", decoded_range_end);
  }
}

// H.1.4 pack_key_range with exact match (empty range_end)
CASE_TEST(atapp_etcd_packer, packer_key_range_exact) {
  rapidjson::Document doc;
  doc.SetObject();
  rapidjson::Value json_val(rapidjson::kObjectType);
  atapp::etcd_packer::pack_key_range(json_val, "exact-key", "", doc);

  std::string decoded_key;
  atapp::etcd_packer::unpack_base64(json_val, "key", decoded_key);
  CASE_EXPECT_EQ("exact-key", decoded_key);

  // range_end should be missing or empty for exact match
  std::string decoded_range_end;
  bool has_range_end = atapp::etcd_packer::unpack_base64(json_val, "range_end", decoded_range_end);
  if (has_range_end) {
    CASE_EXPECT_EQ("", decoded_range_end);
  }
}

// H.1.5 base64 encode/decode round-trip with various strings
CASE_TEST(atapp_etcd_packer, packer_base64_roundtrip) {
  std::string test_cases[] = {
      "",                                   // empty
      "a",                                  // short
      "hello world",                        // normal
      std::string("\0\x01\x02\xff", 4),     // binary with null
      std::string(1024, 'X'),               // long
      "special chars: !@#$%^&*()_+-=[]{}",  // special characters
  };

  for (const auto &input : test_cases) {
    rapidjson::Document doc;
    doc.SetObject();
    rapidjson::Value json_val(rapidjson::kObjectType);
    atapp::etcd_packer::pack_base64(json_val, "data", input, doc);

    std::string output;
    bool ok = atapp::etcd_packer::unpack_base64(json_val, "data", output);
    CASE_EXPECT_TRUE(ok);
    CASE_EXPECT_EQ(input.size(), output.size());
    CASE_EXPECT_EQ(input, output);
  }
}

// H.1.6 unpack_int handles both numeric and string JSON values
CASE_TEST(atapp_etcd_packer, packer_unpack_int_formats) {
  // Test int64 unpack from JSON number
  {
    rapidjson::Document doc;
    doc.Parse("{\"num\": 42, \"neg\": -100}");
    CASE_EXPECT_TRUE(doc.IsObject());

    int64_t val = 0;
    atapp::etcd_packer::unpack_int(doc, "num", val);
    CASE_EXPECT_EQ(42, val);

    atapp::etcd_packer::unpack_int(doc, "neg", val);
    CASE_EXPECT_EQ(-100, val);
  }

  // Test int64 unpack from JSON string (etcd sometimes returns ints as strings)
  {
    rapidjson::Document doc;
    doc.Parse("{\"str_num\": \"12345678901234\"}");
    CASE_EXPECT_TRUE(doc.IsObject());

    int64_t val = 0;
    atapp::etcd_packer::unpack_int(doc, "str_num", val);
    CASE_EXPECT_EQ(12345678901234LL, val);
  }

  // Test uint64 unpack
  {
    rapidjson::Document doc;
    doc.Parse("{\"big\": \"18446744073709551615\"}");
    CASE_EXPECT_TRUE(doc.IsObject());

    uint64_t val = 0;
    atapp::etcd_packer::unpack_int(doc, "big", val);
    CASE_EXPECT_EQ(UINT64_MAX, val);
  }

  // Test missing key defaults to 0
  {
    rapidjson::Document doc;
    doc.Parse("{}");
    CASE_EXPECT_TRUE(doc.IsObject());

    int64_t val = 999;
    atapp::etcd_packer::unpack_int(doc, "missing", val);
    CASE_EXPECT_EQ(0, val);
  }
}

// H.1.7 parse_object handles invalid JSON gracefully
CASE_TEST(atapp_etcd_packer, packer_parse_object_error) {
  rapidjson::Document doc;

  // Invalid JSON
  CASE_EXPECT_FALSE(atapp::etcd_packer::parse_object(doc, "not valid json {{{"));

  // Valid JSON but not an object (array)
  CASE_EXPECT_FALSE(atapp::etcd_packer::parse_object(doc, "[1,2,3]"));

  // Valid JSON but scalar, not object
  CASE_EXPECT_FALSE(atapp::etcd_packer::parse_object(doc, "42"));

  // Valid object should return true
  CASE_EXPECT_TRUE(atapp::etcd_packer::parse_object(doc, "{\"key\": \"value\"}"));

  // Minimal valid empty object
  CASE_EXPECT_TRUE(atapp::etcd_packer::parse_object(doc, "{}"));
}

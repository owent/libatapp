// Copyright 2022 atframework

#include <atframe/etcdcli/etcd_discovery.h>

#include <common/file_system.h>
#include <log/log_wrapper.h>
#include <string/string_format.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <vector>

#include "frame/test_macros.h"

namespace {
atapp::etcd_discovery_set::ptr_t create_discovery_set() {
  auto ret = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_set>();
  for (size_t i = 0; i < 32; ++i) {
    auto node = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
    atapp::protocol::atapp_discovery fake_info;
    atapp::etcd_discovery_node::node_version fake_version;
    fake_version.create_revision = 1;
    fake_version.modify_revision = 1;
    fake_version.version = 1;
    fake_info.set_id(static_cast<uint64_t>(i));
    fake_info.set_name(atfw::util::string::format("node-{}", i));
    node->copy_from(fake_info, fake_version);

    ret->add_node(node);
  }

  return ret;
}
}  // namespace

CASE_TEST(atapp_discovery, metadata_filter) {
  using etcd_discovery_set = atapp::etcd_discovery_set;

  etcd_discovery_set::metadata_type rule;
  etcd_discovery_set::metadata_type metadata;

  metadata.set_namespace_name("namespace");
  metadata.set_api_version("v1");
  metadata.set_kind("unit test");
  metadata.set_group("atapp_discovery");
  metadata.set_service_subset("next");

  (*metadata.mutable_labels())["label1"] = "value1";
  (*metadata.mutable_labels())["label2"] = "value2";

  // empty rule
  CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));

  // full match
  rule = metadata;
  CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  CASE_EXPECT_EQ(etcd_discovery_set::metadata_hash_type()(rule), etcd_discovery_set::metadata_hash_type()(metadata));
  CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type()(rule, metadata));

  // partly match - namespace_name
  rule.clear_namespace_name();
  CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  rule.set_namespace_name("mismatch value");
  CASE_EXPECT_FALSE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  rule.set_namespace_name(metadata.namespace_name());

  // partly match - api_version
  rule.clear_api_version();
  CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  rule.set_api_version("mismatch value");
  CASE_EXPECT_FALSE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  rule.set_api_version(metadata.api_version());

  // partly match - kind
  rule.clear_kind();
  CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  rule.set_kind("mismatch value");
  CASE_EXPECT_FALSE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  rule.set_kind(metadata.kind());

  // partly match - group
  rule.clear_group();
  CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  rule.set_group("mismatch value");
  CASE_EXPECT_FALSE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  rule.set_group(metadata.group());

  // partly match - service_subset
  rule.clear_service_subset();
  CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  rule.set_service_subset("mismatch value");
  CASE_EXPECT_FALSE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  rule.set_service_subset(metadata.service_subset());

  // labels
  {
    rule.mutable_labels()->erase("label1");
    CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
    (*rule.mutable_labels())["label1"] = "";
    CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
    (*rule.mutable_labels())["label1"] = "mismatch value";
    CASE_EXPECT_FALSE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
    rule.mutable_labels()->clear();
    CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  }
}

CASE_TEST(atapp_discovery, get_discovery_by_metadata) {
  using etcd_discovery_set = atapp::etcd_discovery_set;
  using etcd_discovery_node = atapp::etcd_discovery_node;

  etcd_discovery_set::ptr_t discovery_set = atfw::util::memory::make_strong_rc<etcd_discovery_set>();
  CASE_EXPECT_TRUE(discovery_set->empty());

  atapp::protocol::atapp_discovery discovery_data;
  discovery_data.set_type_id(3);
  discovery_data.set_type_name("test");
  discovery_data.set_pid(atbus::node::get_pid());
  discovery_data.set_hostname(atbus::node::get_hostname());
  discovery_data.set_version("v1");
  discovery_data.mutable_metadata()->set_namespace_name("namespace");
  discovery_data.mutable_metadata()->set_api_version("v1");
  discovery_data.mutable_metadata()->set_kind("unit test");
  discovery_data.mutable_metadata()->set_group("atapp_discovery");
  discovery_data.mutable_metadata()->set_service_subset("next");
  (*discovery_data.mutable_metadata()->mutable_labels())["label1"] = "value1";
  (*discovery_data.mutable_metadata()->mutable_labels())["label2"] = "value2";

  etcd_discovery_node::ptr_t node1 = atfw::util::memory::make_strong_rc<etcd_discovery_node>();
  discovery_data.set_id(1);
  discovery_data.set_name("node1");
  discovery_data.set_identity("node-dient-1");

  etcd_discovery_node::node_version test_node_version;
  test_node_version.create_revision = 1;
  test_node_version.modify_revision = 2;
  test_node_version.version = 3;
  node1->copy_from(discovery_data, test_node_version);
  CASE_EXPECT_EQ(1, node1->get_version().create_revision);
  CASE_EXPECT_EQ(2, node1->get_version().modify_revision);
  CASE_EXPECT_EQ(3, node1->get_version().version);

  etcd_discovery_node::ptr_t node2 = atfw::util::memory::make_strong_rc<etcd_discovery_node>();
  discovery_data.set_id(2);
  discovery_data.set_name("node2");
  discovery_data.set_identity("node-dient-2");
  (*discovery_data.mutable_metadata()->mutable_labels())["selector"] = "s2";
  node2->copy_from(discovery_data, test_node_version);

  etcd_discovery_node::ptr_t node3 = atfw::util::memory::make_strong_rc<etcd_discovery_node>();
  discovery_data.set_id(3);
  discovery_data.set_name("node3");
  discovery_data.set_identity("node-dient-3");
  (*discovery_data.mutable_metadata()->mutable_labels())["selector"] = "s3";
  node3->copy_from(discovery_data, test_node_version);

  discovery_set->add_node(node1);
  discovery_set->add_node(node2);
  discovery_set->add_node(node3);

  CASE_EXPECT_FALSE(discovery_set->empty());
  CASE_EXPECT_TRUE(discovery_set->get_node_by_id(1) == discovery_set->get_node_by_name("node1"));
  CASE_EXPECT_EQ(3, discovery_set->get_sorted_nodes().size());
  CASE_EXPECT_TRUE(discovery_set->get_node_by_consistent_hash(123U) == discovery_set->get_node_by_consistent_hash(123));
  CASE_EXPECT_EQ(0, discovery_set->metadata_index_size());

  // With metadata
  CASE_EXPECT_EQ(1, discovery_set->get_sorted_nodes(&discovery_data.metadata()).size());
  CASE_EXPECT_EQ(1, discovery_set->metadata_index_size());
  CASE_EXPECT_TRUE(node3 == discovery_set->get_node_by_consistent_hash(123, &discovery_data.metadata()));
  CASE_EXPECT_TRUE(node3 == discovery_set->get_node_by_random(&discovery_data.metadata()));
  CASE_EXPECT_TRUE(node3 == discovery_set->get_node_by_round_robin(&discovery_data.metadata()));

  discovery_set->remove_node(node2->get_discovery_info().id());
  CASE_EXPECT_EQ(1, discovery_set->metadata_index_size());
  CASE_EXPECT_EQ(2, discovery_set->get_sorted_nodes().size());
  CASE_EXPECT_EQ(1, discovery_set->get_sorted_nodes(&discovery_data.metadata()).size());
  CASE_EXPECT_TRUE(node3 == discovery_set->get_node_by_consistent_hash(123, &discovery_data.metadata()));
  CASE_EXPECT_TRUE(node3 == discovery_set->get_node_by_random(&discovery_data.metadata()));
  CASE_EXPECT_TRUE(node3 == discovery_set->get_node_by_round_robin(&discovery_data.metadata()));

  discovery_set->remove_node(node3->get_discovery_info().name());
  CASE_EXPECT_EQ(0, discovery_set->metadata_index_size());
  CASE_EXPECT_EQ(1, discovery_set->get_sorted_nodes().size());
  CASE_EXPECT_EQ(0, discovery_set->get_sorted_nodes(&discovery_data.metadata()).size());
  CASE_EXPECT_TRUE(nullptr == discovery_set->get_node_by_consistent_hash(123, &discovery_data.metadata()));
  CASE_EXPECT_TRUE(nullptr == discovery_set->get_node_by_random(&discovery_data.metadata()));
  CASE_EXPECT_TRUE(nullptr == discovery_set->get_node_by_round_robin(&discovery_data.metadata()));
}

CASE_TEST(atapp_discovery, round_robin) {
  auto discovery_set = create_discovery_set();

  size_t node_count = discovery_set->get_sorted_nodes().size();

  std::vector<atapp::etcd_discovery_node::ptr_t> check1;
  std::vector<atapp::etcd_discovery_node::ptr_t> check2;
  check1.reserve(node_count);
  check2.reserve(node_count);

  for (size_t i = 0; i < node_count; ++i) {
    check1.push_back(discovery_set->get_node_by_round_robin());
  }

  for (size_t i = 0; i < node_count; ++i) {
    check2.push_back(discovery_set->get_node_by_round_robin());
  }

  CASE_EXPECT_EQ(check1.size(), check2.size());
  CASE_EXPECT_EQ(check1.size(), node_count);

  for (size_t i = 0; i < check1.size(); ++i) {
    CASE_EXPECT_TRUE(check1[i] == check2[i]);
  }
}

CASE_TEST(atapp_discovery, lower_bound_normal) {
  auto discovery_set = create_discovery_set();
  size_t node_count = discovery_set->get_sorted_nodes().size();

  const char* hash_data = "1234567";
  auto node_hash = discovery_set->get_node_hash_by_consistent_hash(hash_data);

  std::vector<atapp::etcd_discovery_set::node_hash_type> check1;
  check1.resize(1);

  auto find_count = discovery_set->lower_bound_node_hash_by_consistent_hash(
      gsl::make_span(check1), node_hash, nullptr, atapp::etcd_discovery_set::node_hash_type::search_mode::kAll);

  CASE_EXPECT_EQ(1, find_count);
  CASE_EXPECT_EQ(check1[0].node->get_discovery_info().id(), node_hash.node->get_discovery_info().id());

  find_count = discovery_set->lower_bound_node_hash_by_consistent_hash(
      gsl::make_span(check1), node_hash, nullptr, atapp::etcd_discovery_set::node_hash_type::search_mode::kNextNode);

  CASE_EXPECT_EQ(1, find_count);
  CASE_EXPECT_NE(check1[0].node->get_discovery_info().id(), node_hash.node->get_discovery_info().id());

  check1.resize(node_count * atapp::etcd_discovery_set::node_hash_type::HASH_POINT_PER_INS + 10);

  // 超出最大数量，会提取所有Hash节点
  find_count = discovery_set->lower_bound_node_hash_by_consistent_hash(
      gsl::make_span(check1), node_hash, nullptr, atapp::etcd_discovery_set::node_hash_type::search_mode::kAll);
  CASE_EXPECT_EQ(node_count * atapp::etcd_discovery_set::node_hash_type::HASH_POINT_PER_INS, find_count);

  std::vector<atapp::etcd_discovery_set::node_hash_type> check2;
  check2.resize(check1.size());

  // 超出最大数量，不包含自己时提取的Hash节点少一个
  auto find_count2 = discovery_set->lower_bound_node_hash_by_consistent_hash(
      gsl::make_span(check2), node_hash, nullptr, atapp::etcd_discovery_set::node_hash_type::search_mode::kNextNode);
  CASE_EXPECT_EQ(find_count2 + 1, find_count);

  for (size_t i = 0; i < find_count2; ++i) {
    CASE_EXPECT_EQ(check2[i].node->get_discovery_info().id(), check1[i + 1].node->get_discovery_info().id());
  }
}

CASE_TEST(atapp_discovery, lower_bound_unique) {
  auto discovery_set = create_discovery_set();
  size_t node_count = discovery_set->get_sorted_nodes().size();

  for (uint64_t t = 0; t < 10; ++t) {
    uint64_t hash_data = 1234567 + t;
    auto node_hash = discovery_set->get_node_hash_by_consistent_hash(hash_data);

    std::vector<atapp::etcd_discovery_set::node_hash_type> check1;
    check1.resize(1);

    auto find_count = discovery_set->lower_bound_node_hash_by_consistent_hash(
        gsl::make_span(check1), node_hash, nullptr,
        atapp::etcd_discovery_set::node_hash_type::search_mode::kUniqueNode);

    CASE_EXPECT_EQ(1, find_count);
    CASE_EXPECT_EQ(check1[0].node->get_discovery_info().id(), node_hash.node->get_discovery_info().id());

    find_count = discovery_set->lower_bound_node_hash_by_consistent_hash(
        gsl::make_span(check1), node_hash, nullptr,
        atapp::etcd_discovery_set::node_hash_type::search_mode::kNextUniqueNode);

    CASE_EXPECT_EQ(1, find_count);
    CASE_EXPECT_NE(check1[0].node->get_discovery_info().id(), node_hash.node->get_discovery_info().id());

    check1.resize(node_count + 10);

    // 超出最大数量，会提取所有服务节点
    find_count = discovery_set->lower_bound_node_hash_by_consistent_hash(
        gsl::make_span(check1), node_hash, nullptr,
        atapp::etcd_discovery_set::node_hash_type::search_mode::kUniqueNode);
    CASE_EXPECT_EQ(node_count, find_count);

    std::vector<atapp::etcd_discovery_set::node_hash_type> check2;
    check2.resize(check1.size());

    // 超出最大数量，不包含自己时提取的Hash节点少一个
    auto find_count2 = discovery_set->lower_bound_node_hash_by_consistent_hash(
        gsl::make_span(check2), node_hash, nullptr,
        atapp::etcd_discovery_set::node_hash_type::search_mode::kNextUniqueNode);
    CASE_EXPECT_EQ(find_count2 + 1, find_count);

    for (size_t i = 0; i < find_count2; ++i) {
      CASE_EXPECT_EQ(check2[i].node->get_discovery_info().id(), check1[i + 1].node->get_discovery_info().id());
    }
  }
}

CASE_TEST(atapp_discovery, lower_bound_compact) {
  auto discovery_set = create_discovery_set();
  size_t node_count = discovery_set->get_sorted_nodes().size();

  std::string hash_data = "987654321";
  auto node_hash = discovery_set->get_node_hash_by_consistent_hash(hash_data);

  std::vector<atapp::etcd_discovery_set::node_hash_type> check1;
  check1.resize(1);

  auto find_count = discovery_set->lower_bound_node_hash_by_consistent_hash(
      gsl::make_span(check1), node_hash, nullptr, atapp::etcd_discovery_set::node_hash_type::search_mode::kCompact);

  CASE_EXPECT_EQ(1, find_count);
  CASE_EXPECT_EQ(check1[0].node->get_discovery_info().id(), node_hash.node->get_discovery_info().id());

  find_count = discovery_set->lower_bound_node_hash_by_consistent_hash(
      gsl::make_span(check1), node_hash, nullptr, atapp::etcd_discovery_set::node_hash_type::search_mode::kNextCompact);

  CASE_EXPECT_EQ(1, find_count);
  CASE_EXPECT_NE(check1[0].node->get_discovery_info().id(), node_hash.node->get_discovery_info().id());

  check1.resize(node_count * atapp::etcd_discovery_set::node_hash_type::HASH_POINT_PER_INS + 10);

  // 超出最大数量，会提取所有紧凑模式服务节点（数量和Hash分布相关，不可控）
  find_count = discovery_set->lower_bound_node_hash_by_consistent_hash(
      gsl::make_span(check1), node_hash, nullptr, atapp::etcd_discovery_set::node_hash_type::search_mode::kCompact);
  // 每个节点hash code必然不同
  for (size_t i = 1; i < find_count; ++i) {
    CASE_EXPECT_FALSE(check1[i].hash_code == check1[i - 1].hash_code);
  }
  CASE_EXPECT_FALSE(check1[0].hash_code == check1[find_count - 1].hash_code);

  std::vector<atapp::etcd_discovery_set::node_hash_type> check2;
  check2.resize(check1.size());

  // 超出最大数量，不包含自己时提取的Hash节点少N个（数量和Hash分布相关，不可控）
  auto find_count2 = discovery_set->lower_bound_node_hash_by_consistent_hash(
      gsl::make_span(check2), node_hash, nullptr, atapp::etcd_discovery_set::node_hash_type::search_mode::kNextCompact);
  CASE_EXPECT_LT(find_count2, find_count);

  for (size_t i = 0; i < find_count2; ++i) {
    CASE_EXPECT_EQ(check2[i].node->get_discovery_info().id(), check1[i + 1].node->get_discovery_info().id());
  }
}

CASE_TEST(atapp_discovery, lower_bound_compact_unique) {
  auto discovery_set = create_discovery_set();
  size_t node_count = discovery_set->get_sorted_nodes().size();

  for (uint64_t t = 0; t < 10; ++t) {
    uint64_t hash_data = 1234567 + t;
    auto node_hash = discovery_set->get_node_hash_by_consistent_hash(hash_data);

    std::vector<atapp::etcd_discovery_set::node_hash_type> check1;
    check1.resize(1);

    auto find_count = discovery_set->lower_bound_node_hash_by_consistent_hash(
        gsl::make_span(check1), node_hash, nullptr,
        atapp::etcd_discovery_set::node_hash_type::search_mode::kCompactUniqueNode);

    CASE_EXPECT_EQ(1, find_count);
    CASE_EXPECT_EQ(check1[0].node->get_discovery_info().id(), node_hash.node->get_discovery_info().id());

    find_count = discovery_set->lower_bound_node_hash_by_consistent_hash(
        gsl::make_span(check1), node_hash, nullptr,
        atapp::etcd_discovery_set::node_hash_type::search_mode::kNextCompactUniqueNode);

    CASE_EXPECT_EQ(1, find_count);
    CASE_EXPECT_NE(check1[0].node->get_discovery_info().id(), node_hash.node->get_discovery_info().id());

    check1.resize(node_count + 10);

    // 超出最大数量，会提取所有服务节点
    find_count = discovery_set->lower_bound_node_hash_by_consistent_hash(
        gsl::make_span(check1), node_hash, nullptr,
        atapp::etcd_discovery_set::node_hash_type::search_mode::kCompactUniqueNode);
    CASE_EXPECT_EQ(node_count, find_count);
    // 每个节点hash code必然不同
    for (size_t i = 1; i < find_count; ++i) {
      CASE_EXPECT_FALSE(check1[i].hash_code == check1[i - 1].hash_code);
    }
    CASE_EXPECT_FALSE(check1[0].hash_code == check1[find_count - 1].hash_code);

    std::vector<atapp::etcd_discovery_set::node_hash_type> check2;
    check2.resize(check1.size());

    // 超出最大数量，不包含自己时提取的Hash节点少一个
    auto find_count2 = discovery_set->lower_bound_node_hash_by_consistent_hash(
        gsl::make_span(check2), node_hash, nullptr,
        atapp::etcd_discovery_set::node_hash_type::search_mode::kNextCompactUniqueNode);
    CASE_EXPECT_EQ(find_count2 + 1, find_count);

    for (size_t i = 0; i < find_count2; ++i) {
      CASE_EXPECT_EQ(check2[i].node->get_discovery_info().id(), check1[i + 1].node->get_discovery_info().id());
    }
  }
}

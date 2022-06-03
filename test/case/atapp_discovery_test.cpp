// Copyright 2022 atframework

#include <atframe/etcdcli/etcd_discovery.h>

#include <common/file_system.h>

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
  (*metadata.mutable_annotations())["annotation1"] = "value3";
  (*metadata.mutable_annotations())["annotation2"] = "value4";

  // empty rule
  CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));

  // full match
  rule = metadata;
  CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));

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

  // annotations
  {
    rule.mutable_annotations()->erase("annotation1");
    CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
    (*rule.mutable_annotations())["annotation1"] = "";
    CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
    (*rule.mutable_annotations())["annotation1"] = "mismatch value";
    CASE_EXPECT_FALSE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
    rule.mutable_annotations()->clear();
    CASE_EXPECT_TRUE(etcd_discovery_set::metadata_equal_type::filter(rule, metadata));
  }
}

CASE_TEST(atapp_discovery, get_discovery_by_metadata) {
  using etcd_discovery_set = atapp::etcd_discovery_set;
  using etcd_discovery_node = atapp::etcd_discovery_node;

  etcd_discovery_set::ptr_t discovery_set = std::make_shared<etcd_discovery_set>();
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
  (*discovery_data.mutable_metadata()->mutable_annotations())["annotation1"] = "value3";
  (*discovery_data.mutable_metadata()->mutable_annotations())["annotation2"] = "value4";

  etcd_discovery_node::ptr_t node1 = std::make_shared<etcd_discovery_node>();
  discovery_data.set_id(1);
  discovery_data.set_name("node1");
  discovery_data.set_identity("node-dient-1");
  node1->copy_from(discovery_data);

  etcd_discovery_node::ptr_t node2 = std::make_shared<etcd_discovery_node>();
  discovery_data.set_id(2);
  discovery_data.set_name("node2");
  discovery_data.set_identity("node-dient-2");
  (*discovery_data.mutable_metadata()->mutable_labels())["selector"] = "s2";
  node2->copy_from(discovery_data);

  etcd_discovery_node::ptr_t node3 = std::make_shared<etcd_discovery_node>();
  discovery_data.set_id(3);
  discovery_data.set_name("node3");
  discovery_data.set_identity("node-dient-3");
  (*discovery_data.mutable_metadata()->mutable_labels())["selector"] = "s3";
  node3->copy_from(discovery_data);

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

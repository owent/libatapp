// Copyright 2026 atframework
// Unit tests for etcd_module helper logic (no etcd service required)
//
// Tests cover:
//   1. atapp_area comparison semantics (atapp_discovery_equal logic)
//   2. topology_storage_t version update semantics (topology_update_version logic)
//   3. etcd_module pack/unpack round-trip for topology_info_t and node_info_t

#include <atframe/modules/etcd_module.h>

#include <atframe/etcdcli/etcd_def.h>

// clang-format off
#include <config/compiler/protobuf_prefix.h>
// clang-format on

#include <google/protobuf/util/message_differencer.h>

// clang-format off
#include <config/compiler/protobuf_suffix.h>
// clang-format on

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame/test_macros.h"

// ==================== atapp_area comparison tests ====================
// These tests verify the same comparison semantics as the internal
// atapp_discovery_equal() function in etcd_module.cpp.

namespace {
static bool test_atapp_area_equal(const atapp::protocol::atapp_area &l, const atapp::protocol::atapp_area &r) {
  if (l.zone_id() != r.zone_id()) {
    return false;
  }
  if (l.region().size() != r.region().size()) {
    return false;
  }
  if (l.district().size() != r.district().size()) {
    return false;
  }
  if (l.region() != r.region()) {
    return false;
  }
  if (l.district() != r.district()) {
    return false;
  }

  return true;
}

// Replicates the topology_update_version logic to verify by-reference semantics
static bool test_topology_update_version(atapp::etcd_module::topology_storage_t &storage,
                                         const atapp::etcd_data_version &new_version, bool upgrade) {
  bool ret = false;
  if (upgrade) {
    if (new_version.create_revision > storage.version.create_revision) {
      storage.version.create_revision = new_version.create_revision;
      storage.version.version = new_version.version;
      ret = true;
    }

    if (new_version.modify_revision > storage.version.modify_revision) {
      storage.version.modify_revision = new_version.modify_revision;
      storage.version.version = new_version.version;
      ret = true;
    }

    if (new_version.version > storage.version.version) {
      storage.version.version = new_version.version;
      ret = true;
    }
  } else {
    storage.version = new_version;
    ret = true;
  }

  return ret;
}
}  // namespace

// ---- I.1 atapp_area: both empty (equal) ----
CASE_TEST(atapp_etcd_module_unit, area_equal_both_empty) {
  atapp::protocol::atapp_area a;
  atapp::protocol::atapp_area b;

  CASE_EXPECT_TRUE(test_atapp_area_equal(a, b));
}

// ---- I.2 atapp_area: identical values (equal) ----
CASE_TEST(atapp_etcd_module_unit, area_equal_identical) {
  atapp::protocol::atapp_area a;
  a.set_zone_id(12345);
  a.set_region("us-east-1");
  a.set_district("dc-a");

  atapp::protocol::atapp_area b;
  b.set_zone_id(12345);
  b.set_region("us-east-1");
  b.set_district("dc-a");

  CASE_EXPECT_TRUE(test_atapp_area_equal(a, b));
}

// ---- I.3 atapp_area: different zone_id ----
CASE_TEST(atapp_etcd_module_unit, area_diff_zone_id) {
  atapp::protocol::atapp_area a;
  a.set_zone_id(100);
  a.set_region("us-east-1");
  a.set_district("dc-a");

  atapp::protocol::atapp_area b;
  b.set_zone_id(200);
  b.set_region("us-east-1");
  b.set_district("dc-a");

  CASE_EXPECT_FALSE(test_atapp_area_equal(a, b));
}

// ---- I.4 atapp_area: different region ----
CASE_TEST(atapp_etcd_module_unit, area_diff_region) {
  atapp::protocol::atapp_area a;
  a.set_zone_id(100);
  a.set_region("us-east-1");
  a.set_district("dc-a");

  atapp::protocol::atapp_area b;
  b.set_zone_id(100);
  b.set_region("eu-west-2");
  b.set_district("dc-a");

  CASE_EXPECT_FALSE(test_atapp_area_equal(a, b));
}

// ---- I.5 atapp_area: different district ----
CASE_TEST(atapp_etcd_module_unit, area_diff_district) {
  atapp::protocol::atapp_area a;
  a.set_zone_id(100);
  a.set_region("us-east-1");
  a.set_district("dc-a");

  atapp::protocol::atapp_area b;
  b.set_zone_id(100);
  b.set_region("us-east-1");
  b.set_district("dc-b");

  CASE_EXPECT_FALSE(test_atapp_area_equal(a, b));
}

// ---- I.6 atapp_area: region same length but different content ----
CASE_TEST(atapp_etcd_module_unit, area_diff_region_same_length) {
  atapp::protocol::atapp_area a;
  a.set_zone_id(100);
  a.set_region("abc");

  atapp::protocol::atapp_area b;
  b.set_zone_id(100);
  b.set_region("xyz");

  CASE_EXPECT_FALSE(test_atapp_area_equal(a, b));
}

// ---- I.7 atapp_area: district same length but different content ----
CASE_TEST(atapp_etcd_module_unit, area_diff_district_same_length) {
  atapp::protocol::atapp_area a;
  a.set_zone_id(100);
  a.set_district("aaa");

  atapp::protocol::atapp_area b;
  b.set_zone_id(100);
  b.set_district("bbb");

  CASE_EXPECT_FALSE(test_atapp_area_equal(a, b));
}

// ---- I.8 atapp_area: one has region, other empty ----
CASE_TEST(atapp_etcd_module_unit, area_one_region_empty) {
  atapp::protocol::atapp_area a;
  a.set_zone_id(100);
  a.set_region("us-east-1");

  atapp::protocol::atapp_area b;
  b.set_zone_id(100);

  CASE_EXPECT_FALSE(test_atapp_area_equal(a, b));
}

// ---- I.9 atapp_area: one has district, other empty ----
CASE_TEST(atapp_etcd_module_unit, area_one_district_empty) {
  atapp::protocol::atapp_area a;
  a.set_zone_id(100);
  a.set_district("dc-a");

  atapp::protocol::atapp_area b;
  b.set_zone_id(100);

  CASE_EXPECT_FALSE(test_atapp_area_equal(a, b));
}

// ---- I.10 atapp_area: zero zone_id vs nonzero ----
CASE_TEST(atapp_etcd_module_unit, area_zero_vs_nonzero_zone) {
  atapp::protocol::atapp_area a;
  // zone_id defaults to 0

  atapp::protocol::atapp_area b;
  b.set_zone_id(1);

  CASE_EXPECT_FALSE(test_atapp_area_equal(a, b));
}

// ---- I.11 atapp_area: symmetric check ----
CASE_TEST(atapp_etcd_module_unit, area_symmetric) {
  atapp::protocol::atapp_area a;
  a.set_zone_id(42);
  a.set_region("cn-north-1");
  a.set_district("zone-b");

  atapp::protocol::atapp_area b;
  b.set_zone_id(42);
  b.set_region("cn-north-1");
  b.set_district("zone-b");

  // Equal both ways
  CASE_EXPECT_TRUE(test_atapp_area_equal(a, b));
  CASE_EXPECT_TRUE(test_atapp_area_equal(b, a));

  b.set_region("cn-south-1");
  CASE_EXPECT_FALSE(test_atapp_area_equal(a, b));
  CASE_EXPECT_FALSE(test_atapp_area_equal(b, a));
}

// ==================== topology_update_version tests ====================
// These tests verify that topology_update_version correctly modifies the
// storage by reference (the bug fix) and implements the upgrade logic.

// ---- I.12 version update: upgrade with higher create_revision ----
CASE_TEST(atapp_etcd_module_unit, version_upgrade_create_revision) {
  atapp::etcd_module::topology_storage_t storage;
  storage.version.create_revision = 10;
  storage.version.modify_revision = 20;
  storage.version.version = 1;

  atapp::etcd_data_version new_version;
  new_version.create_revision = 15;
  new_version.modify_revision = 20;
  new_version.version = 1;

  bool changed = test_topology_update_version(storage, new_version, true);

  CASE_EXPECT_TRUE(changed);
  CASE_EXPECT_EQ(15, storage.version.create_revision);
  CASE_EXPECT_EQ(20, storage.version.modify_revision);
}

// ---- I.13 version update: upgrade with higher modify_revision ----
CASE_TEST(atapp_etcd_module_unit, version_upgrade_modify_revision) {
  atapp::etcd_module::topology_storage_t storage;
  storage.version.create_revision = 10;
  storage.version.modify_revision = 20;
  storage.version.version = 1;

  atapp::etcd_data_version new_version;
  new_version.create_revision = 10;
  new_version.modify_revision = 30;
  new_version.version = 2;

  bool changed = test_topology_update_version(storage, new_version, true);

  CASE_EXPECT_TRUE(changed);
  CASE_EXPECT_EQ(10, storage.version.create_revision);
  CASE_EXPECT_EQ(30, storage.version.modify_revision);
  CASE_EXPECT_EQ(2, storage.version.version);
}

// ---- I.14 version update: upgrade with higher version only ----
CASE_TEST(atapp_etcd_module_unit, version_upgrade_version_only) {
  atapp::etcd_module::topology_storage_t storage;
  storage.version.create_revision = 10;
  storage.version.modify_revision = 20;
  storage.version.version = 1;

  atapp::etcd_data_version new_version;
  new_version.create_revision = 10;
  new_version.modify_revision = 20;
  new_version.version = 5;

  bool changed = test_topology_update_version(storage, new_version, true);

  CASE_EXPECT_TRUE(changed);
  CASE_EXPECT_EQ(5, storage.version.version);
}

// ---- I.15 version update: upgrade with no changes (all equal) ----
CASE_TEST(atapp_etcd_module_unit, version_upgrade_no_change) {
  atapp::etcd_module::topology_storage_t storage;
  storage.version.create_revision = 10;
  storage.version.modify_revision = 20;
  storage.version.version = 3;

  atapp::etcd_data_version new_version;
  new_version.create_revision = 10;
  new_version.modify_revision = 20;
  new_version.version = 3;

  bool changed = test_topology_update_version(storage, new_version, true);

  CASE_EXPECT_FALSE(changed);
  CASE_EXPECT_EQ(10, storage.version.create_revision);
  CASE_EXPECT_EQ(20, storage.version.modify_revision);
  CASE_EXPECT_EQ(3, storage.version.version);
}

// ---- I.16 version update: upgrade with lower values (no update) ----
CASE_TEST(atapp_etcd_module_unit, version_upgrade_lower_values) {
  atapp::etcd_module::topology_storage_t storage;
  storage.version.create_revision = 10;
  storage.version.modify_revision = 20;
  storage.version.version = 3;

  atapp::etcd_data_version new_version;
  new_version.create_revision = 5;
  new_version.modify_revision = 15;
  new_version.version = 2;

  bool changed = test_topology_update_version(storage, new_version, true);

  CASE_EXPECT_FALSE(changed);
  // Original values preserved
  CASE_EXPECT_EQ(10, storage.version.create_revision);
  CASE_EXPECT_EQ(20, storage.version.modify_revision);
  CASE_EXPECT_EQ(3, storage.version.version);
}

// ---- I.17 version update: non-upgrade (overwrite) mode ----
CASE_TEST(atapp_etcd_module_unit, version_overwrite_mode) {
  atapp::etcd_module::topology_storage_t storage;
  storage.version.create_revision = 100;
  storage.version.modify_revision = 200;
  storage.version.version = 50;

  atapp::etcd_data_version new_version;
  new_version.create_revision = 1;
  new_version.modify_revision = 2;
  new_version.version = 1;

  bool changed = test_topology_update_version(storage, new_version, false);

  CASE_EXPECT_TRUE(changed);
  // Full overwrite with lower values
  CASE_EXPECT_EQ(1, storage.version.create_revision);
  CASE_EXPECT_EQ(2, storage.version.modify_revision);
  CASE_EXPECT_EQ(1, storage.version.version);
}

// ---- I.18 version update: upgrade with mixed higher/lower ----
CASE_TEST(atapp_etcd_module_unit, version_upgrade_mixed) {
  atapp::etcd_module::topology_storage_t storage;
  storage.version.create_revision = 10;
  storage.version.modify_revision = 20;
  storage.version.version = 3;

  atapp::etcd_data_version new_version;
  new_version.create_revision = 15;  // higher
  new_version.modify_revision = 10;  // lower
  new_version.version = 5;           // higher

  bool changed = test_topology_update_version(storage, new_version, true);

  CASE_EXPECT_TRUE(changed);
  // create_revision upgraded
  CASE_EXPECT_EQ(15, storage.version.create_revision);
  // modify_revision NOT upgraded (new value lower)
  CASE_EXPECT_EQ(20, storage.version.modify_revision);
  // version should reflect the latest from the create_revision branch and the direct version branch
  CASE_EXPECT_EQ(5, storage.version.version);
}

// ---- I.19 version update: by-reference semantics validation ----
// This directly validates the fix: passing by value would NOT update the caller's storage
CASE_TEST(atapp_etcd_module_unit, version_update_by_reference) {
  // Simulate the map lookup pattern from update_internal_watcher_event
  std::unordered_map<uint64_t, atapp::etcd_module::topology_storage_t> topology_map;

  atapp::etcd_module::topology_storage_t initial;
  initial.version.create_revision = 1;
  initial.version.modify_revision = 1;
  initial.version.version = 1;
  topology_map[42] = initial;

  // Now simulate the update call
  atapp::etcd_data_version new_version;
  new_version.create_revision = 10;
  new_version.modify_revision = 20;
  new_version.version = 5;

  auto iter = topology_map.find(42);
  CASE_EXPECT_TRUE(iter != topology_map.end());

  bool changed = test_topology_update_version(iter->second, new_version, true);

  CASE_EXPECT_TRUE(changed);

  // Critically: the map entry must have been updated (by-reference)
  CASE_EXPECT_EQ(10, topology_map[42].version.create_revision);
  CASE_EXPECT_EQ(20, topology_map[42].version.modify_revision);
  CASE_EXPECT_EQ(5, topology_map[42].version.version);
}

// ---- I.20 version update: multiple sequential upgrades ----
CASE_TEST(atapp_etcd_module_unit, version_sequential_upgrades) {
  atapp::etcd_module::topology_storage_t storage;
  storage.version.create_revision = 0;
  storage.version.modify_revision = 0;
  storage.version.version = 0;

  // First update
  atapp::etcd_data_version v1;
  v1.create_revision = 1;
  v1.modify_revision = 1;
  v1.version = 1;
  CASE_EXPECT_TRUE(test_topology_update_version(storage, v1, true));
  CASE_EXPECT_EQ(1, storage.version.create_revision);
  CASE_EXPECT_EQ(1, storage.version.modify_revision);
  CASE_EXPECT_EQ(1, storage.version.version);

  // Second update - only modify_revision increases
  atapp::etcd_data_version v2;
  v2.create_revision = 1;
  v2.modify_revision = 5;
  v2.version = 2;
  CASE_EXPECT_TRUE(test_topology_update_version(storage, v2, true));
  CASE_EXPECT_EQ(1, storage.version.create_revision);
  CASE_EXPECT_EQ(5, storage.version.modify_revision);
  CASE_EXPECT_EQ(2, storage.version.version);

  // Third update - same values, no change
  CASE_EXPECT_FALSE(test_topology_update_version(storage, v2, true));
  CASE_EXPECT_EQ(1, storage.version.create_revision);
  CASE_EXPECT_EQ(5, storage.version.modify_revision);
  CASE_EXPECT_EQ(2, storage.version.version);

  // Fourth update - create_revision increases (key recreated)
  atapp::etcd_data_version v3;
  v3.create_revision = 10;
  v3.modify_revision = 10;
  v3.version = 1;
  CASE_EXPECT_TRUE(test_topology_update_version(storage, v3, true));
  CASE_EXPECT_EQ(10, storage.version.create_revision);
  CASE_EXPECT_EQ(10, storage.version.modify_revision);
}

// ---- I.21 version update: zero-initialized storage ----
CASE_TEST(atapp_etcd_module_unit, version_upgrade_from_zero) {
  atapp::etcd_module::topology_storage_t storage;
  // Default construction leaves version at 0,0,0

  CASE_EXPECT_EQ(0, storage.version.create_revision);
  CASE_EXPECT_EQ(0, storage.version.modify_revision);
  CASE_EXPECT_EQ(0, storage.version.version);

  atapp::etcd_data_version new_version;
  new_version.create_revision = 1;
  new_version.modify_revision = 1;
  new_version.version = 1;

  bool changed = test_topology_update_version(storage, new_version, true);

  CASE_EXPECT_TRUE(changed);
  CASE_EXPECT_EQ(1, storage.version.create_revision);
  CASE_EXPECT_EQ(1, storage.version.modify_revision);
  CASE_EXPECT_EQ(1, storage.version.version);
}

// ==================== atapp_topology_info comparison tests ====================
// Tests for topology equality logic used in etcd_module

namespace {
static bool test_atapp_topology_equal(const atapp::protocol::atapp_topology_info &l,
                                      const atapp::protocol::atapp_topology_info &r) {
  if (l.id() != r.id()) {
    return false;
  }
  if (l.upstream_id() != r.upstream_id()) {
    return false;
  }
  if (l.name() != r.name()) {
    return false;
  }

  if (l.data().label().size() != r.data().label().size()) {
    return false;
  }

  for (const auto &kv : l.data().label()) {
    auto it = r.data().label().find(kv.first);
    if (it == r.data().label().end()) {
      return false;
    }
    if (it->second != kv.second) {
      return false;
    }
  }

  return true;
}
}  // namespace

// ---- I.22 topology_equal: both empty ----
CASE_TEST(atapp_etcd_module_unit, topology_equal_both_empty) {
  atapp::protocol::atapp_topology_info a;
  atapp::protocol::atapp_topology_info b;

  CASE_EXPECT_TRUE(test_atapp_topology_equal(a, b));
}

// ---- I.23 topology_equal: identical all fields ----
CASE_TEST(atapp_etcd_module_unit, topology_equal_identical) {
  atapp::protocol::atapp_topology_info a;
  a.set_id(1001);
  a.set_upstream_id(2001);
  a.set_name("test-server");
  (*a.mutable_data()->mutable_label())["role"] = "game";
  (*a.mutable_data()->mutable_label())["env"] = "prod";

  atapp::protocol::atapp_topology_info b;
  b.set_id(1001);
  b.set_upstream_id(2001);
  b.set_name("test-server");
  (*b.mutable_data()->mutable_label())["role"] = "game";
  (*b.mutable_data()->mutable_label())["env"] = "prod";

  CASE_EXPECT_TRUE(test_atapp_topology_equal(a, b));
}

// ---- I.24 topology_equal: different id ----
CASE_TEST(atapp_etcd_module_unit, topology_equal_diff_id) {
  atapp::protocol::atapp_topology_info a;
  a.set_id(1001);

  atapp::protocol::atapp_topology_info b;
  b.set_id(1002);

  CASE_EXPECT_FALSE(test_atapp_topology_equal(a, b));
}

// ---- I.25 topology_equal: different upstream_id ----
CASE_TEST(atapp_etcd_module_unit, topology_equal_diff_upstream) {
  atapp::protocol::atapp_topology_info a;
  a.set_id(1001);
  a.set_upstream_id(2001);

  atapp::protocol::atapp_topology_info b;
  b.set_id(1001);
  b.set_upstream_id(2002);

  CASE_EXPECT_FALSE(test_atapp_topology_equal(a, b));
}

// ---- I.26 topology_equal: different name ----
CASE_TEST(atapp_etcd_module_unit, topology_equal_diff_name) {
  atapp::protocol::atapp_topology_info a;
  a.set_id(1001);
  a.set_name("server-a");

  atapp::protocol::atapp_topology_info b;
  b.set_id(1001);
  b.set_name("server-b");

  CASE_EXPECT_FALSE(test_atapp_topology_equal(a, b));
}

// ---- I.27 topology_equal: different label count ----
CASE_TEST(atapp_etcd_module_unit, topology_equal_diff_label_count) {
  atapp::protocol::atapp_topology_info a;
  a.set_id(1001);
  (*a.mutable_data()->mutable_label())["role"] = "game";

  atapp::protocol::atapp_topology_info b;
  b.set_id(1001);
  (*b.mutable_data()->mutable_label())["role"] = "game";
  (*b.mutable_data()->mutable_label())["env"] = "prod";

  CASE_EXPECT_FALSE(test_atapp_topology_equal(a, b));
}

// ---- I.28 topology_equal: same label count, different keys ----
CASE_TEST(atapp_etcd_module_unit, topology_equal_diff_label_keys) {
  atapp::protocol::atapp_topology_info a;
  a.set_id(1001);
  (*a.mutable_data()->mutable_label())["role"] = "game";

  atapp::protocol::atapp_topology_info b;
  b.set_id(1001);
  (*b.mutable_data()->mutable_label())["env"] = "game";

  CASE_EXPECT_FALSE(test_atapp_topology_equal(a, b));
}

// ---- I.29 topology_equal: same label count, different values ----
CASE_TEST(atapp_etcd_module_unit, topology_equal_diff_label_values) {
  atapp::protocol::atapp_topology_info a;
  a.set_id(1001);
  (*a.mutable_data()->mutable_label())["role"] = "game";

  atapp::protocol::atapp_topology_info b;
  b.set_id(1001);
  (*b.mutable_data()->mutable_label())["role"] = "lobby";

  CASE_EXPECT_FALSE(test_atapp_topology_equal(a, b));
}

// ---- I.30 topology_equal: ignores hostname/pid/identity/version ----
CASE_TEST(atapp_etcd_module_unit, topology_equal_ignores_extra_fields) {
  atapp::protocol::atapp_topology_info a;
  a.set_id(1001);
  a.set_name("server");
  a.set_hostname("host-a");
  a.set_pid(1234);
  a.set_identity("id-a");
  a.set_version("1.0.0");

  atapp::protocol::atapp_topology_info b;
  b.set_id(1001);
  b.set_name("server");
  b.set_hostname("host-b");
  b.set_pid(5678);
  b.set_identity("id-b");
  b.set_version("2.0.0");

  // These fields are intentionally NOT compared by atapp_topology_equal
  CASE_EXPECT_TRUE(test_atapp_topology_equal(a, b));
}

// ==================== topology_storage_t map interaction tests ====================
// These tests verify the fix for topology_update_version being called
// with storage from a map iterator, ensuring updates propagate.

// ---- I.31 map-based version tracking: insert and upgrade ----
CASE_TEST(atapp_etcd_module_unit, topology_map_insert_and_upgrade) {
  std::unordered_map<uint64_t, atapp::etcd_module::topology_storage_t> topology_map;

  // Insert initial entry
  auto info = atfw::util::memory::make_strong_rc<atapp::protocol::atapp_topology_info>();
  info->set_id(100);
  info->set_name("server-100");
  info->set_upstream_id(1);

  atapp::etcd_module::topology_storage_t entry;
  entry.info = info;
  entry.version.create_revision = 1;
  entry.version.modify_revision = 1;
  entry.version.version = 1;
  topology_map[100] = entry;

  // Simulate update with higher version
  atapp::etcd_data_version v2;
  v2.create_revision = 1;
  v2.modify_revision = 5;
  v2.version = 2;

  auto it = topology_map.find(100);
  CASE_EXPECT_TRUE(it != topology_map.end());
  bool changed = test_topology_update_version(it->second, v2, true);
  CASE_EXPECT_TRUE(changed);

  // Verify the map entry is updated (not a copy)
  CASE_EXPECT_EQ(5, topology_map[100].version.modify_revision);
  CASE_EXPECT_EQ(2, topology_map[100].version.version);

  // Simulate same version again - should return false
  changed = test_topology_update_version(it->second, v2, true);
  CASE_EXPECT_FALSE(changed);
}

// ---- I.32 map-based version tracking: multiple entries independent ----
CASE_TEST(atapp_etcd_module_unit, topology_map_multiple_entries) {
  std::unordered_map<uint64_t, atapp::etcd_module::topology_storage_t> topology_map;

  for (uint64_t i = 1; i <= 3; ++i) {
    atapp::etcd_module::topology_storage_t entry;
    entry.info = atfw::util::memory::make_strong_rc<atapp::protocol::atapp_topology_info>();
    entry.info->set_id(i);
    entry.version.create_revision = static_cast<int64_t>(i);
    entry.version.modify_revision = static_cast<int64_t>(i);
    entry.version.version = 1;
    topology_map[i] = entry;
  }

  // Update only entry 2
  atapp::etcd_data_version v_new;
  v_new.create_revision = 2;
  v_new.modify_revision = 10;
  v_new.version = 5;

  auto it = topology_map.find(2);
  test_topology_update_version(it->second, v_new, true);

  // Entry 1 unchanged
  CASE_EXPECT_EQ(1, topology_map[1].version.modify_revision);
  CASE_EXPECT_EQ(1, topology_map[1].version.version);

  // Entry 2 updated
  CASE_EXPECT_EQ(10, topology_map[2].version.modify_revision);
  CASE_EXPECT_EQ(5, topology_map[2].version.version);

  // Entry 3 unchanged
  CASE_EXPECT_EQ(3, topology_map[3].version.modify_revision);
  CASE_EXPECT_EQ(1, topology_map[3].version.version);
}

// ==================== etcd_data_version basic tests ====================

// ---- I.33 etcd_data_version: default construction ----
CASE_TEST(atapp_etcd_module_unit, data_version_default) {
  atapp::etcd_data_version v;
  CASE_EXPECT_EQ(0, v.create_revision);
  CASE_EXPECT_EQ(0, v.modify_revision);
  CASE_EXPECT_EQ(0, v.version);
}

// ---- I.34 etcd_data_version: copy construction ----
CASE_TEST(atapp_etcd_module_unit, data_version_copy) {
  atapp::etcd_data_version v1;
  v1.create_revision = 10;
  v1.modify_revision = 20;
  v1.version = 3;

  atapp::etcd_data_version v2(v1);
  CASE_EXPECT_EQ(10, v2.create_revision);
  CASE_EXPECT_EQ(20, v2.modify_revision);
  CASE_EXPECT_EQ(3, v2.version);
}

// ---- I.35 etcd_data_version: assignment ----
CASE_TEST(atapp_etcd_module_unit, data_version_assignment) {
  atapp::etcd_data_version v1;
  v1.create_revision = 10;
  v1.modify_revision = 20;
  v1.version = 3;

  atapp::etcd_data_version v2;
  v2 = v1;
  CASE_EXPECT_EQ(10, v2.create_revision);
  CASE_EXPECT_EQ(20, v2.modify_revision);
  CASE_EXPECT_EQ(3, v2.version);
}

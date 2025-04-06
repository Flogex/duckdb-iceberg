
#include "rest_catalog/objects/remove_snapshots_update.hpp"

#include "yyjson.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "rest_catalog/response_objects.hpp"
#include "rest_catalog/objects/list.hpp"

using namespace duckdb_yyjson;

namespace duckdb {
namespace rest_api_objects {

RemoveSnapshotsUpdate::RemoveSnapshotsUpdate() {
}

RemoveSnapshotsUpdate RemoveSnapshotsUpdate::FromJSON(yyjson_val *obj) {
	RemoveSnapshotsUpdate res;
	auto error = res.TryFromJSON(obj);
	if (!error.empty()) {
		throw InvalidInputException(error);
	}
	return res;
}

string RemoveSnapshotsUpdate::TryFromJSON(yyjson_val *obj) {
	string error;
	error = base_update.TryFromJSON(obj);
	if (!error.empty()) {
		return error;
	}
	auto snapshot_ids_val = yyjson_obj_get(obj, "snapshot_ids");
	if (!snapshot_ids_val) {
		return "RemoveSnapshotsUpdate required property 'snapshot_ids' is missing";
	} else {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(snapshot_ids_val, idx, max, val) {
			auto tmp = yyjson_get_sint(val);
			snapshot_ids.emplace_back(std::move(tmp));
		}
	}
	auto action_val = yyjson_obj_get(obj, "action");
	if (action_val) {
		action = yyjson_get_str(action_val);
	}
	return string();
}

} // namespace rest_api_objects
} // namespace duckdb

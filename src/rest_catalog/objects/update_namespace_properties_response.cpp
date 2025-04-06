
#include "rest_catalog/objects/update_namespace_properties_response.hpp"

#include "yyjson.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "rest_catalog/response_objects.hpp"
#include "rest_catalog/objects/list.hpp"

using namespace duckdb_yyjson;

namespace duckdb {
namespace rest_api_objects {

UpdateNamespacePropertiesResponse::UpdateNamespacePropertiesResponse() {
}

UpdateNamespacePropertiesResponse UpdateNamespacePropertiesResponse::FromJSON(yyjson_val *obj) {
	UpdateNamespacePropertiesResponse res;
	auto error = res.TryFromJSON(obj);
	if (!error.empty()) {
		throw InvalidInputException(error);
	}
	return res;
}

string UpdateNamespacePropertiesResponse::TryFromJSON(yyjson_val *obj) {
	string error;
	auto updated_val = yyjson_obj_get(obj, "updated");
	if (!updated_val) {
		return "UpdateNamespacePropertiesResponse required property 'updated' is missing";
	} else {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(updated_val, idx, max, val) {
			auto tmp = yyjson_get_str(val);
			updated.emplace_back(std::move(tmp));
		}
	}
	auto removed_val = yyjson_obj_get(obj, "removed");
	if (!removed_val) {
		return "UpdateNamespacePropertiesResponse required property 'removed' is missing";
	} else {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(removed_val, idx, max, val) {
			auto tmp = yyjson_get_str(val);
			removed.emplace_back(std::move(tmp));
		}
	}
	auto missing_val = yyjson_obj_get(obj, "missing");
	if (missing_val) {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(missing_val, idx, max, val) {
			auto tmp = yyjson_get_str(val);
			missing.emplace_back(std::move(tmp));
		}
	}
	return string();
}

} // namespace rest_api_objects
} // namespace duckdb

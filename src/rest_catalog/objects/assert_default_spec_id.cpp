
#include "rest_catalog/objects/assert_default_spec_id.hpp"

#include "yyjson.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "rest_catalog/response_objects.hpp"
#include "rest_catalog/objects/list.hpp"

using namespace duckdb_yyjson;

namespace duckdb {
namespace rest_api_objects {

AssertDefaultSpecId::AssertDefaultSpecId() {
}

AssertDefaultSpecId AssertDefaultSpecId::FromJSON(yyjson_val *obj) {
	AssertDefaultSpecId res;
	auto error = res.TryFromJSON(obj);
	if (!error.empty()) {
		throw InvalidInputException(error);
	}
	return res;
}

string AssertDefaultSpecId::TryFromJSON(yyjson_val *obj) {
	string error;
	error = table_requirement.TryFromJSON(obj);
	if (!error.empty()) {
		return error;
	}
	auto default_spec_id_val = yyjson_obj_get(obj, "default-spec-id");
	if (!default_spec_id_val) {
		return "AssertDefaultSpecId required property 'default-spec-id' is missing";
	} else {
		if (yyjson_is_sint(default_spec_id_val)) {
			default_spec_id = yyjson_get_sint(default_spec_id_val);
		} else {
			return "AssertDefaultSpecId property 'default_spec_id' is not of type 'integer'";
		}
	}
	auto type_val = yyjson_obj_get(obj, "type");
	if (type_val) {
		has_type = true;
		if (yyjson_is_str(type_val)) {
			type = yyjson_get_str(type_val);
		} else {
			return "AssertDefaultSpecId property 'type' is not of type 'string'";
		}
	}
	return string();
}

} // namespace rest_api_objects
} // namespace duckdb

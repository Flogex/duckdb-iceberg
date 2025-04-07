
#include "rest_catalog/objects/struct_field.hpp"

#include "yyjson.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "rest_catalog/response_objects.hpp"
#include "rest_catalog/objects/list.hpp"

using namespace duckdb_yyjson;

namespace duckdb {
namespace rest_api_objects {

StructField::StructField() {
}

StructField StructField::FromJSON(yyjson_val *obj) {
	StructField res;
	auto error = res.TryFromJSON(obj);
	if (!error.empty()) {
		throw InvalidInputException(error);
	}
	return res;
}

string StructField::TryFromJSON(yyjson_val *obj) {
	string error;
	auto id_val = yyjson_obj_get(obj, "id");
	if (!id_val) {
		return "StructField required property 'id' is missing";
	} else {
		id = yyjson_get_sint(id_val);
	}
	auto name_val = yyjson_obj_get(obj, "name");
	if (!name_val) {
		return "StructField required property 'name' is missing";
	} else {
		name = yyjson_get_str(name_val);
	}
	auto type_val = yyjson_obj_get(obj, "type");
	if (!type_val) {
		return "StructField required property 'type' is missing";
	} else {
		type = make_uniq<Type>();
		error = type->TryFromJSON(type_val);
		if (!error.empty()) {
			return error;
		}
	}
	auto required_val = yyjson_obj_get(obj, "required");
	if (!required_val) {
		return "StructField required property 'required' is missing";
	} else {
		required = yyjson_get_bool(required_val);
	}
	auto doc_val = yyjson_obj_get(obj, "doc");
	if (doc_val) {
		doc = yyjson_get_str(doc_val);
	}
	auto initial_default_val = yyjson_obj_get(obj, "initial-default");
	if (initial_default_val) {
		error = initial_default.TryFromJSON(initial_default_val);
		if (!error.empty()) {
			return error;
		}
	}
	auto write_default_val = yyjson_obj_get(obj, "write-default");
	if (write_default_val) {
		error = write_default.TryFromJSON(write_default_val);
		if (!error.empty()) {
			return error;
		}
	}
	return string();
}

} // namespace rest_api_objects
} // namespace duckdb

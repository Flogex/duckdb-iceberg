
#include "rest_catalog/objects/double_type_value.hpp"

#include "yyjson.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "rest_catalog/response_objects.hpp"
#include "rest_catalog/objects/list.hpp"

using namespace duckdb_yyjson;

namespace duckdb {
namespace rest_api_objects {

DoubleTypeValue::DoubleTypeValue() {
}

DoubleTypeValue DoubleTypeValue::FromJSON(yyjson_val *obj) {
	DoubleTypeValue res;
	auto error = res.TryFromJSON(obj);
	if (!error.empty()) {
		throw InvalidInputException(error);
	}
	return res;
}

string DoubleTypeValue::TryFromJSON(yyjson_val *obj) {
	string error;
	if (yyjson_is_num(obj)) {
		value = yyjson_get_num(obj);
	} else {
		return "DoubleTypeValue property 'value' is not of type 'number'";
	}
	return string();
}

} // namespace rest_api_objects
} // namespace duckdb

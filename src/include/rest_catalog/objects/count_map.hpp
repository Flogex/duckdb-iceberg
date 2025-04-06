
#pragma once

#include "yyjson.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "rest_catalog/response_objects.hpp"
#include "rest_catalog/objects/integer_type_value.hpp"
#include "rest_catalog/objects/long_type_value.hpp"

using namespace duckdb_yyjson;

namespace duckdb {
namespace rest_api_objects {

class CountMap {
public:
	CountMap() {
	}

public:
	static CountMap FromJSON(yyjson_val *obj) {
		CountMap res;
		auto error = res.TryFromJSON(obj);
		if (!error.empty()) {
			throw InvalidInputException(error);
		}
		return res;
	}

public:
	string TryFromJSON(yyjson_val *obj) {
		string error;
		auto keys_val = yyjson_obj_get(obj, "keys");
		if (keys_val) {
			size_t idx, max;
			yyjson_val *val;
			yyjson_arr_foreach(keys_val, idx, max, val) {
				IntegerTypeValue tmp;
				error = tmp.TryFromJSON(val);
				if (!error.empty()) {
					return error;
				}
				keys.push_back(tmp);
			}
		}
		auto values_val = yyjson_obj_get(obj, "values");
		if (values_val) {
			size_t idx, max;
			yyjson_val *val;
			yyjson_arr_foreach(values_val, idx, max, val) {
				LongTypeValue tmp;
				error = tmp.TryFromJSON(val);
				if (!error.empty()) {
					return error;
				}
				values.push_back(tmp);
			}
		}
		return string();
	}

public:
	vector<IntegerTypeValue> keys;
	vector<LongTypeValue> values;
};

} // namespace rest_api_objects
} // namespace duckdb

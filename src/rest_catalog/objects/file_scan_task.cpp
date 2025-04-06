
#include "rest_catalog/objects/file_scan_task.hpp"

#include "yyjson.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "rest_catalog/response_objects.hpp"
#include "rest_catalog/objects/list.hpp"

using namespace duckdb_yyjson;

namespace duckdb {
namespace rest_api_objects {

FileScanTask::FileScanTask() {
}

FileScanTask FileScanTask::FromJSON(yyjson_val *obj) {
	FileScanTask res;
	auto error = res.TryFromJSON(obj);
	if (!error.empty()) {
		throw InvalidInputException(error);
	}
	return res;
}

string FileScanTask::TryFromJSON(yyjson_val *obj) {
	string error;
	auto data_file_val = yyjson_obj_get(obj, "data_file");
	if (!data_file_val) {
		return "FileScanTask required property 'data_file' is missing";
	} else {
		error = data_file.TryFromJSON(data_file_val);
		if (!error.empty()) {
			return error;
		}
	}
	auto delete_file_references_val = yyjson_obj_get(obj, "delete_file_references");
	if (delete_file_references_val) {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(delete_file_references_val, idx, max, val) {
			auto tmp = yyjson_get_sint(val);
			delete_file_references.emplace_back(std::move(tmp));
		}
	}
	auto residual_filter_val = yyjson_obj_get(obj, "residual_filter");
	if (residual_filter_val) {
		residual_filter = make_uniq<Expression>();
		error = residual_filter->TryFromJSON(residual_filter_val);
		if (!error.empty()) {
			return error;
		}
	}
	return string();
}

} // namespace rest_api_objects
} // namespace duckdb

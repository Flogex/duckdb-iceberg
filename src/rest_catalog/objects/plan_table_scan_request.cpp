
#include "rest_catalog/objects/plan_table_scan_request.hpp"

#include "yyjson.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "rest_catalog/response_objects.hpp"
#include "rest_catalog/objects/list.hpp"

using namespace duckdb_yyjson;

namespace duckdb {
namespace rest_api_objects {

PlanTableScanRequest::PlanTableScanRequest() {
}

PlanTableScanRequest PlanTableScanRequest::FromJSON(yyjson_val *obj) {
	PlanTableScanRequest res;
	auto error = res.TryFromJSON(obj);
	if (!error.empty()) {
		throw InvalidInputException(error);
	}
	return res;
}

string PlanTableScanRequest::TryFromJSON(yyjson_val *obj) {
	string error;
	auto snapshot_id_val = yyjson_obj_get(obj, "snapshot_id");
	if (snapshot_id_val) {
		snapshot_id = yyjson_get_sint(snapshot_id_val);
	}
	auto select_val = yyjson_obj_get(obj, "select");
	if (select_val) {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(select_val, idx, max, val) {
			FieldName tmp;
			error = tmp.TryFromJSON(val);
			if (!error.empty()) {
				return error;
			}
			select.emplace_back(std::move(tmp));
		}
	}
	auto filter_val = yyjson_obj_get(obj, "filter");
	if (filter_val) {
		filter = make_uniq<Expression>();
		error = filter->TryFromJSON(filter_val);
		if (!error.empty()) {
			return error;
		}
	}
	auto case_sensitive_val = yyjson_obj_get(obj, "case_sensitive");
	if (case_sensitive_val) {
		case_sensitive = yyjson_get_bool(case_sensitive_val);
	}
	auto use_snapshot_schema_val = yyjson_obj_get(obj, "use_snapshot_schema");
	if (use_snapshot_schema_val) {
		use_snapshot_schema = yyjson_get_bool(use_snapshot_schema_val);
	}
	auto start_snapshot_id_val = yyjson_obj_get(obj, "start_snapshot_id");
	if (start_snapshot_id_val) {
		start_snapshot_id = yyjson_get_sint(start_snapshot_id_val);
	}
	auto end_snapshot_id_val = yyjson_obj_get(obj, "end_snapshot_id");
	if (end_snapshot_id_val) {
		end_snapshot_id = yyjson_get_sint(end_snapshot_id_val);
	}
	auto stats_fields_val = yyjson_obj_get(obj, "stats_fields");
	if (stats_fields_val) {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(stats_fields_val, idx, max, val) {
			FieldName tmp;
			error = tmp.TryFromJSON(val);
			if (!error.empty()) {
				return error;
			}
			stats_fields.emplace_back(std::move(tmp));
		}
	}
	return string();
}

} // namespace rest_api_objects
} // namespace duckdb

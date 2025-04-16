#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/common/enums/joinref_type.hpp"
#include "duckdb/common/enums/tableref_type.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_system.hpp"
#include "iceberg_metadata.hpp"
#include "iceberg_functions.hpp"
#include "iceberg_utils.hpp"
#include "yyjson.hpp"

#include <string>
#include <numeric>

namespace duckdb {

struct IcebergMetaDataBindData : public TableFunctionData {
	unique_ptr<IcebergTable> iceberg_table;
};

struct IcebergMetaDataGlobalTableFunctionState : public GlobalTableFunctionState {
public:
	IcebergMetaDataGlobalTableFunctionState() {

	};

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
		return make_uniq<IcebergMetaDataGlobalTableFunctionState>();
	}

	idx_t current_manifest_idx = 0;
	idx_t current_manifest_entry_idx = 0;
};

static unique_ptr<FunctionData> IcebergMetaDataBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	// return a TableRef that contains the scans for the
	auto ret = make_uniq<IcebergMetaDataBindData>();

	FileSystem &fs = FileSystem::GetFileSystem(context);
	auto iceberg_path = input.inputs[0].ToString();

	IcebergOptions options;

	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		auto &val = kv.second;
		if (loption == "allow_moved_paths") {
			options.allow_moved_paths = BooleanValue::Get(val);
		} else if (loption == "metadata_compression_codec") {
			options.metadata_compression_codec = StringValue::Get(val);
		} else if (loption == "skip_schema_inference") {
			options.skip_schema_inference = BooleanValue::Get(val);
		} else if (loption == "version") {
			options.table_version = StringValue::Get(val);
		} else if (loption == "version_name_format") {
			auto value = StringValue::Get(kv.second);
			auto string_substitutions = IcebergUtils::CountOccurrences(value, "%s");
			if (string_substitutions != 2) {
				throw InvalidInputException(
				    "'version_name_format' has to contain two occurrences of '%s' in it, found %d", "%s",
				    string_substitutions);
			}
			options.version_name_format = value;
		} else if (loption == "snapshot_from_id") {
			if (options.snapshot_source != SnapshotSource::LATEST) {
				throw InvalidInputException(
				    "Can't use 'snapshot_from_id' in combination with 'snapshot_from_timestamp'");
			}
			options.snapshot_source = SnapshotSource::FROM_ID;
			options.snapshot_id = val.GetValue<uint64_t>();
		} else if (loption == "snapshot_from_timestamp") {
			if (options.snapshot_source != SnapshotSource::LATEST) {
				throw InvalidInputException(
				    "Can't use 'snapshot_from_id' in combination with 'snapshot_from_timestamp'");
			}
			options.snapshot_source = SnapshotSource::FROM_TIMESTAMP;
			options.snapshot_timestamp = val.GetValue<timestamp_t>();
		}
	}

	auto iceberg_meta_path = IcebergSnapshot::GetMetaDataPath(context, iceberg_path, fs, options);
	auto parse_info = IcebergMetadata::Parse(iceberg_meta_path, fs, options.metadata_compression_codec);
	IcebergSnapshot snapshot_to_scan;
	switch (options.snapshot_source) {
	case SnapshotSource::LATEST: {
		snapshot_to_scan = IcebergSnapshot::GetLatestSnapshot(*parse_info, options);
		break;
	}
	case SnapshotSource::FROM_ID: {
		snapshot_to_scan = IcebergSnapshot::GetSnapshotById(*parse_info, options.snapshot_id, options);
		break;
	}
	case SnapshotSource::FROM_TIMESTAMP: {
		snapshot_to_scan = IcebergSnapshot::GetSnapshotByTimestamp(*parse_info, options.snapshot_timestamp, options);
		break;
	}
	default:
		throw InternalException("SnapshotSource type not implemented");
	}

	ret->iceberg_table = make_uniq<IcebergTable>(IcebergTable::Load(iceberg_path, snapshot_to_scan, context, options));

	auto manifest_types = IcebergManifest::Types();
	return_types.insert(return_types.end(), manifest_types.begin(), manifest_types.end());
	auto manifest_entry_types = IcebergManifestEntry::Types();
	return_types.insert(return_types.end(), manifest_entry_types.begin(), manifest_entry_types.end());

	auto manifest_names = IcebergManifest::Names();
	names.insert(names.end(), manifest_names.begin(), manifest_names.end());
	auto manifest_entry_names = IcebergManifestEntry::Names();
	names.insert(names.end(), manifest_entry_names.begin(), manifest_entry_names.end());

	return std::move(ret);
}

static void IcebergMetaDataFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<IcebergMetaDataBindData>();
	auto &global_state = data.global_state->Cast<IcebergMetaDataGlobalTableFunctionState>();

	idx_t out = 0;
	auto manifests = bind_data.iceberg_table->entries;
	for (; global_state.current_manifest_idx < manifests.size(); global_state.current_manifest_idx++) {
		auto manifest_entries = manifests[global_state.current_manifest_idx].manifest_entries;
		for (; global_state.current_manifest_entry_idx < manifest_entries.size();
		     global_state.current_manifest_entry_idx++) {
			if (out >= STANDARD_VECTOR_SIZE) {
				output.SetCardinality(out);
				return;
			}
			auto manifest = manifests[global_state.current_manifest_idx];
			auto manifest_entry = manifest_entries[global_state.current_manifest_entry_idx];

			FlatVector::GetData<string_t>(output.data[0])[out] =
			    StringVector::AddString(output.data[0], string_t(manifest.manifest.manifest_path));
			;
			FlatVector::GetData<int64_t>(output.data[1])[out] = manifest.manifest.sequence_number;
			FlatVector::GetData<string_t>(output.data[2])[out] = StringVector::AddString(
			    output.data[2], string_t(IcebergManifestContentTypeToString(manifest.manifest.content)));

			FlatVector::GetData<string_t>(output.data[3])[out] = StringVector::AddString(
			    output.data[3], string_t(IcebergManifestEntryStatusTypeToString(manifest_entry.status)));
			FlatVector::GetData<string_t>(output.data[4])[out] = StringVector::AddString(
			    output.data[4], string_t(IcebergManifestEntryContentTypeToString(manifest_entry.content)));
			FlatVector::GetData<string_t>(output.data[5])[out] =
			    StringVector::AddString(output.data[5], string_t(manifest_entry.file_path));
			FlatVector::GetData<string_t>(output.data[6])[out] =
			    StringVector::AddString(output.data[6], string_t(manifest_entry.file_format));
			FlatVector::GetData<int64_t>(output.data[7])[out] = manifest_entry.record_count;
			out++;
		}
		global_state.current_manifest_entry_idx = 0;
	}
	output.SetCardinality(out);
}

TableFunctionSet IcebergFunctions::GetIcebergMetadataFunction() {
	TableFunctionSet function_set("iceberg_metadata");

	auto fun = TableFunction({LogicalType::VARCHAR}, IcebergMetaDataFunction, IcebergMetaDataBind,
	                         IcebergMetaDataGlobalTableFunctionState::Init);
	fun.named_parameters["allow_moved_paths"] = LogicalType::BOOLEAN;
	fun.named_parameters["skip_schema_inference"] = LogicalType::BOOLEAN;
	fun.named_parameters["metadata_compression_codec"] = LogicalType::VARCHAR;
	fun.named_parameters["version"] = LogicalType::VARCHAR;
	fun.named_parameters["version_name_format"] = LogicalType::VARCHAR;
	function_set.AddFunction(fun);

	fun = TableFunction({LogicalType::VARCHAR, LogicalType::UBIGINT}, IcebergMetaDataFunction, IcebergMetaDataBind,
	                    IcebergMetaDataGlobalTableFunctionState::Init);
	fun.named_parameters["allow_moved_paths"] = LogicalType::BOOLEAN;
	fun.named_parameters["skip_schema_inference"] = LogicalType::BOOLEAN;
	fun.named_parameters["metadata_compression_codec"] = LogicalType::VARCHAR;
	fun.named_parameters["version"] = LogicalType::VARCHAR;
	fun.named_parameters["version_name_format"] = LogicalType::VARCHAR;
	function_set.AddFunction(fun);

	fun = TableFunction({LogicalType::VARCHAR, LogicalType::TIMESTAMP}, IcebergMetaDataFunction, IcebergMetaDataBind,
	                    IcebergMetaDataGlobalTableFunctionState::Init);
	fun.named_parameters["allow_moved_paths"] = LogicalType::BOOLEAN;
	fun.named_parameters["skip_schema_inference"] = LogicalType::BOOLEAN;
	fun.named_parameters["metadata_compression_codec"] = LogicalType::VARCHAR;
	fun.named_parameters["version"] = LogicalType::VARCHAR;
	fun.named_parameters["version_name_format"] = LogicalType::VARCHAR;
	function_set.AddFunction(fun);

	return function_set;
}

} // namespace duckdb

#include "iceberg_manifest.hpp"

//! Iceberg Manifest scan routines

namespace duckdb {

static void ManifestNameMapping(idx_t column_id, const LogicalType &type, const string &name, case_insensitive_map_t<ColumnIndex> &name_to_vec) {
	name_to_vec[name] = ColumnIndex(column_id);
}

void IcebergManifestV1::ProduceEntry(DataChunk &input, idx_t result_offset, const case_insensitive_map_t<ColumnIndex> &name_to_vec, entry_type &result) {
	auto manifest_path = FlatVector::GetData<string_t>(input.data[name_to_vec.at("manifest_path").GetPrimaryIndex()]);

	result.manifest_path = manifest_path[result_offset].GetString();
	result.content = IcebergManifestContentType::DATA;
	result.sequence_number = 0;
}

bool IcebergManifestV1::VerifySchema(const case_insensitive_map_t<ColumnIndex> &name_to_vec) {
	if (!name_to_vec.count("manifest_path")) {
		return false;
	}
	return true;
}

void IcebergManifestV1::PopulateNameMapping(idx_t column_id, LogicalType &type, const string &name, case_insensitive_map_t<ColumnIndex> &name_to_vec) {
	ManifestNameMapping(column_id, type, name, name_to_vec);
}

void IcebergManifestV2::ProduceEntry(DataChunk &input, idx_t result_offset, const case_insensitive_map_t<ColumnIndex> &name_to_vec, entry_type &result) {
	auto manifest_path = FlatVector::GetData<string_t>(input.data[name_to_vec.at("manifest_path").GetPrimaryIndex()]);
	auto content = FlatVector::GetData<int32_t>(input.data[name_to_vec.at("content").GetPrimaryIndex()]);
	auto sequence_number = FlatVector::GetData<int64_t>(input.data[name_to_vec.at("sequence_number").GetPrimaryIndex()]);

	result.manifest_path = manifest_path[result_offset].GetString();
	result.content = IcebergManifestContentType(content[result_offset]);
	result.sequence_number = sequence_number[result_offset];
}

bool IcebergManifestV2::VerifySchema(const case_insensitive_map_t<ColumnIndex> &name_to_vec) {
	if (!IcebergManifestV1::VerifySchema(name_to_vec)) {
		return false;
	}
	if (!name_to_vec.count("content")) {
		return false;
	}
	return true;
}

void IcebergManifestV2::PopulateNameMapping(idx_t column_id, LogicalType &type, const string &name, case_insensitive_map_t<ColumnIndex> &name_to_vec) {
	ManifestNameMapping(column_id, type, name, name_to_vec);
}

//! Iceberg Manifest Entry scan routines

static void EntryNameMapping(idx_t column_id, const LogicalType &type, const string &name, case_insensitive_map_t<ColumnIndex> &name_to_vec) {
	auto lname = StringUtil::Lower(name);
	if (lname != "data_file") {
		name_to_vec[lname] = ColumnIndex(column_id);
		return;
	}

	if (type.id() != LogicalTypeId::STRUCT) {
		throw InvalidInputException("The 'data_file' of the manifest should be a STRUCT");
	}
	auto &children = StructType::GetChildTypes(type);
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		auto &child = children[child_idx];
		auto child_name = StringUtil::Lower(child.first);

		name_to_vec[child_name] = ColumnIndex(column_id, {ColumnIndex(child_idx)});
	}
}

void IcebergManifestEntryV1::ProduceEntry(DataChunk &input, idx_t result_offset, const case_insensitive_map_t<ColumnIndex> &name_to_vec, entry_type &result) {
	auto status = FlatVector::GetData<int32_t>(input.data[name_to_vec.at("status").GetPrimaryIndex()]);

	auto file_path_idx = name_to_vec.at("file_path");
	auto data_file_idx = file_path_idx.GetPrimaryIndex();
	auto &child_entries = StructVector::GetEntries(input.data[data_file_idx]);
	D_ASSERT(name_to_vec.at("file_format").GetPrimaryIndex());
	D_ASSERT(name_to_vec.at("record_count").GetPrimaryIndex());

	auto file_path = FlatVector::GetData<string_t>(*child_entries[file_path_idx.GetChildIndex(0).GetPrimaryIndex()]);
	auto file_format = FlatVector::GetData<string_t>(*child_entries[name_to_vec.at("file_format").GetChildIndex(0).GetPrimaryIndex()]);
	auto record_count = FlatVector::GetData<int64_t>(*child_entries[name_to_vec.at("record_count").GetChildIndex(0).GetPrimaryIndex()]);

	result.status = (IcebergManifestEntryStatusType)status[result_offset];
	result.content = IcebergManifestEntryContentType::DATA;
	result.file_path = file_path[result_offset].GetString();
	result.file_format = file_format[result_offset].GetString();
	result.record_count = record_count[result_offset];
}

bool IcebergManifestEntryV1::VerifySchema(const case_insensitive_map_t<ColumnIndex> &name_to_vec) {
	if (!name_to_vec.count("status")) {
		return false;
	}
	if (!name_to_vec.count("file_path")) {
		return false;
	}
	if (!name_to_vec.count("file_format")) {
		return false;
	}
	if (!name_to_vec.count("record_count")) {
		return false;
	}
	return true;
}

void IcebergManifestEntryV1::PopulateNameMapping(idx_t column_id, LogicalType &type, const string &name, case_insensitive_map_t<ColumnIndex> &name_to_vec) {
	EntryNameMapping(column_id, type, name, name_to_vec);
}

void IcebergManifestEntryV2::ProduceEntry(DataChunk &input, idx_t result_offset, const case_insensitive_map_t<ColumnIndex> &name_to_vec, entry_type &result) {
	auto status = FlatVector::GetData<int32_t>(input.data[name_to_vec.at("status").GetPrimaryIndex()]);

	auto file_path_idx = name_to_vec.at("file_path");
	auto data_file_idx = file_path_idx.GetPrimaryIndex();
	auto &child_entries = StructVector::GetEntries(input.data[data_file_idx]);
	D_ASSERT(name_to_vec.at("file_format").GetPrimaryIndex() == data_file_idx);
	D_ASSERT(name_to_vec.at("record_count").GetPrimaryIndex() == data_file_idx);
	D_ASSERT(name_to_vec.at("content").GetPrimaryIndex() == data_file_idx);

	auto content = FlatVector::GetData<int32_t>(*child_entries[name_to_vec.at("content").GetChildIndex(0).GetPrimaryIndex()]);
	auto file_path = FlatVector::GetData<string_t>(*child_entries[file_path_idx.GetChildIndex(0).GetPrimaryIndex()]);
	auto file_format = FlatVector::GetData<string_t>(*child_entries[name_to_vec.at("file_format").GetChildIndex(0).GetPrimaryIndex()]);
	auto record_count = FlatVector::GetData<int64_t>(*child_entries[name_to_vec.at("record_count").GetChildIndex(0).GetPrimaryIndex()]);

	result.status = (IcebergManifestEntryStatusType)status[result_offset];
	result.content = (IcebergManifestEntryContentType)content[result_offset];
	result.file_path = file_path[result_offset].GetString();
	result.file_format = file_format[result_offset].GetString();
	result.record_count = record_count[result_offset];
}

bool IcebergManifestEntryV2::VerifySchema(const case_insensitive_map_t<ColumnIndex> &name_to_vec) {
	if (!IcebergManifestEntryV1::VerifySchema(name_to_vec)) {
		return false;
	}
	if (!name_to_vec.count("content")) {
		return false;
	}
	return true;
}

void IcebergManifestEntryV2::PopulateNameMapping(idx_t column_id, LogicalType &type, const string &name, case_insensitive_map_t<ColumnIndex> &name_to_vec) {
	EntryNameMapping(column_id, type, name, name_to_vec);
}

AvroScan::AvroScan(const string &scan_name, ClientContext &context, const string &path) {
	auto &instance = DatabaseInstance::GetDatabase(context);
	auto &avro_scan_entry = ExtensionUtil::GetTableFunction(instance, "read_avro");
	avro_scan = avro_scan_entry.functions.functions[0];

	// Prepare the inputs for the bind
	vector<Value> children;
	children.reserve(1);
	children.push_back(Value(path));
	named_parameter_map_t named_params;
	vector<LogicalType> input_types;
	vector<string> input_names;

	TableFunctionRef empty;
	TableFunction dummy_table_function;
	dummy_table_function.name = StringUtil::Format("%sV%d", OP::NAME, OP::FORMAT_VERSION);
	TableFunctionBindInput bind_input(children, named_params, input_types, input_names, nullptr, nullptr,
	                                  dummy_table_function, empty);
	bind_data = avro_scan.bind(context, bind_input, return_types, return_names);

	vector<column_t> column_ids;
	for (idx_t i = 0; i < return_types.size(); i++) {
		column_ids.push_back(i);
	}

	TableFunctionInitInput input(bind_data.get(), column_ids, vector<idx_t>(), nullptr);
	global_state = avro_scan.init_global(context, input);
}

bool AvroScan::GetNext(DataChunk &result) {
	TableFunctionInput function_input(bind_data.get(), nullptr, global_state.get());
	avro_scan->function(context, function_input, result);

	idx_t count = result.size();
	for (auto &vec : result.data) {
		vec.Flatten(count);
	}
	if (count == 0) {
		return false;
	}
	return true;
}

void AvroScan::InitializeChunk(DataChunk &chunk) {
	chunk.Initialize(context, return_types, STANDARD_VECTOR_SIZE);
}

} // namespace duckdb

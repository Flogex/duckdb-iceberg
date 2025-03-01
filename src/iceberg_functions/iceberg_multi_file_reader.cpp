#include "iceberg_multi_file_reader.hpp"

#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"

namespace duckdb {

IcebergMultiFileList::IcebergMultiFileList(ClientContext &context_p, const string &path, const IcebergOptions &options)
    : MultiFileList({path}, FileGlobOptions::ALLOW_EMPTY), lock(), context(context_p), options(options) {
}

string IcebergMultiFileList::ToDuckDBPath(const string &raw_path) {
	return raw_path;
}

string IcebergMultiFileList::GetPath() const {
	return GetPaths()[0];
}

void IcebergMultiFileList::Bind(vector<LogicalType> &return_types, vector<string> &names) {
	if (!initialized) {
		InitializeFiles();
	}

	auto &schema = snapshot.schema;
	for (auto &schema_entry : schema) {
		names.push_back(schema_entry.name);
		return_types.push_back(schema_entry.type);
	}
}

template <bool LOWER_BOUND = true>
[[noreturn]] static void ThrowBoundError(const string &bound, IcebergColumnDefinition &column) {
	auto bound_type = LOWER_BOUND ? "'lower_bound'" : "'upper_bound'";
	throw InvalidInputException("Invalid %s size (%d) for column %s with type '%s', bound value: '%s'", bound_type, bound.size(), column.name, column.type.ToString(), bound);
}

template <bool LOWER_BOUND = true>
static Value DeserializeBound(const string &bound_value, IcebergColumnDefinition &column) {
	auto &type = column.type;

	switch (type.id()) {
	case LogicalTypeId::INTEGER: {
		if (bound_value.size() != sizeof(int32_t)) {
			ThrowBoundError<LOWER_BOUND>(bound_value, column);
		}
		int32_t val;
		std::memcpy(&val, bound_value.data(), sizeof(int32_t));
		return Value::INTEGER(val);
	}
	case LogicalTypeId::BIGINT: {
		if (bound_value.size() != sizeof(int64_t)) {
			ThrowBoundError<LOWER_BOUND>(bound_value, column);
		}
		int64_t val;
		std::memcpy(&val, bound_value.data(), sizeof(int64_t));
		return Value::BIGINT(val);
	}
	case LogicalTypeId::DATE: {
		if (bound_value.size() != sizeof(int32_t)) { // Dates are typically stored as int32 (days since epoch)
			ThrowBoundError<LOWER_BOUND>(bound_value, column);
		}
		int32_t days_since_epoch;
		std::memcpy(&days_since_epoch, bound_value.data(), sizeof(int32_t));
		// Convert to DuckDB date
		date_t date = Date::EpochDaysToDate(days_since_epoch);
		return Value::DATE(date);
	}
	case LogicalTypeId::TIMESTAMP: {
		if (bound_value.size() != sizeof(int64_t)) { // Timestamps are typically stored as int64 (microseconds since epoch)
			ThrowBoundError<LOWER_BOUND>(bound_value, column);
		}
		int64_t micros_since_epoch;
		std::memcpy(&micros_since_epoch, bound_value.data(), sizeof(int64_t));
		// Convert to DuckDB timestamp using microseconds
		timestamp_t timestamp = Timestamp::FromEpochMicroSeconds(micros_since_epoch);
		return Value::TIMESTAMP(timestamp);
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		if (bound_value.size() != sizeof(int64_t)) { // Assuming stored as int64 (microseconds since epoch)
			ThrowBoundError<LOWER_BOUND>(bound_value, column);
		}
		int64_t micros_since_epoch;
		std::memcpy(&micros_since_epoch, bound_value.data(), sizeof(int64_t));
		// Convert to DuckDB timestamp using microseconds
		timestamp_t timestamp = Timestamp::FromEpochMicroSeconds(micros_since_epoch);
		// Create a TIMESTAMPTZ Value
		return Value::TIMESTAMPTZ(timestamp_tz_t(timestamp));
	}
	case LogicalTypeId::DOUBLE: {
		if (bound_value.size() != sizeof(double)) {
			ThrowBoundError<LOWER_BOUND>(bound_value, column);
		}
		double val;
		std::memcpy(&val, bound_value.data(), sizeof(double));
		return Value::DOUBLE(val);
	}
	case LogicalTypeId::VARCHAR: {
		// Assume the bytes represent a UTF-8 string
		return Value(bound_value);
	}
	// Add more types as needed
	default:
		break;
	}
	ThrowBoundError<LOWER_BOUND>(bound_value, column);
}

unique_ptr<MultiFileList> IcebergMultiFileList::ComplexFilterPushdown(ClientContext &context,
                                                                      const MultiFileReaderOptions &options,
                                                                      MultiFilePushdownInfo &info,
                                                                      vector<unique_ptr<Expression>> &filters) {
	if (!table_filters.filters.empty()) {
		//! Already performed filter pushdown
		return nullptr;
	}

	FilterCombiner combiner(context);
	for (const auto &filter : filters) {
		combiner.AddFilter(filter->Copy());
	}
	auto filterstmp = combiner.GenerateTableScanFilters(info.column_indexes);

	auto filtered_list = make_uniq<IcebergMultiFileList>(context, paths[0], this->options);
	filtered_list->table_filters = std::move(filterstmp);
	filtered_list->names = names;

	return std::move(filtered_list);
}

vector<string> IcebergMultiFileList::GetAllFiles() {
	throw NotImplementedException("NOT IMPLEMENTED");
}

FileExpandResult IcebergMultiFileList::GetExpandResult() {
	// GetFile(1) will ensure files with index 0 and index 1 are expanded if they are available
	GetFile(1);

	if (data_files.size() > 1) {
		return FileExpandResult::MULTIPLE_FILES;
	} else if (data_files.size() == 1) {
		return FileExpandResult::SINGLE_FILE;
	}

	return FileExpandResult::NO_FILES;
}

idx_t IcebergMultiFileList::GetTotalFileCount() {
	// FIXME: the 'added_files_count' + the 'existing_files_count'
	// in the Manifest List should give us this information without scanning the manifest list
	idx_t i = data_files.size();
	while (!GetFile(i).empty()) {
		i++;
	}
	return data_files.size();
}

unique_ptr<NodeStatistics> IcebergMultiFileList::GetCardinality(ClientContext &context) {
	auto total_file_count = IcebergMultiFileList::GetTotalFileCount();

	if (total_file_count == 0) {
		return make_uniq<NodeStatistics>(0, 0);
	}

	// FIXME: visit metadata to get a cardinality count

	return nullptr;
}

bool IcebergMultiFileList::FileMatchesFilter(IcebergManifestEntry &file) {
	D_ASSERT(!table_filters.filters.empty());

	auto &filters = table_filters.filters;
	auto &schema = snapshot.schema;

	for (idx_t column_id = 0; column_id < schema.size(); column_id++) {
		// FIXME: is there a potential mismatch between column_id / field_id lurking here?
		auto &column = schema[column_id];
		auto it = filters.find(column_id);

		if (it == filters.end()) {
			continue;
		}
		if (file.lower_bounds.empty() || file.upper_bounds.empty()) {
			//! There are no bounds statistics for the file, can't filter
			continue;
		}

		auto &field_id = column.id;
		auto lower_bound_it = file.lower_bounds.find(field_id);
		auto upper_bound_it = file.upper_bounds.find(field_id);
		if (lower_bound_it == file.lower_bounds.end() || upper_bound_it == file.upper_bounds.end()) {
			//! There are no bound statistics for this column
			continue;
		}

		auto lower_bound = DeserializeBound<true>(lower_bound_it->second, column);
		auto upper_bound = DeserializeBound<false>(upper_bound_it->second, column);
		auto &filter = *it->second;
		//! TODO: support more filter types
		if (filter.filter_type != TableFilterType::CONSTANT_COMPARISON) {
			continue;
		}
		auto &constant_filter = filter.Cast<ConstantFilter>();
		auto &constant_value = constant_filter.constant;
		bool result = true;
		switch (constant_filter.comparison_type) {
		case ExpressionType::COMPARE_EQUAL:
			result = (constant_value >= lower_bound && constant_value <= upper_bound);
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			result = (constant_value <= upper_bound);
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			result = (constant_value <= upper_bound);
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			result = (constant_value >= lower_bound);
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			result = (constant_value >= lower_bound);
			break;
		default:
			// For other types of comparisons, we can't make a decision based on bounds
			result = true; // Conservative approach
			break;
		}
		if (!result) {
			//! If any predicate fails, exclude the file
			return false;
		}
	}
	return true;
}

string IcebergMultiFileList::GetFile(idx_t file_id) {
	if (!initialized) {
		InitializeFiles();
	}

	auto iceberg_path = GetPath();
	auto &fs = FileSystem::GetFileSystem(context);
	auto &data_files = this->data_files;
	auto &entry_producer = this->entry_producer;

	// Read enough data files
	while (file_id >= data_files.size()) {
		if (data_manifest_entry_reader->Finished()) {
			if (current_data_manifest == data_manifests.end()) {
				break;
			}
			auto &manifest = *current_data_manifest;
			auto manifest_entry_full_path = options.allow_moved_paths
												? IcebergUtils::GetFullPath(iceberg_path, manifest.manifest_path, fs)
												: manifest.manifest_path;
			auto scan = make_uniq<AvroScan>("IcebergManifest", context, manifest_entry_full_path);
			data_manifest_entry_reader->Initialize(std::move(scan));
		}

		idx_t remaining = (file_id + 1) - data_files.size();
		if (!table_filters.filters.empty()) {
			// FIXME: push down the filter into the 'read_avro' scan, so the entries that don't match are just filtered out
			vector<IcebergManifestEntry> intermediate_entries;
			data_manifest_entry_reader->ReadEntries(remaining, [&intermediate_entries, &entry_producer](DataChunk &chunk, idx_t offset, idx_t count, const ManifestReaderInput &input) {
				return entry_producer(chunk, offset, count, input, intermediate_entries);
			});

			for (auto &entry : intermediate_entries) {
				if (!FileMatchesFilter(entry)) {
					//! Skip this file
					continue;
				}
				data_files.push_back(entry);
			}
		} else {
			data_manifest_entry_reader->ReadEntries(remaining, [&data_files, &entry_producer](DataChunk &chunk, idx_t offset, idx_t count, const ManifestReaderInput &input) {
				return entry_producer(chunk, offset, count, input, data_files);
			});
		}

		if (data_manifest_entry_reader->Finished()) {
			current_data_manifest++;
			continue;
		}
	}
	#ifdef DEBUG
	for (auto &entry : data_files) {
		D_ASSERT(entry.content == IcebergManifestEntryContentType::DATA);
		D_ASSERT(entry.status != IcebergManifestEntryStatusType::DELETED);
	}
	#endif

	if (file_id >= data_files.size()) {
		return string();
	}

	D_ASSERT(file_id < data_files.size());
	auto &data_file = data_files[file_id];
	auto &path = data_file.file_path;

	if (options.allow_moved_paths) {
		auto iceberg_path = GetPath();
		auto &fs = FileSystem::GetFileSystem(context);
		return IcebergUtils::GetFullPath(iceberg_path, path, fs);
	} else {
		return path;
	}
}

void IcebergMultiFileList::InitializeFiles() {
	lock_guard<mutex> guard(lock);
	if (initialized) {
		return;
	}
	initialized = true;
	auto &manifest_producer = this->manifest_producer;

	//! Load the snapshot
	auto iceberg_path = GetPath();
	auto &fs = FileSystem::GetFileSystem(context);
	auto iceberg_meta_path = IcebergSnapshot::GetMetaDataPath(context, iceberg_path, fs, options);
	switch (options.snapshot_source) {
	case SnapshotSource::LATEST: {
		snapshot = IcebergSnapshot::GetLatestSnapshot(iceberg_meta_path, fs, options);
		break;
	}
	case SnapshotSource::FROM_ID: {
		snapshot = IcebergSnapshot::GetSnapshotById(iceberg_meta_path, fs, options.snapshot_id, options);
		break;
	}
	case SnapshotSource::FROM_TIMESTAMP: {
		snapshot = IcebergSnapshot::GetSnapshotByTimestamp(iceberg_meta_path, fs, options.snapshot_timestamp, options);
		break;
	}
	default:
		throw InternalException("SnapshotSource type not implemented");
	}

	//! Set up the manifest + manifest entry readers
	if (snapshot.iceberg_format_version == 1) {
		data_manifest_entry_reader = make_uniq<ManifestReader>(IcebergManifestEntryV1::PopulateNameMapping, IcebergManifestEntryV1::VerifySchema);
		delete_manifest_entry_reader = make_uniq<ManifestReader>(IcebergManifestEntryV1::PopulateNameMapping, IcebergManifestEntryV1::VerifySchema);
		delete_manifest_entry_reader->skip_deleted = true;
		data_manifest_entry_reader->skip_deleted = true;
		manifest_reader = make_uniq<ManifestReader>(IcebergManifestV1::PopulateNameMapping, IcebergManifestV1::VerifySchema);

		manifest_producer = IcebergManifestV1::ProduceEntries;
		entry_producer = IcebergManifestEntryV1::ProduceEntries;
	} else if (snapshot.iceberg_format_version == 2) {
		data_manifest_entry_reader = make_uniq<ManifestReader>(IcebergManifestEntryV2::PopulateNameMapping, IcebergManifestEntryV2::VerifySchema);
		delete_manifest_entry_reader = make_uniq<ManifestReader>(IcebergManifestEntryV2::PopulateNameMapping, IcebergManifestEntryV2::VerifySchema);
		delete_manifest_entry_reader->skip_deleted = true;
		data_manifest_entry_reader->skip_deleted = true;
		manifest_reader = make_uniq<ManifestReader>(IcebergManifestV2::PopulateNameMapping, IcebergManifestV2::VerifySchema);

		manifest_producer = IcebergManifestV2::ProduceEntries;
		entry_producer = IcebergManifestEntryV2::ProduceEntries;
	} else {
		throw InvalidInputException("Reading from Iceberg version %d is not supported yet", snapshot.iceberg_format_version);
	}

	// Read the manifest list, we need all the manifests to determine if we've seen all deletes
	auto manifest_list_full_path = options.allow_moved_paths
	                                   ? IcebergUtils::GetFullPath(iceberg_path, snapshot.manifest_list, fs)
	                                   : snapshot.manifest_list;
	auto scan = make_uniq<AvroScan>("IcebergManifestList", context, manifest_list_full_path);
	manifest_reader->Initialize(std::move(scan));

	vector<IcebergManifest> all_manifests;
	while (!manifest_reader->Finished()) {
		manifest_reader->ReadEntries(STANDARD_VECTOR_SIZE, [&all_manifests, manifest_producer](DataChunk &chunk, idx_t offset, idx_t count, const ManifestReaderInput &input) {
			return manifest_producer(chunk, offset, count, input, all_manifests);
		});
	}

	for (auto &manifest : all_manifests) {
		if (manifest.content == IcebergManifestContentType::DATA) {
			data_manifests.push_back(std::move(manifest));
		} else {
			D_ASSERT(manifest.content == IcebergManifestContentType::DELETE);
			delete_manifests.push_back(std::move(manifest));
		}
	}

	current_data_manifest = data_manifests.begin();
	current_delete_manifest = delete_manifests.begin();
}

//! Multi File Reader

unique_ptr<MultiFileReader> IcebergMultiFileReader::CreateInstance(const TableFunction &table) {
	(void)table;
	return make_uniq<IcebergMultiFileReader>();
}

shared_ptr<MultiFileList> IcebergMultiFileReader::CreateFileList(ClientContext &context, const vector<string> &paths,
                                                                 FileGlobOptions options) {
	if (paths.size() != 1) {
		throw BinderException("'iceberg_scan' only supports single path as input");
	}
	return make_shared_ptr<IcebergMultiFileList>(context, paths[0], this->options);
}

bool IcebergMultiFileReader::Bind(MultiFileReaderOptions &options, MultiFileList &files,
                                  vector<LogicalType> &return_types, vector<string> &names,
                                  MultiFileReaderBindData &bind_data) {
	auto &iceberg_multi_file_list = dynamic_cast<IcebergMultiFileList &>(files);

	iceberg_multi_file_list.Bind(return_types, names);
	// FIXME: apply final transformation for 'file_row_number' ???

	auto &schema = iceberg_multi_file_list.snapshot.schema;
	auto &columns = bind_data.schema;
	for (auto &item : schema) {
		MultiFileReaderColumnDefinition column(item.name, item.type);
		column.default_expression = make_uniq<ConstantExpression>(item.default_value);
		column.identifier = Value::INTEGER(item.id);

		columns.push_back(column);
	}
	bind_data.file_row_number_idx = names.size();
	bind_data.mapping = MultiFileReaderColumnMappingMode::BY_FIELD_ID;
	return true;
}

void IcebergMultiFileReader::BindOptions(MultiFileReaderOptions &options, MultiFileList &files,
                                         vector<LogicalType> &return_types, vector<string> &names,
                                         MultiFileReaderBindData &bind_data) {
	// Disable all other multifilereader options
	options.auto_detect_hive_partitioning = false;
	options.hive_partitioning = false;
	options.union_by_name = false;

	MultiFileReader::BindOptions(options, files, return_types, names, bind_data);
}

void IcebergMultiFileReader::CreateColumnMapping(const string &file_name,
                                                 const vector<MultiFileReaderColumnDefinition> &local_columns,
                                                 const vector<MultiFileReaderColumnDefinition> &global_columns,
                                                 const vector<ColumnIndex> &global_column_ids,
                                                 MultiFileReaderData &reader_data,
                                                 const MultiFileReaderBindData &bind_data, const string &initial_file,
                                                 optional_ptr<MultiFileReaderGlobalState> global_state_p) {

	D_ASSERT(bind_data.mapping == MultiFileReaderColumnMappingMode::BY_FIELD_ID);
	MultiFileReader::CreateColumnMappingByFieldId(file_name, local_columns, global_columns, global_column_ids,
	                                              reader_data, bind_data, initial_file, global_state_p);

	auto &global_state = global_state_p->Cast<IcebergMultiFileReaderGlobalState>();
    // Check if the file_row_number column is an "extra_column" which is not part of the projection
	if (!global_state.file_row_number_idx.IsValid()) {
		return;
	}
	auto file_row_number_idx = global_state.file_row_number_idx.GetIndex();
    if (file_row_number_idx >= global_column_ids.size()) {
        // Build the name map
        case_insensitive_map_t<idx_t> name_map;
        for (idx_t col_idx = 0; col_idx < local_columns.size(); col_idx++) {
            name_map[local_columns[col_idx].name] = col_idx;
        }

        // Lookup the required column in the local map
        auto entry = name_map.find("file_row_number");
        if (entry == name_map.end()) {
            throw IOException("Failed to find the file_row_number column");
        }

        // Register the column to be scanned from this file
        reader_data.column_ids.push_back(entry->second);
        reader_data.column_indexes.emplace_back(entry->second);
        reader_data.column_mapping.push_back(file_row_number_idx);
    }

    // This may have changed: update it
    reader_data.empty_columns = reader_data.column_ids.empty();

}

unique_ptr<MultiFileReaderGlobalState>
IcebergMultiFileReader::InitializeGlobalState(ClientContext &context, const MultiFileReaderOptions &file_options,
                                              const MultiFileReaderBindData &bind_data, const MultiFileList &file_list,
                                              const vector<MultiFileReaderColumnDefinition> &global_columns,
                                              const vector<ColumnIndex> &global_column_ids) {

	vector<LogicalType> extra_columns;
	// Map of column_name -> column_index
	vector<pair<string, idx_t>> mapped_columns;

	// TODO: only add file_row_number column if there are deletes
	case_insensitive_map_t<LogicalType> columns_to_map = {
	    {"file_row_number", LogicalType::BIGINT},
	};

	// Create a map of the columns that are in the projection
	// So we can detect that the projection already contains the 'extra_column' below
	case_insensitive_map_t<idx_t> selected_columns;
	for (idx_t i = 0; i < global_column_ids.size(); i++) {
		auto global_id = global_column_ids[i];
		if (global_id.IsRowIdColumn()) {
			continue;
		}

		auto &global_name = global_columns[global_id.GetPrimaryIndex()].name;
		selected_columns.insert({global_name, i});
	}

	// Map every column to either a column in the projection, or add it to the extra columns if it doesn't exist
	idx_t col_offset = 0;
	for (const auto &extra_column : columns_to_map) {
		// First check if the column is in the projection
		auto res = selected_columns.find(extra_column.first);
		if (res != selected_columns.end()) {
			// The column is in the projection, no special handling is required; we simply store the index
			mapped_columns.push_back({extra_column.first, res->second});
			continue;
		}

		// The column is NOT in the projection: it needs to be added as an extra_column

		// Calculate the index of the added column (extra columns are added after all other columns)
		idx_t current_col_idx = global_column_ids.size() + col_offset++;

		// Add column to the map, to ensure the MultiFileReader can find it when processing the Chunk
		mapped_columns.push_back({extra_column.first, current_col_idx});

		// Ensure the result DataChunk has a vector of the correct type to store this column
		extra_columns.push_back(extra_column.second);
	}

	auto res = make_uniq<IcebergMultiFileReaderGlobalState>(extra_columns, file_list);

	// Parse all the mapped columns into the DeltaMultiFileReaderGlobalState for easy use;
	for (const auto &mapped_column : mapped_columns) {
		auto &column_name = mapped_column.first;
		auto column_index = mapped_column.second;
		if (StringUtil::CIEquals(column_name, "file_row_number")) {
			if (res->file_row_number_idx.IsValid()) {
				throw InvalidInputException("'file_row_number' already set!");
			}
			res->file_row_number_idx = column_index;
		} else {
			throw InternalException("Extra column type not handled");
		}
	}
	return std::move(res);
}

void IcebergMultiFileReader::FinalizeBind(const MultiFileReaderOptions &file_options,
	                                     const MultiFileReaderBindData &options, const string &filename,
	                                     const vector<MultiFileReaderColumnDefinition> &local_columns,
	                                     const vector<MultiFileReaderColumnDefinition> &global_columns,
	                                     const vector<ColumnIndex> &global_column_ids, MultiFileReaderData &reader_data,
	                                     ClientContext &context, optional_ptr<MultiFileReaderGlobalState> global_state) {
	MultiFileReader::FinalizeBind(file_options, options, filename, local_columns, global_columns,
	                              global_column_ids, reader_data, context, global_state);
	return;
}

void IcebergMultiFileList::ScanDeleteFile(const string &delete_file_path) const {
	auto &instance = DatabaseInstance::GetDatabase(context);
	auto &parquet_scan_entry = ExtensionUtil::GetTableFunction(instance, "parquet_scan");
	auto &parquet_scan = parquet_scan_entry.functions.functions[0];

	// Prepare the inputs for the bind
	vector<Value> children;
	children.reserve(1);
	children.push_back(Value(delete_file_path));
	named_parameter_map_t named_params;
	vector<LogicalType> input_types;
	vector<string> input_names;

	TableFunctionRef empty;
	TableFunction dummy_table_function;
	dummy_table_function.name = "IcebergDeleteScan";
	TableFunctionBindInput bind_input(children, named_params, input_types, input_names, nullptr, nullptr,
	                                  dummy_table_function, empty);
	vector<LogicalType> return_types;
	vector<string> return_names;

	auto bind_data = parquet_scan.bind(context, bind_input, return_types, return_names);

	DataChunk result;
	// Reserve for STANDARD_VECTOR_SIZE instead of count, in case the returned table contains too many tuples
	result.Initialize(context, return_types, STANDARD_VECTOR_SIZE);

	ThreadContext thread_context(context);
	ExecutionContext execution_context(context, thread_context, nullptr);

	vector<column_t> column_ids;
	for (idx_t i = 0; i < return_types.size(); i++) {
		column_ids.push_back(i);
	}
	TableFunctionInitInput input(bind_data.get(), column_ids, vector<idx_t>(), nullptr);
	auto global_state = parquet_scan.init_global(context, input);
	auto local_state = parquet_scan.init_local(execution_context, input, global_state.get());

	do {
		TableFunctionInput function_input(bind_data.get(), local_state.get(), global_state.get());
		result.Reset();
		parquet_scan.function(context, function_input, result);

		idx_t count = result.size();
		for (auto &vec : result.data) {
			vec.Flatten(count);
		}

		auto names = FlatVector::GetData<string_t>(result.data[0]);
		auto row_ids = FlatVector::GetData<int64_t>(result.data[1]);

		if (count == 0) {
			continue;
		}
		reference<string_t> current_file_path = names[0];
		reference<IcebergDeleteData> deletes = delete_data[current_file_path.get().GetString()];

		for (idx_t i = 0; i < count; i++) {
			auto &name = names[i];
			auto &row_id = row_ids[i];

			if (name != current_file_path.get()) {
				current_file_path = name;
				deletes = delete_data[current_file_path.get().GetString()];
			}

			deletes.get().AddRow(row_id);
		}
	} while (result.size() != 0);
}

optional_ptr<IcebergDeleteData> IcebergMultiFileList::GetDeletesForFile(const string &file_path) const {
	auto it = delete_data.find(file_path);
	if (it != delete_data.end()) {
		// There is delete data for this file, return it
		auto &deletes = it->second;
		return deletes;
	}
	return nullptr;
}

void IcebergMultiFileList::ProcessDeletes() const {
	// In <=v2 we now have to process *all* delete manifests
	// before we can be certain that we have all the delete data for the current file.

	// v3 solves this, `referenced_data_file` will tell us which file the `data_file`
	// is targeting before we open it, and there can only be one deletion vector per data file.

	// From the spec: "At most one deletion vector is allowed per data file in a snapshot"

	auto iceberg_path = GetPath();
	auto &fs = FileSystem::GetFileSystem(context);
	auto &entry_producer = this->entry_producer;

	vector<IcebergManifestEntry> delete_files;
	while (current_delete_manifest != delete_manifests.end()) {
		if (delete_manifest_entry_reader->Finished()) {
			if (current_delete_manifest == delete_manifests.end()) {
				break;
			}
			auto &manifest = *current_delete_manifest;
			auto manifest_entry_full_path = options.allow_moved_paths
												? IcebergUtils::GetFullPath(iceberg_path, manifest.manifest_path, fs)
												: manifest.manifest_path;
			auto scan = make_uniq<AvroScan>("IcebergManifest", context, manifest_entry_full_path);
			delete_manifest_entry_reader->Initialize(std::move(scan));
		}

		delete_manifest_entry_reader->ReadEntries(STANDARD_VECTOR_SIZE, [&delete_files, &entry_producer](DataChunk &chunk, idx_t offset, idx_t count, const ManifestReaderInput &input) {
			return entry_producer(chunk, offset, count, input, delete_files);
		});

		if (delete_manifest_entry_reader->Finished()) {
			current_delete_manifest++;
			continue;
		}
	}

	#ifdef DEBUG
	for (auto &entry : data_files) {
		D_ASSERT(entry.content == IcebergManifestEntryContentType::DATA);
		D_ASSERT(entry.status != IcebergManifestEntryStatusType::DELETED);
	}
	#endif

	for (auto &entry : delete_files) {
		ScanDeleteFile(entry.file_path);
	}

	D_ASSERT(current_delete_manifest == delete_manifests.end());
}

void IcebergMultiFileReader::FinalizeChunk(ClientContext &context, const MultiFileReaderBindData &bind_data,
                                           const MultiFileReaderData &reader_data, DataChunk &chunk,
                                           optional_ptr<MultiFileReaderGlobalState> global_state) {
	// Base class finalization first
	MultiFileReader::FinalizeChunk(context, bind_data, reader_data, chunk, global_state);

	D_ASSERT(global_state);
	auto &iceberg_global_state = global_state->Cast<IcebergMultiFileReaderGlobalState>();
	D_ASSERT(iceberg_global_state.file_list);

	// Get the metadata for this file
	const auto &multi_file_list = dynamic_cast<const IcebergMultiFileList &>(*global_state->file_list);
	auto file_id = reader_data.file_list_idx.GetIndex();
	auto &data_file = multi_file_list.data_files[file_id];

	// The path of the data file where this chunk was read from
	auto &file_path = data_file.file_path;
	optional_ptr<IcebergDeleteData> delete_data;
	{
		std::lock_guard<mutex> guard(multi_file_list.delete_lock);
		if (multi_file_list.current_delete_manifest != multi_file_list.delete_manifests.end()) {
			multi_file_list.ProcessDeletes();
		}
		delete_data = multi_file_list.GetDeletesForFile(file_path);
	}

	//! FIXME: how can we retrieve which rows these were in the file?
	// Looks like delta does this by adding an extra projection so the chunk has a file_row_id column
	if (delete_data) {
		D_ASSERT(iceberg_global_state.file_row_number_idx.IsValid());
		auto &file_row_number_column = chunk.data[iceberg_global_state.file_row_number_idx.GetIndex()];

		delete_data->Apply(chunk, file_row_number_column);
	}
}

bool IcebergMultiFileReader::ParseOption(const string &key, const Value &val, MultiFileReaderOptions &options,
                                         ClientContext &context) {
	auto loption = StringUtil::Lower(key);
	if (loption == "allow_moved_paths") {
		this->options.allow_moved_paths = BooleanValue::Get(val);
		return true;
	}
	if (loption == "metadata_compression_codec") {
		this->options.metadata_compression_codec = StringValue::Get(val);
		return true;
	}
	if (loption == "skip_schema_inference") {
		this->options.skip_schema_inference = BooleanValue::Get(val);
		return true;
	}
	if (loption == "version") {
		this->options.table_version = StringValue::Get(val);
		return true;
	}
	if (loption == "version_name_format") {
		this->options.version_name_format = StringValue::Get(val);
		return true;
	}
	if (loption == "snapshot_from_id") {
		if (this->options.snapshot_source != SnapshotSource::LATEST) {
			throw InvalidInputException("Can't use 'snapshot_from_id' in combination with 'snapshot_from_timestamp'");
		}
		this->options.snapshot_source = SnapshotSource::FROM_ID;
		this->options.snapshot_id = val.GetValue<uint64_t>();
		return true;
	}
	if (loption == "snapshot_from_timestamp") {
		if (this->options.snapshot_source != SnapshotSource::LATEST) {
			throw InvalidInputException("Can't use 'snapshot_from_id' in combination with 'snapshot_from_timestamp'");
		}
		this->options.snapshot_source = SnapshotSource::FROM_TIMESTAMP;
		this->options.snapshot_timestamp = val.GetValue<timestamp_t>();
		return true;
	}
	return MultiFileReader::ParseOption(key, val, options, context);
}

} // namespace duckdb

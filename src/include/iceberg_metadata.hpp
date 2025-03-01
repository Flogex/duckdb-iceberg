//===----------------------------------------------------------------------===//
//                         DuckDB
//
// iceberg_metadata.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "yyjson.hpp"
#include "iceberg_types.hpp"
#include "iceberg_options.hpp"

using namespace duckdb_yyjson;

namespace duckdb {

struct IcebergColumnDefinition {
public:
	static IcebergColumnDefinition ParseFromJson(yyjson_val *val);
public:
	LogicalType ToDuckDBType() {
		return type;
	}
public:
	int32_t id;
	string name;
	LogicalType type;
	Value default_value;
	bool required;
};

struct IcebergPartitionSpecField {
public:
	static IcebergPartitionSpecField ParseFromJson(yyjson_val *val);
public:
	string name;
	//! FIXME: parse this, there are a set amount of valid transforms
	//! See: https://iceberg.apache.org/spec/#partition-specs
	//! "Applied to the source column(s) to produce a partition value"
	string transform;
	//! NOTE: v3 replaces 'source-id' with 'source-ids'
	//! "A source column id or a list of source column ids from the table’s schema"
	uint64_t source_id;
	//! "Used to identify a partition field and is unique within a partition spec"
	uint64_t partition_field_id;
};

struct IcebergPartitionSpec {
public:
	static IcebergPartitionSpec ParseFromJson(yyjson_val *val);
public:
	uint64_t spec_id;
	vector<IcebergPartitionSpecField> fields;
};

struct IcebergMetadata {
private:
	IcebergMetadata() = default;
public:
	static unique_ptr<IcebergMetadata> Parse(const string &path, FileSystem &fs, const string &metadata_compression_codec);
	~IcebergMetadata() {
		if (doc) {
			yyjson_doc_free(doc);
		}
	}
public:
	unordered_map<int64_t, IcebergPartitionSpec> ParsePartitionSpecs();
public:
	// Ownership of parse data
	yyjson_doc *doc = nullptr;
	string document;

	//! Parsed info
	yyjson_val *snapshots;
	yyjson_val *partition_specs;
	vector<yyjson_val *> schemas;
	uint64_t iceberg_version;
	uint64_t schema_id;
};

//! An Iceberg snapshot https://iceberg.apache.org/spec/#snapshots
class IcebergSnapshot {
public:
	//! Snapshot metadata
	uint64_t snapshot_id;
	uint64_t sequence_number;
	string manifest_list;
	timestamp_t timestamp_ms;
	idx_t iceberg_format_version;
	uint64_t schema_id;
	unordered_map<uint64_t, IcebergColumnDefinition> schema;
	string metadata_compression_codec = "none";
public:
	static IcebergSnapshot GetLatestSnapshot(IcebergMetadata &info, const IcebergOptions &options);
	static IcebergSnapshot GetSnapshotById(IcebergMetadata &info, idx_t snapshot_id, const IcebergOptions &options);
	static IcebergSnapshot GetSnapshotByTimestamp(IcebergMetadata &info, timestamp_t timestamp, const IcebergOptions &options);

	static IcebergSnapshot ParseSnapShot(yyjson_val *snapshot, idx_t iceberg_format_version, idx_t schema_id,
	                                     vector<yyjson_val *> &schemas, const IcebergOptions &options);
	static string GetMetaDataPath(ClientContext &context, const string &path, FileSystem &fs, const IcebergOptions &options);

protected:
	//! Version extraction and identification
	static bool UnsafeVersionGuessingEnabled(ClientContext &context);
	static string GetTableVersionFromHint(const string &path, FileSystem &fs, string version_format);
	static string GuessTableVersion(const string &meta_path, FileSystem &fs, const IcebergOptions &options);
	static string PickTableVersion(vector<string> &found_metadata, string &version_pattern, string &glob);

	//! Internal JSON parsing functions
	static yyjson_val *FindLatestSnapshotInternal(yyjson_val *snapshots);
	static yyjson_val *FindSnapshotByIdInternal(yyjson_val *snapshots, idx_t target_id);
	static yyjson_val *FindSnapshotByIdTimestampInternal(yyjson_val *snapshots, timestamp_t timestamp);
	static unordered_map<uint64_t, IcebergColumnDefinition> ParseSchema(vector<yyjson_val *> &schemas, idx_t schema_id);
};

//! Represents the iceberg table at a specific IcebergSnapshot. Corresponds to a single Manifest List.
struct IcebergTable {
public:
	//! Loads all(!) metadata of into IcebergTable object
	static IcebergTable Load(const string &iceberg_path, IcebergSnapshot &snapshot, ClientContext &context, const IcebergOptions &options);

public:
	//! Returns all paths to be scanned for the IcebergManifestContentType
	template <IcebergManifestContentType TYPE>
	vector<string> GetPaths() {
		vector<string> ret;
		for (auto &entry : entries) {
			if (entry.manifest.content != TYPE) {
				continue;
			}
			for (auto &manifest_entry : entry.manifest_entries) {
				if (manifest_entry.status == IcebergManifestEntryStatusType::DELETED) {
					continue;
				}
				ret.push_back(manifest_entry.file_path);
			}
		}
		return ret;
	}
	vector<IcebergManifestEntry> GetAllPaths() {
		vector<IcebergManifestEntry> ret;
		for (auto &entry : entries) {
			for (auto &manifest_entry : entry.manifest_entries) {
				if (manifest_entry.status == IcebergManifestEntryStatusType::DELETED) {
					continue;
				}
				ret.push_back(manifest_entry);
			}
		}
		return ret;
	}

	void Print() {
		Printer::Print("Iceberg table (" + path + ")");
		for (auto &entry : entries) {
			entry.Print();
		}
	}

	//! The snapshot of this table
	IcebergSnapshot snapshot;
	//! The entries (manifests) of this table
	vector<IcebergTableEntry> entries;

protected:
	string path;
};

} // namespace duckdb

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

#include "metadata/iceberg_manifest.hpp"
#include "metadata/iceberg_manifest_list.hpp"

#include "iceberg_options.hpp"
#include "manifest_cache.hpp"

#include "duckdb/common/open_file_info.hpp"
#include "duckdb/function/table_function.hpp"

#include "rest_catalog/objects/table_metadata.hpp"

#include "metadata/iceberg_snapshot.hpp"
#include "metadata/iceberg_table_metadata.hpp"
#include "metadata/iceberg_partition_spec.hpp"
#include "metadata/iceberg_table_schema.hpp"
#include "metadata/iceberg_field_mapping.hpp"
#include "metadata/iceberg_manifest.hpp"

using namespace duckdb_yyjson;

namespace duckdb {

//! Used when we are not scanning from a REST Catalog
struct IcebergScanTemporaryData {
	IcebergTableMetadata metadata;
	IcebergManifestFileCache manifest_file_cache;
	IcebergManifestListCache manifest_list_cache;
};

struct IcebergScanInfo : public TableFunctionInfo {
public:
	IcebergScanInfo(const string &metadata_path, IcebergTableMetadata &metadata,
	                IcebergManifestListCache &manifest_list_cache, IcebergManifestFileCache &manifest_file_cache,
	                optional_ptr<IcebergSnapshot> snapshot, IcebergTableSchema &schema)
	    : metadata_path(metadata_path), metadata(metadata), manifest_list_cache(manifest_list_cache),
	      manifest_file_cache(manifest_file_cache), snapshot(snapshot), schema(schema) {
	}
	IcebergScanInfo(const string &metadata_path, unique_ptr<IcebergScanTemporaryData> owned_temp_data_p,
	                optional_ptr<IcebergSnapshot> snapshot, IcebergTableSchema &schema)
	    : metadata_path(metadata_path), owned_temp_data(std::move(owned_temp_data_p)),
	      metadata(owned_temp_data->metadata), manifest_list_cache(owned_temp_data->manifest_list_cache),
	      manifest_file_cache(owned_temp_data->manifest_file_cache), snapshot(snapshot), schema(schema) {
	}

public:
	string metadata_path;
	unique_ptr<IcebergScanTemporaryData> owned_temp_data;
	IcebergTableMetadata &metadata;
	IcebergManifestListCache &manifest_list_cache;
	IcebergManifestFileCache &manifest_file_cache;

	optional_ptr<IcebergSnapshot> snapshot;
	IcebergTableSchema &schema;
};

//! ------------- ICEBERG_METADATA TABLE FUNCTION -------------

struct IcebergTableEntry {
	IcebergManifest manifest;
	vector<IcebergManifestEntry> manifest_entries;
};

struct IcebergTable {
public:
	IcebergTable(const IcebergSnapshot &snapshot);

public:
	//! Loads all(!) metadata of into IcebergTable object
	static IcebergTable Load(const string &iceberg_path, const IcebergTableMetadata &metadata,
	                         const IcebergSnapshot &snapshot, ClientContext &context, const IcebergOptions &options);

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

	//! The snapshot of this table
	const IcebergSnapshot &snapshot;
	//! The entries (manifests) of this table
	vector<IcebergTableEntry> entries;

protected:
	string path;
};

} // namespace duckdb

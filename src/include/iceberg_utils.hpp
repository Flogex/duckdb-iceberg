//===----------------------------------------------------------------------===//
//                         DuckDB
//
// iceberg_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/printer.hpp"
#include "iceberg_types.hpp"
#include "yyjson.hpp"
#include "duckdb/common/file_system.hpp"

using namespace duckdb_yyjson;

namespace duckdb {

class IcebergUtils {
public:
	//! Downloads a file fully into a string
	static string FileToString(const string &path, FileSystem &fs);
	//! Downloads a gz file fully into a string
	static string GzFileToString(const string &path, FileSystem &fs);
	//! Somewhat hacky function that allows relative paths in iceberg tables to be resolved,
	//! used for the allow_moved_paths debug option which allows us to test with iceberg tables that
	//! were moved without their paths updated
	static string GetFullPath(const string &iceberg_path, const string &relative_file_path, FileSystem &fs);

	//! YYJSON utility functions
	static uint64_t TryGetNumFromObject(yyjson_val *obj, const string &field);
	static string TryGetStrFromObject(yyjson_val *obj, const string &field);
	static bool TryGetBoolFromObject(yyjson_val *obj, const string &field);

	static uint64_t TryGetNumFromObject(yyjson_val *obj, const string &field, bool fail_on_missing,
	                                    uint64_t default_val = 0);
	static bool TryGetBoolFromObject(yyjson_val *obj, const string &field, bool fail_on_missing,
	                                 bool default_val = false);
	static string TryGetStrFromObject(yyjson_val *obj, const string &field, bool fail_on_missing,
	                                  const char *default_val = "");
};

} // namespace duckdb
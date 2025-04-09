//===----------------------------------------------------------------------===//
//                         DuckDB
//
// url_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

class IRCEndpointBuilder {
public:
	void AddPathComponent(const string &component);

	void SetHost(const string &host);
	string GetHost() const;

	void SetVersion(const string &version_);
	string GetVersion() const;

	void SetParam(const string &key, const string &value);
	string GetParam(const string &key) const;
	const unordered_map<string, string> GetParams();

	string GetURL() const;

	//! path components when querying. Like namespaces/tables etc.
	vector<string> path_components;

private:
	string host;
	//! version
	string version;
	unordered_map<string, string> params;
};

} // namespace duckdb

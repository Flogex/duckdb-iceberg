#define DUCKDB_EXTENSION_MAIN

#include "iceberg_extension.hpp"
#include "storage/irc_catalog.hpp"
#include "storage/irc_transaction_manager.hpp"

#include "duckdb.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "iceberg_functions.hpp"
#include "yyjson.hpp"
#include "catalog_api.hpp"
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {

static unique_ptr<BaseSecret> CreateCatalogSecretFunction(ClientContext &context, CreateSecretInput &input) {
	// apply any overridden settings
	vector<string> prefix_paths;
	auto result = make_uniq<KeyValueSecret>(prefix_paths, "iceberg", "config", input.name);
	result->redact_keys = {"token", "client_id", "client_secret"};

	case_insensitive_set_t accepted_parameters {"client_id", "client_secret", "endpoint", "oauth2_scope",
	                                            "oauth2_server_uri"};
	for (const auto &named_param : input.options) {
		auto &param_name = named_param.first;
		auto it = accepted_parameters.find(param_name);
		if (it != accepted_parameters.end()) {
			result->secret_map[param_name] = named_param.second.ToString();
		} else {
			throw InvalidInputException("Unknown named parameter passed to CreateIRCSecretFunction: %s", param_name);
		}
	}

	//! If the bearer token is explicitly given, there is no need to make a request, use it directly.
	auto token_it = result->secret_map.find("token");
	if (token_it != result->secret_map.end()) {
		return std::move(result);
	}

	// Check if we have an oauth2_server_uri, or fall back to the deprecated oauth endpoint
	string server_uri;
	auto oauth2_server_uri_it = result->secret_map.find("oauth2_server_uri");
	auto endpoint_it = result->secret_map.find("endpoint");
	if (oauth2_server_uri_it != result->secret_map.end()) {
		server_uri = oauth2_server_uri_it->second.ToString();
	} else if (endpoint_it != result->secret_map.end()) {
		DUCKDB_LOG_WARN(
		    context, "iceberg",
		    "'oauth2_server_uri' is not set, defaulting to deprecated '{endpoint}/v1/oauth/tokens' oauth2_server_uri");
		server_uri = endpoint_it->second.ToString();
	} else {
		throw InvalidInputException(
		    "No 'oauth2_server_uri' was provided, and no 'endpoint' was provided to fall back on");
	}

	// Make a request to the oauth2 server uri to get the (bearer) token
	result->secret_map["token"] =
	    IRCAPI::GetToken(context, server_uri, result->secret_map["client_id"].ToString(),
	                     result->secret_map["client_secret"].ToString(), result->secret_map["oauth2_scope"].ToString());
	return std::move(result);
}

static void SetCatalogSecretParameters(CreateSecretFunction &function) {
	function.named_parameters["client_id"] = LogicalType::VARCHAR;
	function.named_parameters["client_secret"] = LogicalType::VARCHAR;
	function.named_parameters["endpoint"] = LogicalType::VARCHAR;
	function.named_parameters["token"] = LogicalType::VARCHAR;
	function.named_parameters["oauth2_scope"] = LogicalType::VARCHAR;
	function.named_parameters["oauth2_server_uri"] = LogicalType::VARCHAR;
}

static bool SanityCheckGlueWarehouse(string warehouse) {
	// valid glue catalog warehouse is <account_id>:s3tablescatalog/<bucket>
	auto end_account_id = warehouse.find_first_of(':');
	bool account_id_correct = end_account_id == 12;
	auto bucket_sep = warehouse.find_first_of('/');
	bool bucket_sep_correct = bucket_sep == 28;
	if (!account_id_correct) {
		throw IOException("Invalid Glue Catalog Format: '" + warehouse + "'. Expect 12 digits for account_id.");
	}
	if (bucket_sep_correct) {
		return true;
	}
	throw IOException("Invalid Glue Catalog Format: '" + warehouse +
	                  "'. Expected '<account_id>:s3tablescatalog/<bucket>");
}

static unique_ptr<Catalog> IcebergCatalogAttach(StorageExtensionInfo *storage_info, ClientContext &context,
                                                AttachedDatabase &db, const string &name, AttachInfo &info,
                                                AccessMode access_mode) {
	IRCCredentials credentials;
	IRCEndpointBuilder endpoint_builder;

	string account_id;
	string service;
	string endpoint_type;
	string endpoint;
	string oauth2_server_uri;

	auto &oauth2_scope = credentials.oauth2_scope;

	// check if we have a secret provided
	string storage_secret;
	string catalog_secret;
	for (auto &entry : info.options) {
		auto lower_name = StringUtil::Lower(entry.first);
		if (lower_name == "type" || lower_name == "read_only") {
			// already handled
		} else if (lower_name == "secret") {
			if (!storage_secret.empty() && !catalog_secret.empty()) {
				throw InvalidInputException("duplicate 'secret' (or 'catalog_secret' + 'storage_secret') found");
			}
			auto secret_name = StringUtil::Lower(entry.second.ToString());
			storage_secret = secret_name;
			catalog_secret = secret_name;
		} else if (lower_name == "catalog_secret") {
			if (!catalog_secret.empty()) {
				throw InvalidInputException("duplicate 'catalog_secret' found (or 'secret' was already set)");
			}
			catalog_secret = StringUtil::Lower(entry.second.ToString());
		} else if (lower_name == "storage_secret") {
			if (!storage_secret.empty()) {
				throw InvalidInputException("duplicate 'storage_secret' found (or 'secret' was already set)");
			}
			storage_secret = StringUtil::Lower(entry.second.ToString());
		} else if (lower_name == "endpoint_type") {
			endpoint_type = StringUtil::Lower(entry.second.ToString());
		} else if (lower_name == "endpoint") {
			endpoint = StringUtil::Lower(entry.second.ToString());
			StringUtil::RTrim(endpoint, "/");
		} else if (lower_name == "oauth2_scope") {
			oauth2_scope = StringUtil::Lower(entry.second.ToString());
		} else if (lower_name == "oauth2_server_uri") {
			oauth2_server_uri = StringUtil::Lower(entry.second.ToString());
		} else {
			throw BinderException("Unrecognized option for PC attach: %s", entry.first);
		}
	}
	auto warehouse = info.path;

	if (oauth2_scope.empty()) {
		//! Default to the Polaris scope: 'PRINCIPAL_ROLE:ALL'
		oauth2_scope = "PRINCIPAL_ROLE:ALL";
	}

	if (oauth2_server_uri.empty()) {
		//! If no oauth2_server_uri is provided, default to the (deprecated) REST API endpoint for it
		DUCKDB_LOG_WARN(
		    context, "iceberg",
		    "'oauth2_server_uri' is not set, defaulting to deprecated '{endpoint}/v1/oauth/tokens' oauth2_server_uri");
		oauth2_server_uri = StringUtil::Format("%s/v1/oauth/tokens", endpoint);
	}
	auto catalog_type = ICEBERG_CATALOG_TYPE::INVALID;

	if (endpoint_type == "glue" || endpoint_type == "s3_tables") {
		if (endpoint_type == "s3_tables") {
			service = "s3tables";
			catalog_type = ICEBERG_CATALOG_TYPE::AWS_S3TABLES;
		} else {
			service = endpoint_type;
			catalog_type = ICEBERG_CATALOG_TYPE::AWS_GLUE;
		}
		// look up any s3 secret

		// if there is no secret, an error will be thrown
		auto secret_entry = IRCatalog::GetS3Secret(context, storage_secret);
		auto kv_secret = dynamic_cast<const KeyValueSecret &>(*secret_entry->secret);
		auto region = kv_secret.TryGetValue("region");

		if (region.IsNull()) {
			throw IOException("Assumed catalog secret " + secret_entry->secret->GetName() + " for catalog " + name +
			                  " does not have a region");
		}
		switch (catalog_type) {
		case ICEBERG_CATALOG_TYPE::AWS_S3TABLES: {
			// extract region from the amazon ARN
			auto substrings = StringUtil::Split(warehouse, ":");
			if (substrings.size() != 6) {
				throw InvalidInputException("Could not parse S3 Tables arn warehouse value");
			}
			region = Value::CreateValue<string>(substrings[3]);
			break;
		}
		case ICEBERG_CATALOG_TYPE::AWS_GLUE:
			SanityCheckGlueWarehouse(warehouse);
			break;
		default:
			throw IOException("Unsupported AWS catalog type");
		}

		auto catalog_host = StringUtil::Format("%s.%s.amazonaws.com", service, region.ToString());
		auto catalog = make_uniq<IRCatalog>(db, access_mode, credentials, warehouse, catalog_host, storage_secret);
		catalog->catalog_type = catalog_type;
		catalog->GetConfig(context);
		return std::move(catalog);
	}

	// Check no endpoint type has been passed.
	if (!endpoint_type.empty()) {
		throw IOException("Unrecognized endpoint_type: %s. Expected either S3_TABLES or GLUE", endpoint_type);
	}
	if (endpoint_type.empty() && endpoint.empty()) {
		throw IOException("No endpoint type or endpoint provided");
	}

	catalog_type = ICEBERG_CATALOG_TYPE::OTHER;

	Value token;
	auto iceberg_secret = IRCatalog::GetIcebergSecret(context, catalog_secret);
	if (iceberg_secret) {
		auto &kv_iceberg_secret = dynamic_cast<const KeyValueSecret &>(*iceberg_secret->secret);
		token = kv_iceberg_secret.TryGetValue("token");
	} else {
		// Default IRC path
		Value endpoint_val;
		// Lookup a secret we can use to access the rest catalog.
		// if no secret is referenced, this method throws
		auto secret_entry = IRCatalog::GetS3Secret(context, storage_secret);
		D_ASSERT(secret_entry);
		// secret found - read data
		const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*secret_entry->secret);
		Value key_val = kv_secret.TryGetValue("key_id");
		Value secret_val = kv_secret.TryGetValue("secret");
		CreateSecretInput create_secret_input;
		create_secret_input.options["oauth2_server_uri"] = oauth2_server_uri;
		create_secret_input.options["client_id"] = key_val;
		create_secret_input.options["client_secret"] = secret_val;
		create_secret_input.options["endpoint"] = endpoint;
		create_secret_input.options["oauth2_scope"] = oauth2_scope;

		auto new_secret = CreateCatalogSecretFunction(context, create_secret_input);
		auto &kv_iceberg_secret = dynamic_cast<KeyValueSecret &>(*new_secret);
		token = kv_iceberg_secret.TryGetValue("token");
	}
	if (token.IsNull()) {
		throw IOException("Failed to generate oath token");
	}
	credentials.token = token.ToString();

	auto catalog = make_uniq<IRCatalog>(db, access_mode, credentials, warehouse, endpoint, storage_secret);
	catalog->catalog_type = catalog_type;
	catalog->GetConfig(context);
	return std::move(catalog);
}

static unique_ptr<TransactionManager> CreateTransactionManager(StorageExtensionInfo *storage_info, AttachedDatabase &db,
                                                               Catalog &catalog) {
	auto &ic_catalog = catalog.Cast<IRCatalog>();
	return make_uniq<ICTransactionManager>(db, ic_catalog);
}

class IRCStorageExtension : public StorageExtension {
public:
	IRCStorageExtension() {
		attach = IcebergCatalogAttach;
		create_transaction_manager = CreateTransactionManager;
	}
};

static void LoadInternal(DatabaseInstance &instance) {
	Aws::SDKOptions options;
	Aws::InitAPI(options); // Should only be called once.

	ExtensionHelper::AutoLoadExtension(instance, "avro");
	if (!instance.ExtensionIsLoaded("avro")) {
		throw MissingExtensionException("The iceberg extension requires the avro extension to be loaded!");
	}
	ExtensionHelper::AutoLoadExtension(instance, "parquet");
	if (!instance.ExtensionIsLoaded("parquet")) {
		throw MissingExtensionException("The iceberg extension requires the parquet extension to be loaded!");
	}

	auto &config = DBConfig::GetConfig(instance);

	config.AddExtensionOption("unsafe_enable_version_guessing",
	                          "Enable globbing the filesystem (if possible) to find the latest version metadata. This "
	                          "could result in reading an uncommitted version.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));

	// Iceberg Table Functions
	for (auto &fun : IcebergFunctions::GetTableFunctions(instance)) {
		ExtensionUtil::RegisterFunction(instance, fun);
	}

	// Iceberg Scalar Functions
	for (auto &fun : IcebergFunctions::GetScalarFunctions()) {
		ExtensionUtil::RegisterFunction(instance, fun);
	}

	IRCAPI::InitializeCurl();

	SecretType secret_type;
	secret_type.name = "iceberg";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";

	ExtensionUtil::RegisterSecretType(instance, secret_type);
	CreateSecretFunction secret_function = {"iceberg", "config", CreateCatalogSecretFunction};
	SetCatalogSecretParameters(secret_function);
	ExtensionUtil::RegisterFunction(instance, secret_function);

	config.storage_extensions["iceberg"] = make_uniq<IRCStorageExtension>();
}

void IcebergExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
}
std::string IcebergExtension::Name() {
	return "iceberg";
}

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void iceberg_init(duckdb::DatabaseInstance &db) {
	LoadInternal(db);
}

DUCKDB_EXTENSION_API const char *iceberg_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif

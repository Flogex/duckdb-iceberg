#pragma once

#include "storage/irc_authorization.hpp"

namespace duckdb {

class SIGV4Authorization : public IRCAuthorization {
public:
	static constexpr const IRCAuthorizationType TYPE = IRCAuthorizationType::SIGV4;

public:
	SIGV4Authorization();
	SIGV4Authorization(const string &secret);

public:
	static unique_ptr<IRCAuthorization> FromAttachOptions(IcebergAttachOptions &input);
	unique_ptr<HTTPResponse> GetRequest(ClientContext &context, const IRCEndpointBuilder &endpoint_builder) override;

public:
	string secret;
	string region;
};

} // namespace duckdb

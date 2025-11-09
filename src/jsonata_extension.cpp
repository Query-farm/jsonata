#define DUCKDB_EXTENSION_MAIN

#include "jsonata_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#include <jsonata/Jsonata.h>
#include <nlohmann/json.hpp>
#include "query_farm_telemetry.hpp"

namespace duckdb {

inline void JsonataScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &jsonata_vector = args.data[0];
	auto &data_vector = args.data[1];

	if (jsonata_vector.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(jsonata_vector)) {
			// If the JSONata expression is NULL, the result is NULL
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(result, true);
			return;
		}
		auto jsonata = ConstantVector::GetData<string_t>(jsonata_vector)[0];
		auto jsonata_expr = jsonata::Jsonata(jsonata.GetString());

		UnaryExecutor::Execute<string_t, string_t>(data_vector, result, args.size(), [&](string_t data) {
			auto parsed = nlohmann::json::parse(string(data.GetData(), data.GetSize()));
			auto output = jsonata_expr.evaluate(parsed);
			auto output_str = output.dump();
			return StringVector::AddString(result, output_str);
		});
	} else {
		BinaryExecutor::Execute<string_t, string_t, string_t>(
		    jsonata_vector, data_vector, result, args.size(), [&](string_t jsonata, string_t data) {
			    auto parsed = nlohmann::json::parse(string(data.GetData(), data.GetSize()));
			    auto jsonata_expr = jsonata::Jsonata(string(jsonata.GetData(), jsonata.GetSize()));
			    auto output = jsonata_expr.evaluate(parsed);
			    auto output_str = output.dump();
			    return StringVector::AddString(result, output_str);
		    });
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	auto jsonata_scalar_function =
	    ScalarFunction("jsonata", {LogicalType::VARCHAR, LogicalType::JSON()}, LogicalType::JSON(), JsonataScalarFun);
	loader.RegisterFunction(jsonata_scalar_function);

	QueryFarmSendTelemetry(loader, "jsonata", "2025110901");
}

void JsonataExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string JsonataExtension::Name() {
	return "jsonata";
}

std::string JsonataExtension::Version() const {
	return "2025110901";
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(jsonata, loader) {
	duckdb::LoadInternal(loader);
}
}

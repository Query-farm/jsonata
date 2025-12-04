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
		auto jsonata_str = ConstantVector::GetData<string_t>(jsonata_vector)[0];

		// Parse the JSONata expression once for all rows (constant optimization)
		std::unique_ptr<jsonata::Jsonata> jsonata_expr;
		try {
			jsonata_expr = make_uniq<jsonata::Jsonata>(jsonata_str.GetString());
		} catch (const std::exception &e) {
			throw InvalidInputException("Invalid JSONata expression: %s", e.what());
		}

		UnaryExecutor::Execute<string_t, string_t>(data_vector, result, args.size(), [&](string_t data) {
			nlohmann::json parsed;
			try {
				parsed = nlohmann::json::parse(string(data.GetData(), data.GetSize()));
			} catch (const nlohmann::json::parse_error &e) {
				throw InvalidInputException("Invalid JSON data: %s", e.what());
			}

			nlohmann::json output;
			try {
				output = jsonata_expr->evaluate(parsed);
			} catch (const std::exception &e) {
				throw InvalidInputException("JSONata evaluation error: %s", e.what());
			}

			return StringVector::AddString(result, output.dump());
		});
	} else {
		BinaryExecutor::Execute<string_t, string_t, string_t>(
		    jsonata_vector, data_vector, result, args.size(), [&](string_t jsonata_str, string_t data) {
			    nlohmann::json parsed;
			    try {
				    parsed = nlohmann::json::parse(string(data.GetData(), data.GetSize()));
			    } catch (const nlohmann::json::parse_error &e) {
				    throw InvalidInputException("Invalid JSON data: %s", e.what());
			    }

			    std::unique_ptr<jsonata::Jsonata> jsonata_expr;
			    try {
				    jsonata_expr = make_uniq<jsonata::Jsonata>(string(jsonata_str.GetData(), jsonata_str.GetSize()));
			    } catch (const std::exception &e) {
				    throw InvalidInputException("Invalid JSONata expression: %s", e.what());
			    }

			    nlohmann::json output;
			    try {
				    output = jsonata_expr->evaluate(parsed);
			    } catch (const std::exception &e) {
				    throw InvalidInputException("JSONata evaluation error: %s", e.what());
			    }

			    return StringVector::AddString(result, output.dump());
		    });
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	ScalarFunctionSet jsonata_function_set("jsonata");
	auto jsonata_scalar_function =
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::JSON()}, LogicalType::JSON(), JsonataScalarFun);
	jsonata_function_set.AddFunction(jsonata_scalar_function);

	CreateScalarFunctionInfo info(jsonata_function_set);
	info.descriptions.push_back({
	    // parameter_types
	    {LogicalType::VARCHAR, LogicalType::JSON()},
	    // parameter_names
	    {"expression", "json_data"},
	    // description
	    "Evaluates a JSONata expression against JSON data. JSONata is a lightweight query and transformation "
	    "language for JSON data. See https://jsonata.org for the full language reference.",
	    // examples
	    {"jsonata('Account.Name', '{\"Account\": {\"Name\": \"Firefly\"}}')",
	     "jsonata('$.prices[price > 100]', my_json_column)",
	     "jsonata('$sum(Order.Product.Price)', orders)"},
	    // categories
	    {"json"},
	});

	loader.RegisterFunction(info);

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

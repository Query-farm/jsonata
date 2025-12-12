#define DUCKDB_EXTENSION_MAIN

#include "jsonata_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#include <jsonata/Jsonata.h>
#include <nlohmann/json.hpp>
#include "query_farm_telemetry.hpp"

namespace duckdb {

static std::unique_ptr<jsonata::Jsonata> ParseJsonataExpression(const string &expr_str) {
	try {
		return make_uniq<jsonata::Jsonata>(expr_str);
	} catch (const std::exception &e) {
		throw InvalidInputException("Invalid JSONata expression: %s", e.what());
	}
}

static nlohmann::json ParseJsonData(string_t data) {
	try {
		return nlohmann::json::parse(data.GetData(), data.GetData() + data.GetSize());
	} catch (const nlohmann::json::parse_error &e) {
		throw InvalidInputException("Invalid JSON data: %s", e.what());
	}
}

static void BindJsonToFrame(std::shared_ptr<jsonata::Frame> frame, const nlohmann::json &bindings) {
	if (!bindings.is_object()) {
		throw InvalidInputException("Bindings must be a JSON object");
	}
	for (auto &[key, value] : bindings.items()) {
		frame->bind(key, jsonata::Jsonata::jsonToAny(value));
	}
}

static string_t EvaluateJsonata(jsonata::Jsonata &expr, const nlohmann::json &data, Vector &result) {
	nlohmann::json output;
	try {
		output = expr.evaluate(data);
	} catch (const std::exception &e) {
		throw InvalidInputException("JSONata evaluation error: %s", e.what());
	}
	return StringVector::AddString(result, output.dump());
}

static string_t EvaluateJsonataWithBindings(jsonata::Jsonata &expr, const nlohmann::json &data,
                                            const nlohmann::json &bindings, Vector &result) {
	nlohmann::json output;
	try {
		auto frame = expr.createFrame();
		BindJsonToFrame(frame, bindings);
		output = expr.evaluate(data, frame);
	} catch (const std::exception &e) {
		throw InvalidInputException("JSONata evaluation error: %s", e.what());
	}
	return StringVector::AddString(result, output.dump());
}

inline void JsonataScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &jsonata_vector = args.data[0];
	auto &data_vector = args.data[1];

	if (jsonata_vector.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(jsonata_vector)) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(result, true);
			return;
		}
		auto jsonata_str = ConstantVector::GetData<string_t>(jsonata_vector)[0];
		auto jsonata_expr = ParseJsonataExpression(jsonata_str.GetString());

		UnaryExecutor::Execute<string_t, string_t>(data_vector, result, args.size(), [&](string_t data) {
			auto parsed = ParseJsonData(data);
			return EvaluateJsonata(*jsonata_expr, parsed, result);
		});
	} else {
		BinaryExecutor::Execute<string_t, string_t, string_t>(
		    jsonata_vector, data_vector, result, args.size(), [&](string_t jsonata_str, string_t data) {
			    auto jsonata_expr = ParseJsonataExpression(string(jsonata_str.GetData(), jsonata_str.GetSize()));
			    auto parsed = ParseJsonData(data);
			    return EvaluateJsonata(*jsonata_expr, parsed, result);
		    });
	}
}

inline void JsonataScalarFunWithBindings(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &jsonata_vector = args.data[0];
	auto &data_vector = args.data[1];
	auto &bindings_vector = args.data[2];

	if (jsonata_vector.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(jsonata_vector)) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(result, true);
			return;
		}
		auto jsonata_str = ConstantVector::GetData<string_t>(jsonata_vector)[0];
		auto jsonata_expr = ParseJsonataExpression(jsonata_str.GetString());

		BinaryExecutor::Execute<string_t, string_t, string_t>(
		    data_vector, bindings_vector, result, args.size(), [&](string_t data, string_t bindings) {
			    auto parsed = ParseJsonData(data);
			    auto parsed_bindings = ParseJsonData(bindings);
			    return EvaluateJsonataWithBindings(*jsonata_expr, parsed, parsed_bindings, result);
		    });
	} else {
		TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
		    jsonata_vector, data_vector, bindings_vector, result, args.size(),
		    [&](string_t jsonata_str, string_t data, string_t bindings) {
			    auto jsonata_expr = ParseJsonataExpression(string(jsonata_str.GetData(), jsonata_str.GetSize()));
			    auto parsed = ParseJsonData(data);
			    auto parsed_bindings = ParseJsonData(bindings);
			    return EvaluateJsonataWithBindings(*jsonata_expr, parsed, parsed_bindings, result);
		    });
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	ScalarFunctionSet jsonata_function_set("jsonata");

	// 2-argument version: jsonata(expression, json_data)
	auto jsonata_scalar_function =
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::JSON()}, LogicalType::JSON(), JsonataScalarFun);
	jsonata_function_set.AddFunction(jsonata_scalar_function);

	// 3-argument version: jsonata(expression, json_data, bindings)
	auto jsonata_with_bindings = ScalarFunction({LogicalType::VARCHAR, LogicalType::JSON(), LogicalType::JSON()},
	                                            LogicalType::JSON(), JsonataScalarFunWithBindings);
	jsonata_function_set.AddFunction(jsonata_with_bindings);

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
	     "jsonata('$.prices[price > 100]', my_json_column)", "jsonata('$sum(Order.Product.Price)', orders)"},
	    // categories
	    {"json"},
	});
	info.descriptions.push_back({
	    // parameter_types
	    {LogicalType::VARCHAR, LogicalType::JSON(), LogicalType::JSON()},
	    // parameter_names
	    {"expression", "json_data", "bindings"},
	    // description
	    "Evaluates a JSONata expression against JSON data with external variable bindings. "
	    "The bindings parameter is a JSON object where keys become variable names accessible in the expression "
	    "using $variable_name syntax.",
	    // examples
	    {"jsonata('$name', '{}', '{\"name\": \"Alice\"}')", "jsonata('$x + $y', '{}', '{\"x\": 10, \"y\": 20}')",
	     "jsonata('items[price > $threshold]', my_json, '{\"threshold\": 100}')"},
	    // categories
	    {"json"},
	});

	loader.RegisterFunction(info);

	QueryFarmSendTelemetry(loader, "jsonata", "2025121201");
}

void JsonataExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string JsonataExtension::Name() {
	return "jsonata";
}

std::string JsonataExtension::Version() const {
	return "2025121201";
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(jsonata, loader) {
	duckdb::LoadInternal(loader);
}
}

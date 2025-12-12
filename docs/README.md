# DuckDB JSONata Extension by [Query.Farm](https://query.farm)

The **JSONata** extension, developed by **[Query.Farm](https://query.farm)**, brings the power of JSONata query and transformation language directly to your SQL queries in DuckDB. Transform, query, and manipulate JSON data with sophisticated expressions—all without leaving your database environment.

JSONata is a lightweight query and transformation language for JSON data, inspired by the location path semantics of XPath. Whether you're extracting nested values, restructuring JSON documents, or performing complex transformations, JSONata makes JSON manipulation in SQL elegant and efficient.

## What is JSONata?

[JSONata](https://jsonata.org) is a sophisticated query and transformation language for JSON data. It allows you to:
- Extract values from complex nested JSON structures
- Transform and reshape JSON documents
- Filter and aggregate JSON arrays
- Perform calculations and string manipulations
- Create new JSON structures from existing data

Learn more at [https://jsonata.org](https://jsonata.org)

## Use Cases

The JSONata extension is perfect for:

- **JSON data extraction**: Pull specific values from deeply nested JSON structures
- **Data transformation**: Reshape and restructure JSON documents to match target schemas
- **ETL pipelines**: Transform JSON data during ingestion or export
- **API response processing**: Extract and transform data from API responses stored as JSON
- **Schema analysis**: Query JSON schema documents to extract property names and structure
- **Log analysis**: Parse and transform JSON log entries
- **Configuration management**: Extract and transform configuration data stored as JSON

## Installation

**`jsonata` is a [DuckDB Community Extension](https://github.com/duckdb/community-extensions).**

You can install and load it with:

```sql
INSTALL jsonata FROM community;
LOAD jsonata;
```

## Quick Start

```sql
-- Install and load the extension
INSTALL jsonata FROM community;
LOAD jsonata;

-- Extract a simple value
SELECT jsonata('Account', '{"Account": 5}');
┌──────────────────────────────────────┐
│ jsonata('Account', '{"Account": 5}') │
│                 json                 │
├──────────────────────────────────────┤
│ 5                                    │
└──────────────────────────────────────┘


-- Extract and transform data
SELECT jsonata('FirstName & " " & Surname',
  '{"FirstName": "Fred", "Surname": "Smith"}') as r;
┌──────────────┐
│      r       │
│     json     │
├──────────────┤
│ "Fred Smith" │
└──────────────┘

-- Create new JSON structure from existing data
SELECT jsonata('{
  "name": FirstName & " " & Surname,
  "mobile": Phone[type = "mobile"].number
}', '{
  "FirstName": "Fred",
  "Surname": "Smith",
  "Phone": [
    {"type": "home", "number": "0203 544 1234"},
    {"type": "mobile", "number": "077 7700 1234"}
  ]
}') as r
┌────────────────────────────────────────────────┐
│                       r                        │
│                      json                      │
├────────────────────────────────────────────────┤
│ {"mobile":"077 7700 1234","name":"Fred Smith"} │
└────────────────────────────────────────────────┘
```

## Functions

### `jsonata(expression, json_data)`

Evaluates a JSONata expression against JSON data.

**Parameters:**
- `expression` (VARCHAR): The JSONata expression to evaluate
- `json_data` (JSON): The JSON data to query/transform

**Returns:** JSON

### `jsonata(expression, json_data, bindings)`

Evaluates a JSONata expression against JSON data with external variable bindings.

**Parameters:**
- `expression` (VARCHAR): The JSONata expression to evaluate
- `json_data` (JSON): The JSON data to query/transform
- `bindings` (JSON): A JSON object where keys become variable names accessible in the expression using `$variable_name` syntax

**Returns:** JSON

**Bindings Example:**

```sql
-- Simple variable binding
SELECT jsonata('$x + $y', '{}', '{"x": 10, "y": 20}') as result;
┌────────┐
│ result │
│  json  │
├────────┤
│ 30     │
└────────┘

-- Use bindings for dynamic filtering
SELECT jsonata(
  'items[price > $threshold]',
  '{"items": [{"price": 50}, {"price": 150}, {"price": 200}]}',
  '{"threshold": 100}'
) as expensive_items;
┌─────────────────────────────────┐
│         expensive_items         │
│              json               │
├─────────────────────────────────┤
│ [{"price":150},{"price":200}]   │
└─────────────────────────────────┘

-- Combine data access with bindings
SELECT jsonata(
  'name & " - " & $status',
  '{"name": "Order #123"}',
  '{"status": "shipped"}'
) as message;
┌─────────────────────────┐
│         message         │
│          json           │
├─────────────────────────┤
│ "Order #123 - shipped"  │
└─────────────────────────┘
```

Bindings are useful for:
- Passing parameters into JSONata expressions without modifying the expression string
- Dynamic filtering with thresholds or values from other columns
- Separating configuration from data transformation logic

**Examples:**


#### 1. Extract a Simple Value

```sql
SELECT jsonata('Account', '{"Account": 5}') as r
┌──────┐
│  r   │
│ json │
├──────┤
│ 5    │
└──────┘
```

Extract a single value from a JSON object using the field name.

#### 2. String Concatenation

```sql
SELECT jsonata(
  'FirstName & " " & Surname',
  '{"FirstName": "Fred", "Surname": "Smith"}'
);
┌──────────────┐
│      r       │
│     json     │
├──────────────┤
│ "Fred Smith" │
└──────────────┘
```

Combine multiple fields using the `&` concatenation operator.

#### 3. Filter and Extract from Arrays

```sql
SELECT jsonata(
  'Phone[type = "mobile"].number',
  '{
    "Phone": [
      {"type": "home", "number": "0203 544 1234"},
      {"type": "office", "number": "01962 001234"},
      {"type": "mobile", "number": "077 7700 1234"}
    ]
  }'
);
┌─────────────────┐
│        r        │
│      json       │
├─────────────────┤
│ "077 7700 1234" │
└─────────────────┘
```

Filter an array by a condition and extract specific fields.

#### 4. Create New JSON Structures

```sql
SELECT jsonata(
  '{
    "name": FirstName & " " & Surname,
    "mobile": Phone[type = "mobile"].number
  }',
  '{
    "FirstName": "Fred",
    "Surname": "Smith",
    "Age": 28,
    "Address": {
      "Street": "Hursley Park",
      "City": "Winchester",
      "Postcode": "SO21 2JN"
    },
    "Phone": [
      {"type": "home", "number": "0203 544 1234"},
      {"type": "office", "number": "01962 001234"},
      {"type": "mobile", "number": "077 7700 1234"}
    ]
  }'
) as r;
┌────────────────────────────────────────────────┐
│                       r                        │
│                      json                      │
├────────────────────────────────────────────────┤
│ {"mobile":"077 7700 1234","name":"Fred Smith"} │
└────────────────────────────────────────────────┘
```

Transform input JSON into a completely new structure, selecting and combining fields as needed.

#### 5. Query Nested Structures with Wildcards

```sql
SELECT jsonata(
  '**.properties ~> $keys()',
  '{
    "$schema": "http://json-schema.org/draft-04/schema#",
    "type": "object",
    "properties": {
      "Account": {
        "type": "object",
        "properties": {
          "Customer": {
            "type": "object",
            "properties": {
              "First Name": {"type": "string"},
              "Surname": {"type": "string"}
            }
          },
          "AccID": {"type": "string"},
          "Order": {
            "type": "array",
            "items": {
              "type": "object",
              "properties": {
                "OrderID": {"type": "string"},
                "Product": {"type": "array"}
              }
            }
          }
        }
      }
    }
  }'
) as r;
┌───────────────────────────────────────────────────────────────────────────────────┐
│                                         r                                         │
│                                       json                                        │
├───────────────────────────────────────────────────────────────────────────────────┤
│ ["AccID","Account","Customer","First Name","Order","OrderID","Product","Surname"] │
└───────────────────────────────────────────────────────────────────────────────────┘
```

Use the descendant wildcard operator (`**`) to traverse all levels of a nested structure and extract all property keys.

## Practical Examples

### Using Bindings with Table Data

```sql
-- Use column values as bindings for dynamic filtering
CREATE TABLE products (
  category VARCHAR,
  inventory JSON,
  min_quantity INTEGER
);

INSERT INTO products VALUES
  ('electronics', '{"items": [{"name": "Phone", "qty": 5}, {"name": "Laptop", "qty": 12}]}', 10),
  ('clothing', '{"items": [{"name": "Shirt", "qty": 25}, {"name": "Pants", "qty": 8}]}', 15);

-- Filter items based on per-row threshold from min_quantity column
SELECT
  category,
  jsonata(
    'items[qty >= $min].name',
    inventory,
    json_object('min', min_quantity)
  ) as items_in_stock
FROM products;
```

### Extract Data from API Responses

```sql
-- Extract specific fields from stored API responses
CREATE TABLE api_responses (
  id INTEGER,
  response JSON
);

INSERT INTO api_responses VALUES
  (1, '{"user": {"name": "Alice", "email": "alice@example.com", "roles": ["admin", "user"]}}');

SELECT
  id,
  jsonata('user.name', response) as name,
  jsonata('user.roles[0]', response) as primary_role
FROM api_responses;
```

### Transform JSON Arrays

```sql
-- Transform an array of objects
SELECT jsonata(
  'products.{
    "item": name,
    "cost": price * 1.2,
    "available": inStock
  }',
  '{
    "products": [
      {"name": "Widget", "price": 10.00, "inStock": true},
      {"name": "Gadget", "price": 25.00, "inStock": false}
    ]
  }'
) as r;
┌──────────────────────────────────────────────────────────────────────────────────────────────┐
│                                              r                                               │
│                                             json                                             │
├──────────────────────────────────────────────────────────────────────────────────────────────┤
│ [{"available":true,"cost":12,"item":"Widget"},{"available":false,"cost":30,"item":"Gadget"}] │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
```


### Extract Deeply Nested Values

```sql
-- Navigate complex nested structures
SELECT jsonata(
  'Account.Order[0].Product[0]."Product Name"',
  '{
    "Account": {
      "Order": [
        {
          "OrderID": "order1",
          "Product": [
            {"Product Name": "Super Widget", "Price": 99.99}
          ]
        }
      ]
    }
  }'
) as r;
┌────────────────┐
│       r        │
│      json      │
├────────────────┤
│ "Super Widget" │
└────────────────┘
```

## JSONata Expression Syntax

JSONata provides a rich set of operators and functions. Here are some commonly used features:

### Path Navigation
- `.` - Navigate object properties: `Address.City`
- `[]` - Array indexing and filtering: `Phone[0]`, `Phone[type="mobile"]`
- `**` - Descendant wildcard: `**.Price` finds all Price fields at any depth

### Operators
- `&` - String concatenation: `FirstName & " " & LastName`
- `+`, `-`, `*`, `/` - Arithmetic operations
- `=`, `!=`, `<`, `<=`, `>`, `>=` - Comparison operators
- `and`, `or` - Boolean logic

### Functions
- `$count()` - Count array elements
- `$sum()` - Sum numeric values
- `$keys()` - Get object keys
- `$uppercase()`, `$lowercase()` - String transformations
- `$substring()` - Extract substrings
- Many more available at [https://docs.jsonata.org/](https://docs.jsonata.org/)

### Object Construction
```json
{
  "newField": expression,
  "anotherField": expression
}
```

For complete syntax and function reference, visit the [JSONata documentation](https://docs.jsonata.org/).

## Performance Considerations

- **Constant Expressions**: When the JSONata expression is constant (same for all rows), the extension optimizes by parsing the expression once and reusing it for all rows
- **Complex Expressions**: Very complex JSONata expressions or deeply nested JSON documents may require more processing time
- **Large JSON Documents**: Performance scales with the size and complexity of both the JSONata expression and the JSON data

## Working with Tables

You can use JSONata with JSON columns in your tables:

```sql
-- Create a table with JSON data
CREATE TABLE events (
  id INTEGER,
  event_data JSON
);

-- Extract and transform data using JSONata
SELECT
  id,
  jsonata('event.type', event_data) as event_type,
  jsonata('event.timestamp', event_data) as timestamp,
  jsonata('user.name', event_data) as user_name
FROM events;

-- Transform entire JSON documents
SELECT
  id,
  jsonata('{
    "type": event.type,
    "user": user.name,
    "location": event.location.city
  }', event_data) as summary
FROM events;
```

## Learn More

- **JSONata Official Site**: [https://jsonata.org](https://jsonata.org)
- **JSONata Documentation**: [https://docs.jsonata.org](https://docs.jsonata.org)
- **JSONata Playground**: [https://try.jsonata.org](https://try.jsonata.org) - Interactive tool to test JSONata expressions
- **Query.Farm**: [https://query.farm](https://query.farm) - More DuckDB extensions and tools

## Contributing

This extension is developed and maintained by [Query.Farm](https://query.farm). For issues, feature requests, or contributions, please visit our repository.

## License

See the LICENSE file for details.

---

**Developed with ❤️ by [Query.Farm](https://query.farm)**

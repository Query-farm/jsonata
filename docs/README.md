<p align="center">
  <a href="https://query.farm">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://query.farm/media-kit/logo/wordmark-dark.svg">
      <img alt="Query.Farm" src="https://query.farm/media-kit/logo/wordmark-light.svg" height="64">
    </picture>
  </a>
</p>

# DuckDB JSONata Extension by Query.Farm

[![DuckDB](https://img.shields.io/badge/DuckDB-community_extension-fdf1e0?logo=duckdb&logoColor=fff000)](https://duckdb.org/community_extensions/extensions/jsonata.html)
[![v1.5 build](https://github.com/Query-farm/jsonata/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=v1.5)](https://github.com/Query-farm/jsonata/actions/workflows/MainDistributionPipeline.yml?query=branch%3Av1.5)

The **JSONata** extension, developed by **[Query.Farm](https://query.farm)**, brings the power of JSONata query and transformation language directly to your SQL queries in DuckDB. Transform, query, and manipulate JSON data with sophisticated expressions—all without leaving your database environment.

## Documentation

Full documentation, including installation, usage, the function reference, and cookbook examples, is available at:

**[https://query.farm/products/extensions/jsonata](https://query.farm/products/extensions/jsonata)**

## Installation

```sql
INSTALL jsonata FROM community;
LOAD jsonata;
```

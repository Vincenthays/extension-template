#define DUCKDB_EXTENSION_MAIN

#include "bigtable2_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include <google/cloud/bigtable/table.h>

using ::google::cloud::StatusOr;
using google::cloud::bigtable::Filter;
using google::cloud::bigtable::MakeDataClient;
using google::cloud::bigtable::RowReader;
using google::cloud::bigtable::Table;

namespace cbt = ::google::cloud::bigtable;

namespace duckdb {

struct Bigtable2FunctionData : TableFunctionData {
  idx_t row_idx = 0;
  idx_t prefix_idx = 0;
  idx_t prefix_count;
  vector<string> prefixes_start;
  vector<string> prefixes_end;

  shared_ptr<Table> table;
};

static unique_ptr<FunctionData> Bigtable2FunctionBind(
  ClientContext &context, 
  TableFunctionBindInput &input,
  vector<LogicalType> &return_types,
  vector<string> &names
) {
  names.emplace_back("pe_id");
  return_types.emplace_back(LogicalType::UBIGINT);
  names.emplace_back("date");
  return_types.emplace_back(LogicalType::DATE);
  names.emplace_back("shop_id");
  return_types.emplace_back(LogicalType::UINTEGER);
  names.emplace_back("price");
  return_types.emplace_back(LogicalType::FLOAT);
  names.emplace_back("base_price");
  return_types.emplace_back(LogicalType::FLOAT);
  names.emplace_back("unit_price");
  return_types.emplace_back(LogicalType::FLOAT);
  names.emplace_back("promo_id");
  return_types.emplace_back(LogicalType::UINTEGER);
  names.emplace_back("promo_text");
  return_types.emplace_back(LogicalType::VARCHAR);
  names.emplace_back("shelf");
  return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
  names.emplace_back("position");
  return_types.emplace_back(LogicalType::LIST(LogicalType::UINTEGER));
  names.emplace_back("is_paid");
  return_types.emplace_back(LogicalType::LIST(LogicalType::BOOLEAN));

  auto bind_data = make_uniq<Bigtable2FunctionData>();

  auto data_client = MakeDataClient("dataimpact-processing", "processing");
  bind_data->table = make_shared_ptr<Table>(data_client, "product");

  auto ls_pe_id = ListValue::GetChildren(input.inputs[0]);
  bind_data->prefix_count = ls_pe_id.size();

  for (auto &pe_id : ls_pe_id) {
    string prefix_id = StringValue::Get(pe_id);
    reverse(prefix_id.begin(), prefix_id.end());
    bind_data->prefixes_start.emplace_back(prefix_id + "/202424/");
    bind_data->prefixes_end.emplace_back(prefix_id + "/2024240");
  }
  
  return std::move(bind_data);
}

void Bigtable2Function(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
  auto &state = (Bigtable2FunctionData &)*data.bind_data;

  idx_t cardinality = 0;

  auto range = cbt::RowRange::Range(
    state.prefixes_start[state.prefix_idx], 
    state.prefixes_end[state.prefix_idx]
  );
  auto filter = Filter::PassAllFilter();

  if (++state.prefix_idx == state.prefix_count) {
    output.SetCardinality(cardinality);
    return;
  }

  for (StatusOr<cbt::Row> &row : state.table->ReadRows(range, filter)) {
    if (!row) throw std::move(row).status();

    auto row_key = row.value().row_key();
    auto index_1 = row_key.find_first_of('/');
    auto index_2 = row_key.find_last_of('/');

    string prefix_id = row_key.substr(0, index_1);
    reverse(prefix_id.begin(), prefix_id.end());

    uint64_t pe_id = std::stoul(prefix_id);
    uint32_t shop_id = std::stoul(row_key.substr(index_2 + 1));

    std::cout << pe_id << std::endl;

    std::array<bool, 7> arr_mask;
    std::array<Value, 7> arr_date;
    std::array<Value, 7> arr_price;
    std::array<Value, 7> arr_base_price;
    std::array<Value, 7> arr_unit_price;
    std::array<Value, 7> arr_promo_id;
    std::array<Value, 7> arr_promo_text;
    std::array<vector<Value>, 7> arr_shelf;
    std::array<vector<Value>, 7> arr_position;
    std::array<vector<Value>, 7> arr_is_paid;

    for (int i = 0; i < 7; i++) {
      arr_mask[i] = false;
    }

    for (auto &cell : row.value().cells()) {

      date_t date = Date::EpochToDate(cell.timestamp().count() / 1000000);
      int32_t weekday = Date::ExtractISODayOfTheWeek(date) - 1;

      arr_mask[weekday] = true;
      arr_date[weekday] = Value::DATE(date);

      switch (cell.family_name().at(0)) {
      case 'p':
        switch (cell.column_qualifier().at(0)) {
        case 'p':
          arr_price[weekday] = std::stod(cell.value());
          break;
        case 'b':
          arr_base_price[weekday] = std::stod(cell.value());
          break;
        case 'u':
          arr_unit_price[weekday] = std::stod(cell.value());
          break;
        }
        break;
      case 'd':
        arr_promo_id[weekday] = std::stoi(cell.column_qualifier());
        arr_promo_text[weekday] = cell.value();
        break;
      case 's':
      case 'S':
        arr_shelf[weekday].emplace_back(cell.column_qualifier());
        arr_position[weekday].emplace_back(std::stoi(cell.value()));
        arr_is_paid[weekday].emplace_back(cell.family_name().at(0) == 'S');
        break;
      }
    }

    for (int i = 0; i < 7; i++) {
      if (!arr_mask[i]) continue;

      output.SetValue(0, state.row_idx, Value::UBIGINT(pe_id));
      output.SetValue(1, state.row_idx, arr_date[i]);
      output.SetValue(2, state.row_idx, Value::UINTEGER(shop_id));
      output.SetValue(3, state.row_idx, arr_price[i]);
      output.SetValue(4, state.row_idx, arr_base_price[i]);
      output.SetValue(5, state.row_idx, arr_unit_price[i]);
      output.SetValue(6, state.row_idx, arr_promo_id[i]);
      output.SetValue(7, state.row_idx, arr_promo_text[i]);

      if (!arr_shelf[i].empty()) {
        output.SetValue(8, state.row_idx, Value::LIST(arr_shelf[i]));
        output.SetValue(9, state.row_idx, Value::LIST(arr_position[i]));
        output.SetValue(10, state.row_idx, Value::LIST(arr_is_paid[i]));
      }

      cardinality++;
      state.row_idx++;
    }
  }

  std::cout 
    << state.prefixes_start[state.prefix_idx] 
    << " - " 
    << state.prefixes_end[state.prefix_idx] 
    << " - "
    << state.row_idx
    << std::endl;

  output.SetCardinality(cardinality);
}

void Bigtable2Extension::Load(DuckDB &db) {
  TableFunction bigtable_function("bigtable2", {LogicalType::LIST(LogicalType::VARCHAR)}, Bigtable2Function, Bigtable2FunctionBind);
  ExtensionUtil::RegisterFunction(*db.instance, bigtable_function);
}

std::string Bigtable2Extension::Name() { 
  return "bigtable2"; 
}

std::string Bigtable2Extension::Version() const {
#ifdef EXT_VERSION_BIGTABLE2
  return EXT_VERSION_BIGTABLE2;
#else
  return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void bigtable2_init(duckdb::DatabaseInstance &db) {
  duckdb::DuckDB db_wrapper(db);
  db_wrapper.LoadExtension<duckdb::Bigtable2Extension>();
}

DUCKDB_EXTENSION_API const char *bigtable2_version() {
  return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif

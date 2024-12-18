#include "duckdb.hpp"
#include "search.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include <google/cloud/bigtable/table.h>

using ::google::cloud::GrpcNumChannelsOption;
using ::google::cloud::Options;
using ::google::cloud::StatusOr;
namespace cbt = ::google::cloud::bigtable;

namespace duckdb {

struct Keyword {
	bool valid = false;
	Value keyword_id;
	Value shop_id;
	Value date;
	Value position;
	Value pe_id;
	Value retailer_p_id;
	Value is_paid = false;
};

struct SearchFunctionData : TableFunctionData {
	id_t ranges_idx = 0;
	vector<cbt::RowRange> ranges;

	idx_t remainder_idx = 0;
	vector<Keyword> remainder;
};

unique_ptr<FunctionData> SearchFunctionBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	names = {"keyword_id", "shop_id", "date", "position", "pe_id", "retailer_p_id", "is_paid"};

	return_types = {LogicalType::UINTEGER, LogicalType::UINTEGER, LogicalType::TIMESTAMP_S, LogicalType::UTINYINT,
	                LogicalType::UBIGINT,  LogicalType::VARCHAR,  LogicalType::BOOLEAN};

	auto bind_data = make_uniq<SearchFunctionData>();
	const auto week_start = std::to_string(IntegerValue::Get(input.inputs[0]));
	const auto week_end = std::to_string(IntegerValue::Get(input.inputs[1]));
	const auto ls_keyword_id = ListValue::GetChildren(input.inputs[2]);

	for (const auto &keyword_id : ls_keyword_id) {
		string prefix_id = std::to_string(IntegerValue::Get(keyword_id));
		reverse(prefix_id.begin(), prefix_id.end());
		bind_data->ranges.emplace_back(
		    cbt::RowRange::Closed(prefix_id + "/" + week_start + "/", prefix_id + "/" + week_end + "0"));
	}

	return std::move(bind_data);
}

struct SearchGlobalState : GlobalTableFunctionState {
	vector<column_t> column_ids;
	cbt::Filter filter = cbt::Filter::PassAllFilter();
	cbt::Table table = cbt::Table(cbt::MakeDataConnection(Options {}.set<GrpcNumChannelsOption>(8)),
	                              cbt::TableResource("dataimpact-processing", "processing", "search"));
};

unique_ptr<GlobalTableFunctionState> SearchInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto global_state = make_uniq<SearchGlobalState>();
	global_state->column_ids = input.column_ids;
	global_state->filter = SearchFilter(input.column_ids);
	return std::move(global_state);
}

void SearchFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	const auto filter = cbt::Filter::PassAllFilter();
	auto &global_state = data.global_state->Cast<SearchGlobalState>();
	auto &bind_data = data.bind_data->CastNoConst<SearchFunctionData>();

	vector<Keyword> keyword_week(200 * 7 * 24);

	while (bind_data.ranges_idx < bind_data.ranges.size() &&
	       (bind_data.remainder.size() - bind_data.remainder_idx) < STANDARD_VECTOR_SIZE) {
		const auto &range = bind_data.ranges[bind_data.ranges_idx++];

		for (StatusOr<cbt::Row> &row_result : global_state.table.ReadRows(range, global_state.filter)) {
			if (!row_result)
				throw std::runtime_error(row_result.status().message());

			const auto &row = row_result.value();
			const auto &row_key = row.row_key();

			const auto &index_1 = row_key.find_first_of('/');
			const auto &index_2 = row_key.find_last_of('/');
			string prefix_id = row_key.substr(0, index_1);
			reverse(prefix_id.begin(), prefix_id.end());
			const auto keyword_id = Value::UINTEGER(std::stoull(prefix_id));
			const auto shop_id = Value::UINTEGER(std::stoul(row_key.substr(index_2 + 1)));

			for (const auto &cell : row.cells()) {
				const int32_t &position = std::stoul(cell.column_qualifier());

				if (position > 200 || cell.value().starts_with("id_ret_pos_"))
					continue;

				const timestamp_t timestamp = Timestamp::FromEpochMicroSeconds(cell.timestamp().count());
				const date_t &date = Timestamp::GetDate(timestamp);
				const int32_t &weekday = Date::ExtractISODayOfTheWeek(date) - 1;
				const int32_t &hour = Timestamp::GetTime(timestamp).micros / 3'600'000'000;
				const int32_t &week_hour = weekday * 24 + hour;

				auto &keyword_day = keyword_week[200 * week_hour + position - 1];
				keyword_day.valid = true;
				keyword_day.keyword_id = keyword_id;
				keyword_day.shop_id = shop_id;
				keyword_day.date = Value::TIMESTAMP(timestamp);
				keyword_day.position = Value::UTINYINT(position);

				if (global_state.filter == cbt::Filter::StripValueTransformer())
					continue;

				switch (cell.family_name()[0]) {
				case 'p':
					if (cell.value().starts_with("id_ret_"))
						keyword_day.retailer_p_id = Value(cell.value().substr(7));
					else
						keyword_day.pe_id = Value::UBIGINT(std::stoull(cell.value()));
					break;
				case 's':
					keyword_day.is_paid = Value::BOOLEAN(true);
					break;
				}
			}

			for (auto &keyword : keyword_week) {
				if (keyword.valid) {
					bind_data.remainder.emplace_back(keyword);
					keyword = Keyword();
				}
			}
		}
	}

	idx_t cardinality = 0;

	while (bind_data.remainder_idx < bind_data.remainder.size()) {
		const auto &day = bind_data.remainder[bind_data.remainder_idx++];

		for (idx_t i = 0; i < global_state.column_ids.size(); i++) {
			switch (global_state.column_ids[i]) {
			case 0:
				output.SetValue(i, cardinality, day.keyword_id);
				break;
			case 1:
				output.SetValue(i, cardinality, day.shop_id);
				break;
			case 2:
				output.SetValue(i, cardinality, day.date);
				break;
			case 3:
				output.SetValue(i, cardinality, day.position);
				break;
			case 4:
				output.SetValue(i, cardinality, day.pe_id);
				break;
			case 5:
				output.SetValue(i, cardinality, day.retailer_p_id);
				break;
			case 6:
				output.SetValue(i, cardinality, day.is_paid);
				break;
			}
		}

		if (++cardinality == STANDARD_VECTOR_SIZE) {
			output.SetCardinality(cardinality);
			return;
		}
	}

	output.SetCardinality(cardinality);
}

cbt::Filter SearchFilter(const vector<column_t> &column_ids) {
	set<string> filters_cf;

	for (const auto &column_id : column_ids) {
		switch (column_id) {
		case 3:
		case 4:
		case 5:
			filters_cf.emplace("p");
			break;
		case 6:
			filters_cf.emplace("s");
			break;
		}
	}

	vector<string> filters(filters_cf.begin(), filters_cf.end());

	switch (filters.size()) {
	case 1:
		return cbt::Filter::FamilyRegex(filters[0]);
	case 2:
		return cbt::Filter::PassAllFilter();
	default:
		return cbt::Filter::StripValueTransformer();
	}
}
} // namespace duckdb